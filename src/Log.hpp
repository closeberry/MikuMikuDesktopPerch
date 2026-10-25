#pragma once

namespace MikuMikuDesktopPerch
{
    // A log for finding out what went wrong on someone else's PC: %LOCALAPPDATA%\MikuMikuDesktopPerch\
    // log.txt (UTF-8). Each start moves the last run's log to log.old.txt, so there are never more
    // than two files, and a file stops growing at LogLimitBytes.
    constexpr long LogLimitBytes=512*1024;

    // Starts this run's log. Until it is called (and in diagnostic runs, which leave it
    // uncalled), Log writes nothing.
    void OpenLog();

    // One line, printf-style, after the time of day.
    void Log(const wchar_t* format,...);
}
