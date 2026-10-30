#include "Hanafuda.hpp"
#include "Text.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <sstream>

namespace MikuMikuDesktopPerch::Hanafuda
{
    namespace
    {
        constexpr Kind Kinds[Cards]={
            Kind::Bright,Kind::Ribbon,Kind::Chaff,Kind::Chaff,     // pine
            Kind::Animal,Kind::Ribbon,Kind::Chaff,Kind::Chaff,     // plum
            Kind::Bright,Kind::Ribbon,Kind::Chaff,Kind::Chaff,     // cherry
            Kind::Animal,Kind::Ribbon,Kind::Chaff,Kind::Chaff,     // wisteria
            Kind::Animal,Kind::Ribbon,Kind::Chaff,Kind::Chaff,     // iris
            Kind::Animal,Kind::Ribbon,Kind::Chaff,Kind::Chaff,     // peony
            Kind::Animal,Kind::Ribbon,Kind::Chaff,Kind::Chaff,     // bush clover
            Kind::Bright,Kind::Animal,Kind::Chaff,Kind::Chaff,     // susuki
            Kind::Animal,Kind::Ribbon,Kind::Chaff,Kind::Chaff,     // chrysanthemum
            Kind::Animal,Kind::Ribbon,Kind::Chaff,Kind::Chaff,     // maple
            Kind::Bright,Kind::Animal,Kind::Ribbon,Kind::Chaff,    // willow
            Kind::Bright,Kind::Chaff,Kind::Chaff,Kind::Chaff};     // paulownia

        const wchar_t* const MonthsJa[Months]={L"松",L"梅",L"桜",L"藤",L"菖蒲",L"牡丹",L"萩",L"芒",L"菊",L"紅葉",L"柳",L"桐"};
        const wchar_t* const MonthsEn[Months]={L"Pine",L"Plum",L"Cherry",L"Wisteria",L"Iris",L"Peony",L"Bush clover",L"Susuki",
                                               L"Chrysanthemum",L"Maple",L"Willow",L"Paulownia"};
        // The named cards (the rest are the month's chaff).
        struct Named {int card;const wchar_t* ja;const wchar_t* en;};
        constexpr Named Names[]={
            {0,L"松に鶴",L"Crane"},{1,L"松の赤短",L"Pine poem ribbon"},
            {4,L"梅に鶯",L"Bush warbler"},{5,L"梅の赤短",L"Plum poem ribbon"},
            {8,L"桜に幕",L"Curtain"},{9,L"桜の赤短",L"Cherry poem ribbon"},
            {12,L"藤に不如帰",L"Cuckoo"},{13,L"藤の短冊",L"Wisteria ribbon"},
            {16,L"菖蒲に八橋",L"Eight-plank bridge"},{17,L"菖蒲の短冊",L"Iris ribbon"},
            {20,L"牡丹に蝶",L"Butterflies"},{21,L"牡丹の青短",L"Peony blue ribbon"},
            {24,L"萩に猪",L"Boar"},{25,L"萩の短冊",L"Bush clover ribbon"},
            {28,L"芒に月",L"Moon"},{29,L"芒に雁",L"Geese"},
            {32,L"菊に盃",L"Sake cup"},{33,L"菊の青短",L"Chrysanthemum blue ribbon"},
            {36,L"紅葉に鹿",L"Deer"},{37,L"紅葉の青短",L"Maple blue ribbon"},
            {40,L"柳に小野道風",L"Poet in the rain"},{41,L"柳に燕",L"Swallow"},{42,L"柳の短冊",L"Willow ribbon"},{43,L"柳に雷",L"Lightning"},
            {44,L"桐に鳳凰",L"Phoenix"}};

        bool Has(const std::vector<int>& cards,int card){return std::find(cards.begin(),cards.end(),card)!=cards.end();}

