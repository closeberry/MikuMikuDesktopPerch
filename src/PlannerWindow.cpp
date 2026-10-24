#include "PlannerWindow.hpp"
#include "Memory.hpp"
#include "Text.hpp"
#include <commctrl.h>
#include <algorithm>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch
{
    namespace
    {
        constexpr wchar_t ClassName[]=L"MikuMikuDesktopPerchPlanner";
        enum ControlId:int {IdAlarmList=2001,IdAlarmNew,IdAlarmAdd,IdAlarmDelete,IdTodoList,IdTodoNew,IdTodoAdd,IdTodoDone};

        struct State
        {
            HWND window=nullptr;
            HFONT font=nullptr;
            Planner* planner=nullptr;
            int dpi=96;
        } state;

        int Px(int logical){return MulDiv(logical,state.dpi,96);}

        HWND Add(const wchar_t* cls,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id=0)
        {
            HWND c=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,Px(x),Px(y),Px(w),Px(h),state.window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(state.font),TRUE);
            return c;
        }

        // Fill a list box with (line, id) rows, keeping its scroll position.
        void Fill(int id,const std::vector<std::pair<std::wstring,int>>& rows)
        {
            HWND list=GetDlgItem(state.window,id);
            const auto top=SendMessageW(list,LB_GETTOPINDEX,0,0);
            SendMessageW(list,WM_SETREDRAW,FALSE,0);
            SendMessageW(list,LB_RESETCONTENT,0,0);
            for(const auto& [line,value]:rows)
            {
                const auto index=SendMessageW(list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(line.c_str()));
                SendMessageW(list,LB_SETITEMDATA,index,value);
            }
            SendMessageW(list,LB_SETTOPINDEX,(std::max<LRESULT>)(0,top),0);
            SendMessageW(list,WM_SETREDRAW,TRUE,0);
            InvalidateRect(list,nullptr,TRUE);
        }

        void FillLists()
        {
            if(!state.window||!state.planner)return;
            const std::int64_t now=Now();
            std::vector<std::pair<std::wstring,int>> rows;
            for(const auto& a:state.planner->Alarms())
            {
                std::wstring line=TimeText(a.due,now,a.daily);
                if(a.timerSeconds>0)line+=Fmt(T(L"（{0}のタイマー）",L" ({0} timer)"),{DurationText(a.timerSeconds)});
                if(!a.text.empty())line+=L"　"+a.text;
                rows.push_back({line,a.id});
            }
            Fill(IdAlarmList,rows);
            rows.clear();
            for(const auto& t:state.planner->Todos())rows.push_back({t.text,t.id});
            Fill(IdTodoList,rows);
        }

        void Save()
        {
            if(state.planner&&!state.planner->Save())
                MessageBoxW(state.window,T(L"予定とやることを保存できませんでした。",L"Could not save the reminders and to-dos."),L"MikuMikuDesktopPerch",MB_ICONERROR);
            FillLists();
        }

        std::wstring Typed(int id)
        {
            std::wstring text(Planner::MaxText*2+1,L'\0');
            text.resize(static_cast<size_t>(GetDlgItemTextW(state.window,id,text.data(),static_cast<int>(text.size()))));
            return text;
        }

        // The selected rows' ids.
        std::vector<int> Selected(int id)
        {
            HWND list=GetDlgItem(state.window,id);
            const int count=static_cast<int>(SendMessageW(list,LB_GETSELCOUNT,0,0));
            std::vector<int> rows(static_cast<size_t>((std::max)(0,count))),ids;
            if(count>0)SendMessageW(list,LB_GETSELITEMS,count,reinterpret_cast<LPARAM>(rows.data()));
            for(const int row:rows)ids.push_back(static_cast<int>(SendMessageW(list,LB_GETITEMDATA,row,0)));
            return ids;
        }

        void AddAlarm()
        {
            const std::wstring typed=Typed(IdAlarmNew);
            if(typed.empty()||!state.planner)return;
            const std::int64_t now=Now();
            const PlanRequest r=ReadPlanRequest(typed,now,true);
            if(r.kind!=PlanRequest::Kind::Timer&&r.kind!=PlanRequest::Kind::Remind)
            {
                MessageBoxW(state.window,T(L"時刻か時間を読み取れませんでした。\n\n例：15時に会議　／　25分　／　毎日8時に薬　／　明日9時に電話",
                                           L"Could not read a time or a length of time.\n\nFor example: 3pm meeting / 25 minutes / every day at 8am medicine / tomorrow at 9 call"),
                            L"MikuMikuDesktopPerch",MB_ICONINFORMATION);
                SetFocus(GetDlgItem(state.window,IdAlarmNew));
                return;
            }
            Alarm a;
            a.due=r.due;
            a.timerSeconds=r.kind==PlanRequest::Kind::Timer?r.seconds:0;
            a.daily=r.daily;
            a.text=r.text;
            if(state.planner->AddAlarm(a))
            {
                SetDlgItemTextW(state.window,IdAlarmNew,L"");
                Save();
            }
            SetFocus(GetDlgItem(state.window,IdAlarmNew));
        }

        void AddTodo()
        {
            const std::wstring typed=Typed(IdTodoNew);
            if(state.planner&&state.planner->AddTodo(typed,TodayDate()))
            {
                SetDlgItemTextW(state.window,IdTodoNew,L"");
                Save();
                HWND list=GetDlgItem(state.window,IdTodoList);
                SendMessageW(list,LB_SETTOPINDEX,SendMessageW(list,LB_GETCOUNT,0,0)-1,0);
            }
            SetFocus(GetDlgItem(state.window,IdTodoNew));
        }

        void Close(){if(state.window)DestroyWindow(state.window);}

        LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            switch(message)
            {
            case WM_COMMAND:
                switch(LOWORD(wParam))
                {
                case IdAlarmAdd:AddAlarm();return 0;
                case IdTodoAdd:AddTodo();return 0;
                case IdAlarmDelete:
                    if(!state.planner)return 0;
                    for(const int id:Selected(IdAlarmList))state.planner->RemoveAlarm(id);
                    Save();
                    return 0;
                case IdTodoDone:
                    if(!state.planner)return 0;
                    for(const int id:Selected(IdTodoList))state.planner->RemoveTodo(id);
                    Save();
                    return 0;
                case IDOK:
                    // Enter in a box adds what is typed; anywhere else it closes.
                    if(GetFocus()==GetDlgItem(hwnd,IdAlarmNew)){AddAlarm();return 0;}
                    if(GetFocus()==GetDlgItem(hwnd,IdTodoNew)){AddTodo();return 0;}
                    Close();
                    return 0;
                case IDCANCEL:
                    Close();
                    return 0;
                }
                break;
            case WM_CLOSE:
                Close();
                return 0;
            case WM_DESTROY:
                // Closed, or gone with the character that owns it.
                state.window=nullptr;
                if(state.font){DeleteObject(state.font);state.font=nullptr;}
                return 0;
            }
            return DefWindowProcW(hwnd,message,wParam,lParam);
        }
    }

    void ShowPlannerWindow(HWND owner,Planner& planner)
    {
        state.planner=&planner;
        if(state.window)
        {
            FillLists();
            ShowWindow(state.window,SW_SHOWNORMAL);
            SetForegroundWindow(state.window);
            return;
        }
        static bool registered=false;
        const HINSTANCE instance=GetModuleHandleW(nullptr);
        if(!registered)
        {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc=Proc;
            wc.hInstance=instance;
            wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
            wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);
            wc.lpszClassName=ClassName;
            wc.hIcon=reinterpret_cast<HICON>(GetClassLongPtrW(owner,GCLP_HICON));
            wc.hIconSm=reinterpret_cast<HICON>(GetClassLongPtrW(owner,GCLP_HICONSM));
            if(!RegisterClassExW(&wc))return;
            registered=true;
        }
        state.dpi=static_cast<int>(GetDpiForSystem());
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0,state.dpi);
        state.font=CreateFontIndirectW(&metrics.lfMessageFont);

        // Client 480 x 560 logical pixels, centred on the screen the cursor is on.
        const DWORD style=WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
        const DWORD exStyle=WS_EX_CONTROLPARENT|WS_EX_DLGMODALFRAME;
        RECT frame{0,0,Px(480),Px(560)};
        AdjustWindowRectExForDpi(&frame,style,FALSE,exStyle,state.dpi);
        POINT cursor{};
        GetCursorPos(&cursor);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST),&monitor);
        const RECT& work=monitor.rcWork;
        const int w=frame.right-frame.left,h=frame.bottom-frame.top;
        // Owned by the mascot's window: kept above it (it is topmost) and gone with it.
        state.window=CreateWindowExW(exStyle,ClassName,T(L"予定とやること",L"Reminders and to-dos"),style,
                                     work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,
                                     owner,nullptr,instance,nullptr);
        if(!state.window)return;

        Add(L"STATIC",T(L"タイマーとリマインダー（時間になると、キャラクターが知らせます）",L"Timers and reminders (a character tells you when it is time)"),SS_LEFT,16,12,448,20);
        Add(L"LISTBOX",L"",LBS_EXTENDEDSEL|LBS_NOINTEGRALHEIGHT|LBS_NOTIFY|WS_VSCROLL|WS_BORDER|WS_TABSTOP,16,34,448,150,IdAlarmList);
        HWND alarmInput=Add(L"EDIT",L"",ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,16,194,330,24,IdAlarmNew);
        SendMessageW(alarmInput,EM_SETLIMITTEXT,Planner::MaxText*2,0);
        SendMessageW(alarmInput,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(T(L"例: 15時に会議 / 25分 / 毎日8時に薬",L"e.g. 3pm meeting / 25 min / every day at 8am medicine")));
        Add(L"BUTTON",T(L"追加",L"Add"),BS_PUSHBUTTON|WS_TABSTOP,354,192,110,28,IdAlarmAdd);
        Add(L"BUTTON",T(L"選んだものを削除",L"Delete selected"),BS_PUSHBUTTON|WS_TABSTOP,304,226,160,28,IdAlarmDelete);

        Add(L"STATIC",T(L"やること",L"To-dos"),SS_LEFT,16,272,448,20);
        Add(L"LISTBOX",L"",LBS_EXTENDEDSEL|LBS_NOINTEGRALHEIGHT|LBS_NOTIFY|WS_VSCROLL|WS_BORDER|WS_TABSTOP,16,294,448,150,IdTodoList);
        HWND todoInput=Add(L"EDIT",L"",ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,16,454,330,24,IdTodoNew);
        SendMessageW(todoInput,EM_SETLIMITTEXT,Planner::MaxText,0);
        SendMessageW(todoInput,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(T(L"例: 牛乳を買う",L"e.g. Buy milk")));
        Add(L"BUTTON",T(L"追加",L"Add"),BS_PUSHBUTTON|WS_TABSTOP,354,452,110,28,IdTodoAdd);
        Add(L"BUTTON",T(L"選んだものを済みにする",L"Mark selected done"),BS_PUSHBUTTON|WS_TABSTOP,284,486,180,28,IdTodoDone);

        Add(L"BUTTON",T(L"閉じる",L"Close"),BS_PUSHBUTTON|WS_TABSTOP,374,524,90,28,IDCANCEL);
        FillLists();
        ShowWindow(state.window,SW_SHOWNORMAL);
        SetForegroundWindow(state.window);
        SetFocus(alarmInput);
    }

    HWND PlannerWindowHandle(){return state.window;}

    void RefreshPlannerWindow(){FillLists();}
}
