#include "hooks/frame_sync.h"
#include "hooks/game_addresses.h"
#include "hooks/pattern_scan.h"
#include "common/log.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstring>

namespace hooks {
namespace {

// Every argument declared, floats included. A hook on OnFrameMove once
// corrupted the cameras (then mistaken for a "camera consumer", July build) for
// exactly the want of the two time registers -- xmm0 and xmm1 here, xmm1 and
// xmm2 on the render callback, which takes the device first.
using PFN_FrameMove   = void (*)(double time, float elapsed, void* user);
using PFN_FrameRender = void (*)(void* device, double time, float elapsed, void* user);
PFN_FrameMove   real_FrameMove   = nullptr;
PFN_FrameRender real_FrameRender = nullptr;

std::atomic<bool>     g_ready{false};
std::atomic<bool>     g_freeze{false};
std::atomic<uint32_t> g_update{0};
std::atomic<int>      g_lag{0};
volatile int*         g_clampFlag = nullptr;

// The recent updates' kind, readable from any thread: the camera hooks that
// ask run on the engine's render workers. Packed as (index << 1) | frozen, so
// a slot that has moved on to a newer update is told apart from the one asked
// for.
constexpr uint32_t kRing = 32;
std::atomic<uint64_t> g_kind[kRing]{};

// The time each recent update was handed, for the render that shows it. Main
// thread only -- both callbacks run there.
double g_time[kRing]{};
float  g_elapsed[kRing]{};

// The alternation (main thread only). Negative last-real time = not started.
double g_lastRealT = -1.0;
std::atomic<bool> g_nextFrozen{false};
// A frozen update's OnFrameMove is running, read wherever its work lands.
std::atomic<bool> g_frozenMove{false};

// FORCE FEEDBACK ACROSS A FROZEN UPDATE. The truck control update turns the
// truck's yaw since its last run into a rate by multiplying by 1/elapsed, and
// guards that against NaN only: the game itself passes elapsed 0 only while
// paused, which skips that code. A frozen update reaches it with 0, and the
// truck's heading can still differ from the last run's, since physics lands
// after the update that reads it. A turn then gives an infinite rate; a negative
// one passes the clamp into a peak and a filter that hold -inf for good, which
// the wheel gets as full lock (-10000). A vibration's phase, advanced by k/elapsed,
// goes the same way. So a frozen update runs it with the force state saved and
// put back, and the commands it posts are dropped: the wheel keeps the last real
// update's force, and the next real one measures the turn from the last real
// one, over the time that actually passed.
constexpr size_t kForceStateOffset = 0x30;   // to +0x6C, see game_addresses.h
constexpr size_t kForceStateSize   = 0x3C;
using PFN_TruckControl = void (*)(void* self, void* arg, float a, float elapsed);
using PFN_ForcePost    = void (*)(void* queue, const void* cmd);
PFN_TruckControl real_TruckControl = nullptr;
PFN_ForcePost    real_ForcePost    = nullptr;
thread_local bool t_holdPosts = false;
std::atomic<uint32_t> g_frozenTruckRuns{0};
std::atomic<uint32_t> g_heldPosts{0};

void det_TruckControl(void* self, void* arg, float a, float elapsed)
{
    if (!self || elapsed != 0.0f || !g_frozenMove.load()) {
        real_TruckControl(self, arg, a, elapsed);
        return;
    }
    uint8_t* state = static_cast<uint8_t*>(self) + kForceStateOffset;
    uint8_t saved[kForceStateSize];
    memcpy(saved, state, sizeof(saved));
    t_holdPosts = true;
    real_TruckControl(self, arg, a, elapsed);
    t_holdPosts = false;
    memcpy(state, saved, sizeof(saved));
    if (g_frozenTruckRuns.fetch_add(1) == 0)
        VRLOG("frame sync: a frozen update ran the truck control -- its force feedback "
              "state kept and its wheel commands held back");
}

void det_ForcePost(void* queue, const void* cmd)
{
    if (t_holdPosts) {
        g_heldPosts.fetch_add(1);
        return;
    }
    real_ForcePost(queue, cmd);
}

void det_FrameMove(double t, float dt, void* user)
{
    const uint32_t idx = g_update.load() + 1;
    double tt = t;
    float  dd = dt;
    bool frozen = false;
    if (g_freeze.load()) {
        if (g_lastRealT < 0.0) {           // starting: the first one is real
            g_lastRealT = t - dt;
            g_nextFrozen.store(false);
        }
        frozen = g_nextFrozen.load();
        g_nextFrozen.store(!frozen);
        if (frozen) {
            tt = g_lastRealT;
            dd = 0.0f;
        } else {
            dd = (float)(t - g_lastRealT);
            g_lastRealT = t;
        }
    } else {
        g_lastRealT = -1.0;
    }
    g_time[idx % kRing] = tt;
    g_elapsed[idx % kRing] = dd;
    g_kind[idx % kRing].store(((uint64_t)idx << 1) | (frozen ? 1u : 0u));
    g_update.store(idx);

    // OnFrameMove clamps the elapsed time it hands the game into [1/60, 1/20]
    // while this flag is set, which would give a frozen update a sixtieth of a
    // second anyway. It read 0 in every session measured; held off for the
    // length of a frozen update in case it ever is not.
    g_frozenMove.store(frozen);
    if (frozen && g_clampFlag && *g_clampFlag) {
        const int saved = *g_clampFlag;
        *g_clampFlag = 0;
        real_FrameMove(tt, dd, user);
        *g_clampFlag = saved;
    } else {
        real_FrameMove(tt, dd, user);
    }
    g_frozenMove.store(false);
}

void det_FrameRender(void* device, double t, float dt, void* user)
{
    // The time of the update this render shows, not of the one just run --
    // in-game those are an update apart.
    if (g_freeze.load() && g_lastRealT >= 0.0) {
        const uint32_t shown = g_update.load() - (uint32_t)g_lag.load();
        const uint64_t k = g_kind[shown % kRing].load();
        if ((uint32_t)(k >> 1) == shown) {
            t = g_time[shown % kRing];
            dt = g_elapsed[shown % kRing];
        }
    }
    real_FrameRender(device, t, dt, user);
}

template <typename T>
bool hook_callback(const char* name, uintptr_t rva, void* detour, T** real,
                   const char* lost = "Synched Sequential unavailable")
{
    if (!rva) {
        VRLOG("frame sync: %s not found in this game build -- %s", name, lost);
        return false;
    }
    void* target = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)) + rva;
    if (!is_function_entry(target)) {
        VRLOG("frame sync: %s at exe+0x%llX is not a function entry -- refused",
              name, (unsigned long long)rva);
        return false;
    }
    if (MH_CreateHook(target, detour, reinterpret_cast<void**>(real)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        VRLOG("frame sync: %s hook FAILED", name);
        return false;
    }
    return true;
}

} // namespace

