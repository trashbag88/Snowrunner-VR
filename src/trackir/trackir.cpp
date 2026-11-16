#include "trackir.h"
#include "../common/log.h"
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <string>

// NPClient interface, declared from NaturalPoint's public documentation
// (docs.trackir.com/trackir-sdk). Only the calls this mod uses.
namespace {

#pragma pack(push, 1)
struct NpData {
    unsigned short status;          // 0 = remote active, 1 = mouse emulation on
    unsigned short frameSignature;  // increments per new frame, 1..32766
    unsigned int   checksum;
    float roll, pitch, yaw;         // TIR units: +-16383 = +-180 deg
    float x, y, z;                  // TIR units: +-16383 = +-50 cm
    float reserved[9];
};
#pragma pack(pop)

constexpr unsigned short kNpRoll = 1, kNpPitch = 2, kNpYaw = 4, kNpX = 16, kNpY = 32, kNpZ = 64;
constexpr float kMaxValue = 16383.0f;
constexpr float kRadPerUnit = 3.14159265f / kMaxValue;
constexpr float kMetresPerUnit = 0.50f / kMaxValue;

using PfnHwnd   = int(__stdcall*)(HWND);
using PfnVoid   = int(__stdcall*)();
using PfnUShort = int(__stdcall*)(unsigned short);
using PfnPUShort= int(__stdcall*)(unsigned short*);
using PfnData   = int(__stdcall*)(NpData*);

struct Api {
    HMODULE    dll = nullptr;
    PfnHwnd    registerWindow = nullptr;
    PfnVoid    unregisterWindow = nullptr;
    PfnUShort  registerProfile = nullptr;
    PfnPUShort queryVersion = nullptr;
    PfnUShort  requestData = nullptr;
    PfnData    getData = nullptr;
    PfnVoid    startData = nullptr;
    PfnVoid    stopData = nullptr;
} g_api;

struct Settings {
    int   profileId   = 1000;   // NaturalPoint's SDK sample ID
    float yawScale    = 1.0f, pitchScale = 1.0f, rollScale = 1.0f;
    float xScale      = 1.0f, yScale = 1.0f, zScale = 1.0f;
    bool  useRoll     = true;
    bool  usePosition = true;
    bool  cockpitOnly = true;
};

Settings          g_set;
SRWLOCK           g_lock = SRWLOCK_INIT;
trackir::Pose     g_raw;            // latest frame, converted, no recenter/scale
float             g_center[6] = {}; // yaw pitch roll x y z at the last recenter
bool              g_connected = false;
HWND              g_hwnd = nullptr;
DWORD             g_nextAttempt = 0;
DWORD             g_nextIniCheck = 0;
FILETIME          g_iniTime{};
unsigned short    g_lastFrame = 0;
DWORD             g_lastNewFrameMs = 0;
std::atomic<bool> g_recenterPending{false};
std::atomic<unsigned> g_dropped{0};
std::wstring      g_iniPath;
char              g_status[128] = "Waiting for TrackIR";   // guarded by g_lock

void set_status(const char* fmt, ...)
{
    char buf[128];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    AcquireSRWLockExclusive(&g_lock);
    memcpy(g_status, buf, sizeof(g_status));
    ReleaseSRWLockExclusive(&g_lock);
}

std::wstring dll_dir()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&dll_dir), &self);
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(self, buf, MAX_PATH);
    std::wstring s = buf;
    const size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : s.substr(0, slash + 1);
}

float ini_float(const wchar_t* key, float def)
{
    wchar_t d[32], v[64];
    swprintf_s(d, L"%g", def);
    GetPrivateProfileStringW(L"TrackIR", key, d, v, 64, g_iniPath.c_str());
    return static_cast<float>(_wtof(v));
}

