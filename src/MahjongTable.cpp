#include "Mahjong.hpp"
#include <algorithm>
#include <chrono>
#include <map>
#include <cmath>

namespace MikuMikuDesktopPerch::Mahjong
{
    namespace
    {
        constexpr int StartPoints=25000;

        bool Before(const Piece& a,const Piece& b){return a.kind!=b.kind?a.kind<b.kind:a.red&&!b.red;}
        int Other(int who){return 1-who;}
        int DoraOf(int indicator)
        {
            if(indicator<East)return indicator/9*9+(indicator%9+1)%9;
            if(indicator<White)return East+(indicator-East+1)%4;
            return White+(indicator-White+1)%3;
        }
        int Total(const Counts& c){int n=0;for(int v:c)n+=v;return n;}
    }

    void Table::NewGame(std::uint32_t seed,double carefully)
    {
        rng.seed(seed);
        care=std::clamp(carefully,0.0,1.0);
        seats={};
        for(Seat& s:seats)s.points=StartPoints;
        honba=sticks=0;
        round=1;
        dealer=std::uniform_int_distribution<int>(0,1)(rng);
        Deal();
    }

    void Table::Deal()
    {
        unseen=Unseen::All();
        for(Seat& s:seats)
        {
            const int points=s.points;
            s=Seat{};
            s.points=points;
        }
        dora.clear();
        indicatorRed={};
        dora.push_back(DrawIndicator());
        for(Seat& s:seats)
        {
            for(int i=0;i<13;++i)
            {
                bool red=false;
                const int k=unseen.Draw(rng,&red);
                s.hand.push_back({k,red});
            }
            std::sort(s.hand.begin(),s.hand.end(),Before);
        }
        wall=2*DrawsEach;
        calledYet=rinshan=herDrawn=false;
        planned=-1;
        ended=RoundEnd{};
        callJa=callEn=L"";
        if(dealer==You)Draw(You);
        else phase=Phase::HerTurn;
    }

    int Table::DrawIndicator()
    {
        bool red=false;
        const int k=unseen.Draw(rng,&red);
        if(red)++indicatorRed[Suit(k)];
        return k;
    }

    void Table::NextRound()
    {
        if(phase!=Phase::RoundOver)return;
        if(gameOver){phase=Phase::GameOver;return;}
        if(!dealerStays){dealer=Other(dealer);++round;}
        honba=nextHonba;
        Deal();
    }

    Situation Table::SituationOf(int who,bool tsumo) const
    {
        const Seat& me=seats[who];
        Situation s;
        s.tsumo=tsumo;
        s.riichi=me.riichi&&!me.doubleRiichi;
        s.doubleRiichi=me.doubleRiichi;
        s.ippatsu=me.ippatsu;
        s.rinshan=tsumo&&rinshan;
        s.lastTile=wall==0&&!s.rinshan;
        s.firstDraw=tsumo&&!calledYet&&me.draws==1;
        s.roundWind=RoundWind();
        s.seatWind=SeatWind(who);
        s.dora=dora;
        s.honba=honba;
        return s;
    }

    Counts Table::CountsOf(int who,bool withDrawn) const
    {
        Counts c{};
        const Seat& me=seats[who];
        for(const Piece& p:me.hand)++c[p.kind];
        if(withDrawn&&me.hasDrawn)++c[me.drawn.kind];
        return c;
    }

    int Table::RedOf(int who,bool withDrawn) const
    {
        const Seat& me=seats[who];
        int red=me.meldRed;
        for(const Piece& p:me.hand)red+=p.red;
        if(withDrawn&&me.hasDrawn)red+=me.drawn.red;
        return red;
    }

    Counts Table::RiverCounts(int who) const
    {
        Counts c{};
        for(const RiverTile& r:seats[who].river)++c[r.piece.kind];
        return c;
    }

    Counts Table::SeenBy(int who) const
    {
        Counts c=CountsOf(who,true);
        for(const Seat& s:seats)
        {
            for(const RiverTile& r:s.river)if(!r.taken)++c[r.piece.kind];
            for(const Meld& m:s.melds)c[m.tile]+=m.kind==MeldKind::Pon?3:4;
        }
        for(int d:dora)++c[d];
        return c;
    }

