#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch
{
    class MascotModel;

    // The blackjack mini game's window (docs/Blackjack.md): she deals, sitting across the table
    // in 3D (the mahjong room's); you bet chips and play with the buttons (or the keys: the
    // arrows and Enter to bet and deal, H, S, D and P). Esc gives up. One at a time.
    struct BlackjackOpponent
    {
        std::wstring name;
        std::vector<std::uint8_t> face;   // premultiplied BGRA, faceSize square (empty: her initial)
        int faceSize=0;
        int bond=0;                       // how close she and the user are (0-4)
        // Her model, drawn sitting at the table (null: 2D only, her face at the top). It must
        // stay loaded while the window is open; TickBlackjackWindow is given it every frame.
        MascotModel* model=nullptr;
        std::wstring toonDir;
        float edgeScale=1;
        int msaa=4;                       // MSAA samples and the textures' largest side
        unsigned maxTexture=1024;
        bool light=false;                 // fewer pixels drawn (the settings' light 3D)
        std::function<void()> onGameOver; // each game played to its end (not one given up)
        // Her words in her own voice: asks the LLM about situation (a stage direction), the line
        // coming back through BlackjackHeard. False when it cannot now (a line of her own is shown).
        std::function<bool(const std::wstring& situation)> talk;
        // Something those watching may speak of: what happened, told as to one watching, how
        // much it matters (1-9), and a line to say without the LLM.
        std::function<void(const std::wstring& facts,int weight,const std::wstring& fallback)> onEvent;
        // Little lines of hers asked for ahead, all at once (for dealing, for drawing):
        // sends ask to the LLM, the lines coming back through BlackjackLines.
        std::function<bool(const std::wstring& ask)> prefetch;
    };

    // Opens it (or brings it forward when open). onEnd: once, when the window closes.
    void ShowBlackjackWindow(HWND owner,const BlackjackOpponent& her,std::function<void()> onEnd);
    bool BlackjackWindowOpen();
    HWND BlackjackWindowHandle();
    bool BlackjackWindowRect(RECT& r);

    // After the window closes: what to say on the game (a stage direction for the LLM, and her
    // own line without it). False when there was none.
    bool BlackjackAfterword(std::wstring& situation,std::wstring& fallback);
    // The lines asked for through BlackjackOpponent::prefetch: the LLM's answer, whole (empty: none).
    void BlackjackLines(const std::wstring& answer);
    // Her line asked for through BlackjackOpponent::talk, as it grows; done: it is all there.
    void BlackjackHeard(const std::wstring& line,bool done);

    // Every frame while open: her model, as it is posed now, drawn at the table (she sits), and
    // the cards on their way.
    void TickBlackjackWindow(MascotModel& model,double dt);

    // Diagnostics (--snapshot with --blackjack [moves]): the table's 3D view as last drawn, to a
    // PNG, after moves steps played for both (the camera then turned by yaw radians, as with
    // --view-yaw); "blackjack:" lines for the snapshot's report.
    bool SaveBlackjackView(const std::wstring& path);
    std::wstring DescribeBlackjack();
    void PlayBlackjackMoves(int moves,float yaw=0);
    // Diagnostics (--blackjack-shot): a game played for moves steps (yours with the basic
    // strategy), drawn as the 2D window would be, to a PNG.
    bool SaveBlackjackShot(const std::wstring& path,int moves,std::uint32_t seed);
}
