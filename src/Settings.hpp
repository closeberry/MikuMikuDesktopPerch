#pragma once
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch
{
    // MikuMikuDesktopPerch.ini next to the executable (UTF-16LE so Japanese paths survive).
    struct Settings
    {
        std::wstring modelPath;
        std::wstring motionPath;      // played once when opened, then now and then
        // Played while idle, one at a time in random order (empty: the generated standing
        // pose). Stored as one IdleMotion line, paths separated by '|'.
        std::vector<std::wstring> idleMotionPaths;
        // Played on a click while standing, one at random (empty: built-in reactions). Stored
        // like IdleMotion, as the ClickMotion line.
        std::vector<std::wstring> clickMotionPaths;
        std::wstring toonDir;       // folder holding toon01.bmp..toon10.bmp (MMD's Data folder)
        static constexpr int MinScale=50,MaxScale=200;
        int windowHeight=300;       // logical pixels at 96 DPI, at 100% scale
        int scale=100;              // percent, 10% steps from the menu
        int x=-1,y=-1;              // -1: place at bottom-right of the work area
        int fps=30;
        bool physics=true;
        int physicsFps=60;
        int msaa=4;
        bool topmost=true;
        float edgeScale=1.0f;
        int maxTextureSize=1024;   // larger textures are downscaled at load (0 = keep original)
        bool blink=true;
        bool breath=true;
        bool lookAtMouse=true;
        bool autoBehavior=true;     // walk around on its own
        bool windowMode=false;      // go to the active window, sit or hang there

        static std::wstring DefaultPath();
        void Load(const std::wstring& path);
        void Save(const std::wstring& path) const;
    };
}
