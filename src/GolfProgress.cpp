#include "GolfProgress.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace MikuMikuDesktopPerch::GolfProgress
{
    const AchievementInfo& InfoOf(int a)
    {
        static const AchievementInfo infos[AchievementCount]={
            {L"ホールインワン",L"Hole in one",L"ティーショットでカップに入れる",L"Hole your tee shot"},
            {L"アルバトロス",L"Albatross",L"パーより3打少なく上がる",L"Finish a hole three under par"},
            {L"イーグル",L"Eagle",L"パーより2打少なく上がる",L"Finish a hole two under par"},
            {L"バーディー",L"Birdie",L"パーより1打少なく上がる",L"Finish a hole one under par"},
            {L"チップイン",L"Chip-in",L"グリーンの外からパター以外でカップに入れる",L"Hole out from off the green, not with the putter"},
            {L"ライトフォール",L"Lightfall",L"空中でライトピラーに吸い込ませてカップに入れる",L"Hole out drawn down by the light pillar"},
            {L"スピンイン",L"Spin-in",L"バックスピンショットでカップに入れる",L"Hole out with a backspin shot"},
            {L"メテオイーグル",L"Meteor eagle",L"メテオショットを使ったホールでイーグル以上",L"Eagle or better on a hole you used a meteor shot"},
            {L"ロングドライブ",L"Long drive",L"ドライバーで280ヤード以上飛ばす",L"Drive 280 yards or more"},
            {L"ロングパット",L"Long putt",L"10ヤード以上のパットを決める",L"Sink a putt of 10 yards or more"},
            {L"ジャスト5連続",L"Five squares",L"1ラウンドで白いところに5回続けて当てる",L"Strike five square shots in a row in a round"},
            {L"アンダーパー",L"Under par",L"9ホールをパーより少ない打数で回る",L"Go round nine holes under par"},
            {L"ボギーなし",L"Bogey-free",L"ボギー以上なしで1ラウンドを終える",L"Finish a round with no hole over par"},
            {L"池もOBもなし",L"Clean round",L"9ホールを池にもOBにも入れずに回る",L"Go round nine holes without water or out of bounds"},
            {L"初勝利",L"First win",L"キャラクターに勝つ",L"Beat her"},
            {L"五段",L"Fifth rank",L"キャラクターの段位を5より上にする",L"Raise her rank past 5"},
            {L"九段に勝つ",L"Beat the ninth rank",L"段位9のキャラクターに勝つ",L"Beat her at rank 9"},
            {L"強風に勝つ",L"Wind tamer",L"強風の日に勝つ",L"Win on a windy day"},
            {L"雨に勝つ",L"Rain or shine",L"雨の日に勝つ",L"Win on a rainy day"},
            {L"夕暮れのラウンド",L"Dusk round",L"夕暮れのラウンドを回り終える",L"Finish a round at dusk"},
            {L"難ピンバーディー",L"Hard-pin birdie",L"難しいピンの日にバーディーを取る",L"Make a birdie on a hard-pin day"},
            {L"ミッション全制覇",L"All missions",L"6ホール以上のラウンドでミッションをすべて達成",L"Clear every mission in a round of six holes or more"}};
        return infos[std::clamp(a,0,AchievementCount-1)];
    }

    const std::vector<Look>& LooksOf(int kind)
    {
        static const std::vector<Look> looks[LookKindCount]={
            {{L"青",L"Blue",-1},{L"金",L"Gold",Birdie},{L"エメラルド",L"Emerald",ChipIn},{L"すみれ",L"Violet",SpinIn},{L"虹",L"Rainbow",HoleInOne}},
            {{L"六芒星",L"Hexagram",-1},{L"五芒星",L"Pentagram",Lightfall},{L"花紋",L"Flower",Eagle},{L"歯車",L"Cogwheel",JustStreak}},
            {{L"白",L"White",-1},{L"金",L"Gold",FirstWin},{L"桜",L"Cherry",BogeyFree},{L"空色",L"Sky",DryRound},{L"黒曜",L"Obsidian",RankTop}},
            {{L"橙",L"Orange",-1},{L"蒼炎",L"Blue fire",MeteorEagle},{L"紫炎",L"Violet fire",LongDrive},{L"翠炎",L"Green fire",AllMissions}}};
        return looks[std::clamp(kind,0,LookKindCount-1)];
    }

    bool Data::Unlocked(int kind,int look) const
    {
        const auto& list=LooksOf(kind);
        if(look<0||look>=static_cast<int>(list.size()))return false;
        const int needs=list[static_cast<size_t>(look)].needs;
        return needs<0||got[static_cast<size_t>(needs)];
    }

    bool Load(const std::wstring& file,Data& data)
    {
        data=Data{};
        if(file.empty())return false;
        std::ifstream in{std::filesystem::path(file)};
        if(!in)return false;
        std::string line;
        while(std::getline(in,line))
        {
            std::istringstream words(line);
            std::string key;
            words>>key;
            if(key=="got"){int a;while(words>>a)if(a>=0&&a<AchievementCount)data.got[static_cast<size_t>(a)]=true;}
            else if(key=="hole"){for(int& n:data.bestHole)words>>n;}
            else if(key=="round"){for(int& n:data.bestRound)words>>n;}
            else if(key=="rank"){words>>data.rank;data.rank=std::clamp(data.rank,1,9);}
            else if(key=="played"){words>>data.rounds>>data.wins>>data.stars;}
            else if(key=="looks"){for(int& n:data.chosen)words>>n;}
        }
        for(int k=0;k<LookKindCount;++k)
            if(!data.Unlocked(k,data.chosen[static_cast<size_t>(k)]))data.chosen[static_cast<size_t>(k)]=0;
        return true;
    }

    bool Save(const std::wstring& file,const Data& data)
    {
        if(file.empty())return false;
        std::error_code e;
        std::filesystem::create_directories(std::filesystem::path(file).parent_path(),e);
        std::ofstream out{std::filesystem::path(file),std::ios::trunc};
        if(!out)return false;
        out<<"got";
        for(int a=0;a<AchievementCount;++a)if(data.got[static_cast<size_t>(a)])out<<' '<<a;
        out<<"\nhole";
        for(int n:data.bestHole)out<<' '<<n;
        out<<"\nround";
        for(int n:data.bestRound)out<<' '<<n;
        out<<"\nrank "<<data.rank<<"\nplayed "<<data.rounds<<' '<<data.wins<<' '<<data.stars<<"\nlooks";
        for(int n:data.chosen)out<<' '<<n;
        out<<'\n';
        return static_cast<bool>(out);
    }
}
