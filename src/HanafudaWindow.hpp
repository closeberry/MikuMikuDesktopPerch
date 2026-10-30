#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch
{
    class MascotModel;

    // The hanafuda (koi-koi) mini game's window (docs/Hanafuda.md): you against her, she sitting
    // across the table in 3D (the mahjong room's), the field, the pile and both piles taken laid
    // on it, your hand standing at your edge. Click a card (or ←→ and Enter) to play it; when
    // it could take either of two, click the one to take. With a yaku made, koi-koi or stop by
    // the buttons (or 1 and 2). Esc gives up. One at a time.
    struct HanafudaOpponent
    {
        std::wstring name;
        std::vector<std::uint8_t> face;   // premultiplied BGRA, faceSize square (empty: her initial)
        int faceSize=0;
        int bond=0;                       // how close she and the user are (0-4)
        // Her model, drawn sitting at the table (null: 2D only, her face at the top). It must
        // stay loaded while the window is open; TickHanafudaWindow is given it every frame.
        MascotModel* model=nullptr;
        std::wstring toonDir;
        float edgeScale=1;
        int msaa=4;                       // MSAA samples and the textures' largest side
        unsigned maxTexture=1024;
        bool light=false;                 // fewer pixels drawn (the settings' light 3D)
        std::function<void()> onGameOver; // each game played to its end (not one given up)
        // Her words in her own voice: asks the LLM about situation (a stage direction), the line
        // coming back through HanafudaHeard. False when it cannot now (a line of her own is shown).
        std::function<bool(const std::wstring& situation)> talk;
        // Something those watching may speak of: what happened, told as to one watching, how
        // much it matters (1-9), and a line to say without the LLM.
        std::function<void(const std::wstring& facts,int weight,const std::wstring& fallback)> onEvent;
        // Little lines of hers asked for ahead, all at once (for a good card taken, for
        // thinking): sends ask to the LLM, the lines coming back through HanafudaLines.
        std::function<bool(const std::wstring& ask)> prefetch;
    };

    // Opens it (or brings it forward when open). onEnd: once, when the window closes.
    void ShowHanafudaWindow(HWND owner,const HanafudaOpponent& her,std::function<void()> onEnd);
    bool HanafudaWindowOpen();
    HWND HanafudaWindowHandle();
    bool HanafudaWindowRect(RECT& r);

    // After the window closes: what to say on the game (a stage direction for the LLM, and her
    // own line without it). False when there was none.
    bool HanafudaAfterword(std::wstring& situation,std::wstring& fallback);
    // The lines asked for through HanafudaOpponent::prefetch: the LLM's answer, whole (empty: none).
    void HanafudaLines(const std::wstring& answer);
    // Her line asked for through HanafudaOpponent::talk, as it grows; done: it is all there.
    void HanafudaHeard(const std::wstring& line,bool done);

    // Every frame while open: her model, as it is posed now, drawn at the table (she sits), and
    // the cards on their way.
    void TickHanafudaWindow(MascotModel& model,double dt);

    // Diagnostics (--snapshot with --hanafuda [moves]): the table's 3D view as last drawn, to a
    // PNG, after moves steps played for you (the camera then turned by yaw radians, as with
    // --view-yaw); "hanafuda:" lines for the snapshot's report.
    bool SaveHanafudaView(const std::wstring& path);
    std::wstring DescribeHanafuda();
    void PlayHanafudaMoves(int moves,float yaw=0);
    // Diagnostics (--hanafuda-shot): a game played for you for moves steps, drawn as the 2D
    // window would be, to a PNG. stop: L"koi" stops instead where you can first koi-koi,
    // L"pick" where you first have two cards to take from, L"end" at the first round won by a
    // yaku (moves then counts steps through the rounds).
    bool SaveHanafudaShot(const std::wstring& path,int moves,std::uint32_t seed,const std::wstring& stop={});
    // Diagnostics (--hanafuda-cards): every card's face, as in the hand and on the field, to a PNG.
    bool SaveHanafudaCards(const std::wstring& path);
}