void write_default_ini()
{
    const char* text =
        "; SnowRunner TrackIR -- settings. Saved changes are picked up while the game runs.\n"
        ";\n"
        "; ProfileID   TrackIR profile ID the mod registers as. 1000 is NaturalPoint's SDK\n"
        ";             sample ID. The TrackIR software applies whichever of ITS profiles is\n"
        ";             mapped to this ID, so set your curves there.\n"
        "; *Scale      Multiplier per axis. Use a negative number to flip a direction that\n"
        ";             comes out backwards (e.g. YawScale=-1 if turning left looks right).\n"
        ";             X/Y/Z are lean: left-right, up-down, forward-back.\n"
        "; UseRoll     1 = tilt the view with head roll.\n"
        "; UsePosition 1 = lean (X/Y/Z) moves the camera.\n"
        "; CockpitOnly 1 = head look only in the cab view; exterior/orbit camera is left alone.\n"
        "\n"
        "[TrackIR]\n"
        "ProfileID=1000\n"
        "YawScale=1\n"
        "PitchScale=1\n"
        "RollScale=1\n"
        "XScale=1\n"
        "YScale=1\n"
        "ZScale=1\n"
        "UseRoll=1\n"
        "UsePosition=1\n"
        "CockpitOnly=1\n";
    HANDLE f = CreateFileW(g_iniPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteFile(f, text, static_cast<DWORD>(strlen(text)), &n, nullptr);
    CloseHandle(f);
}

void load_ini()
{
    Settings s;
    s.profileId   = GetPrivateProfileIntW(L"TrackIR", L"ProfileID", 1000, g_iniPath.c_str());
    s.yawScale    = ini_float(L"YawScale", 1.0f);
    s.pitchScale  = ini_float(L"PitchScale", 1.0f);
    s.rollScale   = ini_float(L"RollScale", 1.0f);
    s.xScale      = ini_float(L"XScale", 1.0f);
    s.yScale      = ini_float(L"YScale", 1.0f);
    s.zScale      = ini_float(L"ZScale", 1.0f);
    s.useRoll     = GetPrivateProfileIntW(L"TrackIR", L"UseRoll", 1, g_iniPath.c_str()) != 0;
    s.usePosition = GetPrivateProfileIntW(L"TrackIR", L"UsePosition", 1, g_iniPath.c_str()) != 0;
    s.cockpitOnly = GetPrivateProfileIntW(L"TrackIR", L"CockpitOnly", 1, g_iniPath.c_str()) != 0;

    const bool idChanged = s.profileId != g_set.profileId;
    AcquireSRWLockExclusive(&g_lock);
    g_set = s;
    ReleaseSRWLockExclusive(&g_lock);
    VRLOG("TrackIR ini: profile %d  scale yaw %.2f pitch %.2f roll %.2f  x %.2f y %.2f z %.2f  "
          "roll %s  position %s  cockpitOnly %s", s.profileId, s.yawScale, s.pitchScale, s.rollScale,
          s.xScale, s.yScale, s.zScale, s.useRoll ? "on" : "off", s.usePosition ? "on" : "off",
          s.cockpitOnly ? "on" : "off");
    if (idChanged && g_connected) {
        VRLOG("TrackIR: profile ID changed -- reconnecting");
        trackir::shutdown();
    }
}

void check_ini()
{
    const DWORD now = GetTickCount();
    if (now < g_nextIniCheck) return;
    g_nextIniCheck = now + 1000;
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(g_iniPath.c_str(), GetFileExInfoStandard, &a)) {
        write_default_ini();
        if (!GetFileAttributesExW(g_iniPath.c_str(), GetFileExInfoStandard, &a)) return;
    }
    if (CompareFileTime(&a.ftLastWriteTime, &g_iniTime) != 0) {
        g_iniTime = a.ftLastWriteTime;
        load_ini();
    }
}