void install_frame_sync()
{
    static std::atomic<bool> s_done{false};
    if (s_done.exchange(true)) return;
    const GameAddresses& a = game_addresses();
    if (a.elapsedClampFlag)
        g_clampFlag = reinterpret_cast<volatile int*>(
            reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)) + a.elapsedClampFlag);
    const bool m = hook_callback("OnFrameMove", a.frameMove,
                                 reinterpret_cast<void*>(&det_FrameMove), &real_FrameMove);
    const bool r = m && hook_callback("OnFrameRender", a.frameRender,
                                      reinterpret_cast<void*>(&det_FrameRender), &real_FrameRender);
    g_ready.store(m && r);
    if (m && r)
        VRLOG("frame sync: OnFrameMove and OnFrameRender hooked -- Synched Sequential available");

    // The post first: the truck control hook alone would let a frozen update's
    // commands through.
    constexpr const char* kFfbLost = "a wheel's constant force can stick at full lock "
                                     "under Synched Sequential";
    if (m && r &&
        hook_callback("force post", a.forcePost, reinterpret_cast<void*>(&det_ForcePost),
                      &real_ForcePost, kFfbLost) &&
        hook_callback("truck control", a.truckControl,
                      reinterpret_cast<void*>(&det_TruckControl), &real_TruckControl, kFfbLost))
        VRLOG("frame sync: force feedback guard in -- a frozen update leaves the wheel's "
              "force alone");
}

bool frame_sync_ready() { return g_ready.load(); }

void set_frame_freeze(bool on)
{
    if (on && !g_ready.load()) return;
    if (g_freeze.exchange(on) != on) {
        VRLOG("frame sync: every second update frozen -> %s", on ? "ON" : "OFF");
        if (!on && g_frozenTruckRuns.load())
            VRLOG("frame sync: %u frozen truck control run(s) kept out of the force "
                  "feedback, %u wheel command(s) held back",
                  g_frozenTruckRuns.exchange(0), g_heldPosts.exchange(0));
    }
}

bool frame_freeze_on() { return g_freeze.load(); }

uint32_t frame_update_index() { return g_update.load(); }

bool frame_update_frozen(uint32_t index)
{
    const uint64_t k = g_kind[index % kRing].load();
    return (uint32_t)(k >> 1) == index && (k & 1u) != 0;
}

bool frame_next_update_frozen() { return g_freeze.load() && g_nextFrozen.load(); }

void set_frame_render_lag(int lag) { g_lag.store(lag < 0 ? 0 : (lag > 2 ? 2 : lag)); }

} // namespace hooks
