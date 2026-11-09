#include "PageOne.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace MikuMikuDesktopPerch::PageOne
{
    Kind KindOf(int card)
    {
        if(IsJoker(card))return Kind::DrawFour;
        switch(RankOf(card))
        {
        case 2:return Kind::DrawTwo;
        case 8:return Kind::Wild;
        case 11:return Kind::Skip;
        case 1:return Kind::Reverse;
        default:return Kind::Plain;
        }
    }

    int Points(int card)
    {
        if(IsJoker(card))return 50;
        const int r=RankOf(card);
        if(r==8)return 50;
        if(r==2||r==1||r==11)return 20;
        if(r>=12)return 10;
        return r;
    }

    void Table::NewGame(std::uint32_t seed,double howCarefully)
    {
        rng.seed(seed);
        think.seed(seed^0x5bd1e995u);
        care=howCarefully;
        scores={};
        handNumber=0;
        dealer=(rng()&1)?You:Her;   // NextHand passes the deal, so this is drawn
        moves=0;
        stacked.clear();
        hands[0].clear();hands[1].clear();
        pile.clear();
        stock.clear();
        for(int c=0;c<52;++c)stock.push_back(c);
        stock.push_back(Joker);
        phase=Phase::HandOver;
        NextHand();
    }

    void Table::NextHand()
    {
        if(phase!=Phase::HandOver)return;
        for(auto& h:hands){stock.insert(stock.end(),h.begin(),h.end());h.clear();}
        stock.insert(stock.end(),pile.begin(),pile.end());
        pile.clear();
        did={};
        lastPlayed={-1,-1};
        called={};forgot={};
        lacks={};
        outcome={};
        justDrawn=-1;
        plannedAt=catchPlannedAt=-1;
        turns=0;
        penalty=0;
        jokerPending=false;
        refills=0;
        ++moves;
        if(scores[0]>=Target||scores[1]>=Target){phase=Phase::GameOver;return;}
        ++handNumber;
        dealer=Other(dealer);
        phase=Phase::Dealing;
    }

    void Table::StackDeck(const std::vector<int>& next){stacked=next;}

    void Table::Deal()
    {
        std::shuffle(stock.begin(),stock.end(),rng);
        for(size_t i=0;i<stacked.size();++i)
        {
            auto at=std::find(stock.begin(),stock.end(),stacked[i]);
            if(at!=stock.end()){stock.erase(at);stock.insert(stock.begin()+static_cast<std::ptrdiff_t>(i),stacked[i]);}
        }
        stacked.clear();
        std::reverse(stock.begin(),stock.end());   // dealt from the back
        const int first=Other(dealer);
        for(int i=0;i<HandSize*2;++i)
        {
            hands[i%2==0?first:dealer].push_back(stock.back());
            stock.pop_back();
        }
        // The first card turned up is a plain one (the others go to the bottom).
        for(int guard=0;guard<60;++guard)
        {
            const int c=stock.back();
            stock.pop_back();
            if(KindOf(c)==Kind::Plain||guard==59){pile.push_back(c);break;}
            stock.insert(stock.begin(),c);
        }
        suit=std::max(0,SuitOf(Top()));
        toAct=first;
        phase=TurnOf(first);
    }

    void Table::Arrange(const std::vector<int>& you,const std::vector<int>& her,const std::vector<int>& on,int who,
                        const std::vector<int>& next,bool restToPile)
    {
        hands[You]=you;
        hands[Her]=her;
        std::vector<bool> used(Cards,false);
        for(int c:you)used[static_cast<size_t>(c)]=true;
        for(int c:her)used[static_cast<size_t>(c)]=true;
        for(int c:on)used[static_cast<size_t>(c)]=true;
        for(int c:next)used[static_cast<size_t>(c)]=true;
        std::vector<int> rest;
        for(int c=0;c<Cards;++c)if(c!=Back&&!used[static_cast<size_t>(c)])rest.push_back(c);
        pile.clear();
        stock.clear();
        if(restToPile)pile=rest;
        else stock=rest;
        pile.insert(pile.end(),on.begin(),on.end());
        for(auto c=next.rbegin();c!=next.rend();++c)stock.push_back(*c);   // drawn from the back
        did={};
        lastPlayed={-1,-1};
        called={};forgot={};
        lacks={};
        outcome={};
        justDrawn=-1;
        plannedAt=catchPlannedAt=-1;
        penalty=0;
        jokerPending=false;
        refills=0;
        turns=0;
        handNumber=std::max(handNumber,1);
        suit=std::max(0,SuitOf(Top()));
        toAct=who;
        phase=TurnOf(who);
        ++moves;
    }

    int Table::TakeFromStock()
    {
        if(stock.empty()&&pile.size()>1)
        {
            // The pile but its top, shuffled, is the stock again.
            const int top=pile.back();
            pile.pop_back();
            stock=std::move(pile);
            pile.clear();
            pile.push_back(top);
            std::shuffle(stock.begin(),stock.end(),rng);
            ++refills;
        }
        if(stock.empty())return -1;
        const int c=stock.back();
        stock.pop_back();
        return c;
    }

    bool Table::CanPlay(int card) const
    {
        if(!Playing())return false;
        const auto& hand=hands[toAct];
        if(std::find(hand.begin(),hand.end(),card)==hand.end())return false;
        if(Drawn()&&card!=justDrawn)return false;
        if(penalty>0)return (KindOf(card)==Kind::DrawTwo&&!jokerPending)||IsJoker(card);
        if(IsWild(card))return true;
        const int top=Top();
        return SuitOf(card)==suit||(top>=0&&!IsJoker(top)&&RankOf(card)==RankOf(top));
    }

    bool Table::CanPlayAny() const
    {
        if(!Playing())return false;
        for(int c:hands[toAct])if(CanPlay(c))return true;
        return false;
    }

    bool Table::CanDraw() const {return phase==Phase::YourTurn||phase==Phase::HerTurn;}
    bool Table::CanKeep() const {return Drawn();}

    bool Table::CanCatch(int who) const
    {
        const int o=Other(who);
        return (phase==Phase::YourTurn||phase==Phase::HerTurn)&&toAct==who&&forgot[o]&&hands[o].size()==1;
    }

    void Table::PassTurn()
    {
        toAct=Other(toAct);
        justDrawn=-1;
        phase=TurnOf(toAct);
        if(turns>=TurnLimit)Settle(-1);
    }

    void Table::Play(int card,int named,bool call)
    {
        if(!CanPlay(card))return;
        const int who=toAct,o=Other(who);
        auto& hand=hands[who];
        hand.erase(std::find(hand.begin(),hand.end(),card));
        pile.push_back(card);
        ++moves;
        ++turns;
        did[who]=Did::Played;
        lastPlayed[who]=card;
        justDrawn=-1;
        forgot[o]=false;   // the chance to point it out has gone
        if(!IsJoker(card))lacks[who][static_cast<size_t>(SuitOf(card))]=false;
        suit=IsWild(card)?(named>=0&&named<4?named:BestSuit(who,-1)):SuitOf(card);
        const Kind kind=KindOf(card);
        if(kind==Kind::DrawTwo)penalty+=2;
        else if(kind==Kind::DrawFour){penalty+=4;jokerPending=true;}
        if(hand.size()==1){called[who]=call;forgot[who]=!call;}
        else called[who]=forgot[who]=false;
        if(hand.empty()){Settle(who);return;}
        if(kind==Kind::Skip||kind==Kind::Reverse)
        {
            // With two, the other is passed over: the same one plays again.
            justDrawn=-1;
            phase=TurnOf(who);
            if(turns>=TurnLimit)Settle(-1);
            return;
        }
        PassTurn();
    }

    void Table::Draw()
    {
        if(!CanDraw())return;
        const int who=toAct,o=Other(who);
        forgot[o]=false;
        called[who]=forgot[who]=false;
        ++moves;
        ++turns;
        if(penalty>0)
        {
            for(int i=0;i<penalty;++i)if(const int c=TakeFromStock();c>=0)hands[who].push_back(c);
            penalty=0;
            jokerPending=false;
            did[who]=Did::Penalty;
            PassTurn();
            return;
        }
        lacks[who][static_cast<size_t>(suit)]=true;
        const int c=TakeFromStock();
        if(c<0){did[who]=Did::Kept;PassTurn();return;}
        hands[who].push_back(c);
        justDrawn=c;
        did[who]=Did::Drew;
        phase=who==You?Phase::YourDrawn:Phase::HerDrawn;
    }

    void Table::Keep()
    {
        if(!CanKeep())return;
        did[toAct]=Did::Kept;
        ++moves;
        PassTurn();
    }

    void Table::Catch(int who)
    {
        if(!CanCatch(who))return;
        const int o=Other(who);
        for(int i=0;i<CaughtDraw;++i)if(const int c=TakeFromStock();c>=0)hands[o].push_back(c);
        forgot[o]=called[o]=false;
        did[who]=Did::Caught;
        ++moves;
    }

    void Table::Settle(int winner)
    {
        outcome={};
        outcome.winner=winner;
        if(winner>=0)
        {
            const int o=Other(winner);
            outcome.lastCard=lastPlayed[winner];
            // Going out with a 2 or the joker: the other still draws for it.
            for(int i=0;i<penalty;++i)if(const int c=TakeFromStock();c>=0){hands[o].push_back(c);++outcome.drawnAtEnd;}
            for(int c:hands[o])outcome.points+=Points(c);
            scores[winner]+=outcome.points;
        }
        penalty=0;
        jokerPending=false;
        for(int w=0;w<2;++w)for(int c:hands[w])outcome.left[w]+=Points(c);
        justDrawn=-1;
        phase=Phase::HandOver;
    }

    void Table::Step()
    {
        switch(phase)
        {
        case Phase::Dealing:
            Deal();
            ++moves;
            break;
        case Phase::HerTurn:
        {
            if(HerCatches()){Catch(Her);break;}
            int named=-1;
            bool call=true;
            const int card=HerPlay(named,call);
            if(card>=0)Play(card,named,call);
            else Draw();
            break;
        }
        case Phase::HerDrawn:
        {
            int named=-1;
            bool call=true;
            const int card=HerPlay(named,call);
            if(card>=0)Play(card,named,call);
            else Keep();
            break;
        }
        default:
            break;
        }
    }

    bool Table::HerCatches()
    {
        if(catchPlannedAt!=moves)
        {
            catchPlannedAt=moves;
            std::uniform_real_distribution<double> any(0,1);
            plannedCatch=CanCatch(Her)&&any(think)<0.55+0.45*CareNow();
        }
        return plannedCatch&&CanCatch(Her);
    }

    int Table::HerPlay(int& named,bool& call)
    {
        if(plannedAt!=moves)
        {
            plannedAt=moves;
            plannedCard=ChoosePlay(Her,CareNow(),plannedSuit);
            std::uniform_real_distribution<double> any(0,1);
            plannedCall=any(think)>=(1-CareNow())*0.5;   // now and then, with less care, she forgets
        }
        named=plannedSuit;
        call=plannedCall;
        return plannedCard;
    }

    // Eased a little further while she is well ahead.
    double Table::CareNow() const
    {
        const int lead=scores[Her]-scores[You];
        return care-(lead>=Target*6/10?0.3:lead>=Target*3/10?0.15:0.0);
    }

    // The suit to name with a wild: the one most held (wilds and except left out), better still
    // one the other seems not to hold.
    int Table::BestSuit(int who,int except) const
    {
        std::array<double,4> worth{};
        for(int c:hands[who])
            if(c!=except&&!IsWild(c))worth[static_cast<size_t>(SuitOf(c))]+=3+Points(c)*0.02;
        for(int s=0;s<4;++s)if(lacks[Other(who)][static_cast<size_t>(s)])worth[static_cast<size_t>(s)]+=4;
        return static_cast<int>(std::max_element(worth.begin(),worth.end())-worth.begin());
    }

    // The card to play (-1: draw, or keep the card drawn), weighing what it sheds, what it does
    // to the other, and what it leaves to follow; named: the suit with a wild.
    int Table::ChoosePlay(int who,double carefully,int& named)
    {
        named=-1;
        const auto& hand=hands[who];
        const int o=Other(who);
        const int mine=static_cast<int>(hand.size()),theirs=static_cast<int>(hands[o].size());
        std::vector<std::pair<double,int>> scored;
        for(int c:hand)
        {
            if(!CanPlay(c))continue;
            double s=0;
            if(mine==1)s=1000;   // going out
            else
            {
                s=Points(c)*(theirs<=2?0.5:0.12);
                switch(KindOf(c))
                {
                case Kind::Wild:s-=(theirs<=2||mine<=2)?0:14;break;
                case Kind::DrawFour:s+=penalty>0?20:theirs<=2?18:-10;break;
                case Kind::DrawTwo:s+=penalty>0?20:4+(theirs<=3?8:0);break;
                case Kind::Skip:
                case Kind::Reverse:
                {
                    // Another turn is worth most with a card to follow it.
                    bool follow=false;
                    for(int d:hand)
                        if(d!=c&&(IsWild(d)||SuitOf(d)==SuitOf(c)||RankOf(d)==RankOf(c)))follow=true;
                    s+=follow?7:1;
                    break;
                }
                default:break;
                }
                const int next=IsWild(c)?BestSuit(who,c):SuitOf(c);
                for(int d:hand)
                {
                    if(d==c||IsWild(d))continue;
                    if(SuitOf(d)==next)s+=2.5;
                    else if(!IsWild(c)&&RankOf(d)==RankOf(c))s+=1;
                }
                if(lacks[o][static_cast<size_t>(next)])s+=5;
                // The card just drawn: a wild is kept for later while nobody is close.
                if(Drawn()&&IsWild(c)&&mine>2&&theirs>2)s=-1;
            }
            scored.push_back({s,c});
        }
        if(scored.empty())return -1;
        std::sort(scored.begin(),scored.end(),[](const auto& a,const auto& b){return a.first>b.first;});
        if(scored[0].first<0)return -1;
        size_t pick=0;
        std::uniform_real_distribution<double> any(0,1);
        if(scored.size()>1&&scored[0].first<1000&&scored[1].first>=0&&any(think)<(1-carefully)*0.6)pick=1;
        const int card=scored[pick].second;
        if(IsWild(card))named=BestSuit(who,card);
        return card;
    }

    void Table::PlayForYou(int style)
    {
        if(phase==Phase::HandOver){NextHand();return;}
        if(phase==Phase::Dealing||phase==Phase::HerTurn||phase==Phase::HerDrawn){Step();return;}
        if(!Playing())return;
        std::uniform_real_distribution<double> any(0,1);
        const bool drawn=Drawn();
        if(!drawn&&CanCatch(You)&&(style<2||any(think)<0.5)){Catch(You);return;}
        int card=-1,named=-1;
        bool call=true;
        if(style==0)card=ChoosePlay(You,1,named);
        else if(style==1)
        {
            int best=-1;
            for(int c:hands[You])if(CanPlay(c)&&Points(c)>best){best=Points(c);card=c;}
            if(card>=0&&IsWild(card))named=BestSuit(You,card);
        }
        else
        {
            std::vector<int> can;
            for(int c:hands[You])if(CanPlay(c))can.push_back(c);
            if(!can.empty()&&any(think)<0.8)card=can[think()%can.size()];
            named=static_cast<int>(think()%4);
            call=any(think)<0.7;
        }
        if(card>=0)Play(card,named,call);
        else if(drawn)Keep();
        else Draw();
    }

    bool Table::Consistent() const
    {
        std::vector<int> all(stock);
        all.insert(all.end(),pile.begin(),pile.end());
        for(const auto& h:hands)all.insert(all.end(),h.begin(),h.end());
        if(all.size()!=53)return false;
        std::sort(all.begin(),all.end());
        for(int i=0;i<52;++i)if(all[static_cast<size_t>(i)]!=i)return false;
        return all[52]==Joker;
    }

    std::wstring CheckRules()
    {
        std::wostringstream lines;
        int failed=0;
        auto check=[&](bool ok,const wchar_t* what){if(!ok)++failed;lines<<what<<L": "<<(ok?L"ok":L"WRONG")<<L"\n";};
        auto C=[](int suit,int rank){return suit*13+rank-1;};
        constexpr int S=0,H=1,D=2,K=3;   // spades, hearts, diamonds, clubs
        Table t;
        t.NewGame(1);
        check(t.Now()==Phase::Dealing,L"a new game starts with the deal");
        t.Step();
        check(t.HandOf(You).size()==HandSize&&t.HandOf(Her).size()==HandSize&&t.Pile().size()==1&&KindOf(t.Top())==Kind::Plain&&t.Consistent(),
              L"seven each, a plain card turned up, every card accounted for");
        check(t.ToAct()!=t.Dealer(),L"the one not dealing plays first");
        // Following by suit or rank; wilds at any time.
        t.Arrange({C(S,5),C(H,5),C(H,3),C(K,8),Joker,C(H,7)},{C(D,9),C(D,10)},{C(S,7)},You);
        check(t.CanPlay(C(S,5))&&!t.CanPlay(C(H,5))&&!t.CanPlay(C(H,3)),L"the suit follows, another suit of another rank does not");
        check(t.CanPlay(C(H,7)),L"the same rank in another suit follows");
        check(t.CanPlay(C(K,8))&&t.CanPlay(Joker),L"an 8 and the joker go on anything");
        check(!t.CanPlay(C(D,9)),L"only the one to play plays");
        t.Play(C(K,8),H);
        check(t.Suit()==H&&t.Now()==Phase::HerTurn,L"an 8 names the suit, and the turn passes");
        // Draw two, answered.
        t.Arrange({C(S,2),C(D,2),C(K,4),C(K,5),Joker},{C(H,2),C(K,9),C(K,10)},{C(S,7)},You,{C(H,4),C(H,5),C(H,6),C(H,9),C(H,10),C(H,11),C(H,12),C(H,13)});
        t.Play(C(S,2));
        check(t.Penalty()==2&&t.Now()==Phase::HerTurn,L"a 2: the other is to draw two");
        check(!t.CanPlay(C(K,9))&&t.CanPlay(C(H,2)),L"only a 2 (or the joker) answers it");
        t.Play(C(H,2));
        check(t.Penalty()==4&&t.Now()==Phase::YourTurn&&t.CanPlay(C(D,2))&&t.CanPlay(Joker),L"answered with a 2: four for the next");
        t.Draw();
        check(t.HandOf(You).size()==8&&t.Penalty()==0&&t.Now()==Phase::HerTurn&&t.Suit()==H,L"drawing them ends the turn");
        // The joker on a draw two.
        t.Arrange({Joker,C(K,4),C(K,5)},{C(H,2),C(K,9),C(K,10),C(D,2)},{C(S,2)},You);
        // (a 2 already lying there asks nothing: a penalty is only made by playing one)
        check(t.Penalty()==0,L"a 2 turned up asks nothing");
        t.Play(Joker,K);
        check(t.Penalty()==4&&t.Suit()==K&&!t.CanPlay(C(D,2))&&!t.CanPlay(C(K,9)),L"the joker: four to draw, a suit named, no 2 answers it");
        t.Draw();
        check(t.HandOf(Her).size()==8&&t.Now()==Phase::YourTurn,L"she draws four");
        // Skips: the same one plays again.
        t.Arrange({C(S,11),C(S,1),C(S,4),C(K,4)},{C(H,9)},{C(S,7)},You);
        t.Play(C(S,11));
        check(t.Now()==Phase::YourTurn,L"a J: you play again");
        t.Play(C(S,1));
        check(t.Now()==Phase::YourTurn,L"an ace (the order turned back, with two): you play again");
        t.Play(C(S,4));
        check(t.Now()==Phase::HerTurn,L"then the turn passes");
        // Drawing one: it may be played, or kept.
        t.Arrange({C(H,3),C(D,4)},{C(K,9),C(K,10)},{C(S,7)},You,{C(S,9),C(H,9)});
        check(!t.CanPlayAny()&&t.CanDraw(),L"nothing to play: draw");
        t.Draw();
        check(t.Now()==Phase::YourDrawn&&t.JustDrawn()==C(S,9)&&t.CanPlay(C(S,9))&&t.CanKeep(),L"the card drawn may be played");
        t.Keep();
        check(t.Now()==Phase::HerTurn&&t.HandOf(You).size()==3,L"or kept, and the turn passes");
        // "Page one".
        t.Arrange({C(S,5),C(S,6)},{C(K,9),C(K,10),C(D,3)},{C(S,7)},You,{C(H,4),C(H,5)});
        t.Play(C(S,5),-1,false);
        check(t.CanCatch(Her)&&!t.Called(You),L"down to one without saying it: she may point it out");
        t.Catch(Her);
        check(t.HandOf(You).size()==3&&!t.CanCatch(Her)&&t.Now()==Phase::HerTurn,L"caught: two drawn, and she still plays");
        t.Arrange({C(S,5),C(S,6)},{C(K,9),C(K,10),C(D,3)},{C(S,7)},You);
        t.Play(C(S,5),-1,true);
        check(t.Called(You)&&!t.CanCatch(Her),L"said: nothing to point out");
        t.Arrange({C(S,5),C(S,6)},{C(D,9),C(K,10),C(D,3)},{C(S,7)},You);
        t.Play(C(S,5),-1,false);
        t.Draw();   // she does not point it out: she draws instead
        check(!t.CanCatch(Her)&&t.Now()==Phase::HerDrawn,L"the chance goes once she plays or draws");
        // Going out.
        t.Arrange({C(S,5)},{C(H,13),C(K,8),Joker,C(D,3)},{C(S,7)},You);
        t.Play(C(S,5));
        check(t.Now()==Phase::HandOver&&t.Result().winner==You&&t.Result().points==113&&t.Score(You)==113,L"out: her cards' points (10 + 50 + 50 + 3)");
        t.Arrange({C(S,2)},{C(D,3)},{C(S,7)},Her,{C(H,4),C(H,5),C(H,6)});
        check(t.Now()==Phase::HerTurn,L"she to play");
        t.Draw();
        t.Keep();
        t.Play(C(S,2));
        check(t.Now()==Phase::HandOver&&t.Result().drawnAtEnd==2&&t.Result().points==3+4+5+6,L"out with a 2: she still draws two, and they count");
        // The stock refilled from the pile.
        t.Arrange({C(H,3)},{C(K,9)},{C(S,7)},You,{},true);
        const size_t pileBefore=t.Pile().size();
        t.Draw();
        check(t.Refills()==1&&t.Pile().size()==1&&t.Top()==C(S,7)&&t.Stock().size()==pileBefore-2&&t.Consistent(),L"an empty stock: the pile but its top is shuffled into it");
        // The deal passes, and the game ends at the target.
        {
            Table g;
            g.NewGame(3);
            const int first=g.Dealer();
            g.Step();
            g.Arrange({C(S,5)},{C(H,13),C(K,8),Joker,C(D,3),C(D,13),C(H,12),C(K,12)},{C(S,7)},You);
            g.Play(C(S,5));
            check(g.Score(You)==143,L"143 points");
            g.NextHand();
            check(g.Now()==Phase::Dealing&&g.Dealer()!=first&&g.HandNumber()==2,L"the deal passes");
            g.Step();
            g.Arrange({C(S,5)},{C(D,9),C(D,10)},{C(S,7)},You);
            g.Play(C(S,5));
            g.NextHand();
            check(g.Now()==Phase::GameOver&&g.Score(You)>=Target,L"the target reached: the game is over");
        }
        std::wostringstream out;
        out<<failed<<L" failed\n"<<lines.str();
        return out.str();
    }

    std::wstring CheckPlaying(int count,std::uint32_t seed)
    {
        std::wostringstream out,table;
        int wrong=0;
        const wchar_t* names[]={L"herself",L"greedy",L"anyhow"};
        for(int style=0;style<3;++style)
            for(double care:{1.0,0.6})
            {
                int herWins=0,yourWins=0,hands=0,voids=0,turnsAll=0,refills=0,caught=0;
                long long gain=0;
                for(int g=0;g<count;++g)
                {
                    Table t;
                    t.NewGame(seed+g*7919u,care);
                    int caughtBefore=0;
                    for(int step=0;step<200000&&t.Now()!=Phase::GameOver;++step)
                    {
                        if(t.Now()==Phase::HandOver)
                        {
                            ++hands;
                            turnsAll+=t.Turns();
                            refills+=t.Refills();
                            if(t.Result().winner<0)++voids;
                        }
                        t.PlayForYou(style);
                        if(t.LastDid(Her)==Did::Caught||t.LastDid(You)==Did::Caught)
                        {
                            if(!caughtBefore)++caught;
                            caughtBefore=1;
                        }
                        else caughtBefore=0;
                        if(!t.Consistent()){++wrong;break;}
                    }
                    if(t.Now()!=Phase::GameOver){++wrong;continue;}
                    gain+=t.Score(Her)-t.Score(You);
                    if(t.Score(Her)>t.Score(You))++herWins;else ++yourWins;
                }
                table<<L"against "<<names[style]<<L", care "<<care<<L": she won "<<herWins<<L", lost "<<yourWins<<L"; hands "<<hands<<L" ("<<voids<<L" void), "
                     <<(hands?turnsAll/hands:0)<<L" turns a hand, "<<refills<<L" refills, "<<caught<<L" caught; her points "<<(gain>=0?L"+":L"")<<gain<<L" over all\n";
            }
        out<<wrong<<L" wrong\n"<<table.str();
        return out.str();
    }
}