bool load_dll()
{
    if (g_api.dll) return true;
    wchar_t dir[MAX_PATH] = {};
    DWORD size = sizeof(dir);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\NaturalPoint\\NaturalPoint\\NPClient Location", L"Path",
                     RRF_RT_REG_SZ, nullptr, dir, &size) != ERROR_SUCCESS) {
        VRLOG("TrackIR: NPClient location not in the registry -- is the TrackIR software installed?");
        return false;
    }
    std::wstring path = dir;
    if (!path.empty() && path.back() != L'\\') path += L'\\';
    path += L"NPClient64.dll";
    g_api.dll = LoadLibraryW(path.c_str());
    if (!g_api.dll) {
        VRLOG("TrackIR: could not load %ls (error %lu)", path.c_str(), GetLastError());
        return false;
    }
    auto get = [](const char* n) { return GetProcAddress(g_api.dll, n); };
    g_api.registerWindow   = reinterpret_cast<PfnHwnd>(get("NP_RegisterWindowHandle"));
    g_api.unregisterWindow = reinterpret_cast<PfnVoid>(get("NP_UnregisterWindowHandle"));
    g_api.registerProfile  = reinterpret_cast<PfnUShort>(get("NP_RegisterProgramProfileID"));
    g_api.queryVersion     = reinterpret_cast<PfnPUShort>(get("NP_QueryVersion"));
    g_api.requestData      = reinterpret_cast<PfnUShort>(get("NP_RequestData"));
    g_api.getData          = reinterpret_cast<PfnData>(get("NP_GetData"));
    g_api.startData        = reinterpret_cast<PfnVoid>(get("NP_StartDataTransmission"));
    g_api.stopData         = reinterpret_cast<PfnVoid>(get("NP_StopDataTransmission"));
    if (!g_api.registerWindow || !g_api.unregisterWindow || !g_api.registerProfile || !g_api.requestData ||
        !g_api.getData || !g_api.startData || !g_api.stopData) {
        VRLOG("TrackIR: %ls is missing NPClient exports", path.c_str());
        FreeLibrary(g_api.dll);
        g_api = Api{};
        return false;
    }
    VRLOG("TrackIR: loaded %ls", path.c_str());
    return true;
}

void connect()
{
    const DWORD now = GetTickCount();
    if (g_connected || !g_hwnd || now < g_nextAttempt) return;
    g_nextAttempt = now + 3000;
    if (!load_dll()) { g_nextAttempt = now + 15000; return; }

    static int s_lastErr = -1;
    auto fail = [](const char* step, int r) {
        if (r != s_lastErr) VRLOG("TrackIR: %s failed (NPResult %d) -- retrying every 3 s", step, r);
        s_lastErr = r;
        set_status("Not connected: %s failed (%d), retrying", step, r);
    };

    int r = g_api.registerWindow(g_hwnd);
    if (r != 0) {
        // 7 = already registered (a previous session did not unregister). Clear it and retry.
        g_api.unregisterWindow();
        r = g_api.registerWindow(g_hwnd);
        if (r != 0) { fail("NP_RegisterWindowHandle (is TrackIR running?)", r); return; }
    }
    if (g_api.queryVersion) {
        unsigned short v = 0;
        if (g_api.queryVersion(&v) == 0) VRLOG("TrackIR: NPClient version %d.%02d", v >> 8, v & 0xFF);
    }
    r = g_api.registerProfile(static_cast<unsigned short>(g_set.profileId));
    if (r != 0) {
        fail("NP_RegisterProgramProfileID", r);
        g_api.unregisterWindow();
        return;
    }
    g_api.requestData(kNpRoll | kNpPitch | kNpYaw | kNpX | kNpY | kNpZ);
    r = g_api.startData();
    if (r != 0) {
        fail("NP_StartDataTransmission", r);
        g_api.unregisterWindow();
        return;
    }
    g_connected = true;
    g_lastNewFrameMs = now;
    s_lastErr = -1;
    VRLOG("TrackIR: connected as profile ID %d", g_set.profileId);
    set_status("Connected, profile ID %d", g_set.profileId);
}

