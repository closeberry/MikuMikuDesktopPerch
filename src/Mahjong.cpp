#include "Mahjong.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <sstream>

namespace MikuMikuDesktopPerch::Mahjong
{
    namespace
    {
        // A set of three (or a kan) in a reading of the hand. Run: tile is its lowest.
        // Concealed: counted as concealed for fu and the concealed triplets (a triplet finished by
        // ron is not, though the hand stays closed). Called: an open meld.
        struct Group {bool run=false,kan=false;int tile=0;bool concealed=true,called=false;};
        enum class Wait {Ryanmen,Kanchan,Penchan,Tanki,Shanpon,Other};

        // One way to read a winning hand.
        struct Reading
        {
            std::vector<Group> groups;
            int pair=-1;
            Wait wait=Wait::Other;
            bool sevenPairs=false,orphans=false;
        };

        bool Orphan(int t){return !Simple(t);}
        int DoraOf(int indicator)
        {
            if(indicator<East)return indicator/9*9+(indicator%9+1)%9;
            if(indicator<White)return East+(indicator-East+1)%4;
            return White+(indicator-White+1)%3;
        }
        int Total(const Counts& c){int n=0;for(int v:c)n+=v;return n;}

        // Every way to split the tiles into sets of three (from tile `from` on).
        void Split(Counts& c,int from,std::vector<Group>& sets,std::vector<std::vector<Group>>& out)
        {
            int t=from;
            while(t<Kinds&&c[t]==0)++t;
            if(t==Kinds){out.push_back(sets);return;}
            if(c[t]>=3)
            {
                c[t]-=3;sets.push_back({false,false,t});
                Split(c,t,sets,out);
                sets.pop_back();c[t]+=3;
            }
            if(!Honor(t)&&t%9<=6&&c[t+1]>0&&c[t+2]>0)
            {
                --c[t];--c[t+1];--c[t+2];sets.push_back({true,false,t});
                Split(c,t,sets,out);
                sets.pop_back();++c[t];++c[t+1];++c[t+2];
            }
        }

        // The splits into a pair and sets of three: (pair, sets).
        std::vector<std::pair<int,std::vector<Group>>> FourSetsAndPair(const Counts& concealed)
        {
            std::vector<std::pair<int,std::vector<Group>>> found;
            if(Total(concealed)%3!=2)return found;
            Counts c=concealed;
            for(int p=0;p<Kinds;++p)
            {
                if(c[p]<2)continue;
                c[p]-=2;
                std::vector<Group> sets;
                std::vector<std::vector<Group>> splits;
                Split(c,0,sets,splits);
                for(auto& s:splits)found.push_back({p,std::move(s)});
                c[p]+=2;
            }
            return found;
        }

        bool SevenPairs(const Counts& c)
        {
            int pairs=0;
            for(int v:c){if(v==2)++pairs;else if(v!=0)return false;}
            return pairs==7;
        }
        bool ThirteenOrphans(const Counts& c)
        {
            if(Total(c)!=14)return false;
            for(int t=0;t<Kinds;++t)if(Orphan(t)!=(c[t]>0))return false;
            return true;
        }

        struct Tally
        {
            std::vector<YakuHan> yaku;
            int han=0;
            bool yakuman=false;
            void Add(const wchar_t* ja,const wchar_t* en,int h){yaku.push_back({ja,en,h});han+=h;}
            void Yakuman(const wchar_t* ja,const wchar_t* en)
            {
                if(!yakuman){yaku.clear();han=0;yakuman=true;}
                Add(ja,en,13);
            }
        };

