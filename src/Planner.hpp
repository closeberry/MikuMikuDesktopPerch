#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch
{
    // Timers, reminders and the to-do list: the user's, shared by all characters. Kept in
    // %LOCALAPPDATA%\MikuMikuDesktopPerch\planner.txt (UTF-8), so they survive a restart; a
    // reminder that came due while the app was closed is told late.
    //
    // Times are seconds since 1970 (UTC, as std::time gives them).
    struct Alarm
    {
        int id=0;                // stable while the program runs (for the list window)
        std::int64_t due=0;
        std::wstring text;       // what to remind of (may be empty for a plain timer)
        int timerSeconds=0;      // a timer: how long it was set for (0: a reminder at a time)
        bool daily=false;        // comes again the next day at the same time
    };

    struct TodoItem
    {
        int id=0;
        std::wstring text;
        std::wstring added;      // YYYY-MM-DD
        std::wstring due;        // YYYY-MM-DD it is due by (empty: no deadline)
        std::wstring told;       // YYYY-MM-DD its deadline was last told (once a day)
    };

    class Planner
    {
    public:
        static constexpr size_t MaxText=120;
        static constexpr size_t MaxAlarms=100,MaxTodos=200;

        void Load();
        bool Save() const;

        const std::vector<Alarm>& Alarms() const {return alarms;}         // soonest first
        const std::vector<TodoItem>& Todos() const {return todos;}        // oldest first
        int AddAlarm(Alarm alarm);          // returns its id (0: too many)
        void RemoveAlarm(int id);
        int RemoveTimers();                 // takes off every timer; returns how many there were
        int AddTodo(const std::wstring& text,const std::wstring& date,const std::wstring& due={});   // 0: blank or too many
        void RemoveTodo(int id);
        void SetTodoTold(int id,const std::wstring& date);
        // The to-do item the words point at (its text in them, or they in its text), or 0.
        int FindTodo(const std::wstring& words) const;

        // The alarms due by now. Those that repeat move on to their next day; the rest are
        // taken off. Ones more than a day late are dropped without being told.
        std::vector<Alarm> TakeDue(std::int64_t now);

    private:
        void Sort();
        std::vector<Alarm> alarms;
        std::vector<TodoItem> todos;
        int nextId=1;
    };

    // What the user asked for, read from their words.
    struct PlanRequest
    {
        enum class Kind {None,Timer,Remind,TodoAdd,TodoDone,TodoList,AlarmList,CancelTimers,Today,FocusStart,FocusStop} kind=Kind::None;
        std::int64_t due=0;     // Timer, Remind
        int seconds=0;          // Timer: its length
        bool daily=false;       // Remind every day
        std::wstring text;      // what to remind of, the to-do item's words
        // TodoDone said in passing ("宿題終わった"): only a request when it names an item on
        // the list (otherwise it is just talk).
        bool sure=true;
        std::wstring deadline;  // TodoAdd: the day it is due by (YYYY-MM-DD), if said
    };

    // Reads a timer, a reminder or a to-do request out of what the user said, without the LLM
    // (Japanese and English, the common ways of putting them). loose: the words are only a
    // time and what to remind of (typed in the list window), no "tell me" needed.
    PlanRequest ReadPlanRequest(const std::wstring& said,std::int64_t now,bool loose=false);

    // A deadline in the words of a to-do item ("金曜までに", "10月30日まで", "by friday"): taken
    // out of text, with the day it means (YYYY-MM-DD). False when there is none.
    bool ReadDeadline(std::wstring& text,std::int64_t now,std::wstring& date);

    // The words might be such a request put some other way (worth asking the LLM about).
    bool MightBePlanRequest(const std::wstring& said);

    // What the LLM is asked to answer with, and its answer read back (Kind::None when it is
    // "none" or cannot be read).
    std::wstring PlanCommandPrompt(const std::wstring& said,std::int64_t now);
    PlanRequest ReadPlanCommand(const std::wstring& answer,std::int64_t now);

    // Words for times, in the language in use: "25分" / "25 minutes", "明日の9時" /
    // "tomorrow at 9:00 AM".
    std::wstring DurationText(int seconds);
    std::wstring TimeText(std::int64_t at,std::int64_t now,bool daily=false);
    std::int64_t Now();
    std::int64_t DayEnd(std::int64_t at);   // the next local midnight after at
    std::wstring DateOf(std::int64_t at);   // YYYY-MM-DD, local
    // A day (YYYY-MM-DD) in words: "今日", "明日", "10月30日（金）" / "today", "Fri, Oct 30".
    std::wstring DayText(const std::wstring& date,std::int64_t now);
}
