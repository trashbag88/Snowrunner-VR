#pragma once
#include <windows.h>

// TRACKIR HEAD POSE, for the flat-screen build (SRVR_TRACKIR).
//
// Reads NaturalPoint's NPClient64.dll -- the DLL the TrackIR 5 software installs
// and names in HKCU\Software\NaturalPoint\NaturalPoint\NPClient Location -- and
// turns its frames into the same shape the VR build gets from OpenXR
// (xr::HeadLook: radians, metres, OpenXR axes). Everything downstream of
// xr::head_look() is untouched, which is the point: the camera injection sites
// in viewbuild_hook.cpp do not know or care where the pose came from.
//
// Settings live in Snowrunner_TrackIR.ini beside the DLL (written with defaults
// on first run): profile ID, per-axis scale/sign, which axes are on, and whether
// head look is limited to the cockpit view. Read once at start and again when
// the file changes on disk, so signs can be flipped without restarting the game.
//
// Every entry point is best-effort: if TrackIR is not running, the DLL is
// missing or registration fails, pose() reports nothing and the game camera is
// the game's own. It retries every few seconds, so starting TrackIR after the
// game works.
namespace trackir {

struct Pose {
    bool  valid = false;
    float yaw = 0, pitch = 0, roll = 0;   // radians, OpenXR sense (+yaw = left, +pitch = up)
    float x = 0, y = 0, z = 0;            // metres, OpenXR axes (x right, y up, z back)
};

// Call once per Present (render thread). Lazily connects, then reads a frame.
void poll(HWND gameWindow);

// Latest pose relative to the recenter point, with the ini's scales applied.
// Thread-safe; called from the game's update thread.
Pose pose();

// Zero the pose at the current head position (HOME / the menu's Recenter).
void recenter();

// Head look only in the cockpit view (ini: CockpitOnly, default on).
bool cockpit_only();

// One line for the settings menu: connected / waiting / why not.
void status_text(char* out, size_t size);

// Glitch frames dropped this session (axes pinned at the limit).
unsigned dropped_frames();

void shutdown();

} // namespace trackir
