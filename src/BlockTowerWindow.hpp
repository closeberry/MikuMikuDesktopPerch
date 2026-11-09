#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch
{
    class MascotModel;

    // The block tower mini game's window (docs/BlockTower.md): she plays you, sitting across the
    // table in 3D (the Western parlour) with the tower between you. A block is chosen with a click
    // and drawn out by dragging it (or W and S); B puts it back while it may go back; 1-3 (or a
    // click) lay it on the top. Tab tells how to play. Esc gives up. 3D only; one at a time.
    struct BlockTowerOpponent
    {
        std::wstring name;
        std::vector<std::uint8_t> face;   // premultiplied BGRA, faceSize square (empty: her initial)
        int faceSize=0;
        int bond=0;                       // how close she and the user are (0-4)
        // Her model, drawn sitting at the table. It must stay loaded while the window is open;
        // TickBlockTowerWindow is given it every frame.
        MascotModel* model=nullptr;
        std::wstring toonDir;
        float edgeScale=1;
        int msaa=4;                       // MSAA samples and the textures' largest side
        unsigned maxTexture=1024;
        bool light=false;                 // fewer pixels drawn (the settings' light 3D)
        std::function<void()> onGameOver; // each game played to its end (not one given up)
        // Her words in her own voice: asks the LLM about situation (a stage direction), the line
        // coming back through BlockTowerHeard. False when it cannot now (a line of her own is shown).
        std::function<bool(const std::wstring& situation)> talk;
        // Something those watching may speak of: what happened, told as to one watching, how
        // much it matters (1-9), and a line to say without the LLM.
        std::function<void(const std::wstring& facts,int weight,const std::wstring& fallback)> onEvent;
        // Little lines of hers asked for ahead, all at once (by scene):
        // sends ask to the LLM, the lines coming back through BlockTowerLines.
        std::function<bool(const std::wstring& ask)> prefetch;
        // Her lines in her own words are kept in this file (empty: not kept), under this key
        // (her name and persona: a change asks again).
        std::wstring linesFile,styleKey;
        // The user's own lines (JSON; docs/GameLines.md), when there is such a file.
        std::wstring ownLinesFile;
        // Her line said aloud in her voice (the voice engine), when speaking aloud is on; an empty
        // line stops what she is saying.
        std::function<void(const std::wstring& line)> speak;
    };

    // Opens it (or brings it forward when open). onEnd: once, when the window closes.
    void ShowBlockTowerWindow(HWND owner,const BlockTowerOpponent& her,std::function<void()> onEnd);
    bool BlockTowerWindowOpen();
    HWND BlockTowerWindowHandle();
    bool BlockTowerWindowRect(RECT& r);

    // After the window closes: what to say on the game (a stage direction for the LLM, and her
    // own line without it). False when there was none.
    bool BlockTowerAfterword(std::wstring& situation,std::wstring& fallback);
    // The lines asked for through BlockTowerOpponent::prefetch: the LLM's answer, whole (empty: none).
    void BlockTowerLines(const std::wstring& answer);
    // Her line asked for through BlockTowerOpponent::talk, as it grows; done: it is all there.
    void BlockTowerHeard(const std::wstring& line,bool done);

    // Every frame while open: the tower's physics on by dt, her model as it is posed now drawn at
    // the table (she sits), and the tower.
    void TickBlockTowerWindow(MascotModel& model,double dt);

    // Diagnostics (--snapshot with --blocktower [turns [stage]]): the table's 3D view as last
    // drawn, to a PNG, after turns played for both with her judgment (the camera then turned by
    // yaw radians, as with --view-yaw); stage 1: the next block then half drawn out, 2: out and
    // over the top, 3: the rules shown, 4: the tower pushed over; "blocktower:" lines for the
    // snapshot's report.
    bool SaveBlockTowerView(const std::wstring& path);
    std::wstring DescribeBlockTower();
    void PlayBlockTowerMoves(int turns,float yaw=0,int stage=0);
}
