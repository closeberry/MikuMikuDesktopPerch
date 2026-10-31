#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch
{
    class MascotModel;

    // The shogi mini game's window (docs/Shogi.md): you against her, she sitting across the
    // table in 3D (the mahjong room's), the board between you and each side's stand of pieces
    // taken at its right. Click a piece of yours (or the arrow keys and Enter, 1-7 for one in
    // hand), then where it goes. Esc chooses again, or resigns. One at a time.
    struct ShogiOpponent
    {
        std::wstring name;
        std::vector<std::uint8_t> face;   // premultiplied BGRA, faceSize square (empty: her initial)
        int faceSize=0;
        int bond=0;                       // how close she and the user are (0-4)
        // Her model, drawn sitting at the table (null: 2D only, her face at the top). It must
        // stay loaded while the window is open; TickShogiWindow is given it every frame.
        MascotModel* model=nullptr;
        std::wstring toonDir;
        float edgeScale=1;
        int msaa=4;                       // MSAA samples and the textures' largest side
        unsigned maxTexture=1024;
        bool light=false;                 // fewer pixels drawn (the settings' light 3D)
        std::function<void()> onGameOver; // each game played to its end (a resignation too)
        // Her words in her own voice: asks the LLM about situation (a stage direction), the line
        // coming back through ShogiHeard. False when it cannot now (a line of her own is shown).
        // Not used now: her lines come from her scenes (prefetch below).
        std::function<bool(const std::wstring& situation)> talk;
        // Something those watching may speak of: what happened, told as to one watching, how
        // much it matters (1-9), and a line to say without the LLM.
        std::function<void(const std::wstring& facts,int weight,const std::wstring& fallback)> onEvent;
        // Asks the LLM for her lines in her own words, a scene at a time: sends ask, the answer
        // coming back through ShogiLines.
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
    void ShowShogiWindow(HWND owner,const ShogiOpponent& her,std::function<void()> onEnd);
    bool ShogiWindowOpen();
    HWND ShogiWindowHandle();
    bool ShogiWindowRect(RECT& r);

    // After the window closes: what to say on the game (a stage direction for the LLM, and her
    // own line without it). False when there was none.
    bool ShogiAfterword(std::wstring& situation,std::wstring& fallback);
    // The lines asked for through ShogiOpponent::prefetch: the LLM's answer, whole (empty: none).
    void ShogiLines(const std::wstring& answer);
    // Her line asked for through ShogiOpponent::talk, as it grows; done: it is all there.
    void ShogiHeard(const std::wstring& line,bool done);

    // Every frame while open: her model, as it is posed now, drawn at the table (she sits), and
    // the pieces on their way.
    void TickShogiWindow(MascotModel& model,double dt);

    // Diagnostics (--snapshot with --shogi [moves]): the table's 3D view as last drawn, to a
    // PNG, after moves played for both (the camera then turned by yaw radians, as with
    // --view-yaw); "shogi:" lines for the snapshot's report.
    bool SaveShogiView(const std::wstring& path);
    std::wstring DescribeShogi();
    void PlayShogiMoves(int moves,float yaw=0);
    // Diagnostics (--shogi-shot): a game played for moves moves (yours as she would play
    // them), drawn as the 2D window would be, to a PNG.
    bool SaveShogiShot(const std::wstring& path,int moves,std::uint32_t seed);
}
