#include "Blackjack.hpp"
#include <algorithm>
#include <numeric>
#include <sstream>

namespace MikuMikuDesktopPerch::Blackjack
{
    int Total(const std::vector<int>& cards,bool* soft)
    {
        int total=0,aces=0;
        for(int c:cards)
        {
            const int r=RankOf(c);
            if(r==1){++aces;total+=1;}
            else total+=std::min(r,10);
        }
        const bool high=aces>0&&total+10<=21;
        if(soft)*soft=high;
        return high?total+10:total;
    }

    namespace
    {
        int Value(int card){const int r=RankOf(card);return r==1?11:std::min(r,10);}
    }

    void Table::NewGame(std::uint32_t seed)
    {
        rng.seed(seed);
        shoe.resize(Cards);
        std::iota(shoe.begin(),shoe.end(),0);
        std::shuffle(shoe.begin(),shoe.end(),rng);
        discards.clear();
        hands.clear();
        dealer.clear();
        active=0;
        holeShown=false;
        chips=StartChips;
        refills=0;
        round=1;
        moves=0;
        shuffles=0;
        roundGain=0;
        phase=Phase::Betting;
    }

    void Table::Shuffle()
    {
        shoe.insert(shoe.end(),discards.begin(),discards.end());
        discards.clear();
        std::shuffle(shoe.begin(),shoe.end(),rng);
        ++shuffles;
    }

    int Table::Draw()
    {
        if(shoe.empty())Shuffle();
        const int c=shoe.back();
        shoe.pop_back();
        ++moves;
        return c;
    }

    void Table::StackShoe(const std::vector<int>& next)
    {
        // The given cards taken out wherever they are and put on top, the first drawn first.
        for(int c:next)
        {
            shoe.erase(std::remove(shoe.begin(),shoe.end(),c),shoe.end());
            discards.erase(std::remove(discards.begin(),discards.end(),c),discards.end());
        }
        for(auto it=next.rbegin();it!=next.rend();++it)shoe.push_back(*it);
    }

    bool Table::CanDeal(int bet) const {return phase==Phase::Betting&&bet>0&&bet<=chips;}

    bool Table::CanHit() const
    {
        return phase==Phase::YourTurn&&active<static_cast<int>(hands.size())&&!hands[static_cast<size_t>(active)].done;
    }

    bool Table::CanDouble() const
    {
        if(!CanHit())return false;
        const Hand& h=hands[static_cast<size_t>(active)];
        return h.cards.size()==2&&!h.splitAces&&chips>=h.bet;
    }

    bool Table::CanSplit() const
    {
        if(!CanHit()||hands.size()!=1)return false;
        const Hand& h=hands[0];
        return h.cards.size()==2&&Value(h.cards[0])==Value(h.cards[1])&&chips>=h.bet;
    }

    void Table::Deal(int bet)
    {
        if(!CanDeal(bet))return;
        if(shoe.size()<15)Shuffle();
        chips-=bet;
        hands.assign(1,Hand{});
        hands[0].bet=bet;
        dealer.clear();
        active=0;
        holeShown=false;
        roundGain=0;
        hands[0].cards.push_back(Draw());
        dealer.push_back(Draw());
        hands[0].cards.push_back(Draw());
        dealer.push_back(Draw());
        // Her up card an ace or worth ten: she looks under it for a blackjack.
        const bool herNatural=Natural(dealer),yourNatural=Natural(hands[0].cards);
        if(herNatural||yourNatural)
        {
            holeShown=true;
            ++moves;
            Settle();
            return;
        }
        phase=Phase::YourTurn;
    }

    void Table::NextHand()
    {
        while(active<static_cast<int>(hands.size())&&hands[static_cast<size_t>(active)].done)++active;
        if(active<static_cast<int>(hands.size()))
        {
            // A hand split off: its second card now.
            Hand& h=hands[static_cast<size_t>(active)];
            if(h.cards.size()<2)
            {
                h.cards.push_back(Draw());
                if(h.splitAces||Total(h.cards)==21){h.done=true;NextHand();}
            }
            return;
        }
        const bool allBust=std::all_of(hands.begin(),hands.end(),[](const Hand& h){return h.result==Result::Bust;});
        if(allBust){holeShown=true;++moves;Settle();return;}
        phase=Phase::HerTurn;
    }