    std::vector<int> Table::Waits() const
    {
        return Mahjong::Waits(CountsOf(You,false));
    }

    bool Table::FuritenOf(int who) const
    {
        const Seat& me=seats[who];
        if(me.passedWin||me.passedInRiichi)return true;
        const Counts river=RiverCounts(who);
        for(int w:Mahjong::Waits(CountsOf(who,false)))if(river[w])return true;
        return false;
    }

    bool Table::CanTsumoFor(int who) const
    {
        const Seat& me=seats[who];
        if(!me.hasDrawn)return false;
        Situation s=SituationOf(who,true);
        s.red=RedOf(who,true);
        return Evaluate(CountsOf(who,true),me.melds,me.drawn.kind,s).win;
    }

    int Table::TsumoPoints() const
    {
        const Seat& me=seats[You];
        if(phase!=Phase::YourTurn||!me.hasDrawn)return 0;
        Situation s=SituationOf(You,true);
        s.red=RedOf(You,true);
        const Score score=Evaluate(CountsOf(You,true),me.melds,me.drawn.kind,s);
        return score.win?score.points:0;
    }

    bool Table::CanRonFor(int who) const
    {
        const Seat& other=seats[Other(who)];
        if(other.river.empty()||other.river.back().taken||FuritenOf(who))return false;
        const Piece tile=other.river.back().piece;
        Counts c=CountsOf(who,false);
        if(Total(c)%3!=1)return false;
        ++c[tile.kind];
        Situation s=SituationOf(who,false);
        s.red=RedOf(who,false)+tile.red;
        return Evaluate(c,seats[who].melds,tile.kind,s).win;
    }

    std::vector<int> Table::RiichiDiscardsFor(int who) const
    {
        std::vector<int> out;
        const Seat& me=seats[who];
        if(!me.hasDrawn||me.riichi||me.points<1000||wall<1)return out;
        for(const Meld& m:me.melds)if(m.kind!=MeldKind::ClosedKan)return out;
        const Counts all=CountsOf(who,true);
        for(size_t i=0;i<=me.hand.size();++i)
        {
            Counts c=all;
            --c[i<me.hand.size()?me.hand[i].kind:me.drawn.kind];
            if(!Mahjong::Waits(c).empty())out.push_back(static_cast<int>(i));
        }
        return out;
    }

    std::vector<RiichiWait> Table::RiichiWaits(int index) const
    {
        std::vector<RiichiWait> out;
        const Seat& me=seats[You];
        if(phase!=Phase::YourTurn||!me.hasDrawn||index<0||index>static_cast<int>(me.hand.size()))return out;
        const Piece gone=index<static_cast<int>(me.hand.size())?me.hand[static_cast<size_t>(index)]:me.drawn;
        Counts c=CountsOf(You,true);
        --c[gone.kind];
        const std::vector<int> waits=Mahjong::Waits(c);
        // Furiten when one of them is in your river, the tile going there now included: only a
        // tsumo wins then.
        Counts river=RiverCounts(You);
        ++river[gone.kind];
        bool furiten=false;
        for(int w:waits)furiten=furiten||river[w]>0;
        const Counts seen=SeenBy(You);
        for(int w:waits)
        {
            Counts won=c;
            ++won[w];
            Situation s=SituationOf(You,furiten);
            s.doubleRiichi=me.draws==1&&!calledYet;
            s.riichi=!s.doubleRiichi;
            s.ippatsu=s.rinshan=s.lastTile=s.firstDraw=false;
            s.red=RedOf(You,true)-(gone.red?1:0);
            const Score score=Evaluate(won,me.melds,w,s);
            out.push_back({w,score.win?score.points:0,std::max(0,4-seen[w]),furiten});
        }
        return out;
    }

    std::vector<int> Table::RiichiDiscards() const
    {
        return phase==Phase::YourTurn?RiichiDiscardsFor(You):std::vector<int>{};
    }

