#pragma once
#include <windows.h>
#include <atomic>
#include <string>
#include <thread>

namespace MikuMikuDesktopPerch
{
    // A release on GitHub (closeberry/MikuMikuDesktopPerch) newer than this exe.
    struct Release
    {
        std::wstring version;    // "1.2.0": the tag without its leading v
        std::wstring zipUrl;     // the *-win-x64.zip asset, on github.com
        std::wstring sha256;     // its digest as GitHub gives it (hex), empty when it gives none
    };

    // This exe's version from its version resource, like "1.1.1".
    std::wstring CurrentVersion();
    // Diagnostics (--pretend-version): take this exe for that version, to try an update.
    void PretendVersion(const std::wstring& version);
    // Whether version a comes after b (dotted numbers).
    bool IsNewer(const std::wstring& a,const std::wstring& b);

    // Updating from the latest GitHub release: Check asks GitHub (over HTTPS) for it; Download
    // fetches its zip, checks it against the SHA-256 GitHub gives, unpacks it (Windows' own
    // tar) into a folder under %TEMP% and checks the exe in it carries the release's version;
    // Install then puts the files in place of this exe's (the running exe is renamed out of
    // the way, which Windows allows) and the program restarts on the new one.
    class Updater
    {
    public:
        // Check is done: wParam is a heap-allocated Release* the receiver deletes (nullptr when
        // there is nothing newer or GitHub could not be asked).
        static constexpr UINT CheckedMessage=WM_APP+30;
        // Download is done: wParam is a heap-allocated std::wstring* error (empty: ready to
        // install) the receiver deletes.
        static constexpr UINT DownloadedMessage=WM_APP+31;

        explicit Updater(HWND notify):notify(notify){}
        ~Updater();
        Updater(const Updater&)=delete;
        Updater& operator=(const Updater&)=delete;

        void Check();
        void Download(const Release& release);
        bool Busy() const {return busy;}

        // After a Download without error: returns an explanation when the files could not be
        // put in place (the installed ones are then left as they were).
        static std::wstring Install();
        // At start-up: remove what the last update left behind (the old exe, the unpacked files).
        static void CleanUp();

    private:
        void Start(std::thread work);

        HWND notify;
        std::thread worker;
        std::atomic<bool> busy{false};
    };
}