        // An estimate of what a pile of taken cards is worth to its holder: the yaku made, and
        // how far toward those still to be had (blocked: the other's pile, out of reach).
        double Worth(const std::vector<int>& taken,const std::vector<int>& blocked)
        {
            std::array<bool,Cards> has{},gone{};
            for(int c:taken)has[c]=true;
            for(int c:blocked)gone[c]=true;
            double w=PointsOf(YakuOf(taken))*12.0;
            // Sets of named cards: worth more the more of them, while the rest can still come.
            auto set=[&](std::initializer_list<int> cards,int points){
                int k=0,n=0;
                bool possible=true;
                for(int c:cards){++n;if(has[c])++k;else if(gone[c])possible=false;}
                if(k>0&&k<n&&possible)w+=points*8.0*k*k/(n*n);
            };
            set({Boar,Deer,Butterflies},5);
            set({PoemRibbons[0],PoemRibbons[1],PoemRibbons[2]},5);
            set({BlueRibbons[0],BlueRibbons[1],BlueRibbons[2]},5);
            set({Curtain,SakeCup},5);
            set({Moon,SakeCup},5);
            // Toward three brights (the poet in the rain does not make three).
            int b=0,free=0;
            for(int c:{Crane,Curtain,Moon,Phoenix}){if(has[c])++b;else if(!gone[c])++free;}
            if(b>0&&b<3&&b+free>=3)w+=5*8.0*b*b/9;
            // The counted ones: five animals, five ribbons, ten chaff.
            auto counted=[&](Kind kind,int need){
                int k=0,reach=0;
                for(int c=0;c<Cards;++c)
                {
                    const bool is=KindOf(c)==kind||(kind==Kind::Chaff&&c==SakeCup);
                    if(!is)continue;
                    if(has[c])++k;
                    if(!gone[c])++reach;
                }
                if(k<need&&reach>=need)w+=5.0*std::pow(static_cast<double>(k)/need,3);
            };
            counted(Kind::Animal,5);
            counted(Kind::Ribbon,5);
            counted(Kind::Chaff,10);
            for(int c:taken)w+=KindOf(c)==Kind::Bright?2:KindOf(c)==Kind::Animal?1.2:KindOf(c)==Kind::Ribbon?1:0.3;
            return w;
        }

        std::vector<int> With(std::vector<int> pile,const std::vector<int>& more)
        {
            pile.insert(pile.end(),more.begin(),more.end());
            return pile;
        }

        // How many yaku the pile is one card from, with that card still to be had.
        int NearYaku(const std::vector<int>& taken,const std::vector<int>& blocked)
        {
            std::array<bool,Cards> has{},gone{};
            for(int c:taken)has[c]=true;
            for(int c:blocked)gone[c]=true;
            int near=0;
            auto set=[&](std::initializer_list<int> cards){
                int k=0,n=0,free=0;
                for(int c:cards){++n;if(has[c])++k;else if(!gone[c])++free;}
                if(k==n-1&&free==1)++near;
            };
            set({Boar,Deer,Butterflies});
            set({PoemRibbons[0],PoemRibbons[1],PoemRibbons[2]});
            set({BlueRibbons[0],BlueRibbons[1],BlueRibbons[2]});
            set({Curtain,SakeCup});
            set({Moon,SakeCup});
            int b=0,free=0;
            for(int c:{Crane,Curtain,Moon,Phoenix}){if(has[c])++b;else if(!gone[c])++free;}
            if((b==2||b==3)&&free>0)++near;
            auto counted=[&](Kind kind,int need){
                int k=0,free=0;
                for(int c=0;c<Cards;++c)
                {
                    if(!(KindOf(c)==kind||(kind==Kind::Chaff&&c==SakeCup)))continue;
                    if(has[c])++k;else if(!gone[c])++free;
                }
                if(k>=need-1&&free>0)++near;   // one more makes it, or adds a point
            };
            counted(Kind::Animal,5);
            counted(Kind::Ribbon,5);
            counted(Kind::Chaff,10);
            return near;
        }
    }

    Kind KindOf(int card){return Kinds[card];}

    const wchar_t* MonthName(int month){return English()?MonthsEn[month]:MonthsJa[month];}

    const wchar_t* CardName(int card)
    {
        for(const Named& n:Names)if(n.card==card)return English()?n.en:n.ja;
        static const std::array<std::wstring,Months> chaffJa=[]{
            std::array<std::wstring,Months> a;
            for(int m=0;m<Months;++m)a[m]=std::wstring(MonthsJa[m])+L"のカス";
            return a;
        }();
        static const std::array<std::wstring,Months> chaffEn=[]{
            std::array<std::wstring,Months> a;
            for(int m=0;m<Months;++m)a[m]=std::wstring(MonthsEn[m])+L" chaff";
            return a;
        }();
        return (English()?chaffEn:chaffJa)[MonthOf(card)].c_str();
    }