void read_frame()
{
    NpData d{};
    const int r = g_api.getData(&d);
    if (r != 0) return;   // NO_DATA between frames is normal
    if (d.status != 0) {
        static DWORD s_warn = 0;
        if (GetTickCount() > s_warn) {
            VRLOG("TrackIR: mouse emulation is on in the TrackIR software -- turn it off for head look");
            s_warn = GetTickCount() + 30000;
        }
        return;
    }
    if (d.frameSignature == g_lastFrame) return;
    g_lastFrame = d.frameSignature;
    g_lastNewFrameMs = GetTickCount();

    // GLITCH FRAMES. Seen 2026-11-15 on TrackIR 5.5.3: frames with several axes
    // pinned at exactly +-16383 at once (yaw +-180 deg AND x/y +-50 cm) between
    // normal ones, presumably the camera briefly mis-reading the clip. Held at
    // the last good pose, or the in-game view snaps round.
    //
    // BUT ONE AXIS ON THE LIMIT CAN BE REAL. Also seen 2026-11-15: a seat and
    // camera placement that keeps one axis at the limit for as long as the
    // player sits there. Dropping every such frame threw away more than half
    // of them. So a frame is a glitch only if
    //   - two or more axes sit on the limit together, or
    //   - one axis JUMPED there from far away (> 40% of its range) -- and even
    //     that stops counting once it has stayed there 0.25 s, because a head
    //     that is really there stays there.
    const float axes[6] = {d.roll, d.pitch, d.yaw, d.x, d.y, d.z};
    static float s_lastGood[6] = {};
    static DWORD s_limitSince[6] = {};   // when each axis reached the limit (0 = not there)
    const DWORD nowMs = GetTickCount();
    unsigned atLimit = 0, jumped = 0, axesAtLimit = 0;   // bit per axis
    for (int i = 0; i < 6; ++i) {
        if (std::fabs(axes[i]) < kMaxValue - 0.5f) { s_limitSince[i] = 0; continue; }
        atLimit |= 1u << i;
        ++axesAtLimit;
        if (!s_limitSince[i]) s_limitSince[i] = nowMs ? nowMs : 1;
        const bool fromFar = std::fabs(axes[i] - s_lastGood[i]) > 0.4f * 2.0f * kMaxValue;
        if (fromFar && nowMs - s_limitSince[i] < 250) jumped |= 1u << i;
    }
    if (axesAtLimit >= 2 || jumped) {
        // Per-axis counts, so the log can say WHICH axis sits on the limit: one
        // axis every frame is a profile curve saturating, several at once is the
        // camera losing the clip.
        static unsigned s_axisCount[6] = {};
        for (int i = 0; i < 6; ++i)
            if (atLimit & (1u << i)) ++s_axisCount[i];
        const unsigned n = g_dropped.fetch_add(1) + 1;
        if ((n & (n - 1)) == 0) {   // 1, 2, 4, 8... so it stays quiet
            VRLOG("TrackIR: dropped %u glitch frame(s) with an axis at the limit -- so far "
                  "roll %u pitch %u yaw %u x %u y %u z %u; this one: %s%s%s%s%s%s(raw yaw %.0f pitch %.0f "
                  "roll %.0f x %.0f y %.0f z %.0f)", n, s_axisCount[0], s_axisCount[1], s_axisCount[2],
                  s_axisCount[3], s_axisCount[4], s_axisCount[5],
                  (atLimit & 1) ? "roll " : "", (atLimit & 2) ? "pitch " : "", (atLimit & 4) ? "yaw " : "",
                  (atLimit & 8) ? "x " : "", (atLimit & 16) ? "y " : "", (atLimit & 32) ? "z " : "",
                  d.yaw, d.pitch, d.roll, d.x, d.y, d.z);
        }
        return;
    }
    for (int i = 0; i < 6; ++i) s_lastGood[i] = axes[i];
    if (atLimit) {
        static DWORD s_nextLimitLog = 0;
        if (nowMs > s_nextLimitLog) {
            s_nextLimitLog = nowMs + 30000;
            VRLOG("TrackIR: %s%s%s%s%s%sat the limit and accepted (a real position; that axis has "
                  "no range left -- recenter, or turn its curve down in the TrackIR profile)",
                  (atLimit & 1) ? "roll " : "", (atLimit & 2) ? "pitch " : "", (atLimit & 4) ? "yaw " : "",
                  (atLimit & 8) ? "x " : "", (atLimit & 16) ? "y " : "", (atLimit & 32) ? "z " : "");
        }
    }

    trackir::Pose p;
    p.valid = true;
    // TrackIR -> OpenXR axes. MEASURED 2026-11-15 with trackir_probe on TrackIR
    // 5.5.3: yaw (+ = look left), roll (+ = tilt left) and y (+ = up) already
    // match; pitch came out negative looking up and x negative leaning right.
    // z was not measured (the profile in use had it off) -- assumed to match.
    p.yaw   =  d.yaw * kRadPerUnit;
    p.pitch = -d.pitch * kRadPerUnit;
    p.roll  =  d.roll * kRadPerUnit;
    p.x     = -d.x * kMetresPerUnit;
    p.y     =  d.y * kMetresPerUnit;
    p.z     =  d.z * kMetresPerUnit;

    // First frames and a slow sample afterwards: the raw signs are what the ini's
    // scales correct, so the log needs to show them.
    static int s_logged = 0;
    static DWORD s_nextLog = 0;
    if (s_logged < 3 || GetTickCount() > s_nextLog) {
        ++s_logged;
        s_nextLog = GetTickCount() + 5000;
        VRLOG("TrackIR raw: yaw %+.1f pitch %+.1f roll %+.1f deg  x %+.1f y %+.1f z %+.1f cm",
              p.yaw * 57.29578f, p.pitch * 57.29578f, p.roll * 57.29578f, p.x * 100, p.y * 100, p.z * 100);
    }

    AcquireSRWLockExclusive(&g_lock);
    g_raw = p;
    if (g_recenterPending.exchange(false)) {
        const float c[6] = {p.yaw, p.pitch, p.roll, p.x, p.y, p.z};
        memcpy(g_center, c, sizeof(c));
    }
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace

namespace trackir {

void poll(HWND gameWindow)
{
    if (g_iniPath.empty()) g_iniPath = dll_dir() + L"Snowrunner_TrackIR.ini";
    check_ini();
    if (gameWindow) g_hwnd = gameWindow;
    connect();
    if (!g_connected) return;
    read_frame();

    // TrackIR closed or the camera unplugged: no new frames for a while.
    if (GetTickCount() - g_lastNewFrameMs > 5000) {
        AcquireSRWLockExclusive(&g_lock);
        const bool wasValid = g_raw.valid;
        g_raw.valid = false;
        ReleaseSRWLockExclusive(&g_lock);
        if (wasValid) VRLOG("TrackIR: no new frames for 5 s (paused, closed or out of view)");
    }
}

Pose pose()
{
    AcquireSRWLockShared(&g_lock);
    const Pose r = g_raw;
    const Settings s = g_set;
    float c[6];
    memcpy(c, g_center, sizeof(c));
    ReleaseSRWLockShared(&g_lock);

    Pose p;
    if (!r.valid) return p;
    p.valid = true;
    p.yaw   = (r.yaw - c[0]) * s.yawScale;
    p.pitch = (r.pitch - c[1]) * s.pitchScale;
    p.roll  = s.useRoll ? (r.roll - c[2]) * s.rollScale : 0.0f;
    if (s.usePosition) {
        p.x = (r.x - c[3]) * s.xScale;
        p.y = (r.y - c[4]) * s.yScale;
        p.z = (r.z - c[5]) * s.zScale;
    }
    return p;
}

void recenter()
{
    g_recenterPending.store(true);
    VRLOG("TrackIR: recenter requested");
}

void status_text(char* out, size_t size)
{
    if (!out || !size) return;
    AcquireSRWLockShared(&g_lock);
    const bool live = g_connected && g_raw.valid;
    snprintf(out, size, "%s%s", g_status, g_connected && !live ? " (no head data: paused or out of view)" : "");
    ReleaseSRWLockShared(&g_lock);
}

unsigned dropped_frames() { return g_dropped.load(); }

bool cockpit_only()
{
    AcquireSRWLockShared(&g_lock);
    const bool v = g_set.cockpitOnly;
    ReleaseSRWLockShared(&g_lock);
    return v;
}

void shutdown()
{
    if (g_connected) {
        g_api.stopData();
        g_api.unregisterWindow();
        g_connected = false;
        VRLOG("TrackIR: disconnected");
    }
    AcquireSRWLockExclusive(&g_lock);
    g_raw.valid = false;
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace trackir
