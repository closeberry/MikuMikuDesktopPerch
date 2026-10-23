#pragma once
#include "Settings.hpp"
#include <windows.h>
#include <functional>

namespace MikuMikuDesktopPerch
{
    // What the settings window edits: the settings file's values, plus starting with Windows
    // (kept in the registry, not in the file).
    struct SettingsChoice
    {
        Settings settings;
        bool startWithWindows=false;
    };

    // Open the settings window (or bring it forward if open) over owner, filled from current.
    // OK calls onApply with the edited values and closes it; Cancel just closes it.
    void ShowSettingsWindow(HWND owner,const SettingsChoice& current,std::function<void(const SettingsChoice&)> onApply);

    // The open settings window (nullptr when closed): the message loop passes it to
    // IsDialogMessage for Tab, Enter and Esc.
    HWND SettingsWindowHandle();
}
