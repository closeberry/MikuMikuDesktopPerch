#include "Forts.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace MikuMikuDesktopPerch::Forts
{
    namespace
    {
        // A soldier costing c has 2c+1 points to spend: attack + health + 1 for Guard or Scout, 3
        // for Tough, 2 for any other word (docs/Forts.md).
        const Card Cards[]=
        {
            // Common
            {L"見習い兵",L"Recruit",L"",L"",1,1,2,false,0,Effect::None,0,0},
            {L"荷運びの驢馬",L"Pack Donkey",L"倒れたとき：1枚引く",L"When it falls: draw a card",1,0,2,false,0,Effect::DrawOnDeath,1,0},
            {L"旅の傭兵",L"Wandering Sword",L"",L"",2,3,2,false,0,Effect::None,0,0},
            {L"伝令",L"Courier",L"出陣：1枚引く",L"On arrival: draw a card",2,1,2,false,0,Effect::DrawOnPlay,1,0},
            {L"街道の番人",L"Road Warden",L"守護",L"Guard",3,2,4,false,Guard,Effect::None,0,0},
            {L"鉄の大男",L"Iron Giant",L"",L"",5,5,6,false,0,Effect::None,0,0},
            // Knights
            {L"盾持ち",L"Shield Bearer",L"守護",L"Guard",1,1,1,false,Guard,Effect::None,0,1},
            {L"防塁",L"Rampart",L"砦1つ：次の自分の番まで、味方の受けるダメージ-1",L"A fort: your soldiers take 1 less until your next turn",1,0,0,true,0,Effect::ShieldFort,1,1},
            {L"旗持ちの従者",L"Standard Page",L"旗手",L"Banner",2,1,2,false,Banner,Effect::None,0,1},
            {L"弓の見張り",L"Tower Archer",L"遠射",L"Ranged",2,2,1,false,Ranged,Effect::None,0,1},
            {L"号令",L"War Cry",L"砦1つ：この番、味方の攻撃+1",L"A fort: your soldiers get +1 attack this turn",2,0,0,true,0,Effect::RallyFort,1,1},
            {L"鉄壁の騎士",L"Bulwark Knight",L"堅固",L"Tough",3,2,2,false,Tough,Effect::None,0,1},
            {L"軍馬の騎士",L"Charger Knight",L"疾駆",L"Swift",3,3,2,false,Haste,Effect::None,0,1},
            {L"癒しの修道女",L"Healing Sister",L"出陣：味方1体の体力を全回復",L"On arrival: an ally heals fully",3,2,3,false,0,Effect::HealAlly,0,1},
            {L"城門の巨兵",L"Gate Colossus",L"守護 堅固",L"Guard, Tough",4,2,3,false,Guard|Tough,Effect::None,0,1},
            {L"騎士団長",L"Knight Marshal",L"旗手 出陣：同じ砦の味方に+1/+1",L"Banner. On arrival: others at its fort +1/+1",6,4,6,false,Banner,Effect::BuffFort,1,1},
            // Forest
            {L"森鼠",L"Wood Rat",L"",L"",1,2,2,false,0,Effect::None,0,2},
            {L"森の斥候",L"Forest Scout",L"移動のあと攻撃もできる",L"Can move, then attack",1,1,1,false,Scout,Effect::None,0,2},
            {L"奇襲",L"Ambush",L"味方1体を好きな砦へ移し、この番動けるようにする",L"An ally goes to any fort, ready to act",1,0,0,true,0,Effect::Ambush,0,2},
            {L"森狼",L"Timber Wolf",L"疾駆",L"Swift",2,2,1,false,Haste,Effect::None,0,2},
            {L"うさぎの群れ",L"Warren of Hares",L"出陣：隣の砦に1/1のうさぎを置く",L"On arrival: a 1/1 hare at the next fort",1,1,1,false,0,Effect::SummonNext,0,2},
            {L"木渡りの射手",L"Treetop Archer",L"遠射",L"Ranged",2,1,2,false,Ranged,Effect::None,0,2},
            {L"大鷲",L"Great Eagle",L"飛翔",L"Flying",3,3,3,false,Fly,Effect::None,0,2},
            {L"花畑の賢者",L"Meadow Sage",L"旗手",L"Banner",3,2,3,false,Banner,Effect::None,0,2},
            {L"角獣",L"Horned Beast",L"疾駆",L"Swift",4,4,4,false,Haste,Effect::None,0,2},
            {L"森の大精霊",L"Grove Spirit",L"出陣：ほかの味方全員の攻撃+1",L"On arrival: all your other soldiers +1 attack",5,4,5,false,0,Effect::RaiseAll,1,2},
            // Mages
            {L"火花の見習い",L"Spark Apprentice",L"出陣：敵1体に1ダメージ",L"On arrival: 1 damage to an enemy",1,1,1,false,0,Effect::HitEnemy,1,3},
            {L"火の矢",L"Fire Arrow",L"敵1体に2ダメージ",L"2 damage to an enemy",1,0,0,true,0,Effect::HitEnemy,2,3},
            {L"護符人形",L"Charm Doll",L"守護",L"Guard",2,0,4,false,Guard,Effect::None,0,3},
            {L"氷結",L"Frost",L"敵1体は次の番、動けない",L"An enemy can't act next turn",2,0,0,true,0,Effect::Freeze,0,3},
            {L"風の転移",L"Gust",L"敵1体を隣の砦へ押し出す",L"An enemy goes to the next fort",2,0,0,true,0,Effect::Push,0,3},
            {L"知恵の書",L"Book of Wits",L"2枚引く",L"Draw two cards",3,0,0,true,0,Effect::DrawOnPlay,2,3},
            {L"箒の魔女",L"Broom Witch",L"飛翔",L"Flying",3,2,3,false,Fly,Effect::None,0,3},
            {L"砦の司書",L"Fort Librarian",L"旗手",L"Banner",4,2,5,false,Banner,Effect::None,0,3},
            {L"炎の嵐",L"Firestorm",L"砦1つ：敵全員に3ダメージ",L"A fort: 3 damage to each enemy there",5,0,0,true,0,Effect::Storm,3,3},
            {L"古竜",L"Elder Dragon",L"飛翔",L"Flying",7,6,7,false,Fly,Effect::None,0,3},
            // Not in a deck
            {L"うさぎ",L"Hare",L"",L"",0,1,1,false,0,Effect::None,0,-1},
        };
        constexpr int HareCard=36;
        constexpr int KindCount=static_cast<int>(sizeof(Cards)/sizeof(Cards[0]));

        bool Has(const Unit& u,Keyword word){return (CardOf(u.card).keywords&word)!=0;}
        int NextDoorCount(int fort){return fort==1?2:1;}
        int NextDoor(int fort,int i){return fort==1?(i==0?0:2):1;}
        bool Beside(int a,int b){return std::abs(a-b)==1;}

        void Say(std::vector<Event>* log,const Event& e){if(log)log->push_back(e);}

        void DrawCards(Side& side,int count)
        {
            for(int i=0;i<count&&side.deckLeft>0;i++)
            {
                side.deckLeft--;
                side.pending++;
            }
        }

        void RemoveAt(Side& side,int fort,int at)
        {
            for(int i=at;i+1<side.count[fort];i++)side.units[fort][i]=side.units[fort][i+1];
            side.count[fort]--;
        }

        int Reduced(const State& s,int who,int fort,const Unit& u,int amount)
        {
            int less=Has(u,Tough)?1:0;
            if(s.side[who].shield[fort])less++;
            return std::max(0,amount-less);
        }

        void Bury(State& s,int who,int fort,std::vector<Event>* log)
        {
            Side& side=s.side[who];
            for(int i=side.count[fort]-1;i>=0;i--)
            {
                const Unit u=side.units[fort][i];
                if(u.health>0)continue;
                Event e;
                e.kind=EventKind::Fell;
                e.who=static_cast<std::int8_t>(who);
                e.fort=static_cast<std::int8_t>(fort);
                e.card=u.card;
                e.unit=u.uid;
                Say(log,e);
                RemoveAt(side,fort,i);
                const Card& card=CardOf(u.card);
                if(card.effect==Effect::DrawOnDeath)DrawCards(side,card.value);
            }
        }

        bool Locate(const State& s,int uid,int& who,int& fort,int& at)
        {
            for(who=0;who<2;who++)
                for(fort=0;fort<FortCount;fort++)
                    for(at=0;at<s.side[who].count[fort];at++)
                        if(s.side[who].units[fort][at].uid==uid)return true;
            return false;
        }

        void StartTurn(State& s,int who,std::vector<Event>* log)
        {
            Side& side=s.side[who];
            s.who=static_cast<std::int8_t>(who);
            Event start;
            start.kind=EventKind::TurnStart;
            start.who=static_cast<std::int8_t>(who);
            Say(log,start);
            side.shield={};
            for(int f=0;f<FortCount;f++)
            {
                if(!Holds(s,who,f))continue;
                const int points=FortScore(s,who,f);
                side.score=static_cast<std::int8_t>(side.score+points);
                Event e;
                e.kind=EventKind::Scored;
                e.who=static_cast<std::int8_t>(who);
                e.fort=static_cast<std::int8_t>(f);
                e.amount=static_cast<std::int8_t>(points);
                Say(log,e);
                if(side.score>=Target)
                {
                    s.over=true;
                    s.winner=static_cast<std::int8_t>(who);
                    return;
                }
            }
            side.maxMagic=static_cast<std::int8_t>(std::min<int>(MaxMagic,side.maxMagic+1));
            side.magic=side.maxMagic;
            side.offered=false;
            for(int f=0;f<FortCount;f++)
                for(int i=0;i<side.count[f];i++)
                {
                    Unit& u=side.units[f][i];
                    u.ready=!u.frozen;
                    u.moved=false;
                    u.frozen=false;
                }
            DrawCards(side,1);
        }

        // ---- what she thinks a board is worth, for the side me ----

        double Worth(const Unit& u)
        {
            const std::uint8_t k=CardOf(u.card).keywords;
            double w=u.attack+0.8*u.health;
            if(k&Guard)w+=0.5;
            if(k&Fly)w+=1;
            if(k&Ranged)w+=1;
            if(k&Tough)w+=1;
            if(k&Banner)w+=1;
            if(u.frozen)w-=0.5;
            return w;
        }

        double Judge(const State& s,int me)
        {
            if(s.over)return s.winner==me?100000.0+s.side[me].score:s.winner==2?0.0:-100000.0;
            const int you=1-me;
            const Side& a=s.side[me];
            const Side& b=s.side[you];
            double q=14.0*(a.score-b.score);
            for(int f=0;f<FortCount;f++)
            {
                double powerA=0,powerB=0;
                for(int i=0;i<a.count[f];i++)
                {
                    q+=Worth(a.units[f][i]);
                    powerA+=a.units[f][i].attack+a.units[f][i].health;
                }
                for(int i=0;i<b.count[f];i++)
                {
                    q-=Worth(b.units[f][i]);
                    powerB+=b.units[f][i].attack+b.units[f][i].health;
                }
                if(a.count[f]&&!b.count[f])
                {
                    q+=FortScore(s,me,f)*6.0;
                    if(a.aim==f)q+=1.5;
                }
                else if(b.count[f]&&!a.count[f])q-=FortScore(s,you,f)*6.0;
                else if(a.count[f])q+=std::clamp(powerA-powerB,-6.0,6.0)*0.25*FortValue[f];
            }
            q+=2.0*(a.handSize+a.pending)-2.0*b.pending;
            if(a.spare)q+=0.4;
            return q;
        }

        State Seen(const State& s)
        {
            State view=s;
            view.side[1-s.who].handSize=0;
            view.side[1-s.who].pending=0;
            return view;
        }

        // Cards not known are drawn at random from what that deck holds.
        int Guess(int deck,std::mt19937& dice)
        {
            const std::vector<int>& list=[&]()->const std::vector<int>&{
                static const std::array<std::vector<int>,DeckKinds> decks{DeckOf(0),DeckOf(1),DeckOf(2)};
                return decks[deck];
            }();
            return list[dice()%list.size()];
        }

        void FillDrawn(State& s,std::mt19937& dice)
        {
            for(int who=0;who<2;who++)
            {
                Side& side=s.side[who];
                for(;side.pending>0;side.pending--)
                    if(side.handSize<MaxHand)side.hand[side.handSize++]=static_cast<std::int8_t>(Guess(side.deck,dice));
            }
        }

        Action BestStep(const State& s)
        {
            const int me=s.who;
            double best=Judge(s,me);
            Action chosen;
            for(const Action& a:Legal(s))
            {
                if(a.kind==ActionKind::End)continue;
                State next=s;
                Apply(next,a);
                const double q=Judge(next,me);
                if(q>best+1e-9){best=q;chosen=a;}
            }
            return chosen;
        }

        // A few turns played out quickly from here, each side by its simple judgment.
        double PlayOut(State s,int me,int turns,std::mt19937& dice)
        {
            FillDrawn(s,dice);
            for(int t=0;t<turns&&!s.over;t++)
                for(int step=0;step<14&&!s.over;step++)
                {
                    const Action a=BestStep(s);
                    Apply(s,a);
                    FillDrawn(s,dice);
                    if(a.kind==ActionKind::End)break;
                }
            return Judge(s,me);
        }
    }

    int CardKinds(){return KindCount;}
    const Card& CardOf(int id){return Cards[id];}

    int GivesMagic(const Card& card)
    {
        return card.cost<=2?1:card.cost<=4?2:3;
    }

    const wchar_t* DeckName(int kind,bool english)
    {
        static const wchar_t* const names[DeckKinds][2]={{L"騎士団",L"Knights"},{L"森の民",L"Forest Folk"},{L"魔術院",L"Mage College"}};
        return names[kind][english?1:0];
    }

    std::vector<int> DeckOf(int kind)
    {
        // Five of the common cards each, and the ten of its own: two of everything.
        static const int common[DeckKinds][5]={{0,1,2,3,5},{0,1,2,3,4},{0,1,3,4,5}};
        std::vector<int> deck;
        for(int i=0;i<5;i++){deck.push_back(common[kind][i]);deck.push_back(common[kind][i]);}
        for(int id=0;id<KindCount;id++)
            if(Cards[id].faction==kind+1){deck.push_back(id);deck.push_back(id);}
        return deck;
    }

    std::vector<int> CollectableCards()
    {
        std::vector<int> out=DeckOf(CollectedDeck);
        out.erase(std::remove_if(out.begin(),out.end(),[](int id){return Cards[id].faction!=CollectedDeck+1;}),out.end());
        return out;
    }

    std::vector<int> CollectedDeckCards(const std::vector<int>& owned)
    {
        std::vector<int> out=DeckOf(CollectedDeck);
        out.erase(std::remove_if(out.begin(),out.end(),[](int id){return Cards[id].faction==CollectedDeck+1;}),out.end());
        out.insert(out.end(),owned.begin(),owned.end());
        return out;
    }

    int Collect(std::vector<int>& owned,std::mt19937& dice)
    {
        std::vector<int> left=CollectableCards();
        for(int id:owned)
            if(const auto k=std::find(left.begin(),left.end(),id);k!=left.end())left.erase(k);
        if(left.empty())return -1;
        const int id=left[dice()%left.size()];
        owned.push_back(id);
        return id;
    }

    Needs NeedsOf(const Card& card)
    {
        switch(card.effect)
        {
        case Effect::HealAlly:case Effect::Ambush:return Needs::Ally;
        case Effect::HitEnemy:case Effect::Freeze:case Effect::Push:return Needs::Enemy;
        case Effect::ShieldFort:case Effect::RallyFort:case Effect::Storm:return Needs::Fort;
        default:return Needs::Nothing;
        }
    }

    const Unit* FindUnit(const State& state,int uid,int* side,int* fort)
    {
        int who=0,f=0,at=0;
        if(!Locate(state,uid,who,f,at))return nullptr;
        if(side)*side=who;
        if(fort)*fort=f;
        return &state.side[who].units[f][at];
    }

    bool Holds(const State& s,int who,int fort){return s.side[who].count[fort]>0&&s.side[1-who].count[fort]==0;}

    int FortScore(const State& s,int who,int fort)
    {
        int points=FortValue[fort];
        const Side& side=s.side[who];
        for(int i=0;i<side.count[fort];i++)
            if(Has(side.units[fort][i],Banner)){points++;break;}
        return points;
    }

    std::vector<Action> Legal(const State& s)
    {
        std::vector<Action> out;
        out.reserve(40);
        out.push_back(Action{});
        if(s.over)return out;
        const int me=s.who,you=1-me;
        const Side& a=s.side[me];
        const Side& b=s.side[you];
        for(int h=0;h<a.handSize;h++)
        {
            const Card& card=CardOf(a.hand[h]);
            if(!a.offered)
            {
                Action give;
                give.kind=ActionKind::Offer;
                give.hand=static_cast<std::int8_t>(h);
                out.push_back(give);
            }
            if(card.cost>a.magic)continue;
            Action play;
            play.kind=ActionKind::Play;
            play.hand=static_cast<std::int8_t>(h);
            const Needs needs=NeedsOf(card);
            const auto aimed=[&](Action base,int fortOf)
            {
                // Soldiers with no one to aim at still come; a spell needs its target.
                bool any=false;
                if(needs==Needs::Ally)
                    for(int f=0;f<FortCount;f++)
                        for(int i=0;i<a.count[f];i++)
                        {
                            if(card.effect==Effect::Ambush)
                            {
                                for(int d=0;d<FortCount;d++)
                                    if(d!=f&&a.count[d]<PerFort)
                                    {
                                        Action x=base;
                                        x.target=a.units[f][i].uid;
                                        x.dest=static_cast<std::int8_t>(d);
                                        out.push_back(x);
                                        any=true;
                                    }
                                // Staying where it is, ready to act.
                                Action x=base;
                                x.target=a.units[f][i].uid;
                                x.dest=static_cast<std::int8_t>(f);
                                out.push_back(x);
                                any=true;
                            }
                            else
                            {
                                Action x=base;
                                x.target=a.units[f][i].uid;
                                out.push_back(x);
                                any=true;
                            }
                        }
                else if(needs==Needs::Enemy)
                    for(int f=0;f<FortCount;f++)
                        for(int i=0;i<b.count[f];i++)
                        {
                            if(card.effect==Effect::Push)
                            {
                                for(int n=0;n<NextDoorCount(f);n++)
                                {
                                    const int d=NextDoor(f,n);
                                    if(b.count[d]>=PerFort)continue;
                                    Action x=base;
                                    x.target=b.units[f][i].uid;
                                    x.dest=static_cast<std::int8_t>(d);
                                    out.push_back(x);
                                    any=true;
                                }
                            }
                            else
                            {
                                Action x=base;
                                x.target=b.units[f][i].uid;
                                out.push_back(x);
                                any=true;
                            }
                        }
                else if(needs==Needs::Fort)
                    for(int f=0;f<FortCount;f++)
                    {
                        // Only where it does something.
                        if(card.effect==Effect::Storm?b.count[f]==0:a.count[f]==0)continue;
                        Action x=base;
                        x.fort=static_cast<std::int8_t>(f);
                        out.push_back(x);
                        any=true;
                    }
                else if(card.effect==Effect::SummonNext)
                    for(int n=0;n<NextDoorCount(fortOf);n++)
                    {
                        const int d=NextDoor(fortOf,n);
                        if(a.count[d]>=PerFort)continue;
                        Action x=base;
                        x.dest=static_cast<std::int8_t>(d);
                        out.push_back(x);
                        any=true;
                    }
                else
                {
                    out.push_back(base);
                    any=true;
                }
                if(!any&&!card.spell)out.push_back(base);
            };
            if(card.spell)aimed(play,-1);
            else
                for(int f=0;f<FortCount;f++)
                {
                    if(a.count[f]>=PerFort)continue;
                    Action x=play;
                    x.fort=static_cast<std::int8_t>(f);
                    aimed(x,f);
                }
        }
        if(a.spare)
        {
            Action x;
            x.kind=ActionKind::Spare;
            out.push_back(x);
        }
        for(int f=0;f<FortCount;f++)
        {
            bool guarded=false;
            for(int i=0;i<b.count[f];i++)
                if(Has(b.units[f][i],Guard))guarded=true;
            for(int i=0;i<a.count[f];i++)
            {
                const Unit& u=a.units[f][i];
                if(!u.ready)continue;
                if(!u.moved)
                    for(int n=0;n<NextDoorCount(f);n++)
                    {
                        const int d=NextDoor(f,n);
                        if(a.count[d]>=PerFort)continue;
                        Action x;
                        x.kind=ActionKind::Move;
                        x.unit=u.uid;
                        x.dest=static_cast<std::int8_t>(d);
                        out.push_back(x);
                    }
                if(u.attack+u.rally<=0)continue;
                for(int t=0;t<b.count[f];t++)
                {
                    if(guarded&&!Has(u,Fly)&&!Has(b.units[f][t],Guard))continue;
                    Action x;
                    x.kind=ActionKind::Attack;
                    x.unit=u.uid;
                    x.target=b.units[f][t].uid;
                    out.push_back(x);
                }
            }
        }
        return out;
    }

    void Apply(State& s,const Action& action,std::vector<Event>* log)
    {
        if(s.over)return;
        const int me=s.who,you=1-me;
        Side& a=s.side[me];
        Side& b=s.side[you];
        Event e;
        e.who=static_cast<std::int8_t>(me);
        switch(action.kind)
        {
        case ActionKind::End:
            for(int f=0;f<FortCount;f++)
                for(int i=0;i<a.count[f];i++)a.units[f][i].rally=0;
            s.turns++;
            if(s.turns>=TurnLimit)
            {
                s.over=true;
                s.winner=a.score==b.score?2:a.score>b.score?static_cast<std::int8_t>(me):static_cast<std::int8_t>(you);
                return;
            }
            StartTurn(s,you,log);
            return;
        case ActionKind::Spare:
            a.spare=false;
            a.magic++;
            e.kind=EventKind::Spare;
            Say(log,e);
            return;
        case ActionKind::Offer:
        {
            const int id=a.hand[action.hand];
            for(int i=action.hand;i+1<a.handSize;i++)a.hand[i]=a.hand[i+1];
            a.handSize--;
            e.amount=static_cast<std::int8_t>(GivesMagic(CardOf(id)));
            a.magic=static_cast<std::int8_t>(a.magic+e.amount);
            a.offered=true;
            e.kind=EventKind::Offered;
            e.card=static_cast<std::int8_t>(id);
            Say(log,e);
            return;
        }
        case ActionKind::Move:
        {
            int who=0,f=0,at=0;
            if(!Locate(s,action.unit,who,f,at))return;
            Unit u=a.units[f][at];
            RemoveAt(a,f,at);
            if(Has(u,Scout))u.moved=true;
            else u.ready=false;
            a.units[action.dest][a.count[action.dest]++]=u;
            e.kind=EventKind::Moved;
            e.unit=u.uid;
            e.card=u.card;
            e.fort=static_cast<std::int8_t>(f);
            e.dest=action.dest;
            Say(log,e);
            return;
        }
        case ActionKind::Attack:
        {
            int w1=0,f1=0,at1=0,w2=0,f2=0,at2=0;
            if(!Locate(s,action.unit,w1,f1,at1)||!Locate(s,action.target,w2,f2,at2))return;
            Unit& u=a.units[f1][at1];
            Unit& t=b.units[f2][at2];
            const int dealt=Reduced(s,you,f2,t,u.attack+u.rally);
            const int back=Has(u,Ranged)?0:Reduced(s,me,f1,u,t.attack);
            u.ready=false;
            t.health=static_cast<std::int8_t>(t.health-dealt);
            u.health=static_cast<std::int8_t>(u.health-back);
            e.kind=EventKind::Hit;
            e.unit=u.uid;
            e.target=t.uid;
            e.card=u.card;
            e.fort=static_cast<std::int8_t>(f1);
            e.amount=static_cast<std::int8_t>(dealt);
            e.back=static_cast<std::int8_t>(back);
            Say(log,e);
            Bury(s,you,f2,log);
            Bury(s,me,f1,log);
            return;
        }
        case ActionKind::Play:
            break;
        }
        // Play.
        const int id=a.hand[action.hand];
        const Card& card=CardOf(id);
        for(int i=action.hand;i+1<a.handSize;i++)a.hand[i]=a.hand[i+1];
        a.handSize--;
        a.magic=static_cast<std::int8_t>(a.magic-card.cost);
        e.card=static_cast<std::int8_t>(id);
        e.fort=action.fort;
        e.dest=action.dest;
        e.target=action.target;
        int here=action.fort;
        if(card.spell)
        {
            e.kind=EventKind::Spell;
            Say(log,e);
        }
        else
        {
            Unit u;
            u.uid=s.nextUid++;
            u.card=static_cast<std::int8_t>(id);
            u.attack=card.attack;
            u.health=card.health;
            u.maxHealth=card.health;
            u.ready=(card.keywords&Haste)!=0;
            a.units[here][a.count[here]++]=u;
            e.kind=EventKind::Placed;
            e.unit=u.uid;
            Say(log,e);
        }
        int w=0,f=0,at=0;
        const bool found=action.target>=0&&Locate(s,action.target,w,f,at);
        Event fx;
        fx.who=static_cast<std::int8_t>(me);
        fx.card=static_cast<std::int8_t>(id);
        switch(card.effect)
        {
        case Effect::DrawOnPlay:
            DrawCards(a,card.value);
            break;
        case Effect::HealAlly:
            if(found)
            {
                Unit& u=a.units[f][at];
                u.health=u.maxHealth;
                fx.kind=EventKind::Healed;
                fx.unit=u.uid;
                fx.fort=static_cast<std::int8_t>(f);
                Say(log,fx);
            }
            break;
        case Effect::ShieldFort:
            a.shield[action.fort]=true;
            fx.kind=EventKind::Shield;
            fx.fort=action.fort;
            Say(log,fx);
            break;
        case Effect::RallyFort:
            for(int i=0;i<a.count[action.fort];i++)
            {
                Unit& u=a.units[action.fort][i];
                u.rally=static_cast<std::int8_t>(u.rally+card.value);
                fx.kind=EventKind::Buffed;
                fx.unit=u.uid;
                fx.fort=action.fort;
                Say(log,fx);
            }
            break;
        case Effect::BuffFort:
            for(int i=0;i<a.count[here]-1;i++)
            {
                Unit& u=a.units[here][i];
                u.attack=static_cast<std::int8_t>(u.attack+card.value);
                u.health=static_cast<std::int8_t>(u.health+card.value);
                u.maxHealth=static_cast<std::int8_t>(u.maxHealth+card.value);
                fx.kind=EventKind::Buffed;
                fx.unit=u.uid;
                fx.fort=static_cast<std::int8_t>(here);
                Say(log,fx);
            }
            break;
        case Effect::SummonNext:
            if(action.dest>=0&&a.count[action.dest]<PerFort)
            {
                Unit u;
                u.uid=s.nextUid++;
                u.card=HareCard;
                u.attack=CardOf(HareCard).attack;
                u.health=CardOf(HareCard).health;
                u.maxHealth=u.health;
                a.units[action.dest][a.count[action.dest]++]=u;
                fx.kind=EventKind::Placed;
                fx.card=HareCard;
                fx.unit=u.uid;
                fx.fort=action.dest;
                Say(log,fx);
            }
            break;
        case Effect::Ambush:
            if(found)
            {
                Unit u=a.units[f][at];
                u.ready=true;
                u.moved=true;
                u.frozen=false;
                if(action.dest!=f)
                {
                    RemoveAt(a,f,at);
                    a.units[action.dest][a.count[action.dest]++]=u;
                    fx.kind=EventKind::Moved;
                    fx.unit=u.uid;
                    fx.card=u.card;
                    fx.fort=static_cast<std::int8_t>(f);
                    fx.dest=action.dest;
                    Say(log,fx);
                }
                else a.units[f][at]=u;
            }
            break;
        case Effect::RaiseAll:
            for(int t=0;t<FortCount;t++)
                for(int i=0;i<a.count[t];i++)
                {
                    Unit& u=a.units[t][i];
                    if(u.uid==s.nextUid-1)continue;
                    u.attack=static_cast<std::int8_t>(u.attack+card.value);
                    fx.kind=EventKind::Buffed;
                    fx.unit=u.uid;
                    fx.fort=static_cast<std::int8_t>(t);
                    Say(log,fx);
                }
            break;
        case Effect::HitEnemy:
            if(found)
            {
                Unit& t=b.units[f][at];
                const int dealt=Reduced(s,you,f,t,card.value);
                t.health=static_cast<std::int8_t>(t.health-dealt);
                fx.kind=EventKind::Hit;
                fx.unit=-1;
                fx.target=t.uid;
                fx.fort=static_cast<std::int8_t>(f);
                fx.amount=static_cast<std::int8_t>(dealt);
                Say(log,fx);
                Bury(s,you,f,log);
            }
            break;
        case Effect::Freeze:
            if(found)
            {
                b.units[f][at].frozen=true;
                fx.kind=EventKind::Frozen;
                fx.unit=b.units[f][at].uid;
                fx.fort=static_cast<std::int8_t>(f);
                Say(log,fx);
            }
            break;
        case Effect::Push:
            if(found&&action.dest>=0&&b.count[action.dest]<PerFort)
            {
                const Unit u=b.units[f][at];
                RemoveAt(b,f,at);
                b.units[action.dest][b.count[action.dest]++]=u;
                fx.kind=EventKind::Moved;
                fx.who=static_cast<std::int8_t>(you);
                fx.unit=u.uid;
                fx.card=u.card;
                fx.fort=static_cast<std::int8_t>(f);
                fx.dest=action.dest;
                Say(log,fx);
            }
            break;
        case Effect::Storm:
            for(int i=0;i<b.count[action.fort];i++)
            {
                Unit& t=b.units[action.fort][i];
                const int dealt=Reduced(s,you,action.fort,t,card.value);
                t.health=static_cast<std::int8_t>(t.health-dealt);
                fx.kind=EventKind::Hit;
                fx.unit=-1;
                fx.target=t.uid;
                fx.fort=action.fort;
                fx.amount=static_cast<std::int8_t>(dealt);
                Say(log,fx);
            }
            Bury(s,you,action.fort,log);
            break;
        default:
            break;
        }
    }

    std::vector<Action> ChoosePlan(const State& state,bool loose,std::mt19937& rng)
    {
        const int me=state.who;
constexpr int Width=20,Depth=12,Finals=12,Samples=12,Horizon=4;
        struct Node {State s;std::array<Action,Depth+1> plan{};int steps=0;double q=0;};
        std::vector<Node> beam,next,finals;
        {
            Node start;
            start.s=Seen(state);
            beam.push_back(start);
        }
        for(int depth=0;depth<Depth&&!beam.empty();depth++)
        {
            next.clear();
            for(const Node& node:beam)
                for(const Action& a:Legal(node.s))
                {
                    if(a.kind==ActionKind::End)
                    {
                        Node done=node;
                        done.q=Judge(done.s,me);
                        finals.push_back(done);
                        continue;
                    }
                    Node child=node;
                    Apply(child.s,a);
                    child.plan[child.steps++]=a;
                    child.q=Judge(child.s,me);
                    if(child.s.over)finals.push_back(child);
                    else next.push_back(child);
                }
            if(static_cast<int>(next.size())>Width)
            {
                std::partial_sort(next.begin(),next.begin()+Width,next.end(),[](const Node& x,const Node& y){return x.q>y.q;});
                next.resize(Width);
            }
            beam.swap(next);
        }
        for(const Node& node:beam)finals.push_back(node);
        if(finals.empty())return {};
        const int keep=std::min<int>(Finals,static_cast<int>(finals.size()));
        std::partial_sort(finals.begin(),finals.begin()+keep,finals.end(),[](const Node& x,const Node& y){return x.q>y.q;});
        finals.resize(keep);
        // Her guess at the other's hand: as many cards as it holds, from what its deck holds.
        const std::uint32_t common=rng();
        for(Node& node:finals)
        {
            if(node.s.over)continue;
            std::mt19937 dice(common);   // the same guesses for every plan
            double sum=0;
            for(int sample=0;sample<Samples;sample++)
            {
                State after=node.s;
                Side& other=after.side[1-me];
                other.handSize=state.side[1-me].handSize;
                for(int i=0;i<other.handSize;i++)other.hand[i]=static_cast<std::int8_t>(Guess(other.deck,dice));
                Apply(after,Action{});
                sum+=PlayOut(after,me,Horizon,dice);
            }
            node.q=sum/Samples;
        }
        std::stable_sort(finals.begin(),finals.end(),[](const Node& x,const Node& y){return x.q>y.q;});
        size_t pick=0;
        // The next-best plan, while it is not throwing a won game.
        if(loose&&finals.size()>1&&finals[0].q<50000&&finals[1].q>finals[0].q-14)pick=1;
        return std::vector<Action>(finals[pick].plan.begin(),finals[pick].plan.begin()+finals[pick].steps);
    }

    Action ChooseAction(const State& state,bool loose,std::mt19937& rng)
    {
        const std::vector<Action> plan=ChoosePlan(state,loose,rng);
        return plan.empty()?Action{}:plan.front();
    }

    std::vector<Action> ClumsyPlan(const State& state,double noise,std::mt19937& rng)
    {
        const int me=state.who;
        std::normal_distribution<double> misjudged(0,noise);
        State s=Seen(state);
        std::vector<Action> plan;
        for(int step=0;step<12&&!s.over;step++)
        {
            double best=Judge(s,me)+misjudged(rng);
            Action chosen;
            for(const Action& a:Legal(s))
            {
                if(a.kind==ActionKind::End)continue;
                State next=s;
                Apply(next,a);
                const double q=Judge(next,me)+misjudged(rng);
                if(q>best){best=q;chosen=a;}
            }
            if(chosen.kind==ActionKind::End)break;
            Apply(s,chosen);
            plan.push_back(chosen);
        }
        return plan;
    }

    Action ChooseGreedy(const State& state)
    {
        const int me=state.who;
        const State view=Seen(state);
        double best=Judge(view,me);
        Action chosen;
        for(const Action& a:Legal(view))
        {
            if(a.kind==ActionKind::End)continue;
            State next=view;
            Apply(next,a);
            const double q=Judge(next,me);
            if(q>best+1e-9){best=q;chosen=a;}
        }
        return chosen;
    }

    Action ChooseAnyhow(const State& state,std::mt19937& rng)
    {
        const std::vector<Action> legal=Legal(state);
        if(legal.size()<=1||rng()%6==0)return Action{};
        return legal[1+rng()%(legal.size()-1)];
    }

    // ---- how hard she plays ----

    namespace
    {
        // care: the turns played by her best plan, the rest by the next-best; clumsy: the turns
        // played by ClumsyPlan, with this noise. Measured by --forts-level-test.
        struct Ease {double care,clumsy,noise;};
        constexpr Ease Eases[Levels]=
        {
            {0.50,1.00,30},
            {0.50,0.90,20},
            {0.50,0.80,14},
            {0.60,0.70,10},
            {0.60,0.55,8},
            {0.70,0.40,6},
            {0.75,0.30,5},
            {0.80,0.20,4},
            {0.90,0.10,3},
            {1.00,0.00,0},
        };
    }

    int Strength::Level() const
    {
        return std::clamp(static_cast<int>(std::lround(level)),0,Levels-1);
    }

    void AfterGame(Strength& strength,int result,int margin,bool gaveUp)
    {
        if(result==0)return;
        // Quicker at first, to find you soon; further for a game far from close, less for a close one.
        double step=strength.games<4?1.5:1.0;
        if(!gaveUp&&margin>=5)step*=1.5;
        else if(!gaveUp&&margin<=1)step*=0.5;
        // A little further down than up: you should win somewhat more than half. And not up too
        // far at once, so that a win or two at the start isn't answered by her best.
        strength.level=std::clamp(strength.level+(result>0?std::min(step,2.0):-1.2*step),0.0,static_cast<double>(Levels-1));
        strength.games++;
    }

    // ---- the table ----

    void Table::NewGame(std::uint32_t seed,int yourDeck,int herDeck,int levelNow,int firstOne,const std::vector<int>* yourCards)
    {
        rng.seed(seed);
        think.seed(seed^0x5bd1e995u);
        level=std::clamp(levelNow,0,Levels-1);
        first=firstOne;
        state=State{};
        events.clear();
        shownAim=-1;
        looseTurn=-1;
        looseNow=false;
        clumsyNow=false;
        redrawDone=false;
        planDone=false;
        plan.clear();
        deckKind={yourDeck,herDeck};
        for(int who=0;who<2;who++)
        {
            decks[who]=who==You&&yourCards?*yourCards:DeckOf(deckKind[who]);
            std::shuffle(decks[who].begin(),decks[who].end(),rng);
            seen[who].clear();
            state.side[who].deckLeft=static_cast<std::int8_t>(decks[who].size());
            state.side[who].deck=static_cast<std::int8_t>(deckKind[who]);
            DrawTo(who,StartHand);
        }
        state.side[1-first].spare=true;
        DrawTo(1-first,1);
        state.who=static_cast<std::int8_t>(first);
    }

    Phase Table::Now() const
    {
        if(state.over)return Phase::Over;
        if(!redrawDone)return Phase::Redraw;
        return state.who==You?Phase::Yours:Phase::Hers;
    }

    void Table::DrawTo(int who,int count)
    {
        Side& side=state.side[who];
        for(int i=0;i<count&&!decks[who].empty();i++)
        {
            const int id=decks[who].back();
            decks[who].pop_back();
            side.deckLeft=static_cast<std::int8_t>(decks[who].size());
            seen[who].push_back(id);
            Event e;
            e.kind=EventKind::Drew;
            e.who=static_cast<std::int8_t>(who);
            e.card=static_cast<std::int8_t>(id);
            if(side.handSize<MaxHand)side.hand[side.handSize++]=static_cast<std::int8_t>(id);
            else e.amount=1;   // too many: lost
            events.push_back(e);
        }
    }

    void Table::TakeDrawn()
    {
        for(int who=0;who<2;who++)
        {
            Side& side=state.side[who];
            const int count=side.pending;
            side.pending=0;
            // deckLeft already counted them down: draw from the real deck.
            side.deckLeft=static_cast<std::int8_t>(decks[who].size());
            DrawTo(who,count);
        }
    }

    void Table::Redraw(unsigned mask)
    {
        if(redrawDone)return;
        const auto change=[&](int who,unsigned bits)
        {
            Side& side=state.side[who];
            std::vector<int> kept,back;
            for(int i=0;i<side.handSize;i++)
                if(bits&(1u<<i))back.push_back(side.hand[i]);
                else kept.push_back(side.hand[i]);
            side.handSize=0;
            for(int id:kept)side.hand[side.handSize++]=static_cast<std::int8_t>(id);
            int count=static_cast<int>(back.size());
            // The new cards first, then the old ones go back and the deck is shuffled.
            DrawTo(who,count);
            decks[who].insert(decks[who].end(),back.begin(),back.end());
            std::shuffle(decks[who].begin(),decks[who].end(),rng);
            side.deckLeft=static_cast<std::int8_t>(decks[who].size());
        };
        change(You,mask);
        change(Her,SuggestRedraw(Her));
        redrawDone=true;
        Begin();
    }

    // She keeps what she can use soon.
    unsigned Table::SuggestRedraw(int who) const
    {
        unsigned bits=0;
        const Side& side=state.side[who];
        for(int i=0;i<side.handSize;i++)
            if(CardOf(side.hand[i]).cost>=4)bits|=1u<<i;
        return bits;
    }

    void Table::Begin()
    {
        StartTurn(state,first,&events);
        TakeDrawn();
    }

    void Table::Do(const Action& action)
    {
        if(state.over||Now()==Phase::Redraw)return;
        const int who=state.who;
        const bool ending=action.kind==ActionKind::End;
        if(ending)AfterHerTurn(who);
        Apply(state,action,&events);
        TakeDrawn();
    }

    double Table::CareNow() const
    {
        // Less care still while she leads comfortably.
        const double care=Eases[state.who==Her?level:Levels-1].care;
        if(care>=1)return 1;
        const int lead=state.side[Her].score-state.side[You].score;
        return lead>=6?care-0.3:lead>=3?care-0.15:care;
    }

    // At the end of a turn: which fort it will go for next, shown to the other. Now and then a bluff.
    void Table::AfterHerTurn(int who)
    {
        Side& a=state.side[who];
        const Side& b=state.side[1-who];
        int best=0;
        double top=-1e9;
        for(int f=0;f<FortCount;f++)
        {
            double mine=0,theirs=0;
            for(int i=0;i<a.count[f];i++)mine+=a.units[f][i].attack+a.units[f][i].health;
            for(int i=0;i<b.count[f];i++)theirs+=b.units[f][i].attack+b.units[f][i].health;
            const double q=FortValue[f]*(1.0+std::clamp(mine-theirs,-6.0,6.0)*0.25)+(a.count[f]<PerFort?0.5:0.0);
            if(q>top){top=q;best=f;}
        }
        const bool bluff=think()%100<25;
        a.aim=bluff?static_cast<std::int8_t>(-1):static_cast<std::int8_t>(best);
        if(who==Her)shownAim=bluff?(best+1+static_cast<int>(think()%2))%FortCount:best;
    }

    Action Table::HerStep()
    {
        if(looseTurn!=state.turns)
        {
            looseTurn=state.turns;
            looseNow=std::uniform_real_distribution<double>(0,1)(think)>CareNow();
            clumsyNow=std::uniform_real_distribution<double>(0,1)(think)<Eases[state.who==Her?level:Levels-1].clumsy;
            plan.clear();
        }
        // She keeps to her plan for the turn, and thinks again when she has drawn a card.
        if(plan.empty()&&!planDone)
        {
            plan=clumsyNow?ClumsyPlan(state,Eases[state.who==Her?level:Levels-1].noise,think):ChoosePlan(state,looseNow,think);
            planDone=true;
            std::reverse(plan.begin(),plan.end());
        }
        Action chosen;
        if(!plan.empty())
        {
            chosen=plan.back();
            plan.pop_back();
        }
        const int handBefore=state.side[state.who].handSize-(chosen.kind==ActionKind::Play||chosen.kind==ActionKind::Offer?1:0);
        const int who=state.who;
        Do(chosen);
        if(state.who==who&&state.side[who].handSize>handBefore){plan.clear();planDone=false;}
        if(chosen.kind==ActionKind::End||state.who!=who)planDone=false;
        return chosen;
    }

    void Table::StepAs(int who,int style)
    {
        if(style==1)Do(ChooseGreedy(state));
        else if(style==2)Do(ChooseAnyhow(state,think));
        else HerStep();
    }

    Action Table::PlayForYou(int style)
    {
        const Action chosen=style==1?ChooseGreedy(state):style==2?ChooseAnyhow(state,think):ChooseAction(state,false,think);
        Do(chosen);
        return chosen;
    }

    std::vector<Event> Table::TakeEvents()
    {
        std::vector<Event> out;
        out.swap(events);
        return out;
    }

    bool Table::Consistent() const
    {
        for(int who=0;who<2;who++)
        {
            const Side& side=state.side[who];
            if(side.deckLeft!=static_cast<int>(decks[who].size())||side.pending!=0)return false;
            if(side.handSize<0||side.handSize>MaxHand||side.magic<0||side.magic>MaxMagic+4)return false;
            for(int f=0;f<FortCount;f++)
            {
                if(side.count[f]<0||side.count[f]>PerFort)return false;
                for(int i=0;i<side.count[f];i++)
                    if(side.units[f][i].health<=0)return false;
            }
        }
        return true;
    }

    // ---- diagnostics ----

    namespace
    {
        Unit Soldier(int card,int uid,bool ready=true)
        {
            Unit u;
            u.uid=static_cast<std::int16_t>(uid);
            u.card=static_cast<std::int8_t>(card);
            u.attack=CardOf(card).attack;
            u.health=CardOf(card).health;
            u.maxHealth=u.health;
            u.ready=ready;
            return u;
        }
        void Put(State& s,int who,int fort,const Unit& u){s.side[who].units[fort][s.side[who].count[fort]++]=u;}
        int IdOf(const wchar_t* nameEn)
        {
            for(int i=0;i<KindCount;i++)
                if(std::wstring(Cards[i].nameEn)==nameEn)return i;
            return -1;
        }
        Action AttackBy(int unit,int target){Action a;a.kind=ActionKind::Attack;a.unit=static_cast<std::int16_t>(unit);a.target=static_cast<std::int16_t>(target);return a;}
        bool LegalHas(const State& s,const Action& want)
        {
            for(const Action& a:Legal(s))
                if(a.kind==want.kind&&a.unit==want.unit&&a.target==want.target&&a.dest==want.dest&&a.hand==want.hand&&a.fort==want.fort)return true;
            return false;
        }
    }

    std::wstring CheckRules()
    {
        std::wstring log;
        int failures=0;
        const auto check=[&](bool ok,const wchar_t* what)
        {
            if(!ok)failures++;
            log+=(ok?L"ok   ":L"FAIL ");
            log+=what;
            log+=L"\n";
        };
        // The points table: a soldier spends 2c+1.
        for(int id=0;id<KindCount;id++)
        {
            const Card& c=Cards[id];
            if(c.spell||c.faction<0)continue;
            int points=c.attack+c.health;
            for(std::uint8_t bit=1;bit;bit=static_cast<std::uint8_t>(bit<<1))
                if(c.keywords&bit)points+=bit==Guard?1:bit==Scout?1:bit==Tough?3:2;
            // What it does on arrival or on falling: a small thing 1, more 2.
            if(c.effect!=Effect::None)points+=(c.effect==Effect::BuffFort||c.effect==Effect::DrawOnDeath||c.effect==Effect::HitEnemy)?1:2;
            // The forest gets one over: its small bodies would lose every fight otherwise.
            if(points>2*c.cost+1+(c.faction==2?1:0))
            {
                failures++;
                wchar_t line[160];
                swprintf_s(line,L"FAIL over budget: %s (%d of %d)\n",c.nameEn,points,2*c.cost+1);
                log+=line;
            }
        }
        for(int d=0;d<DeckKinds;d++)check(DeckOf(d).size()==DeckSize,L"deck of 30");
        check(KindCount==37,L"36 cards and the hare");

        {   // Holding: only your soldiers there.
            State s;
            Put(s,You,1,Soldier(0,1));
            check(Holds(s,You,1)&&!Holds(s,Her,1),L"only you at a fort holds it");
            Put(s,Her,1,Soldier(0,2));
            check(!Holds(s,You,1)&&!Holds(s,Her,1),L"both there: nobody holds");
            s=State{};
            Put(s,You,1,Soldier(IdOf(L"Standard Page"),1));
            check(FortScore(s,You,1)==3,L"banner: the centre scores 3");
            Put(s,You,1,Soldier(IdOf(L"Standard Page"),2));
            check(FortScore(s,You,1)==3,L"two banners count once");
        }
        {   // Scoring at the start of one's turn, not at the end.
            State s;
            Put(s,You,0,Soldier(0,1));
            Apply(s,Action{});   // your turn ends: her turn starts
            check(s.side[You].score==0&&s.who==Her,L"no points at the end of a turn");
            Apply(s,Action{});
            check(s.side[You].score==1&&s.who==You,L"a point at the start of the next");
        }
        {   // Winning at the target, on the spot.
            State s;
            s.side[You].score=Target-1;
            Put(s,You,2,Soldier(0,1));
            s.who=Her;
            Apply(s,Action{});
            check(s.over&&s.winner==You,L"the target ends it");
        }
        {   // Fighting at once; both hurt.
            State s;
            Put(s,You,0,Soldier(IdOf(L"Wandering Sword"),1));
            Put(s,Her,0,Soldier(0,2));
            Apply(s,AttackBy(1,2));
            check(s.side[Her].count[0]==0&&s.side[You].units[0][0].health==1,L"attack: both take damage");
        }
        {   // Ranged takes nothing back.
            State s;
            Put(s,You,0,Soldier(IdOf(L"Tower Archer"),1));
            Put(s,Her,0,Soldier(IdOf(L"Iron Giant"),2));
            Apply(s,AttackBy(1,2));
            check(s.side[You].count[0]==1&&s.side[Her].units[0][0].health==4,L"ranged: no damage back");
        }
        {   // Tough and the shield.
            State s;
            Put(s,You,0,Soldier(0,1));
            Put(s,Her,0,Soldier(IdOf(L"Bulwark Knight"),2));
            Apply(s,AttackBy(1,2));
            check(s.side[Her].units[0][0].health==CardOf(s.side[Her].units[0][0].card).health,L"tough: a 1 does nothing");
        }
        {   // Guard, and flying past it.
            State s;
            Put(s,You,0,Soldier(0,1));
            Put(s,You,0,Soldier(IdOf(L"Great Eagle"),2));
            Put(s,Her,0,Soldier(0,3));
            Put(s,Her,0,Soldier(IdOf(L"Shield Bearer"),4));
            check(!LegalHas(s,AttackBy(1,3))&&LegalHas(s,AttackBy(1,4)),L"guard must be attacked first");
            check(LegalHas(s,AttackBy(2,3)),L"flying goes past guard");
        }
        {   // A new soldier waits; a swift one does not.
            State s;
            s.side[You].magic=8;
            s.side[You].handSize=2;
            s.side[You].hand[0]=0;
            s.side[You].hand[1]=static_cast<std::int8_t>(IdOf(L"Timber Wolf"));
            Action a;
            a.kind=ActionKind::Play;
            a.hand=1;
            a.fort=0;
            Apply(s,a);
            check(s.side[You].units[0][0].ready,L"swift: ready at once");
            a.hand=0;
            Apply(s,a);
            check(!s.side[You].units[0][1].ready,L"others wait a turn");
        }
        {   // Move or attack, not both; the scout does both.
            State s;
            Put(s,You,0,Soldier(0,1));
            Put(s,You,0,Soldier(IdOf(L"Forest Scout"),2));
            Put(s,Her,1,Soldier(0,3));
            Action m;
            m.kind=ActionKind::Move;
            m.unit=1;
            m.dest=1;
            Apply(s,m);
            check(!LegalHas(s,AttackBy(1,3)),L"moved: no attack");
            m.unit=2;
            Apply(s,m);
            check(LegalHas(s,AttackBy(2,3)),L"scout: moves, then attacks");
        }
        {   // Forts next door only.
            State s;
            Put(s,You,0,Soldier(0,1));
            Action m;
            m.kind=ActionKind::Move;
            m.unit=1;
            m.dest=2;
            check(!LegalHas(s,m),L"no move across the centre");
        }
        {   // Freezing: the frozen soldier sits out its next turn only.
            State s;
            Put(s,Her,0,Soldier(0,1));
            s.side[Her].units[0][0].frozen=true;
            Apply(s,Action{});   // her turn starts
            check(!s.side[Her].units[0][0].ready,L"frozen: not ready");
            Apply(s,Action{});
            Apply(s,Action{});
            check(s.side[Her].units[0][0].ready,L"frozen: ready the turn after");
        }
        {   // Offering a card for one magic, once a turn.
            State s;
            s.side[You].handSize=2;
            s.side[You].hand[0]=0;
            s.side[You].hand[1]=1;
            Action a;
            a.kind=ActionKind::Offer;
            a.hand=0;
            Apply(s,a);
            check(s.side[You].magic==1&&s.side[You].handSize==1,L"offer: a card for a magic");
            check(!LegalHas(s,a),L"offer: once a turn");
            for(int id=0;id<KindCount;id++)
            {
                const int gives=GivesMagic(Cards[id]);
                check(gives>=1&&gives<=3,L"offer: 1 to 3 magic");
            }
        }
        {   // Collecting: every card of the deck once each, and a game with few of them.
            std::mt19937 dice(3);
            std::vector<int> owned;
            int got=0;
            while(Collect(owned,dice)>=0)got++;
            std::vector<int> all=CollectableCards(),have=owned;
            check(got==static_cast<int>(all.size()),L"collect: every card to be collected");
            check(std::none_of(all.begin(),all.end(),[](int id){return Cards[id].faction==0;}),L"collect: none of the common cards");
            std::sort(all.begin(),all.end());
            std::sort(have.begin(),have.end());
            check(all==have,L"collect: each as many times as the deck holds it");
            {
                std::vector<int> whole=CollectedDeckCards(owned),deck=DeckOf(CollectedDeck);
                std::sort(whole.begin(),whole.end());
                std::sort(deck.begin(),deck.end());
                check(whole==deck,L"collect: all collected makes the whole deck");
            }
            owned.resize(UnlockAt);
            const std::vector<int> cards=CollectedDeckCards(owned);
            check(static_cast<int>(cards.size())==DeckSize-static_cast<int>(all.size())+UnlockAt,L"collect: the common cards and those you have");
            Table t;
            t.NewGame(9,CollectedDeck,0,Levels-1,You,&cards);
            check(t.Board().side[You].deckLeft==static_cast<int>(cards.size())-StartHand,L"collect: a game with only what you have");
            t.Redraw(0);
            int guard=0;
            while(t.Now()!=Phase::Over&&guard++<4000)
            {
                t.StepAs(t.Board().who,0);
                t.TakeEvents();
            }
            check(t.Now()==Phase::Over&&t.Consistent(),L"collect: a game with few cards ends");
        }
        {   // The turn limit.
            State s;
            s.turns=TurnLimit-1;
            s.side[You].score=3;
            Apply(s,Action{});
            check(s.over&&s.winner==You,L"the turn limit: the higher score wins");
        }
        {   // She does not read your hand: a hand you hold changes nothing of hers.
            Table t;
            t.NewGame(5,0,1);
            t.Redraw(0);
            bool same=true;
            std::mt19937 r1(1),r2(1);
            State a=t.Board();
            a.who=Her;
            State b=a;
            for(int i=0;i<b.side[You].handSize;i++)b.side[You].hand[i]=static_cast<std::int8_t>(IdOf(L"Elder Dragon"));
            const Action x=ChooseAction(a,false,r1),y=ChooseAction(b,false,r2);
            same=x.kind==y.kind&&x.hand==y.hand&&x.fort==y.fort&&x.target==y.target;
            check(same,L"her choice ignores your hand");
        }
        wchar_t head[64];
        swprintf_s(head,L"%d failures\n",failures);
        return head+log;
    }

    std::wstring CheckPlaying(int count,std::uint32_t seed)
    {
        std::wstring out;
        wchar_t line[256];
        int bad=0;
        std::mt19937 dice(seed);

        struct Tally {int wins=0,losses=0,draws=0,firstWins=0,games=0;long long turns=0;int minTurns=9999,maxTurns=0;};
        std::array<int,KindCount> drawnWon{},drawnGames{};

        const auto game=[&](int deckA,int deckB,int styleA,int styleB,std::uint32_t s,int firstOne,Tally& tally,bool cards)
        {
            Table t;
            t.NewGame(s,deckA,deckB,Levels-1,firstOne);
            t.Redraw(styleA==2?0:t.SuggestRedraw(You));
            int guard=0;
            while(t.Now()!=Phase::Over&&guard<4000)
            {
                const int who=t.Board().who;
                t.StepAs(who,who==You?styleA:styleB);
                t.TakeEvents();
                if(!t.Consistent())bad++;
                guard++;
            }
            if(t.Now()!=Phase::Over)bad++;
            const int w=t.Board().winner;
            tally.games++;
            if(w==You)tally.wins++;
            else if(w==Her)tally.losses++;
            else tally.draws++;
            if(w==firstOne)tally.firstWins++;
            const int turns=t.Board().turns+1;
            tally.turns+=turns;
            tally.minTurns=std::min(tally.minTurns,turns);
            tally.maxTurns=std::max(tally.maxTurns,turns);
            if(cards)
                for(int who=0;who<2;who++)
                {
                    std::array<bool,KindCount> had{};
                    for(int id:t.Seen()[who])had[id]=true;
                    for(int id=0;id<KindCount;id++)
                        if(had[id])
                        {
                            drawnGames[id]++;
                            if(w==who)drawnWon[id]++;
                        }
                }
        };

        // Her with each deck against herself: first player's share, and each pairing.
        Tally all;
        out+=L"deck vs deck (row wins, of count with each going first half the time)\n";
        for(int a=0;a<DeckKinds;a++)
        {
            for(int b=0;b<DeckKinds;b++)
            {
                Tally t;
                for(int i=0;i<count;i++)game(a,b,0,0,dice(),i%2,t,true);
                swprintf_s(line,L"  %s vs %s: %d-%d-%d, first won %d\n",DeckName(a,true),DeckName(b,true),t.wins,t.losses,t.draws,t.firstWins);
                out+=line;
                all.games+=t.games;
                all.firstWins+=t.firstWins;
                all.turns+=t.turns;
                all.minTurns=std::min(all.minTurns,t.minTurns);
                all.maxTurns=std::max(all.maxTurns,t.maxTurns);
                all.draws+=t.draws;
            }
        }
        swprintf_s(line,L"first player won %.1f%%, draws %d, turns of both: mean %.1f (%d to %d)\n",100.0*all.firstWins/std::max(1,all.games),all.draws,static_cast<double>(all.turns)/std::max(1,all.games),all.minTurns,all.maxTurns);
        out+=line;

        // Her against the styles.
        const wchar_t* const styles[]={L"",L"greedy",L"anyhow"};
        for(int style=1;style<=2;style++)
        {
            Tally t;
            for(int i=0;i<count;i++)game(dice()%DeckKinds,dice()%DeckKinds,style,0,dice(),i%2,t,false);
            swprintf_s(line,L"she against %s: won %d of %d (lost %d, drawn %d), mean turns %.1f\n",styles[style],t.losses,t.games,t.wins,t.draws,static_cast<double>(t.turns)/std::max(1,t.games));
            out+=line;
        }

        // Each card: how often whoever drew it won.
        out+=L"card: games drawn, share won\n";
        for(int id=0;id<KindCount;id++)
        {
            if(drawnGames[id]==0)continue;
            swprintf_s(line,L"  %-18s %2d %6d %5.1f%%\n",Cards[id].nameEn,Cards[id].cost,drawnGames[id],100.0*drawnWon[id]/drawnGames[id]);
            out+=line;
        }
        wchar_t head[64];
        swprintf_s(head,L"%d failures\n",bad);
        return head+out;
    }

    std::wstring CheckLevels(int count,std::uint32_t seed)
    {
        std::wstring out;
        wchar_t line[256];
        int bad=0;
        std::mt19937 dice(seed);
        // Style against her at a level: 1 she won, -1 she lost, 0 drawn; margin, the points between.
        const auto game=[&](int style,int level,int& margin,int& turns)
        {
            Table t;
            t.NewGame(dice(),dice()%DeckKinds,dice()%DeckKinds,level,dice()%2);
            t.Redraw(style==2?0:t.SuggestRedraw(You));
            int guard=0;
            while(t.Now()!=Phase::Over&&guard<4000)
            {
                const int who=t.Board().who;
                t.StepAs(who,who==You?style:0);
                t.TakeEvents();
                guard++;
            }
            if(t.Now()!=Phase::Over||!t.Consistent())bad++;
            margin=std::abs(t.Board().side[0].score-t.Board().side[1].score);
            turns=t.Board().turns+1;
            return t.Board().winner==Her?1:t.Board().winner==You?-1:0;
        };
        const int styles[]={2,1,0};
        out+=L"her wins (%) at each level against anyhow, greedy, her best; mean turns\n";
        for(int level=0;level<Levels;level++)
        {
            swprintf_s(line,L"  level %d:",level);
            out+=line;
            for(int style:styles)
            {
                int won=0;
                long long sum=0;
                for(int i=0;i<count;i++)
                {
                    int margin=0,turns=0;
                    if(game(style,level,margin,turns)>0)won++;
                    sum+=turns;
                }
                swprintf_s(line,L"  %3d%% (%2.0f)",100*won/std::max(1,count),static_cast<double>(sum)/std::max(1,count));
                out+=line;
            }
            out+=L"\n";
        }
        out+=L"where the level goes, starting anew, against each (level after 1,2,3,5,10,20,30 games; your wins in the last 20)\n";
        const wchar_t* const names[]={L"her best",L"greedy",L"anyhow"};
        for(int style:styles)
        {
            for(int run=0;run<3;run++)
            {
                Strength strength;
                int yourWins=0;
                swprintf_s(line,L"  %-8s:",names[style]);
                out+=line;
                for(int i=1;i<=30;i++)
                {
                    int margin=0,turns=0;
                    const int result=game(style,strength.Level(),margin,turns);
                    AfterGame(strength,-result,margin);
                    if(i>10&&result<0)yourWins++;
                    if(i==1||i==2||i==3||i==5||i==10||i==20||i==30)
                    {
                        swprintf_s(line,L" %d",strength.Level());
                        out+=line;
                    }
                }
                swprintf_s(line,L"; won %d of 20\n",yourWins);
                out+=line;
            }
        }
        swprintf_s(line,L"%d failures\n",bad);
        return line+out;
    }
}
