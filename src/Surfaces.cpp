#define NOMINMAX
#include "Surfaces.hpp"
#include <dwmapi.h>
#include <cwchar>
#include <initializer_list>
#include <iterator>
#pragma comment(lib,"dwmapi.lib")

namespace MikuMikuDesktopPerch
{
    bool VisibleFrame(HWND window,RECT& frame)
    {
        if(SUCCEEDED(DwmGetWindowAttribute(window,DWMWA_EXTENDED_FRAME_BOUNDS,&frame,sizeof(frame))))return true;
        return GetWindowRect(window,&frame)!=FALSE;
    }

    bool IsStandableWindow(HWND window,HWND self,int minWidth)
    {
        if(!window||window==self||!IsWindowVisible(window)||IsIconic(window))return false;
        if(GetAncestor(window,GA_ROOT)!=window)return false;
        BOOL cloaked=FALSE;
        if(SUCCEEDED(DwmGetWindowAttribute(window,DWMWA_CLOAKED,&cloaked,sizeof(cloaked)))&&cloaked)return false;
        if(GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_TOOLWINDOW)return false;
        wchar_t name[64]{};
        GetClassNameW(window,name,static_cast<int>(std::size(name)));
        for(const wchar_t* shell:{L"Shell_TrayWnd",L"Shell_SecondaryTrayWnd",L"Progman",L"WorkerW",L"Windows.UI.Core.CoreWindow"})
            if(wcscmp(name,shell)==0)return false;
        RECT frame{};
        if(!VisibleFrame(window,frame))return false;
        return frame.right-frame.left>=minWidth&&frame.bottom-frame.top>=120;
    }

    RECT WorkAreaAt(POINT point)
    {
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromPoint(point,MONITOR_DEFAULTTONEAREST),&info);
        return info.rcWork;
    }
}
