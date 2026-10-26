#pragma once
#include <map>
#include <set>
#include <string>

namespace MikuMikuDesktopPerch
{
    // How close a character and the user have become (docs/Bond.md): points for using the
    // computer with her there, talking, clicks, finished focus rounds and to-dos, at most 30 a
    // day. The stage (how she talks) tops out after about a month; the points never stop.
    // Kept per model in characters\<name>.bond.dat, scrambled and signed so that it is not
    // edited by hand (reading the source still tells how).
    enum class BondAction {Present,Talk,Click,Focus,Todo,Dungeon};

    class Bond
    {
    public:
        static constexpr int Stages=5;
        static constexpr int StagePoints[Stages]={0,50,150,350,600};
        static constexpr int DailyCap=30;

        // Reads the model's file (or its backup); none or unreadable: a fresh start.
        // problem: what went wrong when a file was there but could not be read.
        void Load(const std::wstring& modelPath,std::wstring& problem);
        bool Save();
        // Nothing was read: a character met just now, or one from before closeness was counted.
        bool Fresh() const {return fresh;}
        // A character the user has had since the day since (YYYY-MM-DD) before closeness was
        // counted: she starts as close as about that many days would have made her (friends at
        // least), and what that would have brought up is taken as said. Updating must never
        // make her colder than she was.
        void CarryOver(const std::wstring& since);

        // Points for an action today (within its cap and the day's): whether any were added.
        // AddPresence counts the user's seconds at the computer with her there: once ten
        // minutes have been spent so, the day counts together (and Present's points come).
        // Returns whether the day was counted.
        bool Add(BondAction action);
        bool AddPresence(double seconds);

        long long Points() const {return points;}
        int Stage() const;
        int DaysTogether() const {return days;}
        const std::wstring& FirstDay() const {return first;}

        // Something to talk about (one a day): a key ("stage2", "days100", "year1", "points1000")
        // not yet told, or empty. Told marks it and the day.
        std::wstring DueCelebration() const;
        void Told(const std::wstring& key);

    private:
        void Roll();   // a new day: today's counts start again

        std::wstring modelPath;
        bool fresh=true;
        long long points=0;
        int days=0;
        std::wstring first,last;        // YYYY-MM-DD: the first day together, the last one counted
        std::wstring today;             // the day the counts below are for
        std::map<std::wstring,int> counts;   // per action, today's points
        std::set<std::wstring> told;
        std::wstring toldOn;            // the day something was last talked about
        double presentSeconds=0;        // today, not saved
    };
}
