#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch
{
    class MascotModel;

    // The poker mini game's window (docs/Poker.md): she plays you at Texas hold'em, sitting across
    // the table in 3D (the mahjong room's); you play with the buttons (or the keys: F, C, 1 to 4,
    // Enter; Tab lists the hands). Esc gives up. One at a time.
    struct PokerOpponent
    {
        std::wstring name;
        std::vector<std::uint8_t> face;   // premultiplied BGRA, faceSize square (empty: her initial)
        int faceSize=0;
        int bond=0;                       // how close she and the user are (0-4)
        // Her model, drawn sitting at the table (null: 2D only, her face at the top). It must
        // stay loaded while the window is open; TickPokerWindow is given it every frame.
        MascotModel* model=nullptr;
        std::wstring toonDir;
        float edgeScale=1;
        int msaa=4;                       // MSAA samples and the textures' largest side
        unsigned maxTexture=1024;
        bool light=false;                 // fewer pixels drawn (the settings' light 3D)
        std::function<void()> onGameOver; // each game played to its end (not one given up)
        // Her words in her own voice: asks the LLM about situation (a stage direction), the line
        // coming back through PokerHeard. False when it cannot now (a line of her own is shown).
        std::function<bool(const std::wstring& situation)> talk;
        // Something those watching may speak of: what happened, told as to one watching, how
        // much it matters (1-9), and a line to say without the LLM.
        std::function<void(const std::wstring& facts,int weight,const std::wstring& fallback)> onEvent;
        // Little lines of hers asked for ahead, all at once (by scene):
        // sends ask to the LLM, the lines coming back through PokerLines.
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
    void ShowPokerWindow(HWND owner,const PokerOpponent& her,std::function<void()> onEnd);
    bool PokerWindowOpen();
    HWND PokerWindowHandle();
    bool PokerWindowRect(RECT& r);

    // After the window closes: what to say on the game (a stage direction for the LLM, and her
    // own line without it). False when there was none.
    bool PokerAfterword(std::wstring& situation,std::wstring& fallback);
    // The lines asked for through PokerOpponent::prefetch: the LLM's answer, whole (empty: none).
    void PokerLines(const std::wstring& answer);
    // Her line asked for through PokerOpponent::talk, as it grows; done: it is all there.
    void PokerHeard(const std::wstring& line,bool done);

    // Every frame while open: her model, as it is posed now, drawn at the table (she sits), and
    // the cards on their way.
    void TickPokerWindow(MascotModel& model,double dt);

    // Diagnostics (--snapshot with --poker [moves]): the table's 3D view as last drawn, to a
    // PNG, after moves steps played for both (the camera then turned by yaw radians, as with
    // --view-yaw); "poker:" lines for the snapshot's report.
    bool SavePokerView(const std::wstring& path);
    std::wstring DescribePoker();
    void PlayPokerMoves(int moves,float yaw=0);
    // Diagnostics (--poker-shot): a game played for moves steps (yours with her own
    // judgment), drawn as the 2D window would be, to a PNG; help: with the list of hands open.
    bool SavePokerShot(const std::wstring& path,int moves,std::uint32_t seed,bool help=false);
}