    std::vector<Yaku> YakuOf(const std::vector<int>& taken)
    {
        std::vector<Yaku> out;
        const bool rain=Has(taken,RainMan);
        int brights=0,animals=0,ribbons=0,chaff=0;
        for(int c:taken)
        {
            switch(KindOf(c))
            {
            case Kind::Bright:++brights;break;
            case Kind::Animal:++animals;break;
            case Kind::Ribbon:++ribbons;break;
            case Kind::Chaff:++chaff;break;
            }
            if(c==SakeCup)++chaff;   // the sake cup counts as chaff too
        }
        if(brights==5)out.push_back({L"五光",L"Five brights",5,10});
        else if(brights==4)out.push_back(rain?Yaku{L"雨四光",L"Rainy four brights",4,7}:Yaku{L"四光",L"Four brights",4,8});
        else if(brights==3&&!rain)out.push_back({L"三光",L"Three brights",3,5});
        if(Has(taken,Curtain)&&Has(taken,SakeCup))out.push_back({L"花見で一杯",L"Drinking under the cherry",2,5});
        if(Has(taken,Moon)&&Has(taken,SakeCup))out.push_back({L"月見で一杯",L"Drinking under the moon",2,5});
        if(Has(taken,Boar)&&Has(taken,Deer)&&Has(taken,Butterflies))out.push_back({L"猪鹿蝶",L"Boar, deer and butterflies",3,5});
        auto all=[&](const std::array<int,3>& cards){return std::all_of(cards.begin(),cards.end(),[&](int c){return Has(taken,c);});};
        if(all(PoemRibbons))out.push_back({L"赤短",L"Red poem ribbons",3,5});
        if(all(BlueRibbons))out.push_back({L"青短",L"Blue ribbons",3,5});
        if(animals>=5)out.push_back({L"タネ",L"Animals",animals,1+animals-5});
        if(ribbons>=5)out.push_back({L"タン",L"Ribbons",ribbons,1+ribbons-5});
        if(chaff>=10)out.push_back({L"カス",L"Chaff",chaff,1+chaff-10});
        return out;
    }

    int PointsOf(const std::vector<Yaku>& yaku)
    {
        int points=0;
        for(const Yaku& y:yaku)points+=y.points;
        return points;
    }

    // ---- The table ----

    void Table::NewGame(std::uint32_t seed,double c)
    {
        rng.seed(seed);
        care=c;
        seats={};
        round=1;
        dealer=Roll(0.5)?You:Her;
        gameOver=false;
        calls=0;
        moves=0;
        Deal();
    }

    void Table::NextRound()
    {
        if(phase!=Phase::RoundOver)return;
        if(round>=Rounds){phase=Phase::GameOver;return;}
        ++round;
        if(ended.winner>=0)dealer=ended.winner;   // no one: the dealer deals again
        Deal();
    }

    void Table::Deal()
    {
        std::vector<int> deck(Cards);
        for(;;)
        {
            std::iota(deck.begin(),deck.end(),0);
            std::shuffle(deck.begin(),deck.end(),rng);
            // Dealt as at a table: to the other, to the field, to the dealer, two at a time.
            for(auto& s:seats){s.hand.clear();s.taken.clear();s.koi=0;s.yakuPoints=0;}
            field.clear();
            size_t at=0;
            for(int round2=0;round2<4;++round2)
                for(int to:{1-dealer,-1,dealer})
                    for(int k=0;k<2;++k)
                    {
                        const int c=deck[at++];
                        if(to<0)field.push_back(c);else seats[to].hand.push_back(c);
                    }
            pile.assign(deck.begin()+static_cast<std::ptrdiff_t>(at),deck.end());
            // Four of a month on the field: dealt again.
            std::array<int,Months> onField{};
            for(int c:field)++onField[MonthOf(c)];
            if(std::find(onField.begin(),onField.end(),4)==onField.end())break;
        }
        for(auto& s:seats)std::sort(s.hand.begin(),s.hand.end());
        pending=-1;
        herPlayed=false;
        planned=plannedPick=-1;
        last={};
        ended={};
        // Four of a month in a hand (teshi), or four pairs (kuttsuki): won at the deal.
        std::array<int,2> dealtWin{-1,-1};
        for(int who:{You,Her})
        {
            std::array<int,Months> n{};
            for(int c:seats[who].hand)++n[MonthOf(c)];
            if(std::find(n.begin(),n.end(),4)!=n.end())dealtWin[who]=0;
            else if(std::count(n.begin(),n.end(),2)==4)dealtWin[who]=1;
        }
        if(dealtWin[You]>=0&&dealtWin[Her]>=0){Deal();return;}   // both: dealt again
        for(int who:{You,Her})
            if(dealtWin[who]>=0)
            {
                ended.winner=who;
                ended.dealt=true;
                ended.yaku={dealtWin[who]==0?Yaku{L"手四",L"Four of a month",4,6}:Yaku{L"くっつき",L"Four pairs",8,6}};
                ended.base=ended.points=6;
                ended.hand=seats[who].hand;
                seats[who].score+=6;
                phase=Phase::RoundOver;
                return;
            }
        phase=dealer==You?Phase::YourPlay:Phase::HerTurn;
    }

