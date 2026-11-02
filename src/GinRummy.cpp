#include "GinRummy.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <sstream>

namespace MikuMikuDesktopPerch::GinRummy
{
    namespace
    {
        // Every meld in the hand, as a mask of its places: three or four of a rank, three or more
        // in a row in a suit (the ace low).
        void FindMelds(const std::vector<int>& hand,std::vector<unsigned>& out)
        {
            out.clear();
            const int n=static_cast<int>(hand.size());
            int byRank[14][4];
            int rankCount[14]{};
            int bySuit[4][14];
            for(auto& s:bySuit)std::fill(std::begin(s),std::end(s),-1);
            for(int i=0;i<n;++i)
            {
                const int r=RankOf(hand[i]);
                byRank[r][rankCount[r]++]=i;
                bySuit[SuitOf(hand[i])][r]=i;
            }
            for(int r=1;r<=13;++r)
            {
                const int c=rankCount[r];
                if(c<3)continue;
                unsigned all=0;
                for(int k=0;k<c;++k)all|=1u<<byRank[r][k];
                if(c==3)out.push_back(all);
                else
                {
                    out.push_back(all);
                    for(int k=0;k<4;++k)out.push_back(all&~(1u<<byRank[r][k]));
                }
            }
            for(int s=0;s<4;++s)
                for(int a=1;a<=11;++a)
                {
                    unsigned mask=0;
                    for(int b=a;b<=13&&bySuit[s][b]>=0;++b)
                    {
                        mask|=1u<<bySuit[s][b];
                        if(b-a>=2)out.push_back(mask);
                    }
                }
        }

        // The least points left unmatched among the places in left; memo per mask.
        int Least(const std::vector<int>& hand,const std::vector<unsigned>& melds,unsigned left,short* memo)
        {
            if(!left)return 0;
            if(memo[left]>=0)return memo[left];
            int low=0;
            while(!(left&(1u<<low)))++low;
            int best=Points(hand[low])+Least(hand,melds,left&~(1u<<low),memo);
            for(unsigned m:melds)
                if((m&(1u<<low))&&(m&left)==m)
                    best=std::min(best,Least(hand,melds,left&~m,memo));
            memo[left]=static_cast<short>(best);
            return best;
        }

        // Whether card extends a meld shown.
        bool Extends(const std::vector<int>& meld,int card)
        {
            const bool set=RankOf(meld[0])==RankOf(meld[1]);
            if(set)return RankOf(card)==RankOf(meld[0])&&meld.size()<4;
            if(SuitOf(card)!=SuitOf(meld[0]))return false;
            int low=14,high=0;
            for(int c:meld){low=std::min(low,RankOf(c));high=std::max(high,RankOf(c));}
            return RankOf(card)==low-1||RankOf(card)==high+1;
        }
    }

    int Deadwood(const std::vector<int>& hand)
    {
        std::vector<unsigned> melds;
        FindMelds(hand,melds);
        short memo[1<<12];
        std::fill(memo,memo+(static_cast<size_t>(1)<<hand.size()),static_cast<short>(-1));
        return Least(hand,melds,(1u<<static_cast<unsigned>(hand.size()))-1,memo);
    }

