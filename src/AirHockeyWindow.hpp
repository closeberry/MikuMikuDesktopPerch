#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>

namespace MikuMikuDesktopPerch
{
    class MascotModel;

    // The air hockey mini game's window (docs/AirHockey.md): the table seen at a slant from
    // behind your end, her standing at the far end with her mallet in hand (from above in 2D when
    // there is no 3D). Move the mouse to move your mallet; drag with the right button to turn the
    // camera (the middle one, or Shift and the right, to move it), the wheel to come nearer, a
    // right double-click or F1 puts it back. Space pauses, Esc gives up (Y to leave, N to go
    // on), Enter plays again once a game is over, 1-4 choose how fast the puck may go, 5-8 how
    // big the puck and mallets are. One at a time.
    struct AirHockeyOpponent
    {
        std::wstring name;
        int puckSpeed=2;                          // 1-4 (AirHockey::PuckSpeeds)
        std::function<void(int)> onPuckSpeed;     // another chosen with the keys (to keep in the settings)
        int pieceSize=3;                          // 1-4 (AirHockey::PieceSizes)
        std::function<void(int)> onPieceSize;     // likewise
        std::function<void()> onGameOver;         // each game played to its end (not one given up)
        // Her model, drawn standing at the table (null: 2D only). It must stay loaded while the
        // window is open; TickAirHockeyWindow is given it every frame.
        MascotModel* model=nullptr;
        std::wstring toonDir;
        float edgeScale=1;
        int msaa=4;                               // MSAA samples and the textures' largest side
        unsigned maxTexture=1024;
        bool light=false;                         // fewer pixels drawn (the settings' light 3D)
        // Something those watching may speak of: what happened, told as to one watching, how
        // much it matters (1-9), and a line to say without the LLM.
        std::function<void(const std::wstring& facts,int weight,const std::wstring& fallback)> onEvent;
        // Her lines for the game's moments, asked for ahead in one go (ask: the request to the
        // LLM), the answer coming back through AirHockeyLines: said at once when the moment
        // comes, with no wait. False when it cannot now.
        std::function<bool(const std::wstring& ask)> prefetch;
    };

    // Opens it (or brings it forward when open). onEnd: once, when the window closes.
    void ShowAirHockeyWindow(HWND owner,const AirHockeyOpponent& her,std::function<void()> onEnd);
    bool AirHockeyWindowOpen();
    // The window, and where it is on the screen while it shows (not minimized): the others
    // watch the game from beside it, as at mahjong.
    HWND AirHockeyWindowHandle();
    bool AirHockeyWindowRect(RECT& r);
    // The lines asked for through AirHockeyOpponent::prefetch: the LLM's answer, whole (empty: none).
    void AirHockeyLines(const std::wstring& answer);
    // After the window closes: what to say on the game (played to its end, or stopped midway),
    // a stage direction for the LLM and her own line without it. False when there was none.
    bool AirHockeyAfterword(std::wstring& situation,std::wstring& fallback);
    // Every frame while open: the game goes on, and her model, as it is posed now, is drawn at
    // the table holding her mallet.
    void TickAirHockeyWindow(MascotModel& model,double dt);

    // Diagnostics (--airhockey-sim): games between two of the opponent's players, checked for
    // what must not happen (the puck off the table, inside a mallet; a game that never ends),
    // as a report whose first line is "<n> wrong". Yours is played as a person might, with
    // the mouse's quick mallet. speed, size: the puck's top speed and the pieces' size chosen
    // (1-4; 0: the default).
    std::wstring RunAirHockeySim(int games,std::uint32_t seed,int speed=0,int size=0);
    // Diagnostics (--snapshot with --airhockey): your side played for you (by a player like hers,
    // the window not paused when away), and the window as last drawn, to a PNG.
    void PlayAirHockeyForTest();
    bool SaveAirHockeyView(const std::wstring& path);
    std::wstring DescribeAirHockey();   // "airhockey:" lines for the snapshot's report
    // Diagnostics (--airhockey-camera pitch,distance,yaw): the camera turned this way.
    void SetAirHockeyCamera(float pitch,float distance,float yaw);
    // Diagnostics (--airhockey-shot): two of them playing for seconds, drawn as the 2D window
    // would be, to a PNG.
    bool SaveAirHockeyShot(const std::wstring& path,double seconds,std::uint32_t seed);
}