    std::vector<int> Table::KanKinds() const
    {
        std::vector<int> out;
        const Seat& me=seats[You];
        if(phase!=Phase::YourTurn||!me.hasDrawn||me.riichi||wall<1)return out;
        const Counts c=CountsOf(You,true);
        for(int k=0;k<Kinds;++k)if(c[k]==4)out.push_back(k);
        for(const Meld& m:me.melds)if(m.kind==MeldKind::Pon&&c[m.tile]>0)out.push_back(m.tile);
        return out;
    }

    bool Table::CanPon() const
    {
        if(phase!=Phase::YourCall||seats[You].riichi||wall<1)return false;
        return CountsOf(You,false)[seats[Her].river.back().piece.kind]>=2;
    }

    bool Table::CanOpenKan() const
    {
        if(phase!=Phase::YourCall||seats[You].riichi||wall<1)return false;
        return CountsOf(You,false)[seats[Her].river.back().piece.kind]>=3;
    }

    void Table::Draw(int who)
    {
        if(wall==0){WallEmpty();return;}
        --wall;
        Seat& me=seats[who];
        bool red=false;
        const int k=unseen.Draw(rng,&red);
        me.drawn={k,red};
        me.hasDrawn=true;
        ++me.draws;
        rinshan=false;
        if(who==You)phase=Phase::YourTurn;
        else {phase=Phase::HerTurn;herDrawn=true;planned=-1;}
    }

    void Table::KanDraw(int who)
    {
        // A new dora indicator, and a tile from the dead wall (the live wall one shorter).
        calledYet=true;
        for(Seat& s:seats)s.ippatsu=false;
        dora.push_back(DrawIndicator());
        --wall;
        Seat& me=seats[who];
        bool red=false;
        const int k=unseen.Draw(rng,&red);
        me.drawn={k,red};
        me.hasDrawn=true;
        rinshan=true;
        if(who==You)phase=Phase::YourTurn;
        else {phase=Phase::HerTurn;herDrawn=true;planned=-1;}
    }

    void Table::Discard(int index,bool riichi)
    {
        const Seat& me=seats[You];
        const int n=static_cast<int>(me.hand.size());
        if(phase!=Phase::YourTurn||index<0||index>n||(index==n&&!me.hasDrawn))return;
        if(riichi)
        {
            const std::vector<int> can=RiichiDiscards();
            if(std::find(can.begin(),can.end(),index)==can.end())return;
        }
        DiscardFor(You,index,riichi);
    }

    void Table::DiscardFor(int who,int index,bool riichi)
    {
        Seat& me=seats[who];
        const int n=static_cast<int>(me.hand.size());
        const Piece tile=index==n?me.drawn:me.hand[index];
        if(index<n)
        {
            me.hand.erase(me.hand.begin()+index);
            if(me.hasDrawn)me.hand.push_back(me.drawn);
        }
        me.hasDrawn=false;
        std::sort(me.hand.begin(),me.hand.end(),Before);
        if(riichi)
        {
            me.riichi=true;
            me.doubleRiichi=me.draws==1&&!calledYet;
            me.ippatsu=true;
            me.points-=1000;
            ++sticks;
            if(who==Her)HerSays(L"リーチ",L"Riichi");
        }
        else me.ippatsu=false;
        me.river.push_back({tile,riichi});
        me.passedWin=false;
        AfterDiscard(who,tile);
    }

    void Table::AfterDiscard(int who,const Piece& tile)
    {
        if(Other(who)==You)
        {
            phase=Phase::YourCall;
            if(CanRon()||CanPon()||CanOpenKan())return;
            Pass();
            return;
        }
        // Her answer to your discard: a ron whenever she can, else perhaps a pon.
        Seat& her=seats[Her];
        if(CanRonFor(Her)){RonFor(Her);return;}
        const std::vector<int> waits=Mahjong::Waits(CountsOf(Her,false));
        if(std::find(waits.begin(),waits.end(),tile.kind)!=waits.end())
        {
            her.passedWin=true;   // a winning shape without a yaku
            if(her.riichi)her.passedInRiichi=true;
        }
        if(her.riichi)her.passedAfterRiichi.push_back(tile.kind);
        if(WantsPon(Her,CareNow())){PonFor(Her);return;}
        phase=Phase::HerTurn;
        herDrawn=false;
    }

