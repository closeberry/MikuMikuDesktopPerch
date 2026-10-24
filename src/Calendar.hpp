#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace MikuMikuDesktopPerch
{
    // A Google Calendar read through its secret address in iCal format (Settings → the
    // calendar → "Secret address in iCal format"). Read only: nothing is ever written to the
    // calendar. The address works like a password, so it is kept encrypted for this Windows
    // user (DPAPI), only ever sent to calendar.google.com over HTTPS, and never written to logs.
    struct CalendarEvent
    {
        std::wstring title;
        std::int64_t start=0,end=0;   // seconds since 1970; an all-day event starts at local midnight
        bool allDay=false;
        std::wstring key;             // the event and its start: told about once
    };

    // Whether text is such an address (https://calendar.google.com/calendar/ical/.../*.ics).
    // Returns an explanation when it is not (empty when fine).
    std::wstring CheckCalendarAddress(const std::wstring& address);

    // The events of an iCal file between from and to, soonest first, repeating ones (RRULE:
    // daily, weekly, monthly, yearly) expanded, moved and cancelled ones taken into account.
    // Times with a time zone are taken as this computer's local time.
    std::vector<CalendarEvent> ReadCalendar(const std::string& ics,std::int64_t from,std::int64_t to);

    // Text encrypted for this Windows user ("dpapi:" + base64), and back (empty when it cannot
    // be read, as on another computer).
    std::wstring ProtectText(const std::wstring& text);
    std::wstring UnprotectText(const std::wstring& stored);

    // Fetching in the background.
    struct CalendarResult
    {
        std::vector<CalendarEvent> events;
        std::wstring error;
        bool report=false;   // the user just set the address: say how it went
    };

    class CalendarFeed
    {
    public:
        // Done: wParam is a heap-allocated CalendarResult* the receiver deletes.
        static constexpr UINT FetchedMessage=WM_APP+40;

        explicit CalendarFeed(HWND notify):notify(notify){}
        ~CalendarFeed();
        CalendarFeed(const CalendarFeed&)=delete;
        CalendarFeed& operator=(const CalendarFeed&)=delete;

        // Today's and the next two days' events.
        void Fetch(const std::wstring& address,bool report);
        bool Busy() const {return busy;}

    private:
        HWND notify;
        std::thread worker;
        std::atomic<bool> busy{false};
    };
}