    std::vector<int> Table::Matches(int card) const
    {
        std::vector<int> out;
        for(size_t i=0;i<field.size();++i)
            if(field[i]>=0&&MonthOf(field[i])==MonthOf(card))out.push_back(static_cast<int>(i));
        return out;
    }

    void Table::Lay(int who,int card,bool fromPile,int pick)
    {
        Move m;
        m.who=who;
        m.card=card;
        m.fromPile=fromPile;
        const std::vector<int> match=Matches(card);
        auto take=[&](int i){m.took.push_back(field[i]);seats[who].taken.push_back(field[i]);field[i]=-1;};
        if(match.empty())
        {
            // Into the first empty place (the field's cards stay where they lie).
            const auto hole=std::find(field.begin(),field.end(),-1);
            if(hole!=field.end())*hole=card;else field.push_back(card);
        }
        else
        {
            seats[who].taken.push_back(card);
            if(match.size()==2)take(std::find(match.begin(),match.end(),pick)!=match.end()?pick:match[0]);
            else for(int i:match)take(i);
        }
        // Empty places at the end go (the rest keep theirs).
        while(!field.empty()&&field.back()<0)field.pop_back();
        last=m;
        ++moves;
    }

    void Table::Play(int index)
    {
        auto& hand=seats[You].hand;
        if(phase!=Phase::YourPlay||index<0||index>=static_cast<int>(hand.size()))return;
        const int card=hand[static_cast<size_t>(index)];
        hand.erase(hand.begin()+index);
        if(Matches(card).size()==2){pending=card;phase=Phase::YourPick;return;}
        Lay(You,card,false,-1);
        phase=Phase::YourDraw;
    }

    void Table::Pick(int fieldIndex)
    {
        if((phase!=Phase::YourPick&&phase!=Phase::YourDrawPick)||pending<0)return;
        const auto match=Matches(pending);
        if(std::find(match.begin(),match.end(),fieldIndex)==match.end())return;
        const bool drawn=phase==Phase::YourDrawPick;
        Lay(You,pending,drawn,fieldIndex);
        pending=-1;
        if(drawn)TurnDone(You);else phase=Phase::YourDraw;
    }

    void Table::Draw()
    {
        if(phase!=Phase::YourDraw||pile.empty())return;
        const int card=pile.back();
        pile.pop_back();
        if(Matches(card).size()==2){pending=card;phase=Phase::YourDrawPick;return;}
        Lay(You,card,true,-1);
        TurnDone(You);
    }

    void Table::Koi(bool goOn)
    {
        if(phase!=Phase::YourKoi)return;
        if(!goOn){Stop(You);return;}
        auto& me=seats[You];
        ++me.koi;
        me.yakuPoints=PointsOf(YakuOf(me.taken));
        phase=Phase::HerTurn;
        herPlayed=false;
        planned=-1;
    }

