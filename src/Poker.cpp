#include "Poker.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace MikuMikuDesktopPerch::Poker
{
    namespace
    {
        // Ranks here run 2 to 14, the ace high.
        int High(int card){const int r=RankOf(card);return r==1?14:r;}

        // The top card of the best straight in these ranks (bit r for rank r), 0 when none.
        int StraightIn(unsigned mask)
        {
            if(mask&(1u<<14))mask|=1u<<1;   // the ace also runs low
            for(int top=14;top>=5;--top)if(((mask>>(top-4))&0x1Fu)==0x1Fu)return top;
            return 0;
        }

        int Bits(unsigned v)
        {
            int n=0;
            for(;v;v&=v-1)++n;
            return n;
        }

        int Packed(int kind,int a=0,int b=0,int c=0,int d=0,int e=0)
        {
            return kind<<20|a<<16|b<<12|c<<8|d<<4|e;
        }

        // How good two cards are before the flop, 0-1 (the usual count: the higher card, a pair
        // doubled, suited and close together worth more).
        double StartingWorth(int a,int b)
        {
            int hi=High(a),lo=High(b);
            if(hi<lo)std::swap(hi,lo);
            double points=hi==14?10:hi==13?8:hi==12?7:hi==11?6:hi/2.0;
            if(hi==lo)points=std::max(5.0,points*2);
            else
            {
                if(SuitOf(a)==SuitOf(b))points+=2;
                const int gap=hi-lo-1;
                points-=gap==0?0:gap==1?1:gap==2?2:gap==3?4:5;
                if(gap<=1&&hi<12)points+=1;
            }
            return std::clamp((points+1.5)/21.5,0.0,1.0);
        }

        // Whether these two cards do anything on this board (a pair or better with it, or four to
        // a flush or a straight).
        bool Connects(int a,int b,const int* shared,int count)
        {
            if(RankOf(a)==RankOf(b))return true;
            unsigned ranks=1u<<High(a)|1u<<High(b);
            int suits[4]{};
            ++suits[SuitOf(a)];++suits[SuitOf(b)];
            for(int i=0;i<count;++i)
            {
                if(RankOf(shared[i])==RankOf(a)||RankOf(shared[i])==RankOf(b))return true;
                ranks|=1u<<High(shared[i]);
                ++suits[SuitOf(shared[i])];
            }
            if(*std::max_element(suits,suits+4)>=4)return true;
            if(ranks&(1u<<14))ranks|=1u<<1;
            for(int low=1;low<=10;++low)if(Bits((ranks>>low)&0x1Fu)>=4)return true;
            return false;
        }
    }

    int Score(const int* cards,int count)
    {
        unsigned suited[4]{},all=0;
        int counts[15]{};
        for(int i=0;i<count;++i)
        {
            const int r=High(cards[i]);
            suited[SuitOf(cards[i])]|=1u<<r;
            all|=1u<<r;
            ++counts[r];
        }
        unsigned flush=0;
        for(unsigned s:suited)if(Bits(s)>=5)flush=s;
        if(flush)if(const int top=StraightIn(flush))return Packed(8,top);
        int four=0,three=0,second=0,pairs[3]{},pairCount=0;
        for(int r=14;r>=2;--r)
        {
            if(counts[r]==4&&!four)four=r;
            else if(counts[r]==3){if(!three)three=r;else if(!second)second=r;}
            else if(counts[r]==2&&pairCount<3)pairs[pairCount++]=r;
        }
        // The highest ranks left, leaving out those used.
        auto kicker=[&](unsigned& left){
            for(int r=14;r>=2;--r)if(left&(1u<<r)){left&=~(1u<<r);return r;}
            return 0;
        };
        if(four){unsigned left=all&~(1u<<four);return Packed(7,four,kicker(left));}
        if(three&&(second||pairCount))return Packed(6,three,std::max(second,pairs[0]));
        if(flush){unsigned left=flush;const int a=kicker(left),b=kicker(left),c=kicker(left),d=kicker(left),e=kicker(left);return Packed(5,a,b,c,d,e);}
        if(const int top=StraightIn(all))return Packed(4,top);
        if(three){unsigned left=all&~(1u<<three);const int a=kicker(left),b=kicker(left);return Packed(3,three,a,b);}
        if(pairCount>=2){unsigned left=all&~(1u<<pairs[0])&~(1u<<pairs[1]);return Packed(2,pairs[0],pairs[1],kicker(left));}
        if(pairCount==1){unsigned left=all&~(1u<<pairs[0]);const int a=kicker(left),b=kicker(left),c=kicker(left);return Packed(1,pairs[0],a,b,c);}
        unsigned left=all;
        const int a=kicker(left),b=kicker(left),c=kicker(left),d=kicker(left),e=kicker(left);
        return Packed(0,a,b,c,d,e);
    }

    std::vector<int> BestFive(const std::vector<int>& cards)
    {
        const int n=static_cast<int>(cards.size());
        std::vector<int> best;
        if(n<5)return cards;
        int top=-1;
        for(int a=0;a<n;++a)for(int b=a+1;b<n;++b)for(int c=b+1;c<n;++c)for(int d=c+1;d<n;++d)for(int e=d+1;e<n;++e)
        {
            const int five[5]={cards[a],cards[b],cards[c],cards[d],cards[e]};
            if(const int s=Score(five,5);s>top){top=s;best.assign(five,five+5);}
        }
        return best;
    }

    void Table::NewGame(std::uint32_t seed,double howCarefully)
    {
        rng.seed(seed);
        think.seed(seed^0x5bd1e995u);
        care=howCarefully;
        chips={StartChips,StartChips};
        hand=0;
        // Who holds the button first is drawn; NextHand passes it, so start from the other.
        button=(rng()&1)?You:Her;
        moves=0;
        holes[0].clear();holes[1].clear();
        board.clear();
        stacked.clear();
        deck.clear();
        for(int c=0;c<Cards;++c)deck.push_back(c);
        phase=Phase::HandOver;
        NextHand();
    }

    int Table::SmallBlind() const
    {
        static constexpr int Levels[]={10,20,30,50};
        const int level=std::clamp((std::max(hand,1)-1)/HandsPerLevel,0,3);
        return Levels[level];
    }

    void Table::NextHand()
    {
        if(phase!=Phase::HandOver)return;
        for(auto& h:holes){deck.insert(deck.end(),h.begin(),h.end());h.clear();}
        deck.insert(deck.end(),board.begin(),board.end());
        board.clear();
        pot=0;
        bets={};
        did={};
        didAmount={};
        raises={};
        earlyRaises={};
        aggressor[0]=aggressor[1]=false;
        shown=false;
        bluffing=false;
        byFold=false;
        winner=-1;
        wonPot=0;
        ordered=false;
        strengthFor=-1;
        ++moves;
        if(chips[0]<=0||chips[1]<=0||hand>=Hands){phase=Phase::GameOver;return;}
        ++hand;
        button=Other(button);
        startChips=chips[You];
        opened=false;
        phase=Phase::Dealing;
    }

    void Table::Put(int who,int amount)
    {
        amount=std::min(amount,chips[who]);
        chips[who]-=amount;
        bets[who]+=amount;
    }

    void Table::StackDeck(const std::vector<int>& next){stacked=next;}

    void Table::DealHand()
    {
        // A fresh shuffle every hand (the cards stacked for a check coming first).
        std::shuffle(deck.begin(),deck.end(),rng);
        for(size_t i=0;i<stacked.size();++i)
        {
            auto at=std::find(deck.begin(),deck.end(),stacked[i]);
            if(at!=deck.end()){deck.erase(at);deck.insert(deck.begin()+static_cast<std::ptrdiff_t>(i),stacked[i]);}
        }
        stacked.clear();
        std::reverse(deck.begin(),deck.end());   // dealt from the back
        auto draw=[&]{const int c=deck.back();deck.pop_back();return c;};
        const int big=Other(button);
        for(int i=0;i<2;++i)
        {
            holes[big].push_back(draw());
            holes[button].push_back(draw());
        }
        Put(button,SmallBlind());
        Put(big,BigBlind());
        did[button]=did[big]=Did::Blind;
        lastRaise=BigBlind();
        acted={};
        opened=true;
        // Short of the blinds: nothing to bet against, or only a call to make.
        toAct=button;
        if(chips[button]==0||(chips[big]==0&&bets[button]>=bets[big])){CloseStreet();return;}
        phase=toAct==You?Phase::YourTurn:Phase::HerTurn;
    }

    void Table::DealStreet()
    {
        auto draw=[&]{const int c=deck.back();deck.pop_back();board.push_back(c);};
        if(board.empty())for(int i=0;i<3;++i)draw();
        else draw();
        strengthFor=-1;
        if(chips[0]>0&&chips[1]>0)StartBetting(Other(button));
    }

    void Table::StartBetting(int first)
    {
        acted={};
        did={};
        didAmount={};
        lastRaise=BigBlind();
        toAct=first;
        phase=first==You?Phase::YourTurn:Phase::HerTurn;
    }

    int Table::ToCall(int who) const {return std::min(chips[who],std::max(0,bets[Other(who)]-bets[who]));}

    int Table::MaxRaiseTo(int who) const
    {
        // No more than the other can answer.
        return std::min(bets[who]+chips[who],bets[Other(who)]+chips[Other(who)]);
    }

    int Table::MinRaiseTo(int who) const
    {
        return std::min(std::max(bets[0],bets[1])+lastRaise,MaxRaiseTo(who));
    }

    bool Table::CanRaise(int who) const
    {
        return chips[Other(who)]>0&&MaxRaiseTo(who)>std::max(bets[0],bets[1]);
    }

    int Table::RaiseToShare(int who,double share) const
    {
        const int call=ToCall(who),base=std::max(bets[0],bets[1]);
        const int more=static_cast<int>(std::lround(share*(Pot()+call)/Unit))*Unit;
        return std::clamp(base+more,MinRaiseTo(who),MaxRaiseTo(who));
    }

    void Table::Fold()
    {
        if(phase!=Phase::YourTurn&&phase!=Phase::HerTurn)return;
        const int who=toAct;
        did[who]=Did::Fold;
        didAmount[who]=0;
        ordered=false;
        byFold=true;
        // What the other put in over this one's bet comes back.
        const int over=bets[Other(who)]-bets[who];
        if(over>0){chips[Other(who)]+=over;bets[Other(who)]-=over;}
        pot+=bets[0]+bets[1];
        bets={};
        Pay(Other(who));
        phase=Phase::HandOver;
        ++moves;
    }

    void Table::CheckCall()
    {
        if(phase!=Phase::YourTurn&&phase!=Phase::HerTurn)return;
        const int who=toAct,call=ToCall(who);
        did[who]=call>0?(call==chips[who]?Did::AllIn:Did::Call):Did::Check;
        didAmount[who]=call;
        Put(who,call);
        acted[who]=true;
        ordered=false;
        Next(who);
        ++moves;
    }

    void Table::RaiseTo(int total)
    {
        if((phase!=Phase::YourTurn&&phase!=Phase::HerTurn)||!CanRaise(toAct)){CheckCall();return;}
        const int who=toAct,top=std::max(bets[0],bets[1]);
        total=std::clamp(total,MinRaiseTo(who),MaxRaiseTo(who));
        if(total-top>=lastRaise)lastRaise=total-top;
        const int put=total-bets[who];
        did[who]=put==chips[who]?Did::AllIn:Did::Raise;
        didAmount[who]=put;
        Put(who,put);
        acted[who]=true;
        acted[Other(who)]=false;
        ++raises[who];
        if(board.empty())++earlyRaises[who];
        aggressor[who]=true;
        aggressor[Other(who)]=false;
        ordered=false;
        Next(who);
        ++moves;
    }

    void Table::Next(int who)
    {
        const int other=Other(who);
        const bool mustAct=chips[other]>0&&(bets[other]<bets[who]||(!acted[other]&&chips[who]>0));
        if(mustAct){toAct=other;phase=other==You?Phase::YourTurn:Phase::HerTurn;return;}
        CloseStreet();
    }

    void Table::CloseStreet()
    {
        // A bet the other could not match comes back.
        const int high=bets[0]>bets[1]?0:1,over=bets[high]-bets[Other(high)];
        if(over>0){bets[high]-=over;chips[high]+=over;}
        pot+=bets[0]+bets[1];
        bets={};
        ordered=false;
        if(chips[0]==0||chips[1]==0)shown=true;   // no more betting: both hands turned up
        if(board.size()==5){Showdown();return;}
        phase=Phase::Dealing;
    }

    void Table::Pay(int who)
    {
        winner=who;
        wonPot=pot;
        chips[who]+=pot;
        pot=0;
    }

    void Table::Showdown()
    {
        shown=true;
        std::array<int,2> score{};
        for(int who=0;who<2;++who)
        {
            std::vector<int> all(holes[who]);
            all.insert(all.end(),board.begin(),board.end());
            score[who]=Score(all.data(),static_cast<int>(all.size()));
        }
        if(score[0]==score[1])
        {
            // Split: an odd chip to the one first to act after the flop.
            wonPot=pot;
            const int half=pot/2/Unit*Unit;
            chips[Other(button)]+=pot-half;
            chips[button]+=half;
            pot=0;
            winner=-1;
        }
        else Pay(score[0]>score[1]?You:Her);
        phase=Phase::HandOver;
    }

    void Table::Step()
    {
        if(phase==Phase::Dealing)
        {
            if(!opened)DealHand();
            else if(board.size()==5)Showdown();
            else DealStreet();
            ++moves;
        }
        else if(phase==Phase::HerTurn)
        {
            const Order o=HerOrder();
            bluffing=o.did==Did::Raise||o.did==Did::AllIn?o.bluff:bluffing;
            if(o.did==Did::Fold)Fold();
            else if(o.did==Did::Raise||o.did==Did::AllIn)RaiseTo(o.raiseTo);
            else CheckCall();
        }
    }

    const Order& Table::HerOrder()
    {
        if(!ordered)
        {
            order=Choose(Her,CareNow());
            ordered=true;
        }
        return order;
    }

    // Eased a little further while she is well ahead.
    double Table::CareNow() const
    {
        const int lead=chips[Her]-chips[You];
        return care-(lead>=StartChips?0.3:lead>=StartChips/2?0.15:0.0);
    }

    double Table::HerStrength()
    {
        if(holes[Her].empty())return 0.5;
        if(strengthFor!=static_cast<int>(board.size()))
        {
            strengthFor=static_cast<int>(board.size());
            strength=Equity(Her,500,true);
        }
        return strength;
    }

    // How often who's cards win, from what who can see: the two cards and the board. The other's
    // two are drawn from the cards unseen, those that would not have bet so hard mostly passed over.
    double Table::Equity(int who,int trials,bool againstAny)
    {
        std::vector<int> unseen;
        unseen.reserve(Cards);
        for(int c=0;c<Cards;++c)
            if(std::find(holes[who].begin(),holes[who].end(),c)==holes[who].end()&&std::find(board.begin(),board.end(),c)==board.end())
                unseen.push_back(c);
        const int other=Other(who),shared=static_cast<int>(board.size()),count=static_cast<int>(unseen.size());
        const int early=againstAny?0:earlyRaises[other],late=againstAny?0:raises[other]-earlyRaises[other];
        const bool allIn=!againstAny&&chips[other]==0;
        const double needed=allIn||early>=3?0.46:early==2?0.34:early==1?0.18:0;
        const double passAir=std::max(0.2,1.0-0.5*late-(allIn?0.4:0.0));
        std::uniform_real_distribution<double> any(0,1);
        int cards[7]{};
        double won=0;
        for(int k=0;k<trials;++k)
        {
            // Partly shuffled: the first places of unseen are this trial's cards.
            auto pick=[&](int at){
                const int with=at+static_cast<int>(think()%static_cast<unsigned>(count-at));
                std::swap(unseen[static_cast<size_t>(at)],unseen[static_cast<size_t>(with)]);
                return unseen[static_cast<size_t>(at)];
            };
            int a=pick(0),b=pick(1);
            for(int tries=0;tries<10;++tries)
            {
                const bool fits=(needed<=0||StartingWorth(a,b)>=needed||any(think)<0.2)&&
                                (shared==0||passAir>=1||Connects(a,b,board.data(),shared)||any(think)<passAir);
                if(fits)break;
                a=pick(0);b=pick(1);
            }
            std::copy(board.begin(),board.end(),cards);
            for(int i=shared;i<5;++i)cards[i]=pick(2+i-shared);
            cards[5]=holes[who][0];cards[6]=holes[who][1];
            const int mine=Score(cards,7);
            cards[5]=a;cards[6]=b;
            const int theirs=Score(cards,7);
            won+=mine>theirs?1:mine==theirs?0.5:0;
        }
        return won/trials;
    }

    // Four to a flush or a straight, with a card to come.
    bool Table::Draws(int who) const
    {
        if(board.size()>=5||board.empty())return false;
        int suits[4]{};
        unsigned ranks=0;
        for(int c:holes[who]){++suits[SuitOf(c)];ranks|=1u<<High(c);}
        for(int c:board){++suits[SuitOf(c)];ranks|=1u<<High(c);}
        if(*std::max_element(suits,suits+4)==4)return true;
        if(ranks&(1u<<14))ranks|=1u<<1;
        for(int low=1;low<=10;++low)if(Bits((ranks>>low)&0x1Fu)==4)return true;
        return false;
    }

    // Her choice: how often she wins from here against what the other's betting says they hold,
    // set against the price of going on; with some bets on little, so that a bet of hers does not
    // always mean strength, and now and then (less care) the next choice down.
    Order Table::Choose(int who,double carefully)
    {
        std::uniform_real_distribution<double> any(0,1);
        const double eq=Equity(who,800,false);
        const int call=ToCall(who),inPot=Pot();
        const double price=call>0?static_cast<double>(call)/(inPot+call):0;
        const bool draw=Draws(who),canRaise=CanRaise(who);
        const double r=any(think);
        Order o;
        auto raiseBy=[&](double share,bool bluff){
            o.raiseTo=RaiseToShare(who,share);
            // Most of what is left: all of it.
            if(o.raiseTo-bets[who]>0.65*chips[who])o.raiseTo=MaxRaiseTo(who);
            o.did=o.raiseTo-bets[who]>=chips[who]?Did::AllIn:Did::Raise;
            o.bluff=bluff;
        };
        auto share=[&](double lo,double hi){return lo+(hi-lo)*any(think);};
        o.did=call>0?Did::Fold:Did::Check;
        if(call==0)
        {
            if(canRaise)
            {
                if(eq>=0.72){if(r<(board.size()==5?0.92:0.78))raiseBy(share(0.5,1.0),false);}
                else if(eq>=0.58){if(r<0.55)raiseBy(share(0.45,0.7),false);}
                else if(draw&&eq>=0.3){if(r<0.4)raiseBy(share(0.5,0.75),true);}
                else if(board.size()==3&&aggressor[who]){if(r<0.5)raiseBy(share(0.4,0.6),eq<0.45);}
                else if(board.size()>=4&&eq<0.36){if(r<0.14)raiseBy(share(0.6,0.9),true);}
            }
        }
        else
        {
            if(eq>=0.80&&canRaise&&r<0.7)raiseBy(share(0.6,1.0),false);
            else if(eq>=0.64&&canRaise&&r<0.3)raiseBy(share(0.5,0.8),false);
            // Each raise of theirs after the flop, and a large one, asks for more than the bare price.
            else if(eq>=price+0.04+0.07*std::max(0,raises[Other(who)]-earlyRaises[Other(who)])+(call*2>=inPot?0.05:0.0))
            {
                o.did=call>=chips[who]?Did::AllIn:Did::Call;
                if(board.empty()&&canRaise&&eq>=0.56&&r<0.6)raiseBy(share(0.7,1.0),false);
                else if(draw&&canRaise&&r<0.07)raiseBy(share(0.7,1.0),true);
            }
            else if(canRaise&&board.size()>=3&&call*3<=inPot&&r<0.05)raiseBy(share(0.8,1.1),true);
        }
        // The next choice down: a call for a fold not far off, a call for a raise, no bet for a bet.
        if(any(think)<(1-carefully)*0.8)
        {
            const bool raised=o.did==Did::Raise||o.did==Did::AllIn;
            if(raised&&call>0)o={call>=chips[who]?Did::AllIn:Did::Call,0,false};
            else if(raised)o={Did::Check,0,false};
            else if(o.did==Did::Fold&&eq>=price-0.15)o={call>=chips[who]?Did::AllIn:Did::Call,0,false};
        }
        return o;
    }

    void Table::PlayForYou(int style)
    {
        if(phase==Phase::HandOver){NextHand();return;}
        if(phase==Phase::Dealing||phase==Phase::HerTurn){Step();return;}
        if(phase!=Phase::YourTurn)return;
        std::uniform_real_distribution<double> any(0,1);
        const int call=ToCall(You);
        if(style==1){CheckCall();return;}
        if(style==3)
        {
            if(CanRaise(You)&&any(think)<0.45)RaiseTo(RaiseToShare(You,any(think)<0.2?5.0:0.8));
            else CheckCall();
            return;
        }
        if(style==2)
        {
            const double eq=Equity(You,300,false),price=call>0?static_cast<double>(call)/(Pot()+call):0;
            if(eq>=0.75&&CanRaise(You))RaiseTo(RaiseToShare(You,0.7));
            else if(call==0||eq>=price+0.12)CheckCall();
            else Fold();
            return;
        }
        const Order o=Choose(You,1);
        if(o.did==Did::Fold)Fold();
        else if(o.raiseTo>0)RaiseTo(o.raiseTo);
        else CheckCall();
    }

    bool Table::Consistent() const
    {
        std::vector<int> all(deck);
        all.insert(all.end(),board.begin(),board.end());
        for(const auto& h:holes)all.insert(all.end(),h.begin(),h.end());
        if(all.size()!=Cards)return false;
        std::sort(all.begin(),all.end());
        for(int i=0;i<Cards;++i)if(all[static_cast<size_t>(i)]!=i)return false;
        return chips[0]>=0&&chips[1]>=0&&chips[0]+chips[1]+Pot()==2*StartChips&&
               chips[0]%Unit==0&&chips[1]%Unit==0&&bets[0]%Unit==0&&bets[1]%Unit==0;
    }

    std::wstring CheckRules()
    {
        std::wostringstream lines;
        int failed=0;
        auto check=[&](bool ok,const wchar_t* what){if(!ok)++failed;lines<<what<<L": "<<(ok?L"ok":L"WRONG")<<L"\n";};
        auto C=[](int suit,int rank){return suit*13+rank-1;};
        auto score=[](std::vector<int> c){return Score(c.data(),static_cast<int>(c.size()));};
        // Every seven cards of 52: how often each kind is the best five (the known counts).
        {
            long long found[9]{};
            int c[7];
            for(c[0]=0;c[0]<Cards;++c[0])for(c[1]=c[0]+1;c[1]<Cards;++c[1])for(c[2]=c[1]+1;c[2]<Cards;++c[2])for(c[3]=c[2]+1;c[3]<Cards;++c[3])
                for(c[4]=c[3]+1;c[4]<Cards;++c[4])for(c[5]=c[4]+1;c[5]<Cards;++c[5])for(c[6]=c[5]+1;c[6]<Cards;++c[6])
                    ++found[KindOf(Score(c,7))];
            const long long known[9]={23294460,58627800,31433400,6461620,6180020,4047644,3473184,224848,41584};
            for(int k=0;k<9;++k)
            {
                const std::wstring what=L"seven-card count of kind "+std::to_wstring(k)+L" ("+std::to_wstring(found[k])+L")";
                check(found[k]==known[k],what.c_str());
            }
        }
        check(KindOf(score({C(0,1),C(1,2),C(2,3),C(3,4),C(0,5),C(1,9),C(2,13)}))==4&&score({C(0,1),C(1,2),C(2,3),C(3,4),C(0,5)})<score({C(0,2),C(1,3),C(2,4),C(3,5),C(0,6)}),L"ace to five is the lowest straight");
        check(score({C(0,1),C(1,1),C(0,13),C(1,5),C(2,7),C(3,2),C(0,3)})>score({C(2,1),C(3,1),C(0,12),C(1,5),C(2,7),C(3,2),C(0,3)}),L"a pair's kicker counts");
        check(score({C(0,10),C(1,10),C(2,10),C(0,9),C(1,9),C(2,9),C(3,2)})==Packed(6,10,9),L"two threes make a full house");
        check(score({C(0,8),C(1,8),C(0,6),C(1,6),C(0,4),C(1,4),C(3,2)})==Packed(2,8,6,4),L"of three pairs the best two, the third as kicker");
        check(score({C(0,2),C(0,5),C(0,9),C(0,11),C(0,13),C(0,3),C(1,3)})>>20==5,L"a flush");
        check(BestFive({C(0,2),C(0,5),C(0,9),C(0,11),C(0,13),C(1,3),C(2,3)}).size()==5&&KindOf(score(BestFive({C(0,2),C(0,5),C(0,9),C(0,11),C(0,13),C(1,3),C(2,3)})))==5,L"best five of seven");
        Table t;
        // Find a game where the button is as wanted (it is drawn).
        auto start=[&](int button,const std::vector<int>& cards){
            for(std::uint32_t seed=1;;++seed)
            {
                t.NewGame(seed);
                if(t.Button()==button)break;
            }
            t.StackDeck(cards);
            t.Step();
        };
        // You on the button: dealt her, you, her, you, then the board.
        start(You,{C(0,2),C(0,1),C(1,7),C(1,1),C(2,1),C(3,9),C(3,4),C(2,3),C(0,13)});
        check(t.Holes()[You][0]==C(0,1)&&t.Holes()[Her][0]==C(0,2)&&t.Holes()[You][1]==C(1,1),L"stacked cards dealt in order");
        check(t.Now()==Phase::YourTurn&&t.BetOf(You)==10&&t.BetOf(Her)==20&&t.Chips(You)==990&&t.ToCall(You)==10&&t.Consistent(),L"the button posts the small blind and acts first");
        t.Fold();
        check(t.Now()==Phase::HandOver&&t.Winner()==Her&&t.ByFold()&&t.Chips(You)==990&&t.Chips(Her)==1010&&!t.HerCardsShown(),L"a fold loses the blind, her cards unseen");
        t.NextHand();
        check(t.Button()==Her&&t.Now()==Phase::Dealing,L"the button passes");
        start(You,{C(0,2),C(0,1),C(1,7),C(1,1),C(2,1),C(3,9),C(3,4),C(2,3),C(0,13)});
        t.CheckCall();
        check(t.Now()==Phase::HerTurn&&t.BetOf(You)==20,L"a call of the blind leaves her the option");
        t.CheckCall();
        check(t.Now()==Phase::Dealing&&t.Collected()==40,L"her check ends the betting before the flop");
        t.Step();
        check(t.Board().size()==3&&t.Now()==Phase::HerTurn,L"the flop: the big blind first");
        t.CheckCall();t.CheckCall();
        t.Step();
        check(t.Board().size()==4&&t.Now()==Phase::HerTurn,L"the turn");
        t.CheckCall();t.RaiseTo(60);
        check(t.Now()==Phase::HerTurn&&t.ToCall(Her)==60&&t.MinRaiseTo(Her)==120,L"a bet: a raise is at least as much again");
        t.CheckCall();
        t.Step();
        check(t.Board().size()==5,L"the river");
        t.CheckCall();t.CheckCall();
        check(t.Now()==Phase::HandOver&&t.Winner()==You&&t.Chips(You)==1000+80&&t.HerCardsShown(),L"the showdown: three aces win");
        // Everything in, unevenly: what the other cannot match comes back.
        for(std::uint32_t seed=1;;++seed){t.NewGame(seed);if(t.Button()==You)break;}
        t.StackDeck({C(0,2),C(0,1),C(1,7),C(1,1),C(2,1),C(3,9),C(3,4),C(2,3),C(0,13)});
        t.Step();
        t.RaiseTo(100000);
        check(t.BetOf(You)==1000&&t.Chips(You)==0&&t.LastDid(You)==Did::AllIn,L"all in");
        t.CheckCall();
        check(t.Now()==Phase::Dealing&&t.HerCardsShown()&&t.Collected()==2000,L"called: both turned up, no more betting");
        t.Step();t.Step();t.Step();
        check(t.Board().size()==5&&t.Now()==Phase::Dealing,L"the rest dealt a street at a time");
        t.Step();
        check(t.Now()==Phase::HandOver&&t.Chips(You)==2000&&t.Chips(Her)==0,L"the winner takes all");
        t.NextHand();
        check(t.Now()==Phase::GameOver,L"one out of chips ends the game");
        // The same five on the board for both: split.
        start(You,{C(0,2),C(0,3),C(1,2),C(1,3),C(2,10),C(2,11),C(2,12),C(2,13),C(2,1)});
        t.CheckCall();t.CheckCall();t.Step();t.CheckCall();t.CheckCall();t.Step();t.CheckCall();t.CheckCall();t.Step();t.CheckCall();t.CheckCall();
        check(t.Now()==Phase::HandOver&&t.Winner()==-1&&t.Chips(You)==1000&&t.Chips(Her)==1000,L"a royal flush on the board: split");
        // A blind raises after five hands.
        for(std::uint32_t seed=1;;++seed){t.NewGame(seed);if(t.Button()==You)break;}
        for(int i=0;i<HandsPerLevel;++i){t.Step();t.Fold();t.NextHand();}
        check(t.Hand()==6&&t.SmallBlind()==20,L"the blinds go up");
        std::wostringstream out;
        out<<failed<<L" failed\n"<<lines.str();
        return out.str();
    }

    std::wstring CheckPlaying(int count,std::uint32_t seed)
    {
        std::wostringstream out,table;
        int wrong=0;
        const wchar_t* names[]={L"herself",L"always calls",L"tight",L"wild"};
        for(int style=0;style<4;++style)
            for(double care:{1.0,0.6})
            {
                int herWins=0,yourWins=0,even=0,hands=0,folds=0,showdowns=0,steps=0;
                long long gain=0;
                for(int g=0;g<count;++g)
                {
                    Table t;
                    t.NewGame(seed+g*7919u,care);
                    for(int step=0;step<5000&&t.Now()!=Phase::GameOver;++step)
                    {
                        ++steps;
                        if(t.Now()==Phase::HandOver)
                        {
                            ++hands;
                            if(t.ByFold())++folds;else ++showdowns;
                        }
                        t.PlayForYou(style);
                        if(!t.Consistent()){++wrong;break;}
                    }
                    if(t.Now()!=Phase::GameOver){++wrong;continue;}
                    const int g2=t.GameGain();
                    gain+=g2;
                    if(g2>0)++yourWins;else if(g2<0)++herWins;else ++even;
                }
                table<<L"against "<<names[style]<<L", care "<<care<<L": she won "<<herWins<<L", lost "<<yourWins<<L", even "<<even
                     <<L"; hands "<<hands<<L" ("<<folds<<L" by a fold, "<<showdowns<<L" shown); her chips "<<(-gain>=0?L"+":L"")<<-gain<<L" over all\n";
            }
        out<<wrong<<L" wrong\n"<<table.str();
        return out.str();
    }
}