    void Table::Pass()
    {
        if(phase!=Phase::YourCall)return;
        Seat& me=seats[You];
        const int kind=seats[Her].river.back().piece.kind;
        const std::vector<int> waits=Waits();
        if(std::find(waits.begin(),waits.end(),kind)!=waits.end())
        {
            me.passedWin=true;
            if(me.riichi)me.passedInRiichi=true;
        }
        if(me.riichi)me.passedAfterRiichi.push_back(kind);
        Draw(You);
    }

    void Table::RonFor(int who)
    {
        Seat& me=seats[who];
        RiverTile& taken=seats[Other(who)].river.back();
        taken.taken=true;
        const Piece tile=taken.piece;
        Counts c=CountsOf(who,false);
        ++c[tile.kind];
        Situation s=SituationOf(who,false);
        s.red=RedOf(who,false)+tile.red;
        std::vector<Piece> shown=me.hand;
        shown.push_back(tile);
        Win(who,false,c,me.melds,tile.kind,s,shown);
    }

    void Table::Ron()
    {
        if(CanRon())RonFor(You);
    }

    void Table::Tsumo()
    {
        if(!CanTsumo())return;
        Seat& me=seats[You];
        Situation s=SituationOf(You,true);
        s.red=RedOf(You,true);
        std::vector<Piece> shown=me.hand;
        shown.push_back(me.drawn);
        Win(You,true,CountsOf(You,true),me.melds,me.drawn.kind,s,shown);
    }

    void Table::PonFor(int who)
    {
        Seat& me=seats[who];
        RiverTile& taken=seats[Other(who)].river.back();
        taken.taken=true;
        int red=taken.piece.red,removed=0;
        for(size_t i=0;i<me.hand.size()&&removed<2;)
        {
            if(me.hand[i].kind==taken.piece.kind){red+=me.hand[i].red;me.hand.erase(me.hand.begin()+i);++removed;}
            else ++i;
        }
        me.melds.push_back({MeldKind::Pon,taken.piece.kind});
        me.meldRed+=red;
        calledYet=true;
        for(Seat& s:seats)s.ippatsu=false;
        if(who==You)phase=Phase::YourTurn;   // a discard, without drawing
        else
        {
            HerSays(L"ポン",L"Pon");
            phase=Phase::HerTurn;
            herDrawn=true;planned=-1;
        }
    }

    void Table::Pon()
    {
        if(CanPon())PonFor(You);
    }

    void Table::OpenKan()
    {
        if(!CanOpenKan())return;
        Seat& me=seats[You];
        RiverTile& taken=seats[Her].river.back();
        taken.taken=true;
        int red=taken.piece.red,removed=0;
        for(size_t i=0;i<me.hand.size()&&removed<3;)
        {
            if(me.hand[i].kind==taken.piece.kind){red+=me.hand[i].red;me.hand.erase(me.hand.begin()+i);++removed;}
            else ++i;
        }
        me.melds.push_back({MeldKind::Kan,taken.piece.kind});
        me.meldRed+=red;
        KanDraw(You);
    }

    void Table::Kan(int kind)
    {
        const std::vector<int> can=KanKinds();
        if(std::find(can.begin(),can.end(),kind)==can.end())return;
        Seat& me=seats[You];
        me.hand.push_back(me.drawn);
        me.hasDrawn=false;
        auto added=std::find_if(me.melds.begin(),me.melds.end(),[&](const Meld& m){return m.kind==MeldKind::Pon&&m.tile==kind;});
        const int take=added!=me.melds.end()?1:4;
        int red=0,removed=0;
        for(size_t i=0;i<me.hand.size()&&removed<take;)
        {
            if(me.hand[i].kind==kind){red+=me.hand[i].red;me.hand.erase(me.hand.begin()+i);++removed;}
            else ++i;
        }
        if(added!=me.melds.end())added->kind=MeldKind::Kan;
        else me.melds.push_back({MeldKind::ClosedKan,kind});
        me.meldRed+=red;
        std::sort(me.hand.begin(),me.hand.end(),Before);
        KanDraw(You);
    }

