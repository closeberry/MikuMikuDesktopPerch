#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch
{
    class MascotModel;

    // The hex war mini game's window (docs/HexWar.md): your army against hers on a map of
    // hexagons drawn flat, she sitting at the table in 3D in the column at the right. Click a
    // unit of yours, then where it goes, then one of hers next to it to attack; "End turn"
    // when done. The wheel (or the middle button) scrolls a map larger than the window.
    struct HexWarOpponent
    {
        std::wstring name;
        std::vector<std::uint8_t> face;   // premultiplied BGRA, faceSize square (empty: her initial)
        int faceSize=0;
        int bond=0;                       // how close she and the user are (0-4)
        // Her model, drawn sitting at the table (null: 2D only, her face at the top). It must
        // stay loaded while the window is open; TickHexWarWindow is given it every frame.
        MascotModel* model=nullptr;
        std::wstring toonDir;
        float edgeScale=1;
        int msaa=4;                       // MSAA samples and the textures' largest side
        unsigned maxTexture=1024;
        bool light=false;                 // fewer pixels drawn (the settings' light 3D)
        std::function<void()> onGameOver; // each game played to its end (giving up too)
        // Her words in her own voice: asks the LLM about situation (a stage direction), the line
        // coming back through HexWarHeard. False when it cannot now (a line of her own is shown).
        // Not used now: her lines come from her scenes (prefetch below).
        std::function<bool(const std::wstring& situation)> talk;
        // Something those watching may speak of: what happened, told as to one watching, how
        // much it matters (1-9), and a line to say without the LLM.
        std::function<void(const std::wstring& facts,int weight,const std::wstring& fallback)> onEvent;
        // Asks the LLM for her lines in her own words, a scene at a time: sends ask, the answer
        // coming back through HexWarLines.
        std::function<bool(const std::wstring& ask)> prefetch;
        // Her lines in her own words are kept in this file (empty: not kept), under this key
        // (her name and persona: a change asks again).
        std::wstring linesFile,styleKey;
        // The user's own lines (JSON; docs/GameLines.md), when there is such a file.
        std::wstring ownLinesFile;
        // The maps played and won with her are kept in this file (empty: not kept).
        std::wstring progressFile;
        // Her line said aloud in her voice (the voice engine), when speaking aloud is on; an empty
        // line stops what she is saying.
        std::function<void(const std::wstring& line)> speak;
    };

    // Opens it (or brings it forward when open). onEnd: once, when the window closes.
    void ShowHexWarWindow(HWND owner,const HexWarOpponent& her,std::function<void()> onEnd);
    bool HexWarWindowOpen();
    HWND HexWarWindowHandle();
    bool HexWarWindowRect(RECT& r);

    // After the window closes: what to say on the game (a stage direction for the LLM, and her
    // own line without it). False when there was none.
    bool HexWarAfterword(std::wstring& situation,std::wstring& fallback);
    // The lines asked for through HexWarOpponent::prefetch: the LLM's answer, whole (empty: none).
    void HexWarLines(const std::wstring& answer);
    // Her line asked for through HexWarOpponent::talk, as it grows; done: it is all there.
    void HexWarHeard(const std::wstring& line,bool done);

    // Every frame while open: her model, as it is posed now, drawn at the table (she sits).
    void TickHexWarWindow(MascotModel& model,double dt);

    // Diagnostics (--snapshot with --hexwar [turns]): the table's 3D view as last drawn, to a
    // PNG, after turns played for both and then your turn ended, hers shown as it is played
    // (the camera then turned by yaw radians, as with --view-yaw); "hexwar:" lines for the
    // snapshot's report.
    bool SaveHexWarView(const std::wstring& path);
    std::wstring DescribeHexWar();
    void PlayHexWarTurns(int turns,float yaw=0);
    // Diagnostics (--hexwar-shot): a game played for turns turns (yours as she would play
    // them), drawn as the 2D window would be, to a PNG; show: "select" (a unit of yours
    // chosen), "aim" (an attack pointed at), "fight" (its battle, seconds in), "turn" (the
    // "Your turn" band, seconds in), "orders" (the orders' menu), "strike" (a strike, seconds
    // in), "end".
    bool SaveHexWarShot(const std::wstring& path,int turns,std::uint32_t seed,const std::wstring& show,double seconds);
}