    void Table::Hit()
    {
        if(!CanHit())return;
        Hand& h=hands[static_cast<size_t>(active)];
        h.cards.push_back(Draw());
        const int t=Total(h.cards);
        if(t>21){h.result=Result::Bust;h.done=true;}
        else if(t==21)h.done=true;
        if(h.done)NextHand();
    }

    void Table::Stand()
    {
        if(!CanHit())return;
        hands[static_cast<size_t>(active)].done=true;
        NextHand();
    }

    void Table::Double()
    {
        if(!CanDouble())return;
        Hand& h=hands[static_cast<size_t>(active)];
        chips-=h.bet;
        h.bet*=2;
        h.doubled=true;
        h.cards.push_back(Draw());
        if(Total(h.cards)>21)h.result=Result::Bust;
        h.done=true;
        NextHand();
    }

    void Table::Split()
    {
        if(!CanSplit())return;
        Hand first=hands[0];
        chips-=first.bet;
        Hand second;
        second.bet=first.bet;
        second.cards={first.cards[1]};
        first.cards.pop_back();
        first.split=second.split=true;
        first.splitAces=second.splitAces=RankOf(first.cards[0])==1;
        hands={first,second};
        // The first hand's second card now; the other's when its turn comes.
        Hand& h=hands[0];
        h.cards.push_back(Draw());
        if(h.splitAces||Total(h.cards)==21){h.done=true;NextHand();}
    }

    void Table::Step()
    {
        if(phase!=Phase::HerTurn)return;
        if(!holeShown){holeShown=true;++moves;return;}
        if(Total(dealer)<17){dealer.push_back(Draw());return;}
        Settle();
    }

    void Table::Settle()
    {
        const int her=Total(dealer);
        const bool herNatural=Natural(dealer);
        int bets=0,paid=0;
        for(Hand& h:hands)
        {
            bets+=h.bet;
            const int you=Total(h.cards);
            const bool yourNatural=!h.split&&Natural(h.cards);
            if(h.result==Result::Bust)h.paid=0;
            else if(yourNatural&&herNatural){h.result=Result::Push;h.paid=h.bet;}
            else if(yourNatural){h.result=Result::Blackjack;h.paid=h.bet+h.bet*3/2;}
            else if(herNatural){h.result=Result::Lose;h.paid=0;}
            else if(you>21){h.result=Result::Bust;h.paid=0;}
            else if(her>21||you>her){h.result=Result::Win;h.paid=2*h.bet;}
            else if(you==her){h.result=Result::Push;h.paid=h.bet;}
            else {h.result=Result::Lose;h.paid=0;}
            h.done=true;
            paid+=h.paid;
        }
        chips+=paid;
        roundGain=paid-bets;
        phase=Phase::RoundOver;
    }

    void Table::NextRound()
    {
        if(phase!=Phase::RoundOver)return;
        for(const Hand& h:hands)discards.insert(discards.end(),h.cards.begin(),h.cards.end());
        discards.insert(discards.end(),dealer.begin(),dealer.end());
        hands.clear();
        dealer.clear();
        active=0;
        holeShown=false;
        ++moves;
        if(round>=Rounds){phase=Phase::GameOver;return;}
        ++round;
        // Out of chips: she lends the starting stack again.
        if(chips<Bets[0]){chips+=StartChips;++refills;}
        phase=Phase::Betting;
    }