    void Table::Step()
    {
        if(phase!=Phase::HerTurn)return;
        if(!herDrawn)
        {
            Draw(Her);
            if(phase==Phase::HerTurn&&CanTsumoFor(Her))
            {
                Seat& her=seats[Her];
                Situation s=SituationOf(Her,true);
                s.red=RedOf(Her,true);
                std::vector<Piece> shown=her.hand;
                shown.push_back(her.drawn);
                Win(Her,true,CountsOf(Her,true),her.melds,her.drawn.kind,s,shown);
            }
            return;
        }
        HerDiscard();
    }

    int Table::HerNextDiscard()
    {
        if(phase!=Phase::HerTurn||!herDrawn)return -1;
        if(planned<0){plannedRiichi=false;planned=ChooseDiscard(Her,CareNow(),plannedRiichi);}
        return planned;
    }

    void Table::HerDiscard()
    {
        const int index=HerNextDiscard();
        const bool riichi=plannedRiichi;
        herDrawn=false;
        planned=-1;
        DiscardFor(Her,index,riichi);
    }

    void Table::PlayForYou()
    {
        if(phase==Phase::YourTurn)
        {
            if(CanTsumo()){Tsumo();return;}
            bool riichi=false;
            const int index=ChooseDiscard(You,1,riichi);
            Discard(index,riichi);
        }
        else if(phase==Phase::YourCall)
        {
            if(CanRon())Ron();
            else if(CanPon()&&WantsPon(You,1))Pon();
            else Pass();
        }
    }

    double Table::CareNow() const
    {
        // Well ahead, she plays a little looser.
        const int lead=seats[Her].points-seats[You].points;
        return std::clamp(care-(lead>=16000?0.3:lead>=8000?0.15:0.0),0.0,1.0);
    }

    double Table::Safety(int who,int kind) const
    {
        const Seat& other=seats[Other(who)];
        // What the other has discarded can never win for them (furiten), nor what they let go after their riichi.
        for(const RiverTile& r:other.river)if(r.piece.kind==kind)return 1.0;
        if(std::find(other.passedAfterRiichi.begin(),other.passedAfterRiichi.end(),kind)!=other.passedAfterRiichi.end())return 1.0;
        const Counts seen=SeenBy(who);
        if(Honor(kind))return seen[kind]>=3?0.95:seen[kind]==2?0.75:seen[kind]==1?0.55:0.45;
        // Suji: both tiles three away discarded rule out a two-sided wait on it.
        const Counts river=RiverCounts(Other(who));
        const int n=kind%9;
        const bool low=n<3||river[kind-3]>0,high=n>5||river[kind+3]>0;
        if(low&&high)return Terminal(kind)?0.8:(n==1||n==7)?0.7:0.6;
        return Terminal(kind)?0.4:(n==1||n==7)?0.3:0.15;
    }

