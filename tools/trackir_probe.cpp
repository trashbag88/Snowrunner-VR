// trackir_probe.exe -- the mod's TrackIR reader (src/trackir) on its own, with
// no game. Prints the pose the camera would get, so the connection, the profile
// ID and each axis's direction can be checked at the desk. Reads/writes
// Snowrunner_TrackIR.ini next to the exe. Ctrl+C to quit; C recenters.
#include "trackir/trackir.h"
#include <conio.h>
#include <cstdarg>
#include <cstdio>

namespace vrlog {
void init(HMODULE) {}
void write(const char* fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    printf("\n[log] ");
    vprintf(fmt, a);
    printf("\n");
    va_end(a);
}
} // namespace vrlog

int main()
{
    HWND hwnd = GetConsoleWindow();
    printf("TrackIR probe -- TrackIR software must be running. C = recenter, Ctrl+C = quit.\n");
    printf("OpenXR sense: +yaw = look LEFT, +pitch = look UP, +roll = tilt LEFT,\n"
           "              +x = lean RIGHT, +y = rise UP, +z = lean BACK.\n\n");
    for (;;) {
        trackir::poll(hwnd);
        const trackir::Pose p = trackir::pose();
        if (p.valid)
            printf("\ryaw %+6.1f  pitch %+6.1f  roll %+6.1f deg   x %+5.1f  y %+5.1f  z %+5.1f cm   ",
                   p.yaw * 57.29578f, p.pitch * 57.29578f, p.roll * 57.29578f, p.x * 100, p.y * 100, p.z * 100);
        else
            printf("\r(no TrackIR data yet)                                                           ");
        if (_kbhit() && (_getch() | 0x20) == 'c') trackir::recenter();
        Sleep(16);
    }
}