        Score Count(const Reading& r,const Counts& all,bool closed,int win,const Situation& s)
        {
            const bool dealer=s.seatWind==East;
            Tally y;
            auto valued=[&](int t){return t>=White||t==s.seatWind||t==s.roundWind;};

            // Over every tile of the hand (melds included).
            bool allSimple=true,allOrphan=true,allHonor=true,allTerminal=true,allGreen=true;
            bool honors=false;
            int suits=0;
            for(int t=0;t<Kinds;++t)
            {
                if(all[t]==0)continue;
                allSimple&=Simple(t);allOrphan&=Orphan(t);allHonor&=Honor(t);allTerminal&=Terminal(t);
                allGreen&=t==19||t==20||t==21||t==23||t==25||t==Green;
                if(Honor(t))honors=true;else suits|=1<<Suit(t);
            }
            const bool oneSuit=suits==1||suits==2||suits==4;

            // The sets, counted.
            int runs=0,triplets=0,concealedTriplets=0,kans=0,dragonTriplets=0,windTriplets=0;
            for(const Group& g:r.groups)
            {
                if(g.run){++runs;continue;}
                ++triplets;
                if(g.concealed)++concealedTriplets;
                if(g.kan)++kans;
                if(g.tile>=White)++dragonTriplets;
                else if(g.tile>=East)++windTriplets;
            }
            const bool standard=!r.sevenPairs&&!r.orphans;
            const bool pinfu=standard&&closed&&runs==4&&!valued(r.pair)&&r.wait==Wait::Ryanmen;

            // Yakuman.
            if(s.firstDraw&&s.tsumo&&closed)
            {
                if(dealer)y.Yakuman(L"天和",L"Blessing of Heaven");
                else y.Yakuman(L"地和",L"Blessing of Earth");
            }
            if(r.orphans)y.Yakuman(L"国士無双",L"Thirteen Orphans");
            if(standard&&concealedTriplets==4)y.Yakuman(L"四暗刻",L"Four Concealed Triplets");
            if(dragonTriplets==3)y.Yakuman(L"大三元",L"Big Three Dragons");
            if(windTriplets==4)y.Yakuman(L"大四喜",L"Big Four Winds");
            else if(windTriplets==3&&r.pair>=East&&r.pair<White)y.Yakuman(L"小四喜",L"Little Four Winds");
            if(allHonor)y.Yakuman(L"字一色",L"All Honors");
            if(allGreen)y.Yakuman(L"緑一色",L"All Green");
            if(allTerminal)y.Yakuman(L"清老頭",L"All Terminals");
            if(kans==4)y.Yakuman(L"四槓子",L"Four Kans");
            if(standard&&closed&&kans==0&&!honors&&oneSuit)
            {
                const int base=Suit(win)*9;
                bool nine=all[base]>=3&&all[base+8]>=3;
                for(int i=1;i<8;++i)nine&=all[base+i]>=1;
                if(nine)y.Yakuman(L"九蓮宝燈",L"Nine Gates");
            }

            if(!y.yakuman)
            {
                // By the moment.
                if(s.doubleRiichi&&closed)y.Add(L"ダブル立直",L"Double riichi",2);
                else if(s.riichi&&closed)y.Add(L"立直",L"Riichi",1);
                if(s.ippatsu&&(s.riichi||s.doubleRiichi)&&closed)y.Add(L"一発",L"Ippatsu",1);
                if(s.tsumo&&closed)y.Add(L"門前清自摸和",L"Fully concealed hand",1);
                if(s.lastTile)
                {
                    if(s.tsumo)y.Add(L"海底摸月",L"Last tile draw",1);
                    else y.Add(L"河底撈魚",L"Last tile discard",1);
                }
                if(s.rinshan&&s.tsumo)y.Add(L"嶺上開花",L"After a kan",1);
                if(s.chankan&&!s.tsumo)y.Add(L"槍槓",L"Robbing a kan",1);

                // By the shape.
                if(pinfu)y.Add(L"平和",L"Pinfu",1);
                if(allSimple)y.Add(L"断幺九",L"All simples",1);
                for(const Group& g:r.groups)
                {
                    if(g.run)continue;
                    if(g.tile==White)y.Add(L"役牌 白",L"White dragon",1);
                    if(g.tile==Green)y.Add(L"役牌 發",L"Green dragon",1);
                    if(g.tile==Red)y.Add(L"役牌 中",L"Red dragon",1);
                    if(g.tile==s.seatWind)y.Add(L"自風牌",L"Seat wind",1);
                    if(g.tile==s.roundWind)y.Add(L"場風牌",L"Round wind",1);
                }
                if(r.sevenPairs)y.Add(L"七対子",L"Seven pairs",2);
                if(standard)
                {
                    if(closed)
                    {
                        std::vector<int> starts;
                        for(const Group& g:r.groups)if(g.run)starts.push_back(g.tile);
                        std::sort(starts.begin(),starts.end());
                        int twins=0;
                        for(size_t i=0;i+1<starts.size();)
                        {
                            if(starts[i]==starts[i+1]){++twins;i+=2;}
                            else ++i;
                        }
                        if(twins==2)y.Add(L"二盃口",L"Twice pure double sequence",3);
                        else if(twins==1)y.Add(L"一盃口",L"Pure double sequence",1);
                    }
                    auto hasRun=[&](int t){return std::any_of(r.groups.begin(),r.groups.end(),[&](const Group& g){return g.run&&g.tile==t;});};
                    auto hasTriplet=[&](int t){return std::any_of(r.groups.begin(),r.groups.end(),[&](const Group& g){return !g.run&&g.tile==t;});};
                    bool sameRuns=false,sameTriplets=false,straight=false;
                    for(int n=0;n<9;++n)
                    {
                        if(n<7&&hasRun(n)&&hasRun(n+9)&&hasRun(n+18))sameRuns=true;
                        if(hasTriplet(n)&&hasTriplet(n+9)&&hasTriplet(n+18))sameTriplets=true;
                    }
                    for(int suit=0;suit<3;++suit)
                        if(hasRun(suit*9)&&hasRun(suit*9+3)&&hasRun(suit*9+6))straight=true;
                    if(sameRuns)y.Add(L"三色同順",L"Mixed triple sequence",closed?2:1);
                    if(straight)y.Add(L"一気通貫",L"Pure straight",closed?2:1);
                    if(runs==0)y.Add(L"対々和",L"All triplets",2);
                    if(concealedTriplets==3)y.Add(L"三暗刻",L"Three concealed triplets",2);
                    if(sameTriplets)y.Add(L"三色同刻",L"Triple triplets",2);
                    if(kans==3)y.Add(L"三槓子",L"Three kans",2);
                    if(dragonTriplets==2&&r.pair>=White)y.Add(L"小三元",L"Little three dragons",2);
                    // Every set and the pair hold a terminal or an honor, with a run among them
                    // (without one, it is all terminals and honors).
                    bool outside=runs>0&&Orphan(r.pair);
                    for(const Group& g:r.groups)
                        outside&=g.run?(g.tile%9==0||g.tile%9==6):Orphan(g.tile);
                    if(outside)
                    {
                        if(honors)y.Add(L"混全帯幺九",L"Half outside hand",closed?2:1);
                        else y.Add(L"純全帯幺九",L"Fully outside hand",closed?3:2);
                    }
                }
                if(allOrphan)y.Add(L"混老頭",L"All terminals and honors",2);
                if(oneSuit&&honors)y.Add(L"混一色",L"Half flush",closed?3:2);
                if(oneSuit&&!honors)y.Add(L"清一色",L"Full flush",closed?6:5);
            }

            Score score;
            if(y.yaku.empty())return score;
            score.win=true;
            score.yakuman=y.yakuman;

            // Fu.
            int fu=20;
            if(r.sevenPairs)fu=25;
            else if(!(pinfu&&s.tsumo))
            {
                if(closed&&!s.tsumo)fu+=10;
                if(s.tsumo)fu+=2;
                for(const Group& g:r.groups)
                {
                    if(g.run)continue;
                    int f=g.kan?8:2;
                    if(Orphan(g.tile))f*=2;
                    if(g.concealed)f*=2;
                    fu+=f;
                }
                if(r.wait==Wait::Kanchan||r.wait==Wait::Penchan||r.wait==Wait::Tanki)fu+=2;
                if(r.pair>=White)fu+=2;
                if(r.pair==s.seatWind)fu+=2;
                if(r.pair==s.roundWind)fu+=2;
                if(fu==20)fu=30;   // an open hand worth nothing more
                fu=(fu+9)/10*10;
            }
            score.fu=fu;

            int han=y.han;
            if(!y.yakuman)
            {
                int dora=0,ura=0;
                for(int d:s.dora)dora+=all[DoraOf(d)];
                if(s.riichi||s.doubleRiichi)for(int d:s.ura)ura+=all[DoraOf(d)];
                if(dora)y.Add(L"ドラ",L"Dora",dora);
                if(ura)y.Add(L"裏ドラ",L"Ura dora",ura);
                if(s.red)y.Add(L"赤ドラ",L"Red five",s.red);
                han=y.han;
            }
            score.yaku=y.yaku;
            score.han=y.yakuman?13:han;   // yakuman together still count as one

            if(y.yakuman||han>=13)
            {
                score.basic=8000;
                score.limitJa=y.yakuman?L"役満":L"数え役満";score.limitEn=y.yakuman?L"Yakuman":L"Counted yakuman";
            }
            else if(han>=11){score.basic=6000;score.limitJa=L"三倍満";score.limitEn=L"Sanbaiman";}
            else if(han>=8){score.basic=4000;score.limitJa=L"倍満";score.limitEn=L"Baiman";}
            else if(han>=6){score.basic=3000;score.limitJa=L"跳満";score.limitEn=L"Haneman";}
            else
            {
                score.basic=std::min(fu<<(han+2),2000);
                if(score.basic==2000){score.limitJa=L"満貫";score.limitEn=L"Mangan";}
            }
            score.points=Payment(score.basic,dealer,s.tsumo,s.honba);
            return score;
        }

