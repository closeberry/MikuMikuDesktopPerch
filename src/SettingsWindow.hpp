#pragma once
#include "Memory.hpp"
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
        CharacterProfile character;   // of the current model
        // The current model's memory: the memory window edits it (and saves it) directly,
        // since notes keep arriving from conversations while the settings are open.
        CharacterMemory* memory=nullptr;
    };

    // Open the settings window (or bring it forward if open) over owner, filled from current.
    // OK calls onApply with the edited values and closes it; Cancel just closes it.
    // Moving the sun's sliders calls onLight at once (to see it on the model); Cancel calls it
    // again with the old position.
    void ShowSettingsWindow(HWND owner,const SettingsChoice& current,std::function<void(const SettingsChoice&)> onApply,
                            std::function<void(int azimuth,int elevation)> onLight);

    // The open settings window, or its persona or memory dialog while one is open (nullptr
    // when closed): the message loop passes it to IsDialogMessage for Tab, Enter and Esc.
    HWND SettingsWindowHandle();

    // Notes were added to the memory: the memory window, if open, shows them.
    void RefreshMemoryWindow();
}
