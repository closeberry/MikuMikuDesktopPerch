#pragma once

// Golf's progress, kept for the user (not per character): achievements, records, the looks they
// unlock and the one chosen of each kind, her rank, the stars from hole missions.

#include <array>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch::GolfProgress
{
    enum Achievement : int
    {
        HoleInOne,Albatross,Eagle,Birdie,ChipIn,Lightfall,SpinIn,MeteorEagle,LongDrive,LongPutt,JustStreak,
        UnderPar,BogeyFree,DryRound,FirstWin,RankFive,RankTop,WindWin,RainWin,DuskRound,HardPinBirdie,AllMissions,
        AchievementCount
    };
    struct AchievementInfo {const wchar_t* jp;const wchar_t* en;const wchar_t* howJp;const wchar_t* howEn;};
    const AchievementInfo& InfoOf(int achievement);

    // Looks: the trail of your shots, your landing circle's design, your ball, your meteor's fire.
    enum LookKind : int {Trail,Circle,BallLook,Flame,LookKindCount};
    struct Look {const wchar_t* jp;const wchar_t* en;int needs;};   // needs: the achievement that unlocks it (-1: from the start)
    const std::vector<Look>& LooksOf(int kind);

    struct Data
    {
        std::array<bool,AchievementCount> got{};
        std::array<int,9> bestHole{};          // fewest strokes on each hole (0: not played)
        std::array<int,3> bestRound{};         // best against par over 3, 6, 9 holes (stored +100; 0: none)
        int rank=5;                            // hers, 1-9
        int rounds=0,wins=0,stars=0;
        std::array<int,LookKindCount> chosen{};
        bool Unlocked(int kind,int look) const;
    };
    bool Load(const std::wstring& file,Data& data);
    bool Save(const std::wstring& file,const Data& data);
}