        bool Better(const Score& a,const Score& b)
        {
            if(a.win!=b.win)return a.win;
            if(a.points!=b.points)return a.points>b.points;
            if(a.han!=b.han)return a.han>b.han;
            return a.fu>b.fu;
        }
    }

    std::vector<int> ParseTiles(const std::wstring& text,int* red)
    {
        std::vector<int> tiles,digits;
        int reds=0;
        for(wchar_t c:text)
        {
            if(c>=L'0'&&c<=L'9'){digits.push_back(c-L'0');continue;}
            const int suit=c==L'm'?0:c==L'p'?1:c==L's'?2:c==L'z'?3:-1;
            if(suit<0||digits.empty())return {};
            for(int d:digits)
            {
                if(suit==3)
                {
                    if(d<1||d>7)return {};
                    tiles.push_back(East+d-1);
                }
                else
                {
                    if(d==0){++reds;d=5;}
                    tiles.push_back(suit*9+d-1);
                }
            }
            digits.clear();
        }
        if(!digits.empty())return {};
        if(red)*red=reds;
        return tiles;
    }

    std::wstring TileText(int tile)
    {
        if(tile<0||tile>=Kinds)return L"?";
        return std::to_wstring(tile<East?tile%9+1:tile-East+1)+(L"mpsz"[Suit(tile)]);
    }