    int Table::ChooseDiscard(int who,double carefully,bool& riichi)
    {
        riichi=false;
        const Seat& me=seats[who];
        const int n=static_cast<int>(me.hand.size());
        if(me.riichi&&me.hasDrawn)return n;   // after riichi, what is drawn goes
        const Counts all=CountsOf(who,true);
        const int melds=static_cast<int>(me.melds.size());
        const Counts seen=SeenBy(who);
        auto left=[&](int k){return std::max(0,4-seen[k]);};
        Counts doras{};
        for(int d:dora)++doras[DoraOf(d)];
        const int now=Shanten(all,melds);

        // Each kind that could go: how far from waiting that leaves the hand, and how many of
        // the tiles not yet seen would bring it closer.
        struct Option {int kind;int shanten;int accept;double value;};
        std::vector<Option> options;
        for(int k=0;k<Kinds;++k)
        {
            if(all[k]==0)continue;
            Counts c=all;
            --c[k];
            const int sh=Shanten(c,melds);
            int accept=0;
            for(int x=0;x<Kinds;++x)
            {
                if(left(x)==0||c[x]>=4)continue;
                ++c[x];
                if(Shanten(c,melds)<sh)accept+=left(x);
                --c[x];
            }
            const bool valued=k>=White||k==SeatWind(who)||k==RoundWind();
            double value=-sh*1000.0+accept*10.0-doras[k]*25.0;
            if(Honor(k)&&all[k]==1&&!valued)value+=4;   // lone honors go first
            if(valued&&all[k]>=2)value-=15;               // a pair of a valued tile is a yaku in waiting
            options.push_back({k,sh,accept,value});
        }
        std::sort(options.begin(),options.end(),[](const Option& a,const Option& b){return a.value>b.value;});
        Option pick=options.front();
        const Seat& other=seats[Other(who)];
        if(other.riichi&&(now>=2||(now==1&&Roll(0.5)))&&Roll(carefully))
        {
            // Too far from waiting against a riichi: the safest tile, the better hand among equals.
            double best=-1e18;
            for(const Option& o:options)
            {
                const double score=Safety(who,o.kind)*100000+o.value;
                if(score>best){best=score;pick=o;}
            }
        }
        else if(options.size()>1&&Roll((1-carefully)*0.6))
        {
            // A looser discard now and then: one of the next best.
            const size_t top=std::min<size_t>(3,options.size());
            pick=options[std::uniform_int_distribution<size_t>(0,top-1)(rng)];
        }

        // Waiting after it: riichi (closed, as long as some of the waits are left).
        if(pick.shanten==0&&!me.riichi&&me.points>=1000&&wall>=1&&
           std::all_of(me.melds.begin(),me.melds.end(),[](const Meld& m){return m.kind==MeldKind::ClosedKan;}))
        {
            Counts c=all;
            --c[pick.kind];
            int waiting=0;
            for(int w:Mahjong::Waits(c))waiting+=left(w);
            riichi=waiting>0;
        }
        // The tile itself: the drawn one when it is of the kind, else one in the hand (a red
        // five kept while a plain one of its kind is there).
        if(me.hasDrawn&&me.drawn.kind==pick.kind)return n;
        int index=-1;
        for(int i=0;i<n;++i)
            if(me.hand[i].kind==pick.kind&&(index<0||(me.hand[index].red&&!me.hand[i].red)))index=i;
        return index;
    }

    bool Table::WantsPon(int who,double carefully)
    {
        const Seat& me=seats[who];
        if(me.riichi||wall<1)return false;
        const int kind=seats[Other(who)].river.back().piece.kind;
        Counts c=CountsOf(who,false);
        if(c[kind]<2)return false;
        auto valued=[&](int k){return k>=White||k==SeatWind(who)||k==RoundWind();};
        // Only toward a yaku: a valued tile, or once one is called; or all simples.
        bool yaku=valued(kind);
        for(const Meld& m:me.melds)yaku|=valued(m.tile);
        bool simples=Simple(kind);
        for(int k=0;k<Kinds;++k)if(c[k]&&!Simple(k))simples=false;
        for(const Meld& m:me.melds)if(!Simple(m.tile))simples=false;
        if(!yaku&&!simples)return false;
        const int melds=static_cast<int>(me.melds.size());
        const int before=Shanten(c,melds);
        c[kind]-=2;
        int after=99;
        for(int k=0;k<Kinds;++k)
        {
            if(!c[k])continue;
            --c[k];
            after=std::min(after,Shanten(c,melds+1));
            ++c[k];
        }
        if(valued(kind))return after<=before&&Roll(0.5+0.5*carefully);
        return after<before&&Roll(0.6);
    }

    void Table::Win(int who,bool tsumo,const Counts& concealed,const std::vector<Meld>& melds,int win,Situation s,std::vector<Piece> shown)
    {
        ended=RoundEnd{};
        ended.winner=who;
        ended.tsumo=tsumo;
        if(seats[who].riichi)
            for(size_t i=0;i<dora.size();++i)ended.ura.push_back(DrawIndicator());
        s.ura=ended.ura;
        ended.score=Evaluate(concealed,melds,win,s);
        ended.hand=std::move(shown);
        ended.melds=melds;
        const int paid=ended.score.points;
        ended.change[who]=paid+sticks*1000;
        ended.change[Other(who)]=-paid;
        sticks=0;
        if(who==Her)HerSays(tsumo?L"ツモ":L"ロン",tsumo?L"Tsumo":L"Ron");
        Settle();
    }

