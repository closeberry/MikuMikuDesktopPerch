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
    // above), the hero's state beside it, her words and what happened below. Arrow keys or
    // WASD move (into a monster: the sword), F casts magic, Space waits (and looks for traps),
    // 1-9 and 0 use or equip, Enter buys, P takes a picture, Z shows the records, V changes the
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
        int bond=0;                 // how close she and the user are (0-4; 0 when not counted): a little help
        std::wstring recordPath;    // her records (characters\<model>.dungeon.txt); empty: not kept
        bool shutter=false;         // the album's shutter sound
        // Her words in her own voice: asks the LLM about situation (a stage direction), the line
        // coming back through DungeonHeard. False when it cannot now (her own line is shown).
        std::function<bool(const std::wstring& situation)> talk;
        // A talk by a campfire: what the user said, the facts of the adventure so far, and the
        // talk before it (true: the user's, false: hers). Her answer comes back whole, as it
        // grows, through DungeonHeard. False when the LLM cannot answer now.
        std::function<bool(const std::wstring& said,const std::wstring& facts,const std::vector<std::pair<bool,std::wstring>>& before)> chat;
    };

    struct DungeonResult
    {
        int floor=1,kills=0,turns=0,rested=0;
        bool died=false,deepest=false,daily=false;
        std::vector<std::wstring> titles;   // earned this time, by name
    };

    // Opens it (or brings it forward when open). onEnd: once, when the adventure is over and
    // the window closes.
    void ShowDungeonWindow(HWND owner,const DungeonHero& hero,std::function<void(const DungeonResult&)> onEnd);
    bool DungeonWindowOpen();

    // Her line asked for with DungeonHero::talk (or her answer by the campfire, DungeonHero::chat),
    // as it comes in (done: the end; an empty line when done means none came, and her own line
    // is shown).
    void DungeonHeard(const std::wstring& line,bool done);

    // Every frame while open: the 3D view drawn with her model as it is posed now. The view's
    // renderer is made when the window opens and released when it closes.
    // She walks there (her facing-forward gait, at its ground speed), so the model is changed.
    void TickDungeonWindow(MascotModel& model,double dt);
}