    std::wstring TilesText(const Counts& counts)
    {
        std::wstring text;
        for(int suit=0;suit<4;++suit)
        {
            std::wstring digits;
            const int first=suit*9,last=suit==3?Kinds:first+9;
            for(int t=first;t<last;++t)
                for(int i=0;i<counts[t];++i)digits+=static_cast<wchar_t>(L'1'+t-first);
            if(!digits.empty())text+=digits+L"mpsz"[suit];
        }
        return text;
    }

    bool WinningShape(const Counts& concealed)
    {
        if(Total(concealed)%3!=2)return false;
        return !FourSetsAndPair(concealed).empty()||SevenPairs(concealed)||ThirteenOrphans(concealed);
    }

    std::vector<int> Waits(const Counts& concealed)
    {
        std::vector<int> waits;
        if(Total(concealed)%3!=1)return waits;
        Counts c=concealed;
        for(int t=0;t<Kinds;++t)
        {
            if(c[t]>=4)continue;
            ++c[t];
            if(WinningShape(c))waits.push_back(t);
            --c[t];
        }
        return waits;
    }

    std::vector<int> TenpaiDiscards(const Counts& concealed)
    {
        std::vector<int> discards;
        if(Total(concealed)%3!=2)return discards;
        Counts c=concealed;
        for(int t=0;t<Kinds;++t)
        {
            if(c[t]==0)continue;
            --c[t];
            if(!Waits(c).empty())discards.push_back(t);
            ++c[t];
        }
        return discards;
    }