    void Table::TurnDone(int who)
    {
        auto& me=seats[who];
        const int points=PointsOf(YakuOf(me.taken));
        if(points>me.yakuPoints)
        {
            // With no card left to play, there is nothing to go on for.
            if(me.hand.empty()){if(who==Her)HerSays(L"勝負",L"Stop");Stop(who);return;}
            if(who==You){phase=Phase::YourKoi;return;}
            if(WantsKoi(Her,CareNow()))
            {
                ++me.koi;
                me.yakuPoints=points;
                HerSays(L"こいこい",L"Koi-koi");
            }
            else {HerSays(L"勝負",L"Stop");Stop(Her);return;}
        }
        if(seats[You].hand.empty()&&seats[Her].hand.empty()){NoOne();return;}
        if(who==You){phase=Phase::HerTurn;herPlayed=false;planned=-1;}
        else phase=Phase::YourPlay;
    }

    void Table::Stop(int who)
    {
        ended={};
        ended.winner=who;
        ended.yaku=YakuOf(seats[who].taken);
        ended.base=PointsOf(ended.yaku);
        ended.doubledSeven=ended.base>=7;
        ended.doubledKoi=seats[1-who].koi>0;
        ended.points=ended.base*(ended.doubledSeven?2:1)*(ended.doubledKoi?2:1);
        seats[who].score+=ended.points;
        phase=Phase::RoundOver;
    }

    void Table::NoOne()
    {
        ended={};
        phase=Phase::RoundOver;
    }

    int Table::HerNextPlay()
    {
        if(phase!=Phase::HerTurn||herPlayed||seats[Her].hand.empty())return -1;
        if(planned<0)planned=ChoosePlay(Her,CareNow(),plannedPick);
        return planned;
    }

    void Table::Step()
    {
        if(phase!=Phase::HerTurn)return;
        if(!herPlayed)
        {
            const int index=HerNextPlay();
            if(index<0){herPlayed=true;return;}
            auto& hand=seats[Her].hand;
            const int card=hand[static_cast<size_t>(index)];
            hand.erase(hand.begin()+index);
            Lay(Her,card,false,plannedPick);
            herPlayed=true;
            planned=plannedPick=-1;
            return;
        }
        const int card=pile.back();
        pile.pop_back();
        const auto match=Matches(card);
        Lay(Her,card,true,match.size()==2?ChoosePick(Her,card,match):-1);
        herPlayed=false;
        TurnDone(Her);
    }

    void Table::PlayForYou()
    {
        switch(phase)
        {
        case Phase::YourPlay:
        {
            int pick=-1;
            const int index=ChoosePlay(You,1,pick);
            Play(index);
            if(phase==Phase::YourPick)Pick(pick);
            break;
        }
        case Phase::YourPick:
        case Phase::YourDrawPick:Pick(ChoosePick(You,pending,Matches(pending)));break;
        case Phase::YourDraw:Draw();break;
        case Phase::YourKoi:Koi(WantsKoi(You,1));break;
        default:break;
        }
    }

    bool Table::Consistent() const
    {
        std::array<int,Cards> n{};
        auto count=[&](const std::vector<int>& cards){for(int c:cards)if(c>=0&&c<Cards)++n[c];};
        for(const auto& s:seats){count(s.hand);count(s.taken);}
        count(field);
        count(pile);
        if(pending>=0)++n[pending];
        return std::all_of(n.begin(),n.end(),[](int k){return k==1;});
    }

    // ---- How she plays ----

    double Table::CareNow() const
    {
        // A little looser while she leads by a lot.
        const int lead=seats[Her].score-seats[You].score;
        return std::max(0.3,care-(lead>=40?0.3:lead>=20?0.15:0.0));
    }

    std::array<bool,Cards> Table::SeenBy(int who) const
    {
        std::array<bool,Cards> seen{};
        for(int c:seats[who].hand)seen[c]=true;
        for(const auto& s:seats)for(int c:s.taken)seen[c]=true;
        for(int c:field)if(c>=0)seen[c]=true;
        if(pending>=0)seen[pending]=true;
        return seen;
    }

    double Table::Gain(int who,const std::vector<int>& cards) const
    {
        const auto& mine=seats[who].taken;
        const auto& theirs=seats[1-who].taken;
        const auto more=With(mine,cards);
        double g=Worth(more,theirs)-Worth(mine,theirs);
        // Taken from the other's reach: what it would have been to them, and what it blocks.
        g+=0.5*(Worth(theirs,mine)-Worth(theirs,more));
        g+=0.3*(Worth(With(theirs,cards),mine)-Worth(theirs,mine));
        return g;
    }

