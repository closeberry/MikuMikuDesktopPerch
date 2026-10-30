#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch
{
    class MascotModel;

    // The Othello mini game's window (docs/Othello.md): you against her, she sitting across the
    // table in 3D (the mahjong room's), the board between you. Click a square (or the arrow keys
    // and Enter) to lay your disc; the squares you can lay on are marked. Esc gives up. One at
    // a time.
    struct OthelloOpponent
    {
        std::wstring name;
        std::vector<std::uint8_t> face;   // premultiplied BGRA, faceSize square (empty: her initial)
        int faceSize=0;
        int bond=0;                       // how close she and the user are (0-4)
        // Her model, drawn sitting at the table (null: 2D only, her face at the top). It must
        // stay loaded while the window is open; TickOthelloWindow is given it every frame.
        MascotModel* model=nullptr;
        std::wstring toonDir;
        float edgeScale=1;
        int msaa=4;                       // MSAA samples and the textures' largest side
        unsigned maxTexture=1024;
        bool light=false;                 // fewer pixels drawn (the settings' light 3D)
        std::function<void()> onGameOver; // each game played to its end (not one given up)
        // Her words in her own voice: asks the LLM about situation (a stage direction), the line
        // coming back through OthelloHeard. False when it cannot now (a line of her own is shown).
        std::function<bool(const std::wstring& situation)> talk;
        // Something those watching may speak of: what happened, told as to one watching, how
        // much it matters (1-9), and a line to say without the LLM.
        std::function<void(const std::wstring& facts,int weight,const std::wstring& fallback)> onEvent;
        // Little lines of hers asked for ahead, all at once (for a good move, for thinking):
        // sends ask to the LLM, the lines coming back through OthelloLines.
        std::function<bool(const std::wstring& ask)> prefetch;
    };

    // Opens it (or brings it forward when open). onEnd: once, when the window closes.
    void ShowOthelloWindow(HWND owner,const OthelloOpponent& her,std::function<void()> onEnd);
    bool OthelloWindowOpen();
    HWND OthelloWindowHandle();
    bool OthelloWindowRect(RECT& r);

    // After the window closes: what to say on the game (a stage direction for the LLM, and her
    // own line without it). False when there was none.
    bool OthelloAfterword(std::wstring& situation,std::wstring& fallback);
    // The lines asked for through OthelloOpponent::prefetch: the LLM's answer, whole (empty: none).
    void OthelloLines(const std::wstring& answer);
    // Her line asked for through OthelloOpponent::talk, as it grows; done: it is all there.
    void OthelloHeard(const std::wstring& line,bool done);

    // Every frame while open: her model, as it is posed now, drawn at the table (she sits), and
    // the discs on their way.
    void TickOthelloWindow(MascotModel& model,double dt);

    // Diagnostics (--snapshot with --othello [moves]): the table's 3D view as last drawn, to a
    // PNG, after moves played for both (the camera then turned by yaw radians, as with
    // --view-yaw); "othello:" lines for the snapshot's report.
    bool SaveOthelloView(const std::wstring& path);
    std::wstring DescribeOthello();
    void PlayOthelloMoves(int moves,float yaw=0);
    // Diagnostics (--othello-shot): a game played for moves moves (yours as she would play
    // them), drawn as the 2D window would be, to a PNG.
    bool SaveOthelloShot(const std::wstring& path,int moves,std::uint32_t seed);
}
