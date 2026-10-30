#include "OldMaid.hpp"
#include <algorithm>
#include <numeric>
#include <sstream>

namespace MikuMikuDesktopPerch::OldMaid
{
    void Table::NewGame(std::uint32_t seed,Side first)
    {
        rng.seed(seed);
        std::vector<int> order(Cards);
        std::iota(order.begin(),order.end(),0);
        order.back()=Joker;
        std::shuffle(order.begin(),order.end(),rng);
        Deal(order,first);
    }

    void Table::Deal(const std::vector<int>& order,Side first)
    {
        hands[You].clear();
        hands[Her].clear();
        discards.clear();
        for(size_t i=0;i<order.size();++i)hands[i%2==0?You:Her].push_back(order[i]);
        ThrowPairs(You);
        ThrowPairs(Her);
        SortYours();
        std::shuffle(hands[Her].begin(),hands[Her].end(),rng);
        moves=draws=0;
        lastCard=-1;
        lastDrawer=first==You?Her:You;
        lastPaired=false;
        phase=first==You?Phase::YourDraw:Phase::HerDraw;
        CheckEnd();
    }

    void Table::ThrowPairs(Side side)
    {
        // Two of a rank thrown out together, as many pairs as there are (the joker has none).
        auto& h=hands[side];
        std::vector<int> kept;
        for(int c:h)
        {
            const auto twin=std::find_if(kept.begin(),kept.end(),[&](int k){return k!=Joker&&c!=Joker&&RankOf(k)==RankOf(c);});
            if(twin!=kept.end())
            {
                discards.push_back(*twin);
                discards.push_back(c);
                kept.erase(twin);
            }
            else kept.push_back(c);
        }
        h=std::move(kept);
    }

    void Table::SortYours()
    {
        // By rank (the ace first), the joker left where it is.
        auto& h=hands[You];
        const auto joker=std::find(h.begin(),h.end(),Joker);
        const bool had=joker!=h.end();
        const size_t at=static_cast<size_t>(joker-h.begin());
        if(had)h.erase(joker);
        std::sort(h.begin(),h.end(),[](int a,int b){return RankOf(a)!=RankOf(b)?RankOf(a)<RankOf(b):a<b;});
        if(had)h.insert(h.begin()+static_cast<std::ptrdiff_t>(std::min(at,h.size())),Joker);
    }

    Side Table::JokerHolder() const
    {
        return std::find(hands[Her].begin(),hands[Her].end(),Joker)!=hands[Her].end()?Her:You;
    }

    void Table::CheckEnd()
    {
        if(hands[You].empty()||hands[Her].empty())phase=Phase::GameOver;
    }

    void Table::Draw(int index)
    {
        if(phase==Phase::GameOver)return;
        const Side me=phase==Phase::YourDraw?You:Her,other=me==You?Her:You;
        auto& from=hands[other];
        if(index<0||index>=static_cast<int>(from.size()))return;
        const int card=from[static_cast<size_t>(index)];
        from.erase(from.begin()+index);
        auto& to=hands[me];
        const auto twin=std::find_if(to.begin(),to.end(),[&](int k){return card!=Joker&&k!=Joker&&RankOf(k)==RankOf(card);});
        lastPaired=twin!=to.end();
        if(lastPaired)
        {
            discards.push_back(*twin);
            discards.push_back(card);
            to.erase(twin);
        }
        else if(me==You)
        {
            // A card kept in your hand goes in at random (it is only ever the joker).
            std::uniform_int_distribution<size_t> at(0,to.size());
            to.insert(to.begin()+static_cast<std::ptrdiff_t>(at(rng)),card);
        }
        else to.push_back(card);
        lastCard=card;
        lastDrawer=me;
        ++moves;
        ++draws;
        phase=me==You?Phase::HerDraw:Phase::YourDraw;
        CheckEnd();
    }

    int Table::HerPick()
    {
        if(hands[You].empty())return -1;
        std::uniform_int_distribution<int> any(0,static_cast<int>(hands[You].size())-1);
        return any(rng);
    }

    void Table::ShuffleHer()
    {
        if(hands[Her].size()<2)return;
        std::shuffle(hands[Her].begin(),hands[Her].end(),rng);
        ++moves;
    }

    void Table::PlayForYou()
    {
        if(phase==Phase::YourDraw)
        {
            std::uniform_int_distribution<int> any(0,static_cast<int>(hands[Her].size())-1);
            Draw(any(rng));
        }
        else if(phase==Phase::HerDraw)Draw(HerPick());
    }

    bool Table::Consistent() const
    {
        std::vector<int> all=discards;
        for(const auto& h:hands)all.insert(all.end(),h.begin(),h.end());
        if(all.size()!=Cards)return false;
        std::sort(all.begin(),all.end());
        for(int i=0;i<52;++i)if(all[static_cast<size_t>(i)]!=i)return false;
        if(all.back()!=Joker)return false;
        // No pair left in a hand; the discards in pairs of a rank.
        for(const auto& h:hands)
            for(size_t i=0;i<h.size();++i)
                for(size_t j=i+1;j<h.size();++j)
                    if(h[i]!=Joker&&h[j]!=Joker&&RankOf(h[i])==RankOf(h[j]))return false;
        for(size_t i=0;i+1<discards.size();i+=2)if(RankOf(discards[i])!=RankOf(discards[i+1])||discards[i]==Joker)return false;
        return discards.size()%2==0;
    }