    Split BestSplit(const std::vector<int>& hand)
    {
        std::vector<unsigned> melds;
        FindMelds(hand,melds);
        short memo[1<<12];
        std::fill(memo,memo+(static_cast<size_t>(1)<<hand.size()),static_cast<short>(-1));
        unsigned left=(1u<<static_cast<unsigned>(hand.size()))-1;
        Split out;
        out.deadwood=Least(hand,melds,left,memo);
        while(left)
        {
            int low=0;
            while(!(left&(1u<<low)))++low;
            const int need=Least(hand,melds,left,memo);
            bool used=false;
            for(unsigned m:melds)
                if((m&(1u<<low))&&(m&left)==m&&Least(hand,melds,left&~m,memo)==need)
                {
                    std::vector<int> meld;
                    for(size_t i=0;i<hand.size();++i)if(m&(1u<<i))meld.push_back(hand[i]);
                    std::sort(meld.begin(),meld.end(),[](int a,int b){return RankOf(a)!=RankOf(b)?RankOf(a)<RankOf(b):a<b;});
                    out.melds.push_back(std::move(meld));
                    left&=~m;
                    used=true;
                    break;
                }
            if(!used){out.dead.push_back(hand[low]);left&=~(1u<<low);}
        }
        std::sort(out.dead.begin(),out.dead.end(),[](int a,int b){return RankOf(a)!=RankOf(b)?RankOf(a)<RankOf(b):a<b;});
        return out;
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
        discards.clear();
        stock.clear();
        for(int c=0;c<Cards;++c)stock.push_back(c);
        phase=Phase::HandOver;
        NextHand();
    }

    void Table::NextHand()
    {
        if(phase!=Phase::HandOver)return;
        for(auto& h:hands){stock.insert(stock.end(),h.begin(),h.end());h.clear();}
        stock.insert(stock.end(),discards.begin(),discards.end());
        discards.clear();
        tookUp[0].clear();tookUp[1].clear();
        did={};
        outcome={};
        justDrawn=justTaken=-1;
        plannedAt=-1;
        turns=0;
        offer=0;
        mustStock=false;
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
        discards.push_back(stock.back());
        stock.pop_back();
        toAct=first;
        offer=2;
        phase=first==You?Phase::YourDraw:Phase::HerDraw;
    }

    bool Table::CanDrawStock() const {return (phase==Phase::YourDraw||phase==Phase::HerDraw)&&offer==0;}
    bool Table::CanTakeDiscard() const {return (phase==Phase::YourDraw||phase==Phase::HerDraw)&&!mustStock&&!discards.empty();}

    bool Table::CanDiscard(int card) const
    {
        if(phase!=Phase::YourDiscard&&phase!=Phase::HerDiscard)return false;
        const auto& h=hands[toAct];
        return card!=justTaken&&std::find(h.begin(),h.end(),card)!=h.end();
    }

    bool Table::CanKnockWith(int card) const
    {
        if(!CanDiscard(card))return false;
        std::vector<int> left(hands[toAct]);
        left.erase(std::find(left.begin(),left.end(),card));
        return Deadwood(left)<=KnockMax;
    }

    void Table::DrawStock()
    {
        if(!CanDrawStock())return;
        const int who=toAct;
        justDrawn=stock.back();
        justTaken=-1;
        hands[who].push_back(stock.back());
        stock.pop_back();
        did[who]=Did::Stock;
        mustStock=false;
        phase=who==You?Phase::YourDiscard:Phase::HerDiscard;
        ++moves;
    }

    void Table::TakeDiscard()
    {
        if(!CanTakeDiscard())return;
        const int who=toAct;
        justDrawn=justTaken=discards.back();
        hands[who].push_back(discards.back());
        tookUp[who].push_back(discards.back());
        discards.pop_back();
        did[who]=Did::TookDiscard;
        offer=0;
        phase=who==You?Phase::YourDiscard:Phase::HerDiscard;
        ++moves;
    }

    void Table::Pass()
    {
        if(offer==0||(phase!=Phase::YourDraw&&phase!=Phase::HerDraw))return;
        did[toAct]=Did::Pass;
        --offer;
        toAct=Other(toAct);
        // Both passed: the first player must draw from the stock.
        if(offer==0)mustStock=true;
        phase=toAct==You?Phase::YourDraw:Phase::HerDraw;
        ++moves;
    }

