#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>

namespace MikuMikuDesktopPerch
{
    class MascotModel;

    // The bowling mini game's window (docs/Bowling.md): a bowling alley, your lane seen from
    // behind the approach; ten frames each, you first in each frame, she bowling her own on the
    // same lane (3D only). On your ball: move the mouse sideways to choose where you stand, the
    // wheel sets the spin (a hook to either side); press the left button, draw the mouse back
    // and push it forward to throw (how fast and which way it goes, the ball does). Drag with the
    // middle button to turn the camera (Shift and the middle one to move it), Ctrl and the wheel
    // to come nearer, a middle double-click or F1 puts it back. P pauses, Esc gives up (Y to
    // leave, N to go on), Enter plays again once a game is over, 1-3 choose how well she bowls.
    // One at a time.
    struct BowlingOpponent
    {
        std::wstring name;
        int level=2;                              // 1-3 (Bowling::LevelStyle)
        std::function<void(int)> onLevel;         // another chosen with the keys (to keep in the settings)
        // Her model, drawn on the approach. It must stay loaded while the window is open;
        // TickBowlingWindow is given it every frame.
        MascotModel* model=nullptr;
        std::wstring toonDir;
        float edgeScale=1;
        int msaa=4;                               // MSAA samples and the textures' largest side
        unsigned maxTexture=1024;
        bool light=false;                         // fewer pixels drawn (the settings' light 3D)
        // Something those watching may speak of: what happened, told as to one watching, how
        // much it matters (1-9), and a line to say without the LLM.
        std::function<void(const std::wstring& facts,int weight,const std::wstring& fallback)> onEvent;
        // Her lines for the game's moments, asked for ahead a scene at a time (ask: the request to
        // the LLM), the answer coming back through BowlingLines. False when it cannot now.
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
    void ShowBowlingWindow(HWND owner,const BowlingOpponent& her,std::function<void()> onEnd);
    bool BowlingWindowOpen();
    // The window, and where it is on the screen while it shows (not minimized): the others
    // watch the game from beside it, as at mahjong.
    HWND BowlingWindowHandle();
    bool BowlingWindowRect(RECT& r);
    // The lines asked for through BowlingOpponent::prefetch: the LLM's answer, whole (empty: none).
    void BowlingLines(const std::wstring& answer);
    // After the window closes: what to say on the game (played to its end, or stopped midway),
    // a stage direction for the LLM and her own line without it. False when there was none.
    bool BowlingAfterword(std::wstring& situation,std::wstring& fallback);
    // Every frame while open: the game goes on, and her model, as it is posed now, is drawn on
    // the approach with her ball.
    void TickBowlingWindow(MascotModel& model,double dt);

    // Diagnostics (--snapshot with --bowling): your balls thrown for you (the window not paused
    // when away, no sounds), and the window as last drawn, to a PNG.
    // stage: 0 play on; 1 she stops at the top of her backswing on her first ball, 2 at her
    // release, 3 at her follow-through, 4 your ball held drawn back, 5 she holds her address
    // (the ball before her chest) on her first ball.
    void PlayBowlingForTest(int stage=0);
    bool SaveBowlingView(const std::wstring& path);
    std::wstring DescribeBowling();   // "bowling:" lines for the snapshot's report
    // Diagnostics (--bowling-camera pitch,distance,yaw): the camera turned this way.
    void SetBowlingCamera(float pitch,float distance,float yaw);
}