    int Payment(int basic,bool dealer,bool tsumo,int honba)
    {
        auto up=[](int points){return (points+99)/100*100;};
        int paid=0;
        if(!tsumo)paid=up(basic*(dealer?6:4));
        else if(dealer)paid=3*up(basic*2);
        else paid=up(basic*2)+2*up(basic);
        return paid+300*honba;
    }

    Score Evaluate(const Counts& concealed,const std::vector<Meld>& melds,int win,const Situation& s)
    {
        Score best;
        if(win<0||win>=Kinds||concealed[win]==0||!WinningShape(concealed))return best;

        Counts all=concealed;
        bool closed=true;
        std::vector<Group> called;
        for(const Meld& m:melds)
        {
            if(m.kind==MeldKind::Chi){for(int i=0;i<3;++i)++all[m.tile+i];}
            else all[m.tile]+=m.kind==MeldKind::Pon?3:4;
            if(m.kind!=MeldKind::ClosedKan)closed=false;
            called.push_back({m.kind==MeldKind::Chi,m.kind==MeldKind::Kan||m.kind==MeldKind::ClosedKan,m.tile,m.kind==MeldKind::ClosedKan,m.kind!=MeldKind::ClosedKan});
        }
        auto consider=[&](const Reading& r)
        {
            const Score score=Count(r,all,closed,win,s);
            if(Better(score,best))best=score;
        };

        // Four sets and a pair: each split, with the winning tile in each place it can be.
        for(const auto& [pair,sets]:FourSetsAndPair(concealed))
        {
            if(pair==win)
            {
                Reading r{sets,pair,Wait::Tanki};
                r.groups.insert(r.groups.end(),called.begin(),called.end());
                consider(r);
            }
            for(size_t i=0;i<sets.size();++i)
            {
                const Group& g=sets[i];
                Reading r{sets,pair};
                if(g.run)
                {
                    if(win<g.tile||win>g.tile+2)continue;
                    const int place=win-g.tile;
                    r.wait=place==1?Wait::Kanchan:(place==0&&g.tile%9==6)||(place==2&&g.tile%9==0)?Wait::Penchan:Wait::Ryanmen;
                }
                else
                {
                    if(g.tile!=win)continue;
                    r.wait=Wait::Shanpon;
                    if(!s.tsumo)r.groups[i].concealed=false;   // finished by another's tile
                }
                r.groups.insert(r.groups.end(),called.begin(),called.end());
                consider(r);
            }
        }
        if(melds.empty()&&SevenPairs(concealed))
        {
            Reading r;
            r.sevenPairs=true;r.wait=Wait::Tanki;
            consider(r);
        }
        if(melds.empty()&&ThirteenOrphans(concealed))
        {
            Reading r;
            r.orphans=true;
            consider(r);
        }
        return best;
    }

    Unseen Unseen::All()
    {
        Unseen u;
        u.tiles.fill(4);
        u.red={1,1,1};
        return u;
    }

    int Unseen::Size() const {return Total(tiles);}

    void Unseen::Take(int tile,bool redFive)
    {
        if(tile<0||tile>=Kinds||tiles[tile]==0)return;
        if(tile<East&&tile%9==4)
        {
            int& r=red[Suit(tile)];
            // A plain five can be taken only while one is left; the last five is the red one.
            if(r>0&&(redFive||tiles[tile]==1))--r;
        }
        --tiles[tile];
    }

