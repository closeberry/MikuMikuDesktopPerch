#pragma once
#include <windows.h>

namespace DesktopMascot
{
    // Visible frame of a top-level window (DWM extended frame bounds: no drop shadow).
    bool VisibleFrame(HWND window,RECT& frame);

    // An ordinary application window the mascot can stand on: visible, not minimized or
    // cloaked (other virtual desktop / suspended UWP), not a shell or tool window, not ours,
    // and at least minWidth wide.
    bool IsStandableWindow(HWND window,HWND self,int minWidth);

    // Work area (excluding the taskbar) of the monitor nearest to a point.
    RECT WorkAreaAt(POINT point);
}