    std::wstring CheckRules()
    {
        std::wostringstream lines;
        int failed=0;
        auto check=[&](bool ok,const wchar_t* what){if(!ok)++failed;lines<<what<<L": "<<(ok?L"ok":L"WRONG")<<L"\n";};
        auto C=[](int suit,int rank){return suit*13+rank-1;};
        Table t;
        {
            // A shuffled deal (this one leaves cards in both hands).
            t.NewGame(7,You);
            check(t.Consistent(),L"the deal is consistent");
            bool none=true;
            for(Side s:{You,Her})
            {
                const auto& h=t.Hand(s);
                for(size_t i=0;i<h.size();++i)for(size_t j=i+1;j<h.size();++j)
                    if(h[i]!=Joker&&RankOf(h[i])==RankOf(h[j]))none=false;
            }
            check(none,L"no pair left in a hand after the deal");
            // Each rank of yours is in her hand too (and the other way), but for the joker.
            std::vector<int> a,b;
            for(int c:t.Hand(You))if(c!=Joker)a.push_back(RankOf(c));
            for(int c:t.Hand(Her))if(c!=Joker)b.push_back(RankOf(c));
            std::sort(a.begin(),a.end());std::sort(b.begin(),b.end());
            check(!a.empty()&&a==b,L"the hands match rank for rank but for the joker");
            check(t.Now()==Phase::YourDraw,L"you draw first when told");
        }
        {
            // You: A♠ 2♠ joker; her: A♥ 2♥ (the rest thrown out beforehand).
            std::vector<int> order={C(0,1),C(1,1),C(0,2),C(1,2),Joker};
            t.Deal(order,You);
            check(t.Hand(You).size()==3&&t.Hand(Her).size()==2,L"a small deal");
            const auto& her=t.Hand(Her);
            const int at=static_cast<int>(std::find(her.begin(),her.end(),C(1,1))-her.begin());
            t.Draw(at);
            check(t.LastPaired()&&t.Hand(You).size()==2&&t.Discards().size()==2&&t.Now()==Phase::HerDraw,L"a drawn card that pairs is thrown out with its twin");
            const auto& you=t.Hand(You);
            t.Draw(static_cast<int>(std::find(you.begin(),you.end(),Joker)-you.begin()));
            check(!t.LastPaired()&&t.JokerHolder()==Her&&t.Hand(Her).size()==2&&t.Now()==Phase::YourDraw,L"the joker drawn is kept");
            const auto& her2=t.Hand(Her);
            t.Draw(static_cast<int>(std::find(her2.begin(),her2.end(),C(1,2))-her2.begin()));
            check(t.Now()==Phase::GameOver&&t.Hand(You).empty()&&t.Loser()==Her,L"no cards left: the one with the joker loses");
        }
        {
            // Every pair thrown out at the deal but for the joker's: the game is over at once.
            std::vector<int> order;
            for(int r=1;r<=13;++r)for(int s=0;s<4;++s)order.push_back(C(s,r));
            order.push_back(Joker);
            t.Deal(order,Her);
            check(t.Now()==Phase::GameOver&&t.Hand(Her).empty()&&t.Loser()==You,L"one hand empty from the start: over at once");
        }
        std::wostringstream out;
        out<<failed<<L" failed\n"<<lines.str();
        return out.str();
    }

    std::wstring CheckPlaying(int count,std::uint32_t seed)
    {
        std::wostringstream out;
        int wrong=0,youLost=0,firstLost=0,longest=0;
        long long draws=0,dealt=0;
        for(int g=0;g<count;++g)
        {
            Table t;
            const Side first=g%2?Her:You;
            t.NewGame(seed+g*7919u,first);
            dealt+=t.Hand(You).size()+t.Hand(Her).size();
            if(!t.Consistent()){++wrong;continue;}
            for(int step=0;step<500&&t.Now()!=Phase::GameOver;++step)
            {
                t.PlayForYou();
                if(t.Now()==Phase::YourDraw)t.ShuffleHer();
                if(!t.Consistent()){++wrong;break;}
            }
            if(t.Now()!=Phase::GameOver){++wrong;continue;}
            draws+=t.Draws();
            longest=std::max(longest,t.Draws());
            if(t.Loser()==You)++youLost;
            if(t.Loser()==first)++firstLost;
        }
        out<<wrong<<L" wrong\n"<<count<<L" games: you lost "<<youLost<<L", the first to draw lost "<<firstLost
           <<L"; cards left after the deal "<<(count?dealt/count:0)<<L" a game, draws "<<(count?draws/count:0)<<L" (longest "<<longest<<L")\n";
        return out.str();
    }
}