    void Table::PlayForYou()
    {
        if(phase==Phase::Betting){Deal(std::min(chips,Bets[0]));return;}
        if(phase==Phase::RoundOver){NextRound();return;}
        if(phase!=Phase::YourTurn)return;
        // The usual basic strategy (one deck, she stands on all 17s), simplified.
        const Hand& h=hands[static_cast<size_t>(active)];
        const int up=Value(dealer[0]);
        bool soft=false;
        const int t=Total(h.cards,&soft);
        if(CanSplit())
        {
            const int v=Value(h.cards[0]);
            const bool split=v==11||v==8||((v==2||v==3||v==7)&&up<=7)||(v==6&&up<=6)||(v==9&&up<=9&&up!=7)||(v==4&&(up==5||up==6));
            if(split){Split();return;}
        }
        if(soft)
        {
            if(t>=19){Stand();return;}
            if(t==18){if(up>=3&&up<=6&&CanDouble())Double();else if(up<=8)Stand();else Hit();return;}
            if(up>=(t>=17?3:t>=15?4:5)&&up<=6&&CanDouble()){Double();return;}
            Hit();
            return;
        }
        if(t>=17){Stand();return;}
        if(t>=13){if(up<=6)Stand();else Hit();return;}
        if(t==12){if(up>=4&&up<=6)Stand();else Hit();return;}
        if(t==11&&up<=10&&CanDouble()){Double();return;}
        if(t==10&&up<=9&&CanDouble()){Double();return;}
        if(t==9&&up>=3&&up<=6&&CanDouble()){Double();return;}
        Hit();
    }

    bool Table::Consistent() const
    {
        std::vector<int> all(shoe);
        all.insert(all.end(),discards.begin(),discards.end());
        all.insert(all.end(),dealer.begin(),dealer.end());
        for(const Hand& h:hands)all.insert(all.end(),h.cards.begin(),h.cards.end());
        if(all.size()!=Cards)return false;
        std::sort(all.begin(),all.end());
        for(int i=0;i<Cards;++i)if(all[static_cast<size_t>(i)]!=i)return false;
        return chips>=0;
    }

    std::wstring CheckRules()
    {
        std::wostringstream lines;
        int failed=0;
        auto check=[&](bool ok,const wchar_t* what){if(!ok)++failed;lines<<what<<L": "<<(ok?L"ok":L"WRONG")<<L"\n";};
        // Cards by suit and rank (suits spade, heart, diamond, club).
        auto C=[](int suit,int rank){return suit*13+rank-1;};
        Table t;
        // Dealt in turn: you, her up card, you, her hidden card.
        t.NewGame(1);t.StackShoe({C(0,1),C(1,5),C(0,13),C(2,9)});t.Deal(10);
        check(t.Now()==Phase::RoundOver&&t.Hands()[0].result==Result::Blackjack&&t.Chips()==StartChips+15,L"your blackjack pays 3 to 2");
        t.NewGame(1);t.StackShoe({C(0,10),C(1,1),C(0,9),C(2,13)});t.Deal(10);
        check(t.Now()==Phase::RoundOver&&t.Hands()[0].result==Result::Lose&&t.Chips()==StartChips-10,L"her blackjack wins at once");
        t.NewGame(1);t.StackShoe({C(0,1),C(1,1),C(0,12),C(2,13)});t.Deal(10);
        check(t.Hands()[0].result==Result::Push&&t.Chips()==StartChips,L"two blackjacks push");
        t.NewGame(1);t.StackShoe({C(0,10),C(1,7),C(0,6),C(2,10),C(3,9)});t.Deal(10);t.Hit();
        check(t.Now()==Phase::RoundOver&&t.Hands()[0].result==Result::Bust&&t.Chips()==StartChips-10,L"over 21 loses");
        t.NewGame(1);t.StackShoe({C(0,10),C(1,10),C(0,8),C(2,6),C(3,2)});t.Deal(10);t.Stand();
        while(t.Now()==Phase::HerTurn)t.Step();
        check(t.Dealer().size()==3&&Total(t.Dealer())==18&&t.Hands()[0].result==Result::Push&&t.Chips()==StartChips,L"she draws on 16 (to 18); 18 to 18 pushes");
        t.NewGame(1);t.StackShoe({C(0,10),C(1,10),C(0,9),C(2,6),C(3,2)});t.Deal(10);t.Stand();
        while(t.Now()==Phase::HerTurn)t.Step();
        check(t.Hands()[0].result==Result::Win&&t.Chips()==StartChips+10,L"19 beats her 18");
        t.NewGame(1);t.StackShoe({C(0,10),C(1,10),C(0,7),C(2,7)});t.Deal(10);t.Stand();
        while(t.Now()==Phase::HerTurn)t.Step();
        check(t.Dealer().size()==2&&t.Hands()[0].result==Result::Push&&t.Chips()==StartChips,L"she stands on 17; 17 to 17 pushes");
        t.NewGame(1);t.StackShoe({C(0,10),C(1,10),C(0,6),C(2,6),C(3,10)});t.Deal(10);t.Stand();
        while(t.Now()==Phase::HerTurn)t.Step();
        check(Total(t.Dealer())==26&&t.Hands()[0].result==Result::Win,L"her bust loses to your 16");
        t.NewGame(1);t.StackShoe({C(0,5),C(1,10),C(0,6),C(2,7),C(3,10)});t.Deal(10);t.Double();
        while(t.Now()==Phase::HerTurn)t.Step();
        check(t.Hands()[0].doubled&&t.Hands()[0].cards.size()==3&&t.Hands()[0].result==Result::Win&&t.Chips()==StartChips+20,L"double: one card, twice the bet");
        t.NewGame(1);t.StackShoe({C(0,8),C(1,10),C(1,8),C(2,7),C(3,3),C(3,10)});t.Deal(10);
        check(t.CanSplit(),L"a pair may be split");
        t.Split();
        check(t.Hands().size()==2&&t.Hands()[0].cards.size()==2,L"split: two hands");
        t.Stand();t.Stand();
        while(t.Now()==Phase::HerTurn)t.Step();
        check(t.Now()==Phase::RoundOver&&t.Hands()[0].result==Result::Lose&&t.Hands()[1].result==Result::Win&&t.Chips()==StartChips,L"split hands settled each");
        t.NewGame(1);t.StackShoe({C(0,1),C(1,10),C(1,1),C(2,8),C(3,13),C(3,9)});t.Deal(10);t.Split();
        check(t.Now()==Phase::HerTurn&&t.Hands()[0].cards.size()==2&&t.Hands()[1].cards.size()==2,L"split aces get one card each");
        while(t.Now()==Phase::HerTurn)t.Step();
        check(t.Hands()[0].result==Result::Win&&t.Chips()==StartChips+20,L"21 after a split is no blackjack (pays 1 to 1)");
        {
            bool soft=false;
            const int v=Total({C(0,1),C(1,6)},&soft);
            check(v==17&&soft,L"ace and six is soft 17");
            check(Total({C(0,1),C(1,6),C(2,10)})==17,L"the ace counts 1 when 11 would go over");
            check(Total({C(0,1),C(1,1),C(2,9)})==21,L"two aces and nine make 21");
        }
        std::wostringstream out;
        out<<failed<<L" failed\n"<<lines.str();
        return out.str();
    }

