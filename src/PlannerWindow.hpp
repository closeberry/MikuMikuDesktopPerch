#pragma once
#include "Planner.hpp"
#include <windows.h>
#include <functional>

namespace MikuMikuDesktopPerch
{
    // The timers, reminders and to-dos in two lists, with adding (typed as it would be said:
    // "15時に会議", "25分", "毎日8時に薬") and deleting. Changes are saved at once. Not modal;
    // opening it again brings it forward. todosDone: told how many to-dos were marked done.
    void ShowPlannerWindow(HWND owner,Planner& planner,std::function<void(int)> todosDone={});

    // The window while open (nullptr when closed): the message loop passes it to
    // IsDialogMessage for Tab, Enter and Esc.
    HWND PlannerWindowHandle();

    // The planner changed elsewhere (a request in a talk, an alarm told): the lists, if open,
    // show it.
    void RefreshPlannerWindow();
}