    void Table::WallEmpty()
    {
        ended=RoundEnd{};
        ended.draw=true;
        for(int who=0;who<2;++who)ended.tenpai[who]=!Mahjong::Waits(CountsOf(who,false)).empty();
        if(ended.tenpai[Her])ended.herHand=seats[Her].hand;
        if(ended.tenpai[You]!=ended.tenpai[Her])
        {
            const int waiting=ended.tenpai[You]?You:Her;
            ended.change[waiting]=1000;
            ended.change[Other(waiting)]=-1000;
        }
        Settle();
    }

    void Table::Settle()
    {
        for(int who=0;who<2;++who)seats[who].points+=ended.change[who];
        dealerStays=ended.draw?ended.tenpai[dealer]:ended.winner==dealer;
        nextHonba=dealerStays||ended.draw?honba+1:0;
        gameOver=seats[You].points<=0||seats[Her].points<=0||(!dealerStays&&round==Hands);   // 0 points ends it too
        phase=Phase::RoundOver;
    }

    bool Table::Consistent() const
    {
        Counts seen=unseen.tiles;
        std::array<int,3> red=unseen.red;
        int meldRed=0;
        auto add=[&](const Piece& p){++seen[p.kind];if(p.red)++red[Suit(p.kind)];};
        for(const Seat& s:seats)
        {
            for(const Piece& p:s.hand)add(p);
            if(s.hasDrawn)add(s.drawn);
            for(const RiverTile& r:s.river)if(!r.taken)add(r.piece);
            for(const Meld& m:s.melds)seen[m.tile]+=m.kind==MeldKind::Pon?3:4;
            meldRed+=s.meldRed;
        }
        for(int s=0;s<3;++s)red[s]+=indicatorRed[s];
        for(int d:dora)++seen[d];
        for(int d:ended.ura)++seen[d];
        // A ron tile is in the winner's hand only (taken from the river).
        if((phase==Phase::RoundOver||phase==Phase::GameOver)&&!ended.draw&&!ended.tsumo&&!ended.hand.empty())add(ended.hand.back());
        for(int k=0;k<Kinds;++k)if(seen[k]!=4)return false;
        for(int s=0;s<3;++s)if(red[s]>1)return false;
        return red[0]+red[1]+red[2]+meldRed==3;
    }