    double Table::LeaveCost(int who,int card) const
    {
        // The chance the other holds a card of its month (from what who cannot see).
        const auto seen=SeenBy(who);
        int unseen=0,ofMonth=0;
        for(int c=0;c<Cards;++c)
            if(!seen[c]&&c!=card){++unseen;if(MonthOf(c)==MonthOf(card))++ofMonth;}
        const double h=static_cast<double>(seats[1-who].hand.size());
        const double p=unseen>0?1-std::pow(1-std::min(1.0,h/unseen),ofMonth):0;
        const auto& mine=seats[who].taken;
        const auto& theirs=seats[1-who].taken;
        double cost=p*(Worth(With(theirs,{card}),mine)-Worth(theirs,mine)+0.5);
        // Holding another of its month: who may take it back next turn.
        for(int c:seats[who].hand)
            if(c!=card&&MonthOf(c)==MonthOf(card)){cost-=0.4*(1-p)*(Worth(With(mine,{card,c}),theirs)-Worth(mine,theirs));break;}
        return cost;
    }

    int Table::ChoosePick(int who,int card,const std::vector<int>& matches)
    {
        int best=matches.empty()?-1:matches[0];
        double bestValue=-1e9;
        for(int i:matches)
        {
            const double v=Gain(who,{card,field[static_cast<size_t>(i)]});
            if(v>bestValue){bestValue=v;best=i;}
        }
        return best;
    }

    int Table::ChoosePlay(int who,double carefully,int& pick)
    {
        const auto& hand=seats[who].hand;
        struct Option {double value;int index,pick;};
        std::vector<Option> options;
        for(size_t i=0;i<hand.size();++i)
        {
            const int card=hand[i];
            const auto match=Matches(card);
            if(match.empty())options.push_back({-LeaveCost(who,card),static_cast<int>(i),-1});
            else if(match.size()==2)
            {
                const int p=ChoosePick(who,card,match);
                options.push_back({Gain(who,{card,field[static_cast<size_t>(p)]}),static_cast<int>(i),p});
            }
            else
            {
                std::vector<int> taken{card};
                for(int m:match)taken.push_back(field[static_cast<size_t>(m)]);
                options.push_back({Gain(who,taken),static_cast<int>(i),-1});
            }
        }
        std::stable_sort(options.begin(),options.end(),[](const Option& a,const Option& b){return a.value>b.value;});
        // Less careful: now and then the next best.
        size_t chosen=0;
        if(options.size()>1&&Roll((1-carefully)*0.6))chosen=1;
        pick=options[chosen].pick;
        return options[chosen].index;
    }

    bool Table::WantsKoi(int who,double carefully)
    {
        const auto& me=seats[who];
        const auto& other=seats[1-who];
        const int points=PointsOf(YakuOf(me.taken));
        const int cards=static_cast<int>(me.hand.size());
        const double loose=1-carefully;
        if(other.koi>0)return false;                       // doubled already: take it
        if(points>=7)return Roll(loose*0.15);              // doubled at seven
        if(cards<=2)return false;
        const int danger=NearYaku(other.taken,me.taken);
        if(danger>0&&!Roll(loose*0.3))return false;        // the other is one card from a yaku
        const int hope=NearYaku(me.taken,other.taken);
        double chance=0.2+0.15*hope+(cards>=5?0.15:0)-0.05*points+loose*0.3;
        return Roll(std::clamp(chance,0.0,0.8));
    }

    // ---- Diagnostics ----

