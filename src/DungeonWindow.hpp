#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch
{
    class MascotModel;

    // The dungeon mini game's window (docs/Dungeon.md): the dungeon in 3D (or the map from
    // above), the hero's state beside it and what happened below. Arrow keys or WASD move (into
    // a monster: the sword), F casts magic, Space waits, 1-9 and 0 use or equip, V changes the
    // camera, M the 2D map, Esc gives up. One at a time.
    struct DungeonHero
    {
        std::wstring name;
        std::vector<std::uint8_t> face;   // premultiplied BGRA, faceSize square (empty: her initial)
        int faceSize=0;
        // Her model, drawn in 3D (null: the 2D map only). It must stay loaded while the window
        // is open; TickDungeonWindow is given it every frame.
        MascotModel* model=nullptr;
        std::wstring toonDir;
        float edgeScale=1;
        // How richly the 3D view is drawn: MSAA samples and the textures' largest side; light:
        // fewer pixels drawn too (the settings' "draw the dungeon light").
        int msaa=4;
        unsigned maxTexture=1024;
        bool light=false;
    };

    struct DungeonResult
    {
        int floor=1,kills=0,turns=0;
        bool died=false;
    };

    // Opens it (or brings it forward when open). onEnd: once, when the adventure is over and
    // the window closes.
    void ShowDungeonWindow(HWND owner,const DungeonHero& hero,std::function<void(const DungeonResult&)> onEnd);
    bool DungeonWindowOpen();

    // Every frame while open: the 3D view drawn with her model as it is posed now. The view's
    // renderer is made when the window opens and released when it closes.
    // She walks there (her facing-forward gait, at its ground speed), so the model is changed.
    void TickDungeonWindow(MascotModel& model,double dt);
}
