#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>

namespace MikuMikuDesktopPerch
{
    class MascotModel;

    // The golf mini game's window (docs/Golf.md): three holes of stroke play against her on a
    // course made from a seed, in 3D. Your shot: aim with the left and right keys, the club
    // with up and down, then three clicks (or Space) on the gauge: start, power, and the strike
    // as the marker comes back to the line. She plays her own shots with a swing of her own.
    // Drag with the right button to turn the camera, the wheel to come nearer, F1 puts it back.
    // Esc stops (Y to leave), Enter plays again once the round is over.
    struct GolfOpponent
    {
        std::wstring name;
        std::function<void()> onGameOver;   // each round played to its end
        // Her model, drawn on the course (required: there is no 2D course). It must stay loaded
        // while the window is open; TickGolfWindow is given it every frame.
        MascotModel* model=nullptr;
        std::wstring toonDir;
        float edgeScale=1;
        int msaa=4;
        unsigned maxTexture=1024;
        bool light=false;
        // As at hover hockey: something for those watching, her lines asked for ahead, where
        // they are kept, the user's own lines, her voice.
        std::function<void(const std::wstring& facts,int weight,const std::wstring& fallback)> onEvent;
        std::function<bool(const std::wstring& ask)> prefetch;
        std::wstring linesFile,styleKey;
        std::wstring ownLinesFile;
        std::function<void(const std::wstring& line)> speak;
    };

    void ShowGolfWindow(HWND owner,const GolfOpponent& her,std::function<void()> onEnd);
    bool GolfWindowOpen();
    HWND GolfWindowHandle();
    bool GolfWindowRect(RECT& r);
    void GolfLines(const std::wstring& answer);
    bool GolfAfterword(std::wstring& situation,std::wstring& fallback);
    void TickGolfWindow(MascotModel& model,double dt);

    // Diagnostics (--snapshot with --golf): your shots played for you (as she would choose
    // them), the view as last drawn to a PNG, and "golf:" lines for the report.
    void PlayGolfForTest();
    bool SaveGolfView(const std::wstring& path);
    std::wstring DescribeGolf();
    // Diagnostics (--golf-swing stage [yaw]): she stands at the first tee frozen at this stage of
    // her swing (0 address .. 3 finish), the camera round her by yaw (0: in front of her).
    void SetGolfSwingPose(float stage,float yaw);
    // Diagnostics (--golf-seed n): the first hole's seed.
    void SetGolfSeed(std::uint32_t seed);
}