    int Unseen::Draw(std::mt19937& rng,bool* redFive)
    {
        const int size=Size();
        if(size==0)return -1;
        int n=std::uniform_int_distribution<int>(0,size-1)(rng),t=0;
        while(n>=tiles[t])n-=tiles[t++];
        // Of the kind's tiles, the red five is the first ones counted.
        const bool r=t<East&&t%9==4&&n<red[Suit(t)];
        Take(t,r);
        if(redFive)*redFive=r;
        return t;
    }

    namespace
    {
        // Sets, partial sets and a pair over the tiles from i on: the least shanten found.
        void Search(Counts& c,int i,int sets,int partial,int pair,int& best)
        {
            while(i<Kinds&&c[i]==0)++i;
            if(i==Kinds)
            {
                best=std::min(best,8-2*sets-std::min(partial,4-sets)-pair);
                return;
            }
            if(8-2*sets-std::min(partial+(Kinds-i),4-sets)-1>=best)return;   // cannot do better from here
            if(c[i]>=3){c[i]-=3;Search(c,i,sets+1,partial,pair,best);c[i]+=3;}
            const bool number=!Honor(i);
            if(number&&i%9<=6&&c[i+1]&&c[i+2]){--c[i];--c[i+1];--c[i+2];Search(c,i,sets+1,partial,pair,best);++c[i];++c[i+1];++c[i+2];}
            if(c[i]>=2)
            {
                c[i]-=2;
                if(!pair)Search(c,i,sets,partial,1,best);
                Search(c,i,sets,partial+1,pair,best);
                c[i]+=2;
            }
            if(number&&i%9<=7&&c[i+1]){--c[i];--c[i+1];Search(c,i,sets,partial+1,pair,best);++c[i];++c[i+1];}
            if(number&&i%9<=6&&c[i+2]){--c[i];--c[i+2];Search(c,i,sets,partial+1,pair,best);++c[i];++c[i+2];}
            --c[i];
            Search(c,i,sets,partial,pair,best);   // left on its own
            ++c[i];
        }
    }

    int Shanten(const Counts& concealed,int melds)
    {
        Counts c=concealed;
        int best=8;
        Search(c,0,melds,0,0,best);
        if(melds==0)
        {
            int pairs=0,kinds=0,orphans=0;
            bool orphanPair=false;
            for(int t=0;t<Kinds;++t)
            {
                if(c[t]>=2)++pairs;
                if(c[t]>0)++kinds;
                if(!Simple(t)&&c[t]>0){++orphans;if(c[t]>=2)orphanPair=true;}
            }
            best=std::min(best,6-pairs+std::max(0,7-kinds));
            best=std::min(best,13-orphans-(orphanPair?1:0));
        }
        return best;
    }

