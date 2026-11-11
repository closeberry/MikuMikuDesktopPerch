#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>

namespace MikuMikuDesktopPerch
{
    class MascotModel;

    // The table tennis mini game's window (docs/TableTennis.md): the table seen from behind your
    // end, her standing at the far end with her racket in hand; on your side only your racket
    // (3D only). The mouse moves your racket over and behind your half (its height goes with the
    // ball); the left button strikes, the right one cuts, Space or the mouse's back button
    // smashes. On the half away from your racket hand the racket turns to the backhand. Drag
    // with the middle button to turn the camera (Shift and the middle one to move it), the wheel
    // to come nearer, a middle double-click or F1 puts it back. P pauses, Esc gives up (Y to
    // leave, N to go on), Enter plays again once a game is over, 1-3 choose how strong she is,
    // H changes your racket hand. One at a time.
    struct TableTennisOpponent
    {
        std::wstring name;
        int level=2;                              // 1-3 (TableTennis::LevelStyle)
        std::function<void(int)> onLevel;         // another chosen with the keys (to keep in the settings)
        bool leftHanded=false;                    // your racket hand
        std::function<void(bool)> onHand;         // likewise
        // Her model, drawn standing at the table. It must stay loaded while the window is open;
        // TickTableTennisWindow is given it every frame.
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
        // the LLM), the answer coming back through TableTennisLines. False when it cannot now.
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
    void ShowTableTennisWindow(HWND owner,const TableTennisOpponent& her,std::function<void()> onEnd);
    bool TableTennisWindowOpen();
    // The window, and where it is on the screen while it shows (not minimized): the others
    // watch the game from beside it, as at mahjong.
    HWND TableTennisWindowHandle();
    bool TableTennisWindowRect(RECT& r);
    // The lines asked for through TableTennisOpponent::prefetch: the LLM's answer, whole (empty: none).
    void TableTennisLines(const std::wstring& answer);
    // After the window closes: what to say on the game (played to its end, or stopped midway),
    // a stage direction for the LLM and her own line without it. False when there was none.
    bool TableTennisAfterword(std::wstring& situation,std::wstring& fallback);
    // Every frame while open: the game goes on, and her model, as it is posed now, is drawn at
    // the table with her racket.
    void TickTableTennisWindow(MascotModel& model,double dt);

    // Diagnostics (--snapshot with --tabletennis): your side played for you (the window not
    // paused when away, no sounds), and the window as last drawn, to a PNG.
    // across: every stroke of yours sent there across the table (metres; 9: as the mouse would).
    void PlayTableTennisForTest(float across=9);
    bool SaveTableTennisView(const std::wstring& path);
    std::wstring DescribeTableTennis();   // "tabletennis:" lines for the snapshot's report
    // Diagnostics (--tabletennis-camera pitch,distance,yaw): the camera turned this way.
    void SetTableTennisCamera(float pitch,float distance,float yaw);
}