    std::wstring CheckPlaying(int count,std::uint32_t seed)
    {
        std::mt19937 rng(seed);
        auto any=[&](int n){return std::uniform_int_distribution<int>(0,n-1)(rng);};
        std::wstring report,problems;
        int wrong=0;
        // First against a player of yours who plays as she does at her best, then a simple one
        // (who throws the loneliest tile and wins when it can).
        for(int simple=0;simple<2;++simple)
        {
            int hands=0,yourWins=0,herWins=0,draws=0,herRiichi=0,herPon=0,gamesYouWon=0,stuck=0,yourDealIn=0,herDealIn=0;
            int herYakuman=0,yourYakuman=0;
            std::map<std::wstring,int> yakumanKinds;
            const auto started=std::chrono::steady_clock::now();
            for(int game=0;game<count;++game)
            {
                Table t;
                t.NewGame(seed*7919u+static_cast<std::uint32_t>(game+simple*100000),0.85);
                int steps=0;
                bool riichiCounted=false;
                while(t.Now()!=Phase::GameOver&&steps<20000)
                {
                    ++steps;
                    const int calls=t.HerCalls();
                    const Seat& you=t.Of(You);
                    switch(t.Now())
                    {
                    case Phase::YourTurn:
                    case Phase::YourCall:
                        if(!simple){t.PlayForYou();break;}
                        if(t.Now()==Phase::YourCall){if(t.CanRon())t.Ron();else t.Pass();break;}
                        if(t.CanTsumo()){t.Tsumo();break;}
                        {
                            Counts c{};
                            for(const Piece& p:you.hand)++c[p.kind];
                            if(you.hasDrawn)++c[you.drawn.kind];
                            const int n=static_cast<int>(you.hand.size())+(you.hasDrawn?1:0);
                            int best=0,bestScore=1<<20;
                            for(int i=0;i<n;++i)
                            {
                                const int k=i<static_cast<int>(you.hand.size())?you.hand[i].kind:you.drawn.kind;
                                int score=c[k]*4;
                                if(!Honor(k))for(int d=-2;d<=2;++d){const int m=k+d;if(d&&m>=0&&m<East&&Suit(m)==Suit(k))score+=c[m]*(std::abs(d)==1?2:1);}
                                score=score*8+any(8);
                                if(score<bestScore){bestScore=score;best=i;}
                            }
                            const std::vector<int> riichi=t.RiichiDiscards();
                            if(you.riichi)t.Discard(static_cast<int>(you.hand.size()));
                            else if(!riichi.empty())t.Discard(riichi[0],true);
                            else t.Discard(best);
                        }
                        break;
                    case Phase::HerTurn:
                        t.Step();
                        if(t.Of(Her).riichi&&!riichiCounted){++herRiichi;riichiCounted=true;}
                        break;
                    case Phase::RoundOver:
                    {
                        const RoundEnd& e=t.Ended();
                        ++hands;
                        if(e.draw)++draws;
                        else if(e.winner==You){++yourWins;if(!e.tsumo)++herDealIn;}
                        else {++herWins;if(!e.tsumo)++yourDealIn;}
                        if(!e.draw&&!e.score.win)problems+=L"a win without a yaku\n";
                        if(!e.draw&&e.score.yakuman)
                        {
                            ++(e.winner==You?yourYakuman:herYakuman);
                            for(const auto& y:e.score.yaku)++yakumanKinds[std::wstring(e.winner==You?L"you: ":L"her: ")+y.en];
                        }
                        t.NextRound();
                        riichiCounted=false;
                        break;
                    }
                    default:break;
                    }
                    if(t.HerCalls()!=calls&&std::wstring(t.HerCallEn())==L"Pon")++herPon;
                    if(!t.Consistent()){++wrong;problems+=L"tiles not accounted for (game "+std::to_wstring(game)+L")\n";break;}
                    if(t.Of(You).points+t.Of(Her).points+t.Sticks()*1000!=50000){++wrong;problems+=L"points do not add up (game "+std::to_wstring(game)+L")\n";break;}
                }
                if(steps>=20000){++stuck;++wrong;}
                if(t.Of(You).points>t.Of(Her).points)++gamesYouWon;
            }
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
            auto percent=[&](int n){return std::to_wstring(hands?n*100/hands:0)+L"%";};
            report+=std::wstring(simple?L"against a simple player":L"against one who plays as she does")+L": "+std::to_wstring(count)+L" games, "+
                    std::to_wstring(hands)+L" hands, "+std::to_wstring(static_cast<int>(ms))+L" ms\n";
            report+=L"  games you won "+std::to_wstring(gamesYouWon)+L"; hands won: you "+percent(yourWins)+L", her "+percent(herWins)+L", drawn "+percent(draws)+L"\n";
            report+=L"  dealt in: you "+percent(yourDealIn)+L", her "+percent(herDealIn)+L"; her riichi "+percent(herRiichi)+L", her pons "+std::to_wstring(herPon)+
                    L"; stuck "+std::to_wstring(stuck)+L"\n";
            report+=L"  yakuman: you "+std::to_wstring(yourYakuman)+L", her "+std::to_wstring(herYakuman)+L" (of "+std::to_wstring(hands)+L" hands)";
            for(const auto& [name,n]:yakumanKinds)report+=L"; "+name+L" "+std::to_wstring(n);
            report+=L"\n";
        }
        return std::to_wstring(wrong)+L" wrong\n"+report+problems;
    }
}
