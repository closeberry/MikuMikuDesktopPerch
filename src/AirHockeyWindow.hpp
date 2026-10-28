#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>

namespace MikuMikuDesktopPerch
{
    // The air hockey mini game's window (docs/AirHockey.md): the table seen from above, your end
    // at the bottom. Move the mouse to move your mallet; Space pauses, Esc gives up (Y to leave,
    // N to go on), Enter plays again once a game is over, 1-4 choose how fast the puck may go.
    // One at a time.
    struct AirHockeyOpponent
    {
        std::wstring name;
        int puckSpeed=2;                          // 1-4 (AirHockey::PuckSpeeds)
        std::function<void(int)> onPuckSpeed;     // another chosen with the keys (to keep in the settings)
        std::function<void()> onGameOver;   // each game played to its end (not one given up)
    };

    // Opens it (or brings it forward when open). onEnd: once, when the window closes.
    void ShowAirHockeyWindow(HWND owner,const AirHockeyOpponent& her,std::function<void()> onEnd);
    bool AirHockeyWindowOpen();

    // Diagnostics (--airhockey-sim): games between two of the opponent's players, checked for
    // what must not happen (the puck off the table, inside a mallet; a game that never ends),
    // as a report whose first line is "<n> wrong". Yours is played as a person might, with
    // the mouse's quick mallet. speed: the puck's top speed chosen (1-4; 0: the default).
    std::wstring RunAirHockeySim(int games,std::uint32_t seed,int speed=0);
    // Diagnostics (--snapshot with --airhockey): your side played for you (by a player like hers,
    // the window not paused when away), and the window as last drawn, to a PNG.
    void PlayAirHockeyForTest();
    bool SaveAirHockeyView(const std::wstring& path);
    std::wstring DescribeAirHockey();   // one "airhockey:" line for the snapshot's report
    // Diagnostics (--airhockey-shot): two of them playing for seconds, drawn as the window would
    // be, to a PNG.
    bool SaveAirHockeyShot(const std::wstring& path,double seconds,std::uint32_t seed);
}
