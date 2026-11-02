#pragma once
#include <windows.h>
#include <functional>
#include <string>

namespace MikuMikuDesktopPerch
{
    class MascotModel;

    // The ruins' window (docs/Ruins.md): she runs and jumps through a fixed map seen from
    // the side, drawn in 3D (her model, the rooms' rock), or in 2D when there is no 3D.
    // ←→ (A, D) run, Z (Space) jumps; Esc gives up (Y to leave, N to go on). A gamepad works
    // too: the left stick or the D-pad runs, A jumps, START gives up (Y to leave, B to go on).
    // One at a time.
    struct RuinsHero
    {
        std::wstring name;
        // Her model, drawn in the ruins (null: 2D only). It must stay loaded while the window is
        // open; TickRuinsWindow is given it every frame.
        MascotModel* model=nullptr;
        std::wstring toonDir;
        float edgeScale=1;
        int msaa=4;                               // MSAA samples and the textures' largest side
        unsigned maxTexture=1024;
        bool light=false;                         // fewer pixels drawn (the settings' light 3D)
    };

    // Opens it (or brings it forward when open). onEnd: once, when the window closes.
    void ShowRuinsWindow(HWND owner,const RuinsHero& her,std::function<void()> onEnd);
    bool RuinsWindowOpen();
    HWND RuinsWindowHandle();
    // Where it is on the screen while it shows (not minimized): the others watch from beside it.
    bool RuinsWindowRect(RECT& r);
    // Every frame while open: she goes on through the ruins, her model as it is posed now drawn
    // there.
    void TickRuinsWindow(MascotModel& model,double dt);

    // Diagnostics (--snapshot with --ruins): keys held in turn instead of the keyboard's, as
    // "R1.0 RZ0.3 W0.5" (letters held for the seconds after them: L, R, U, D, Z jump, W none),
    // the window not paused when away; she starts in that room (its number in the map, from 1;
    // 0: the start).
    void PlayRuinsForTest(const std::wstring& keys,int room);
    bool SaveRuinsView(const std::wstring& path);
    std::wstring DescribeRuins();   // "ruins:" lines for the snapshot's report
    // Diagnostics (--ruins-check): what is wrong with the built-in map, "<n> wrong" first.
    std::wstring CheckRuinsMap();
}