    void Table::Discard(int card,bool knock)
    {
        if(!CanDiscard(card))return;
        const int who=toAct;
        auto& h=hands[who];
        h.erase(std::find(h.begin(),h.end(),card));
        discards.push_back(card);
        justDrawn=justTaken=-1;
        plannedAt=-1;
        ++turns;
        ++moves;
        if(knock&&Deadwood(h)<=KnockMax){did[who]=Did::Knock;Settle(who);return;}
        did[who]=Did::Discard;
        if(static_cast<int>(stock.size())<=StockEnd)
        {
            // The hand is void.
            for(int w=0;w<2;++w){outcome.split[w]=BestSplit(hands[w]);outcome.deadwood[w]=outcome.split[w].deadwood;}
            phase=Phase::HandOver;
            return;
        }
        toAct=Other(who);
        phase=toAct==You?Phase::YourDraw:Phase::HerDraw;
    }

    // The other lays off what they can on the knocker's melds (not on a gin), their own melds
    // chosen to leave the least.
    void Table::Settle(int knocker)
    {
        const int other=Other(knocker);
        outcome={};
        outcome.knocker=knocker;
        outcome.split[knocker]=BestSplit(hands[knocker]);
        outcome.deadwood[knocker]=outcome.split[knocker].deadwood;
        outcome.gin=outcome.deadwood[knocker]==0;
        Split best=BestSplit(hands[other]);
        if(!outcome.gin)
        {
            std::vector<unsigned> melds;
            const auto& h=hands[other];
            FindMelds(h,melds);
            int top=best.deadwood+1;
            Split chosen;
            std::vector<int> chosenLaid;
            // Every way of choosing melds that do not overlap, what is left laid off where it can be.
            std::function<void(size_t,unsigned,std::vector<unsigned>&)> walk=[&](size_t from,unsigned used,std::vector<unsigned>& picked){
                std::vector<std::vector<int>> shown=outcome.split[knocker].melds;
                std::vector<int> left,laid;
                for(size_t i=0;i<h.size();++i)if(!(used&(1u<<i)))left.push_back(h[i]);
                for(bool again=true;again;)
                {
                    again=false;
                    for(size_t i=0;i<left.size();++i)
                        for(auto& meld:shown)
                            if(Extends(meld,left[i]))
                            {
                                meld.push_back(left[i]);
                                laid.push_back(left[i]);
                                left.erase(left.begin()+static_cast<std::ptrdiff_t>(i));
                                again=true;
                                goto next;
                            }
                    next:;
                }
                int points=0;
                for(int c:left)points+=Points(c);
                if(points<top)
                {
                    top=points;
                    chosen={};
                    for(unsigned m:picked)
                    {
                        std::vector<int> meld;
                        for(size_t i=0;i<h.size();++i)if(m&(1u<<i))meld.push_back(h[i]);
                        std::sort(meld.begin(),meld.end(),[](int a,int b){return RankOf(a)!=RankOf(b)?RankOf(a)<RankOf(b):a<b;});
                        chosen.melds.push_back(std::move(meld));
                    }
                    chosen.dead=left;
                    chosen.deadwood=points;
                    chosenLaid=laid;
                }
                for(size_t k=from;k<melds.size();++k)
                    if(!(melds[k]&used))
                    {
                        picked.push_back(melds[k]);
                        walk(k+1,used|melds[k],picked);
                        picked.pop_back();
                    }
            };
            std::vector<unsigned> picked;
            walk(0,0,picked);
            best=std::move(chosen);
            outcome.laid=std::move(chosenLaid);
        }
        std::sort(best.dead.begin(),best.dead.end(),[](int a,int b){return RankOf(a)!=RankOf(b)?RankOf(a)<RankOf(b):a<b;});
        outcome.split[other]=best;
        outcome.deadwood[other]=best.deadwood;
        if(outcome.gin)
        {
            outcome.winner=knocker;
            outcome.points=outcome.deadwood[other]+GinBonus;
        }
        else if(outcome.deadwood[other]<=outcome.deadwood[knocker])
        {
            outcome.winner=other;
            outcome.undercut=true;
            outcome.points=outcome.deadwood[knocker]-outcome.deadwood[other]+UndercutBonus;
        }
        else
        {
            outcome.winner=knocker;
            outcome.points=outcome.deadwood[other]-outcome.deadwood[knocker];
        }
        scores[outcome.winner]+=outcome.points;
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
        case Phase::HerDraw:
            if(HerTakes())TakeDiscard();
            else if(offer>0)Pass();
            else DrawStock();
            plannedAt=-1;
            break;
        case Phase::HerDiscard:
        {
            bool knock=false;
            const int card=HerDiscard(knock);
            Discard(card,knock);
            break;
        }
        default:
            break;
        }
    }

    bool Table::HerTakes()
    {
        if(plannedAt!=moves)
        {
            plannedAt=moves;
            plannedTake=CanTakeDiscard()&&WantsDiscard(Her);
            if(plannedTake)
            {
                // Now and then, with less care, a good discard is left lying.
                std::uniform_real_distribution<double> any(0,1);
                if(any(think)<(1-CareNow())*0.35)plannedTake=false;
            }
        }
        return plannedTake;
    }

    int Table::HerDiscard(bool& knock)
    {
        if(plannedAt!=moves)
        {
            plannedAt=moves;
            plannedCard=ChooseDiscard(Her,CareNow(),plannedKnock);
        }
        knock=plannedKnock;
        return plannedCard;
    }

    // Eased a little further while she is well ahead.
    double Table::CareNow() const
    {
        const int lead=scores[Her]-scores[You];
        return care-(lead>=Target*6/10?0.3:lead>=Target*3/10?0.15:0.0);
    }

    // Takes the discard when it makes a meld or nearly so and leaves clearly fewer points.
    bool Table::WantsDiscard(int who)
    {
        const int up=discards.back();
        std::vector<int> h(hands[who]);
        const int now=Deadwood(h);
        h.push_back(up);
        int after=1000;
        for(size_t i=0;i+1<h.size();++i)
        {
            std::vector<int> left(h);
            left.erase(left.begin()+static_cast<std::ptrdiff_t>(i));
            after=std::min(after,Deadwood(left));
        }
        const Split split=BestSplit(h);
        const bool inMeld=std::find(split.dead.begin(),split.dead.end(),up)==split.dead.end();
        return inMeld&&now-after>=2;
    }

    // The discard that leaves the fewest points, counting how far the cards still unseen could
    // bring them down, and keeping back what the other seems to be collecting. Only what who can
    // see is used: their own hand, the discards, what the other took from them.
    int Table::ChooseDiscard(int who,double carefully,bool& knock)
    {
        knock=false;
        const auto& h=hands[who];
        std::vector<int> unseen;
        for(int c=0;c<Cards;++c)
            if(std::find(h.begin(),h.end(),c)==h.end()&&std::find(discards.begin(),discards.end(),c)==discards.end()&&
               std::find(tookUp[Other(who)].begin(),tookUp[Other(who)].end(),c)==tookUp[Other(who)].end())
                unseen.push_back(c);
        std::uniform_real_distribution<double> any(0,1);
        struct Choice {int card;double worth;int deadwood;};
        std::vector<Choice> choices;
        const double late=std::min(1.0,turns/20.0);
        for(int card:h)
        {
            if(card==justTaken)continue;
            std::vector<int> left(h);
            left.erase(std::find(left.begin(),left.end(),card));
            const int dead=Deadwood(left);
            double better=0;
            std::vector<int> more(left);
            more.push_back(0);
            for(int u:unseen)
            {
                more.back()=u;
                // With that card in hand, the highest card may still go.
                const Split split=BestSplit(more);
                int after=split.deadwood;
                if(!split.dead.empty())after-=Points(*std::max_element(split.dead.begin(),split.dead.end(),[](int a,int b){return Points(a)<Points(b);}));
                better+=std::max(0,dead-after);
            }
            double worth=dead-1.6*better/std::max<size_t>(1,unseen.size());
            // What the other has taken says what they collect: a card next to those is held back.
            for(int c:tookUp[Other(who)])
            {
                if(RankOf(c)==RankOf(card))worth+=2+2*late;
                else if(SuitOf(c)==SuitOf(card)&&std::abs(RankOf(c)-RankOf(card))<=2)worth+=1.5+2*late;
            }
            // What they threw away they do not want.
            for(int c:discards)if(RankOf(c)==RankOf(card))worth-=0.7;
            choices.push_back({card,worth+any(think)*0.2,dead});
        }
        std::sort(choices.begin(),choices.end(),[](const Choice& a,const Choice& b){return a.worth<b.worth;});
        // Knock on the discard leaving the fewest points: at once on a gin, for a good while on
        // anything allowed, late in the hand only on little (an undercut grows likelier).
        const Choice low=*std::min_element(choices.begin(),choices.end(),[](const Choice& a,const Choice& b){return a.deadwood<b.deadwood;});
        if(low.deadwood==0){knock=true;return low.card;}
        const int most=turns<=16?KnockMax:turns<=22?6:3;
        if(low.deadwood<=most&&any(think)>=(1-carefully)*0.3)
        {
            knock=true;
            return low.card;
        }
        // The next choice down, now and then.
        if(choices.size()>1&&any(think)<(1-carefully)*0.5)return choices[1].card;
        return choices[0].card;
    }

    void Table::PlayForYou(int style)
    {
        if(phase==Phase::HandOver){NextHand();return;}
        if(phase==Phase::Dealing||phase==Phase::HerDraw||phase==Phase::HerDiscard){Step();return;}
        std::uniform_real_distribution<double> any(0,1);
        if(phase==Phase::YourDraw)
        {
            const bool can=CanTakeDiscard();
            const bool take=style==2?(can&&any(think)<0.3):(can&&WantsDiscard(You));
            if(take)TakeDiscard();
            else if(offer>0)Pass();
            else DrawStock();
            return;
        }
        if(phase!=Phase::YourDiscard)return;
        bool knock=false;
        int card=-1;
        if(style==0)card=ChooseDiscard(You,1,knock);
        else if(style==1)
        {
            // Whichever leaves the fewest points; knocks whenever it may.
            int best=1000;
            for(int c:hands[You])
            {
                if(!CanDiscard(c))continue;
                std::vector<int> left(hands[You]);
                left.erase(std::find(left.begin(),left.end(),c));
                if(const int d=Deadwood(left);d<best){best=d;card=c;}
            }
            knock=best<=KnockMax;
        }
        else
        {
            do{card=hands[You][think()%hands[You].size()];}while(!CanDiscard(card));
            knock=CanKnockWith(card);
        }
        Discard(card,knock);
    }

    bool Table::Consistent() const
    {
        std::vector<int> all(stock);
        all.insert(all.end(),discards.begin(),discards.end());
        for(const auto& h:hands)all.insert(all.end(),h.begin(),h.end());
        if(all.size()!=Cards)return false;
        std::sort(all.begin(),all.end());
        for(int i=0;i<Cards;++i)if(all[static_cast<size_t>(i)]!=i)return false;
        return true;
    }

    std::wstring CheckRules()
    {
        std::wostringstream lines;
        int failed=0;
        auto check=[&](bool ok,const wchar_t* what){if(!ok)++failed;lines<<what<<L": "<<(ok?L"ok":L"WRONG")<<L"\n";};
        auto C=[](int suit,int rank){return suit*13+rank-1;};
        check(Deadwood({C(0,1),C(0,2),C(0,3)})==0,L"ace, two, three in a suit is a run");
        check(Deadwood({C(0,12),C(0,13),C(0,1)})==21,L"queen, king, ace is no run (10 + 10 + 1)");
        check(Deadwood({C(0,5),C(1,5),C(2,5),C(3,5)})==0,L"four of a rank");
        check(Deadwood({C(0,5),C(1,5)})==10,L"a pair is no meld");
        // A card that fits two ways is used in the way that leaves least.
        check(Deadwood({C(0,7),C(1,7),C(2,7),C(0,8),C(0,9),C(0,10)})==0,L"a run of four and three of a rank");
        check(Deadwood({C(0,7),C(1,7),C(2,7),C(0,8),C(0,9)})==14,L"one seven cannot be in both melds (the run, two sevens left)");
        check(BestSplit({C(0,1),C(0,2),C(0,3),C(1,13),C(2,4)}).dead.size()==2&&BestSplit({C(0,1),C(0,2),C(0,3),C(1,13),C(2,4)}).deadwood==14,L"king and four left over");
        Table t;
        auto start=[&](int dealer,const std::vector<int>& cards){
            for(std::uint32_t seed=1;;++seed)
            {
                t.NewGame(seed);
                if(t.Dealer()==dealer)break;
            }
            t.StackDeck(cards);
            t.Step();
        };
        auto run=[&](int suit,int from,int count){std::vector<int> v;for(int i=0;i<count;++i)v.push_back(C(suit,from+i));return v;};
        // Dealt in turn from the one who is not the dealer: hers are the even places when you deal.
        auto dealOf=[](const std::vector<int>& first,const std::vector<int>& second,std::vector<int> rest){
            std::vector<int> all;
            for(size_t i=0;i<first.size();++i){all.push_back(first[i]);all.push_back(second[i]);}
            all.insert(all.end(),rest.begin(),rest.end());
            return all;
        };
        // You hold three melds and a ten: draw the card that makes gin.
        {
            std::vector<int> you=run(0,1,3);
            for(int c:run(1,1,3))you.push_back(c);
            for(int c:run(2,1,3))you.push_back(c);
            you.push_back(C(3,10));
            std::vector<int> her={C(0,6),C(1,6),C(2,6),C(3,6),C(0,8),C(1,8),C(2,8),C(3,8),C(0,12),C(1,13)};
            start(Her,dealOf(you,her,{C(3,3),C(2,4)}));
            check(t.Now()==Phase::YourDraw&&t.InOffer()&&!t.CanDrawStock()&&t.CanTakeDiscard()&&t.HandOf(You).size()==10&&t.Top()==C(3,3),L"the one not dealing is offered the upcard first");
            t.Pass();
            check(t.Now()==Phase::HerDraw&&t.InOffer(),L"then the dealer");
            t.Pass();
            check(t.Now()==Phase::YourDraw&&!t.InOffer()&&t.CanDrawStock()&&!t.CanTakeDiscard(),L"both passed: the first must draw from the stock");
            t.DrawStock();
            check(t.JustDrawn()==C(2,4)&&t.Now()==Phase::YourDiscard,L"drawn from the stock");
            t.Discard(C(3,10),true);
            check(t.Now()==Phase::HandOver&&t.Result().gin&&t.Result().winner==You&&t.Result().points==20+GinBonus&&t.Score(You)==45,L"gin: her points (12 is 10 + 10) and the bonus, no laying off");
        }
        // A knock: the other lays off and the difference scores.
        {
            std::vector<int> you=run(0,1,4);              // ace to four of spades, laid on by a five
            for(int c:{C(1,9),C(2,9),C(3,9)})you.push_back(c);
            for(int c:{C(1,2),C(2,3)})you.push_back(c);    // 5 left
            you.push_back(C(3,13));                        // to be discarded
            std::vector<int> her={C(0,5),C(1,7),C(1,8),C(1,10),C(2,6),C(2,11),C(3,3),C(3,4),C(3,5),C(3,6)};
            start(Her,dealOf(you,her,{C(2,1),C(2,2)}));
            t.Pass();t.Pass();
            t.DrawStock();   // the two of diamonds: a pair of twos, one more low card
            check(t.CanKnockWith(C(3,13)),L"may knock with 10 or less");
            t.Discard(C(3,13),true);
            const auto& r=t.Result();
            check(r.knocker==You&&r.deadwood[You]==7,L"the knocker's points");
            check(std::find(r.laid.begin(),r.laid.end(),C(0,5))!=r.laid.end(),L"her five laid off on your run");
            check(r.winner==You&&r.points==r.deadwood[Her]-7&&!r.undercut,L"the difference scores");
        }
        // An undercut.
        {
            std::vector<int> you=run(0,9,4);
            for(int c:run(1,9,3))you.push_back(c);
            for(int c:{C(3,1),C(2,2),C(3,3)})you.push_back(c);   // 6 left
            std::vector<int> her=run(0,6,3);
            for(int c:run(1,6,3))her.push_back(c);
            for(int c:run(2,6,3))her.push_back(c);
            her.push_back(C(2,1));   // only an ace left: 1 point
            start(Her,dealOf(you,her,{C(3,12),C(3,13)}));
            t.Pass();t.Pass();
            t.DrawStock();
            t.Discard(C(3,13),true);
            const auto& r=t.Result();
            check(r.knocker==You&&r.deadwood[You]==6,L"you knock with 6");
            check(r.undercut&&r.winner==Her&&r.points==5+UndercutBonus&&t.Score(Her)==30,L"she has less: undercut, the difference and the bonus");
        }
        // The card taken from the discard may not go straight back.
        {
            start(Her,{});
            const int up=t.Top();
            t.TakeDiscard();
            check(!t.CanDiscard(up)&&t.Now()==Phase::YourDiscard,L"the card just taken from the discard may not go back");
        }
        // The stock running out voids the hand.
        {
            for(std::uint32_t seed=1;;++seed){t.NewGame(seed);if(t.Dealer()==Her)break;}
            t.Step();
            t.Pass();t.Pass();
            int guard=0;
            while(t.Now()!=Phase::HandOver&&guard++<200)
            {
                t.DrawStock();
                t.Discard(t.JustDrawn(),false);
            }
            check(t.Now()==Phase::HandOver&&static_cast<int>(t.Stock().size())==StockEnd&&t.Result().winner<0&&t.Score(You)==0&&t.Score(Her)==0,L"two left in the stock: the hand is void");
            t.NextHand();
            check(t.Now()==Phase::Dealing&&t.Dealer()==You&&t.HandNumber()==2,L"the deal passes");
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
                int herWins=0,yourWins=0,hands=0,voids=0,gins=0,knocks=0,undercuts=0,turnsAll=0;
                long long gain=0;
                for(int g=0;g<count;++g)
                {
                    Table t;
                    t.NewGame(seed+g*7919u,care);
                    for(int step=0;step<100000&&t.Now()!=Phase::GameOver;++step)
                    {
                        if(t.Now()==Phase::HandOver)
                        {
                            ++hands;
                            turnsAll+=t.Turns();
                            const auto& r=t.Result();
                            if(r.knocker<0)++voids;
                            else if(r.gin)++gins;
                            else if(r.undercut)++undercuts;
                            else ++knocks;
                        }
                        t.PlayForYou(style);
                        if(!t.Consistent()){++wrong;break;}
                    }
                    if(t.Now()!=Phase::GameOver){++wrong;continue;}
                    gain+=t.Score(Her)-t.Score(You);
                    if(t.Score(Her)>t.Score(You))++herWins;else ++yourWins;
                }
                table<<L"against "<<names[style]<<L", care "<<care<<L": she won "<<herWins<<L", lost "<<yourWins<<L"; hands "<<hands<<L" ("<<knocks<<L" knocks, "<<gins<<L" gins, "<<undercuts<<L" undercuts, "<<voids<<L" void), "
                     <<(hands?turnsAll/hands:0)<<L" discards a hand; her points "<<(gain>=0?L"+":L"")<<gain<<L" over all\n";
            }
        out<<wrong<<L" wrong\n"<<table.str();
        return out.str();
    }
}