    std::wstring CheckHands(const std::wstring& cases)
    {
        std::wstring report;
        int checked=0,failed=0;
        auto counts=[](const std::vector<int>& tiles){Counts c{};for(int t:tiles)++c[t];return c;};
        auto list=[](const std::vector<int>& tiles){std::wstring text;for(int t:tiles)text+=TileText(t);return text.empty()?std::wstring(L"none"):text;};
        auto wind=[](const std::wstring& w){return w==L"S"?South:w==L"W"?West:w==L"N"?North:East;};
        std::wstringstream lines(cases);
        for(std::wstring line;std::getline(lines,line);)
        {
            if(!line.empty()&&line.back()==L'\r')line.pop_back();
            if(line.empty()||line[0]==L'#')continue;
            const size_t arrow=line.find(L"=>");
            if(arrow==std::wstring::npos){report+=L"?? "+line+L"\n";++failed;continue;}
            std::wstringstream given(line.substr(0,arrow)),wanted(line.substr(arrow+2));
            std::vector<std::wstring> words,expected;
            for(std::wstring w;given>>w;)words.push_back(w);
            for(std::wstring w;wanted>>w;)expected.push_back(w);
            if(words.empty())continue;
            ++checked;
            std::wstring got;
            bool ok=true;
            if(words[0]==L"shanten")
            {
                const std::vector<int> tiles=ParseTiles(words.size()>1?words[1]:L"");
                const int melds=words.size()>2?_wtoi(words[2].c_str()):0;
                got=std::to_wstring(Shanten(counts(tiles),melds));
                ok=!expected.empty()&&got==expected[0];
            }
            else if(words[0]==L"waits"||words[0]==L"discards")
            {
                const std::vector<int> tiles=ParseTiles(words.size()>1?words[1]:L"");
                got=list(words[0]==L"waits"?Waits(counts(tiles)):TenpaiDiscards(counts(tiles)));
                ok=!expected.empty()&&got==(expected[0]==L"none"?expected[0]:list(ParseTiles(expected[0])));
            }
            else
            {
                int red=0;
                const std::vector<int> tiles=ParseTiles(words[0],&red);
                Situation s;
                s.red=red;
                std::vector<Meld> melds;
                int win=tiles.empty()?-1:tiles.back();
                for(size_t i=1;i<words.size();++i)
                {
                    const std::wstring& w=words[i];
                    const size_t eq=w.find(L'=');
                    const std::wstring key=w.substr(0,eq),value=eq==std::wstring::npos?L"":w.substr(eq+1);
                    int valueRed=0;
                    const std::vector<int> valueTiles=ParseTiles(value,&valueRed);
                    if(key==L"win"&&!valueTiles.empty())win=valueTiles[0];
                    else if(key==L"tsumo")s.tsumo=true;
                    else if(key==L"riichi")s.riichi=true;
                    else if(key==L"wriichi")s.doubleRiichi=true;
                    else if(key==L"ippatsu")s.ippatsu=true;
                    else if(key==L"last")s.lastTile=true;
                    else if(key==L"rinshan")s.rinshan=true;
                    else if(key==L"chankan")s.chankan=true;
                    else if(key==L"first")s.firstDraw=true;
                    else if(key==L"seat")s.seatWind=wind(value);
                    else if(key==L"round")s.roundWind=wind(value);
                    else if(key==L"dora")s.dora=valueTiles;
                    else if(key==L"ura")s.ura=valueTiles;
                    else if(key==L"honba")s.honba=_wtoi(value.c_str());
                    else if((key==L"pon"||key==L"kan"||key==L"ankan"||key==L"chi")&&!valueTiles.empty())
                    {
                        melds.push_back({key==L"pon"?MeldKind::Pon:key==L"kan"?MeldKind::Kan:key==L"ankan"?MeldKind::ClosedKan:MeldKind::Chi,valueTiles[0]});
                        s.red+=valueRed;
                    }
                    else {got=L"unknown option "+w+L"; ";ok=false;}
                }
                const Score score=Evaluate(counts(tiles),melds,win,s);
                if(!score.win)got+=L"nowin";
                else
                {
                    got+=std::to_wstring(score.han)+L"han "+std::to_wstring(score.fu)+L"fu "+std::to_wstring(score.points);
                    if(*score.limitJa)got+=std::wstring(L" ")+score.limitJa;
                    got+=L" :";
                    for(const YakuHan& y:score.yaku)got+=std::wstring(L" ")+y.ja+L" "+std::to_wstring(y.han);
                }
                for(const std::wstring& e:expected)
                {
                    if(e==L"nowin")ok&=!score.win;
                    else if(e.ends_with(L"han"))ok&=score.win&&score.han==_wtoi(e.c_str());
                    else if(e.ends_with(L"fu"))ok&=score.win&&score.fu==_wtoi(e.c_str());
                    else ok&=score.win&&score.points==_wtoi(e.c_str());
                }
            }
            if(!ok)++failed;
            report+=(ok?L"ok  ":L"NG  ")+line+L"\n      "+got+L"\n";
        }
        return std::to_wstring(checked)+L" checked, "+std::to_wstring(failed)+L" failed\n"+report;
    }
}