    std::wstring CheckPlaying(int count,std::uint32_t seed)
    {
        std::wostringstream out,notes;
        int wrong=0;
        for(int mode=0;mode<2;++mode)
        {
            std::mt19937 random(seed+mode*1000003u);
            int rounds=0,herWins=0,yourWins=0,noOne=0,dealt=0,herKoi=0,yourKoi=0,gamesHer=0,gamesYou=0,ties=0;
            long long herPoints=0,yourPoints=0;
            std::map<std::wstring,int> yaku;
            for(int g=0;g<count;++g)
            {
                Table t;
                t.NewGame(seed+static_cast<std::uint32_t>(g)*7919u+mode,1);
                int steps=0;
                auto any=[&](int n){return std::uniform_int_distribution<int>(0,n-1)(random);};
                while(t.Now()!=Phase::GameOver&&++steps<20000)
                {
                    const Phase before=t.Now();
                    const int calls=t.HerCalls();
                    switch(before)
                    {
                    case Phase::HerTurn:t.Step();break;
                    case Phase::RoundOver:
                    {
                        const auto& e=t.Ended();
                        ++rounds;
                        if(e.winner<0)++noOne;
                        else
                        {
                            (e.winner==Her?herWins:yourWins)++;
                            (e.winner==Her?herPoints:yourPoints)+=e.points;
                            if(e.dealt)++dealt;
                            for(const auto& y:e.yaku)++yaku[y.ja];
                            if(e.points!=e.base*(e.doubledSeven?2:1)*(e.doubledKoi?2:1)){++wrong;notes<<L"points wrong in game "<<g<<L"\n";}
                        }
                        t.NextRound();
                        break;
                    }
                    default:
                        if(mode==0)t.PlayForYou();
                        else switch(before)
                        {
                        case Phase::YourPlay:t.Play(any(static_cast<int>(t.Of(You).hand.size())));break;
                        case Phase::YourPick:
                        case Phase::YourDrawPick:{const auto m=t.Matches(t.Pending());t.Pick(m[static_cast<size_t>(any(static_cast<int>(m.size())))]);break;}
                        case Phase::YourDraw:t.Draw();break;
                        case Phase::YourKoi:t.Koi(any(2)==0);break;
                        default:break;
                        }
                        if(before==Phase::YourKoi&&t.Now()!=Phase::RoundOver)++yourKoi;
                    }
                    if(t.HerCalls()!=calls&&std::wstring(t.HerCallJa())==L"こいこい")++herKoi;
                    if(t.Now()==before&&before!=Phase::HerTurn&&before!=Phase::RoundOver){++wrong;notes<<L"stuck in game "<<g<<L"\n";break;}
                    if(!t.Consistent()){++wrong;notes<<L"cards lost in game "<<g<<L" at step "<<steps<<L"\n";break;}
                    if(t.Field().size()>Cards){++wrong;break;}
                }
                if(t.Now()!=Phase::GameOver){++wrong;notes<<L"game "<<g<<L" never ended\n";continue;}
                const int h=t.Of(Her).score,y=t.Of(You).score;
                if(h>y)++gamesHer;else if(y>h)++gamesYou;else ++ties;
            }
            out<<(mode==0?L"against her own play":L"against random play")<<L": "<<count<<L" games, her "<<gamesHer<<L", yours "<<gamesYou<<L", ties "<<ties<<L"\n";
            out<<L"  rounds "<<rounds<<L": she won "<<herWins<<L" ("<<(herWins?herPoints/herWins:0)<<L" mon each), you "<<yourWins<<L" ("<<(yourWins?yourPoints/yourWins:0)
               <<L" each), no one "<<noOne<<L", at the deal "<<dealt<<L"; koi-koi: hers "<<herKoi<<L", yours "<<yourKoi<<L"\n  yaku:";
            for(const auto& [name,n]:yaku)out<<L" "<<name<<L" "<<n;
            out<<L"\n";
        }
        return std::to_wstring(wrong)+L" wrong\n"+out.str()+notes.str();
    }

    std::wstring CheckYaku(const std::wstring& cases)
    {
        std::wistringstream in(cases);
        std::wostringstream out;
        std::wstring line;
        int failed=0,n=0;
        while(std::getline(in,line))
        {
            if(line.empty()||line[0]==L'#'||line.find(L"=>")==std::wstring::npos)continue;
            const size_t arrow=line.find(L"=>");
            std::wistringstream cards(line.substr(0,arrow));
            std::vector<int> taken;
            for(int c;cards>>c;)if(c>=0&&c<Cards)taken.push_back(c);
            const int expected=std::stoi(line.substr(arrow+2));
            const auto yaku=YakuOf(taken);
            const int got=PointsOf(yaku);
            ++n;
            std::wstring names;
            for(const auto& y:yaku)names+=std::wstring(L" ")+y.ja+L"("+std::to_wstring(y.points)+L")";
            if(got!=expected)++failed;
            out<<(got==expected?L"ok   ":L"FAIL ")<<line<<L"  got "<<got<<names<<L"\n";
        }
        return std::to_wstring(failed)+L" failed of "+std::to_wstring(n)+L"\n"+out.str();
    }
}