    std::wstring CheckPlaying(int count,std::uint32_t seed)
    {
        std::wostringstream out;
        int wrong=0,hands=0,wins=0,blackjacks=0,pushes=0,losses=0,busts=0,refills=0;
        long long gain=0;
        for(int g=0;g<count;++g)
        {
            Table t;
            t.NewGame(seed+g*7919u);
            for(int step=0;step<2000&&t.Now()!=Phase::GameOver;++step)
            {
                if(t.Now()==Phase::HerTurn)t.Step();
                else
                {
                    if(t.Now()==Phase::RoundOver)
                        for(const Hand& h:t.Hands())
                        {
                            ++hands;
                            switch(h.result){case Result::Win:++wins;break;case Result::Blackjack:++blackjacks;break;case Result::Push:++pushes;break;
                                             case Result::Bust:++busts;break;default:++losses;break;}
                        }
                    t.PlayForYou();
                }
                if(!t.Consistent()){++wrong;break;}
            }
            if(t.Now()!=Phase::GameOver){++wrong;continue;}
            gain+=t.GameGain();
            refills+=t.Refills();
        }
        out<<wrong<<L" wrong\n"<<count<<L" games, "<<hands<<L" hands: won "<<wins<<L", blackjack "<<blackjacks<<L", push "<<pushes<<L", lost "<<losses<<L", bust "<<busts
           <<L"; chips "<<(gain>=0?L"+":L"")<<gain<<L" over all; out of chips "<<refills<<L" times\n";
        return out.str();
    }
}
