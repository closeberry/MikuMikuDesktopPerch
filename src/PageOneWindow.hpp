#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch
{
    class MascotModel;

    // The Page One mini game's window (docs/PageOne.md): she plays you, sitting across the table
    // in 3D (the Western parlour); your cards lie along the bottom of the window, a click plays one
    // (the keys: D, K, O, C, 1-4, the arrows and Enter; Tab tells how to play). Esc gives up. One at a time.
    struct PageOneOpponent
    {
        std::wstring name;
        std::vector<std::uint8_t> face;   // premultiplied BGRA, faceSize square (empty: her initial)
        int faceSize=0;
        int bond=0;                       // how close she and the user are (0-4)
        // Her model, drawn sitting at the table (null: 2D only, her face at the top). It must
        // stay loaded while the window is open; TickPageOneWindow is given it every frame.
        MascotModel* model=nullptr;
        std::wstring toonDir;
        float edgeScale=1;
        int msaa=4;                       // MSAA samples and the textures' largest side
        unsigned maxTexture=1024;
        bool light=false;                 // fewer pixels drawn (the settings' light 3D)
        std::function<void()> onGameOver; // each game played to its end (not one given up)
        // Her words in her own voice: asks the LLM about situation (a stage direction), the line
        // coming back through PageOneHeard. False when it cannot now (a line of her own is shown).
        std::function<bool(const std::wstring& situation)> talk;
        // Something those watching may speak of: what happened, told as to one watching, how
        // much it matters (1-9), and a line to say without the LLM.
        std::function<void(const std::wstring& facts,int weight,const std::wstring& fallback)> onEvent;
        // Little lines of hers asked for ahead, all at once (by scene):
        // sends ask to the LLM, the lines coming back through PageOneLines.
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
    void ShowPageOneWindow(HWND owner,const PageOneOpponent& her,std::function<void()> onEnd);
    bool PageOneWindowOpen();
    HWND PageOneWindowHandle();
    bool PageOneWindowRect(RECT& r);

    // After the window closes: what to say on the game (a stage direction for the LLM, and her
    // own line without it). False when there was none.
    bool PageOneAfterword(std::wstring& situation,std::wstring& fallback);
    // The lines asked for through PageOneOpponent::prefetch: the LLM's answer, whole (empty: none).
    void PageOneLines(const std::wstring& answer);
    // Her line asked for through PageOneOpponent::talk, as it grows; done: it is all there.
    void PageOneHeard(const std::wstring& line,bool done);

    // Every frame while open: her model, as it is posed now, drawn at the table (she sits), and
    // the cards on their way.
    void TickPageOneWindow(MascotModel& model,double dt);

    // Diagnostics (--snapshot with --pageone [moves]): the table's 3D view as last drawn, to a
    // PNG, after moves steps played for both (the camera then turned by yaw radians, as with
    // --view-yaw); "pageone:" lines for the snapshot's report.
    bool SavePageOneView(const std::wstring& path);
    std::wstring DescribePageOne();
    void PlayPageOneMoves(int moves,float yaw=0);
    // Diagnostics (--pageone-shot): a game played for moves steps (yours with her own
    // judgment), drawn as the 2D window would be, to a PNG; help: with the rules open.
    bool SavePageOneShot(const std::wstring& path,int moves,std::uint32_t seed,bool help=false);
}
