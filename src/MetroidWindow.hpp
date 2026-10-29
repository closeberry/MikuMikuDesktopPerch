#pragma once
#include <windows.h>
#include <functional>
#include <string>

namespace MikuMikuDesktopPerch
{
    class MascotModel;

    // The ruins' window (docs/Metroidvania.md): she runs and jumps through a fixed map seen from
    // the side, drawn in 3D (her model, the rooms' rock), or in 2D when there is no 3D.
    // ←→ (A, D) run, Z (Space) jumps; Esc gives up (Y to leave, N to go on). A gamepad works
    // too: the left stick or the D-pad runs, A jumps, START gives up (Y to leave, B to go on).
    // One at a time.
    struct MetroidHero
    {
        std::wstring name;
        // Her model, drawn in the ruins (null: 2D only). It must stay loaded while the window is
        // open; TickMetroidWindow is given it every frame.
        MascotModel* model=nullptr;
        std::wstring toonDir;
        float edgeScale=1;
        int msaa=4;                               // MSAA samples and the textures' largest side
        unsigned maxTexture=1024;
        bool light=false;                         // fewer pixels drawn (the settings' light 3D)
    };

    // Opens it (or brings it forward when open). onEnd: once, when the window closes.
    void ShowMetroidWindow(HWND owner,const MetroidHero& her,std::function<void()> onEnd);
    bool MetroidWindowOpen();
    HWND MetroidWindowHandle();
    // Where it is on the screen while it shows (not minimized): the others watch from beside it.
    bool MetroidWindowRect(RECT& r);
    // Every frame while open: she goes on through the ruins, her model as it is posed now drawn
    // there.
    void TickMetroidWindow(MascotModel& model,double dt);

    // Diagnostics (--snapshot with --metroid): keys held in turn instead of the keyboard's, as
    // "R1.0 RZ0.3 W0.5" (letters held for the seconds after them: L, R, U, D, Z jump, W none),
    // the window not paused when away; she starts in that room (its number in the map, from 1;
    // 0: the start).
    void PlayMetroidForTest(const std::wstring& keys,int room);
    bool SaveMetroidView(const std::wstring& path);
    std::wstring DescribeMetroid();   // "metroid:" lines for the snapshot's report
    // Diagnostics (--metroid-check): what is wrong with the built-in map, "<n> wrong" first.
    std::wstring CheckMetroidMap();
}
