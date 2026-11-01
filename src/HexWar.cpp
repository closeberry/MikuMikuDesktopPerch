#include "HexWar.hpp"
#include <algorithm>
#include <map>
#include <chrono>
#include <cmath>
#include <cstring>
#include <queue>
#include <sstream>

namespace MikuMikuDesktopPerch::HexWar
{
    namespace
    {
        // Move; attack on men, on armour; defence; worth; takes bases; armoured; range; costs by
        // terrain: plain, rough, forest, mountain, river, road, base, bridge, ruins, factory,
        // foundations (0: impassable); attack on aircraft; flies.
        const KindInfo Kinds_[Kinds]={
            {5,9,9,8,3,false,true,1,1,{1,2,2,0,0,1,1,1,2,1,0},0,false},   // tank: no mountains, rough, forest and ruins at half speed
            {3,4,4,4,1,true,false,1,1,{1,2,1,3,0,1,1,1,1,1,2},2,false},   // infantry: mountains at a third, forest and ruins as plain
            {3,3,10,4,2,true,false,1,1,{1,2,1,3,0,1,1,1,1,1,2},2,false},  // anti-tank infantry: as infantry, deadly to vehicles
            {7,6,4,5,2,false,true,1,1,{1,3,2,0,0,1,1,1,2,1,0},0,false},   // scout car: fast on open ground and roads, slow off them
            {4,8,8,3,3,false,true,2,3,{1,2,2,0,0,1,1,1,2,1,0},0,false},   // self-propelled gun: two or three hexes off
            {3,12,12,11,5,false,true,1,1,{1,2,2,0,0,1,1,1,3,1,0},0,false},  // heavy tank: slow, the strongest
            {6,3,2,6,3,false,true,1,1,{1,1,1,1,1,1,1,1,1,1,1},10,true},     // fighter: brings down aircraft
            {5,8,9,5,4,false,true,1,1,{1,1,1,1,1,1,1,1,1,1,1},0,true},      // attack aircraft: strikes the ground
            {5,3,3,6,3,false,true,1,1,{1,2,2,0,0,1,1,1,2,1,0},11,false},    // anti-aircraft missile truck
        };

        // Axial steps of the six directions, going round from straight up: 0 up, 1 upper
        // right, 2 lower right, 3 down, 4 lower left, 5 upper left.
        constexpr int Dq[6]={0,1,1,0,-1,-1},Dr[6]={-1,-1,0,1,1,0};

        // A shot's chance of hitting, from the shooter's attack against the target's defence.
        double Hit(double attack,double defence)
        {
            const double q=attack/std::max(0.01,attack+defence);
            return std::clamp(0.9*q*q,0.03,0.9);
        }

        // An attack grown by its level.
        double Grown(double v,int exp){return v*(1+Level(exp)/3.0);}

        // Diagnostics: experience gained by those coming through a battle with an equal.
        double equalGained=0;
        int equalBattles=0;

        // A strike: this many shots of this attack.
        constexpr int StrikeShots=10;
        constexpr double StrikePower=10;

        // The maps, each read off its picture by tools/hexwar/read_map.py and set right by hand.
        // Each brings in other kinds besides tanks and infantry: scout cars to the valley and the
        // isle, heavy tanks to the gorge and the ridge, guns to the ridge and the isle, anti-tank
        // infantry and aircraft (fighters, attack aircraft, missile trucks) to the city.
        // The river valley: a river with one bridge, rocks and peaks to the north-west and
        // south-east, forest along the east. The lone bridge: a river from the north to the
        // south-west, one bridge, peaks and a rocky plateau. The horseshoe ridge (a tall map,
        // scrolled): her base inside a ridge open to the north, yours across a river to the
        // south. The ruined city (set by hand: its picture is all grey): ruins everywhere, open
        // only along the avenues, a river with a bridge in a corner. The volcano isle (a wide
        // map, scrolled): one long island, peaks in the west, the volcano in the middle. Last,
        // not offered: a plain broken by ridges, the same for both sides.
        const std::vector<MapInfo>& AllMaps()
        {
            static const std::vector<MapInfo> maps{
                {L"川の谷",L"The River Valley",20,13,{
                    "MMMMMMMRMRPFR=PWWFFF",
                    "MMMMMRRMRRP==PFWWWPF",
                    "MMMMRRRRRRPKPPPWWFFF",
                    "RRRMRRRMR==PPPPWPFFF",
                    "PPRRMRRRR=PPPWWFFFFF",
                    "PFPFRPRPP=PWWPPPFPbF",
                    "FFPRRPPPP#WPPKPPPPFF",
                    "FBP===KWWP=PPPPPPFFF",
                    "P==PPPWPPP==PPPFFFPR",
                    "F=PWWWWFFFPP=FFFFRRM",
                    "FP==WWWWFFFFFFFMRMMM",
                    "WW==PFWFFFFFFFFRRMMR",
                    "WPPP=PFFFFFRFMRMRRMM"},false,
                 {{Kind::Tank,2,7,You},{Kind::Tank,3,8,You},{Kind::Tank,2,6,You},{Kind::Tank,1,9,You},
                  {Kind::Infantry,1,6,You},{Kind::Infantry,3,6,You},
                  {Kind::Tank,17,5,Her},{Kind::Tank,16,6,Her},{Kind::Tank,15,5,Her},{Kind::Tank,16,7,Her},
                  {Kind::Infantry,18,4,Her},{Kind::Infantry,18,6,Her},
                  {Kind::Scout,3,7,You},{Kind::Scout,17,6,Her}},30,L"MAP1",true,true},
                {L"一本橋の渓谷",L"The Lone Bridge",20,13,{
                    "MMMMM=PFPRPWRRMRMRRM",
                    "MMMMM=FPPRRWWRRRRMMM",
                    "MMMMR=FFRPPWPRMMRRRR",
                    "MMMMM=RFPPPWPPRRMRRR",
                    "MMMMR=RRPFPWPPRMRRR=",
                    "MMMMRR=KRPPWPFRPR==R",
                    "MRRRRPPF==#=PRP==PFF",
                    "RRFPBFFPRWWP=K=PPPPF",
                    "FFFFRPPPWFPPPPRR==FF",
                    "FR=PPPRWWFFFFFPFRP==",
                    "PPRPRWWPKFFFFPPPPbPF",
                    "FWFRWRFPFFFFFPPRRRFF",
                    "WWWWWPRFFFFFFFPPRPFF"},false,
                 {{Kind::Tank,3,7,You},{Kind::Tank,5,7,You},{Kind::Tank,3,9,You},{Kind::Tank,5,8,You},
                  {Kind::Infantry,4,6,You},{Kind::Infantry,2,8,You},
                  {Kind::Tank,16,9,Her},{Kind::Tank,17,9,Her},{Kind::Tank,18,10,Her},{Kind::Tank,16,10,Her},
                  {Kind::Infantry,18,9,Her},{Kind::Infantry,16,11,Her},
                  {Kind::Heavy,4,8,You},{Kind::Heavy,17,11,Her}},30,L"MAP2",true,true},
                {L"馬蹄の山脈",L"The Horseshoe Ridge",20,26,{
                    "PWPPFPPFFRPWFPRRRRRW",
                    "R#PPPRFRFPWWWPFPPRRW",
                    "PW=RWRRRPPRWWFFPPRR#",
                    "WR=RRPRRPPWFFWWWPR=W",
                    "WR=RMMRRPFFFFRMMRR=P",
                    "WR=RMMRRRPRPPRMMRR=P",
                    "FR=RMMRFPFKPPRMMRR=R",
                    "RR=RMMRRPPPPRRMMRR=F",
                    "PR=RMMRRFPPPRRMMRR=R",
                    "PR=RMMRRRPPPRRMMRR=R",
                    "MR=RMMRRPPbPRRMMRR=R",
                    "MM=RMMRRFPPPPRMMRR=R",
                    "RRKRMMRRRRPPRRMMRRKR",
                    "FF=RMMRRRRPPRRMMRR=F",
                    "FF=RMMRRRRPRRRMMRR=F",
                    "FP=RMMMMMRRRMMMMRR=P",
                    "FWF=RMMMMMMMMMRRRR=P",
                    "RFPR==RRRRMRRRRF==FF",
                    "RRFFPR==FRRRRR==RRFR",
                    "WWRRRRRR======RRRFRW",
                    "FFWWWRFRPP=FPWWWWWWW",
                    "FPWRWRRWWW#WPWRFRRWR",
                    "PPRPWWWFFP=PWRRKRRRR",
                    "PRRRRFRPFP=PFFFPFRRR",
                    "RRRRRFFPRRBRPPPRRRRR",
                    "FFRPFFPRRR=RRFFFRRRR"},false,
                 {{Kind::Tank,9,23,You},{Kind::Tank,11,23,You},{Kind::Tank,9,24,You},{Kind::Tank,11,24,You},
                  {Kind::Infantry,9,25,You},{Kind::Infantry,11,25,You},
                  {Kind::Tank,9,10,Her},{Kind::Tank,10,11,Her},{Kind::Tank,11,11,Her},{Kind::Tank,10,9,Her},
                  {Kind::Infantry,9,12,Her},{Kind::Infantry,11,12,Her},
                  {Kind::Artillery,10,25,You},{Kind::Heavy,10,23,You},{Kind::Artillery,10,12,Her},{Kind::Heavy,11,10,Her}},40,L"MAP3"},
                {L"廃墟の街",L"The Ruined City",20,13,{
                    "WWWWRHCHHCH==FFHCH=C",
                    "W#H#WHCCHHH==FFHH=CH",
                    "WWWW==CCHHHH=CCC=CHH",
                    "WHHCCR==C==HH====CHH",
                    "HHCFFCCH==FKRRRK==HC",
                    "HCBCCCCCC==HRRRR====",
                    "HCCCCHC==HHHH=RFC=CC",
                    "HHHHCCHCCCH=HHHHC=CF",
                    "HHHRCH=CPP=C=HHC=bCH",
                    "HHRRH==HPP=HHHHC=CCC",
                    "HCRCKCCCCC=HHHFH=CCC",
                    "HHCC=CHCCCCCCCCHHCHH",
                    "HCCC=FFCHCCCCCCC=CHH"},false,
                 {{Kind::Tank,3,5,You},{Kind::Tank,3,6,You},{Kind::Tank,4,5,You},{Kind::Tank,2,6,You},
                  {Kind::Infantry,1,5,You},{Kind::Infantry,2,4,You},
                  {Kind::Tank,16,7,Her},{Kind::Tank,18,8,Her},{Kind::Tank,17,9,Her},{Kind::Tank,16,8,Her},
                  {Kind::Infantry,18,7,Her},{Kind::Infantry,17,7,Her},
                  {Kind::Bazooka,4,6,You},{Kind::Bazooka,1,6,You},{Kind::Bazooka,15,8,Her},{Kind::Bazooka,18,9,Her},
                  {Kind::Fighter,3,4,You},{Kind::Attacker,4,4,You},{Kind::AntiAir,5,5,You},
                  {Kind::Fighter,16,6,Her},{Kind::Attacker,17,6,Her},{Kind::AntiAir,16,9,Her}},30,L"MAP4"},
                {L"火山の島",L"The Volcano Isle",37,11,{
                    "WWWWWWWPWWWRWWWWWWWWWWWWMMRWWWWWWMWWW",
                    "WWFWMWWPFFFPWR=FWWWMWMWFWFMFFFWWMMRWW",
                    "WWWWWWRFFFPMPPFFFRWFRPFFPFFRPPFFRMWWW",
                    "WWWWWFRMMMFFRRR=MFFRRRF=FFMR=FP=bMPFW",
                    "WMWRMFBMMRRFPPPRPPP=RMMR==FFFFFFMMMWM",
                    "RRFRRFRMMFFFRPRRR=PRRRRRPPKFF=FFWWRWW",
                    "WMRWMMFMRMRFPKPPPRPPFPRPMRFFRMMM=WWWW",
                    "WWWWWWWWMMFRFFFFFRRKPFPPRFFRMWWWWWMWW",
                    "WWWWMWWWMMRMFMMRMWWWMWMMPMRMRWMRWWWWW",
                    "WWWWWWWMWWWMWWRWWMMRWWRRMMMWWMRMWWWWW",
                    "WWWWWWWWWWWWWWWWWWMWWWWWWWWWWWMWWWMMW"},false,
                 {{Kind::Tank,7,1,You},{Kind::Tank,7,2,You},{Kind::Tank,6,3,You},{Kind::Tank,6,5,You},
                  {Kind::Infantry,5,4,You},{Kind::Infantry,5,5,You},
                  {Kind::Tank,31,3,Her},{Kind::Tank,31,4,Her},{Kind::Tank,30,3,Her},{Kind::Tank,31,2,Her},
                  {Kind::Infantry,32,2,Her},{Kind::Infantry,32,4,Her},
                  {Kind::Scout,8,1,You},{Kind::Artillery,5,3,You},{Kind::Scout,30,4,Her},{Kind::Artillery,30,2,Her}},40,L"MAP5"},
                {L"はじまりの平原",L"The First Plain",16,11,{
                    "PPPPPPPMMPPPPPPP",
                    "PPPMPPPMMMPPPPPP",
                    "PPMMPPPPMPPPMPPP",
                    "PPPPPPPPPPPMMPPP",
                    "PPPPMMPPPPPPMPPP",
                    "BPPPMMPPPPMMPPPb"},true,
                 {{Kind::Tank,1,2,You},{Kind::Tank,3,4,You},{Kind::Tank,2,7,You},{Kind::Tank,2,8,You},
                  {Kind::Infantry,2,5,You},{Kind::Infantry,4,5,You}},30,nullptr,false},
            };
            return maps;
        }
    }

    const KindInfo& Info(Kind k){return Kinds_[static_cast<int>(k)];}
    int AttackOn(Kind a,Kind d){return Info(d).flies?Info(a).attackAir:Info(d).armoured?Info(a).attackArmour:Info(a).attack;}

    int BuildTurns(Kind k)
    {
        static const int turns[Kinds]={3,2,2,2,3,4,3,3,2};   // tank, infantry, anti-tank, scout, gun, heavy, fighter, attacker, anti-aircraft
        return turns[static_cast<int>(k)];
    }
    int RepairPerTurn(Kind k)
    {
        static const int members[Kinds]={2,3,3,3,2,1,2,2,2};
        return members[static_cast<int>(k)];
    }

    double Cover(Terrain t)
    {
        static const double cover[Terrains]={0,0.15,0.25,0.4,0,0,0.3,0,0.35,0.3,0.5};
        return cover[static_cast<int>(t)];
    }

    // (Against equals, some four a battle: the top after eight.)
    int Level(int exp){return exp>=32?3:exp>=21?2:exp>=11?1:0;}
    double Rank(Kind k,int exp){return Info(k).value*(1+Level(exp)/6.0);}

    int Maps(){return static_cast<int>(AllMaps().size());}
    const MapInfo& Map(int index){return AllMaps()[static_cast<size_t>(std::clamp(index,0,Maps()-1))];}

    // ---- The map ----

    int Table::Next(int hex,int d) const
    {
        const int c=hex%columns,r=hex/columns;
        // Offset (odd columns lower) to axial and back.
        const int q=c,ar=r-(c-(c&1))/2;
        const int q2=q+Dq[d],r2=ar+Dr[d];
        const int c2=q2,row2=r2+(q2-(q2&1))/2;
        return Inside(c2,row2)?Hex(c2,row2):-1;
    }

    int Table::Distance(int a,int b) const
    {
        const int ca=a%columns,cb=b%columns;
        const int qa=ca,ra=a/columns-(ca-(ca&1))/2,qb=cb,rb=b/columns-(cb-(cb&1))/2;
        const int dq=qa-qb,dr=ra-rb;
        return (std::abs(dq)+std::abs(dr)+std::abs(dq+dr))/2;
    }

    const Unit* Table::UnitAt(int hex) const
    {
        for(const Unit& u:units)if(u.at==hex&&u.count>0)return &u;
        return nullptr;
    }
    const Unit* Table::UnitById(int id) const
    {
        for(const Unit& u:units)if(u.id==id)return &u;
        return nullptr;
    }
    Unit* Table::Find(int id){return const_cast<Unit*>(UnitById(id));}
    Unit* Table::FindAt(int hex){return const_cast<Unit*>(UnitAt(hex));}

    int Table::Alive(int who) const
    {
        return static_cast<int>(std::count_if(units.begin(),units.end(),[&](const Unit& u){return u.who==who&&u.count>0;}));
    }

    int Table::Strength(int who) const
    {
        int sum=0;
        for(const Unit& u:units)if(u.who==who)sum+=u.count*Info(u.kind).value;
        return sum;
    }

    double Table::HerShare() const
    {
        const double h=Strength(Her),y=Strength(You);
        return h+y>0?h/(h+y):0.5;
    }

    bool Table::HeldBy(int hex,int who) const
    {
        for(int d=0;d<6;++d)
        {
            const int n=Next(hex,d);
            if(n<0)continue;
            const Unit* o=UnitAt(n);
            if(o&&o->who!=who)return true;
        }
        return false;
    }

    int Table::Cost(const Unit& u,int hex) const
    {
        return Info(u.kind).cost[static_cast<size_t>(At(hex))];
    }

    // ---- Moving ----

    // Where u may move from a hex: by its movement points and the terrain's costs, through its
    // own side's units but not the other's; a hex next to the other side stops it, and from
    // one such it may go only a hex. Ends on its own hex (staying) or an empty one.
    std::vector<Route> Table::ReachFrom(const Unit& u,int from) const
    {
        const int n=Hexes(),move=Info(u.kind).move+(march[static_cast<size_t>(u.who)]?1:0);
        std::vector<int> best(static_cast<size_t>(n),1<<20),prev(static_cast<size_t>(n),-1);
        using Item=std::pair<int,int>;
        std::priority_queue<Item,std::vector<Item>,std::greater<>> open;
        best[static_cast<size_t>(from)]=0;
        open.push({0,from});
        const bool held=HeldBy(from,u.who);
        while(!open.empty())
        {
            const auto [cost,at]=open.top();
            open.pop();
            if(cost>best[static_cast<size_t>(at)])continue;
            // Next to the other side, it stops (only the first step, when it starts so).
            if(at!=from&&HeldBy(at,u.who))continue;
            if(at!=from&&held)continue;
            for(int d=0;d<6;++d)
            {
                const int next=Next(at,d);
                if(next<0)continue;
                const int step=Cost(u,next);
                if(!step)continue;
                const Unit* there=UnitAt(next);
                if(there&&there->who!=u.who)continue;
                // From a held hex: one step whatever it costs, as long as it can go there at all.
                const int total=held&&at==from?std::min(cost+step,move):cost+step;
                if(total>move||total>=best[static_cast<size_t>(next)])continue;
                best[static_cast<size_t>(next)]=total;
                prev[static_cast<size_t>(next)]=at;
                open.push({total,next});
            }
        }
        std::vector<Route> out;
        for(int h=0;h<n;++h)
        {
            if(best[static_cast<size_t>(h)]>move)continue;
            if(h!=from&&Occupied(h))continue;
            Route r;
            r.to=h;
            r.cost=best[static_cast<size_t>(h)];
            for(int at=h;at>=0;at=prev[static_cast<size_t>(at)])r.path.push_back(at);
            std::reverse(r.path.begin(),r.path.end());
            out.push_back(std::move(r));
        }
        return out;
    }

    std::vector<Route> Table::Reach(int id) const
    {
        const Unit* u=UnitById(id);
        if(!u||u->count<=0||u->moved||u->done||phase==Phase::GameOver)return {};
        return ReachFrom(*u,u->at);
    }

    std::vector<Route> Table::Range(int id) const
    {
        const Unit* u=UnitById(id);
        if(!u||u->count<=0)return {};
        return ReachFrom(*u,u->at);
    }

    std::vector<int> Table::Targets(int id) const
    {
        const Unit* u=UnitById(id);
        if(!u||u->count<=0||u->done||phase==Phase::GameOver)return {};
        return TargetsFrom(*u,u->at,u->moved);
    }

    // Next to it; for a gun, two or three hexes off and only when it has not moved. Only those
    // it has an attack against (aircraft: by those with an attack on aircraft).
    std::vector<int> Table::TargetsFrom(const Unit& u,int at,bool moved) const
    {
        std::vector<int> out;
        const KindInfo& k=Info(u.kind);
        if(k.rangeMax<=1)
        {
            for(int d=0;d<6;++d)
            {
                const int n=Next(at,d);
                if(n<0)continue;
                const Unit* o=UnitAt(n);
                if(o&&o->who!=u.who&&o->id!=u.id&&AttackOn(u.kind,o->kind)>0)out.push_back(o->id);
            }
            return out;
        }
        if(moved)return out;
        for(const Unit& o:units)
            if(o.who!=u.who&&o.count>0&&AttackOn(u.kind,o.kind)>0)
                if(const int d=Distance(at,o.at);d>=k.rangeMin&&d<=k.rangeMax)out.push_back(o.id);
        return out;
    }

    // ---- Battle ----

    bool Table::SurroundedAt(const Unit& u,int at,int movedId,int movedTo) const
    {
        bool other[6]{};
        for(int d=0;d<6;++d)
        {
            const int n=Next(at,d);
            if(n<0)continue;
            const Unit* o=nullptr;
            if(movedId>=0&&n==movedTo)o=UnitById(movedId);
            else
            {
                o=UnitAt(n);
                if(o&&o->id==movedId)o=nullptr;
            }
            other[d]=o&&o->who!=u.who;
        }
        for(int d=0;d<3;++d)if(other[d]&&other[d+3])return true;
        return (other[0]&&other[2]&&other[4])||(other[1]&&other[3]&&other[5]);
    }

    bool Table::Surrounded(int id) const
    {
        const Unit* u=UnitById(id);
        return u&&u->count>0&&SurroundedAt(*u,u->at);
    }

    // The battle of a attacking d from aAt (the attacker moved there: movedId, movedTo).
    // The attacker's side next to the defender adds half its attack (by members left) as
    // supporting fire: shots of the attacker's own strength, so that even one member left,
    // well supported, can destroy a whole unit. The defender's side next to the attacker adds
    // half its defence. A side surrounded fights at half its attack and defence. Attack is by
    // what is attacked (armour, men or aircraft). Fired at from afar, being a gun, or having no
    // attack against the attacker, the defender does not fire back. An aircraft is supported
    // only by those of its side that can shoot at aircraft.
    Battle Table::Fight(const Unit& a,int aAt,const Unit& d,int movedId,int movedTo) const
    {
        Battle b;
        b.attacker=a.id;b.defender=d.id;
        b.kinds[0]=a.kind;b.kinds[1]=d.kind;
        b.who[0]=a.who;b.who[1]=d.who;
        b.at[0]=aAt;b.at[1]=d.at;
        b.before[0]=b.after[0]=a.count;b.before[1]=b.after[1]=d.count;
        b.exp[0]=a.exp;b.exp[1]=d.exp;
        b.cover[0]=Cover(At(aAt),a.kind);b.cover[1]=Cover(At(d.at),d.kind);
        const double own=Grown(AttackOn(a.kind,d.kind),a.exp);
        double attack=own,defence=Info(d.kind).defence*(1+b.cover[1]),support=0;
        for(const Unit& o:units)
        {
            if(o.count<=0||o.id==a.id||o.id==d.id)continue;
            const int at=o.id==movedId?movedTo:o.at;
            const bool airCover=Info(o.kind).attackAir>0;
            if(o.who==a.who&&Adjacent(at,d.at)&&(airCover||!Info(a.kind).flies))
            {
                const double added=0.5*Grown(AttackOn(o.kind,d.kind),o.exp);
                attack+=added*o.count/FullStrength;
                support+=added*o.count/own;   // in shots of the attacker's own
            }
            if(o.who==d.who&&Adjacent(at,aAt)&&(airCover||!Info(d.kind).flies))defence+=0.5*Info(o.kind).defence*o.count/FullStrength;
        }
        b.own[0]=a.count;b.own[1]=d.count;
        b.support=static_cast<int>(std::lround(support));
        b.answers=Distance(aAt,d.at)<=1&&!Ranged(d.kind)&&AttackOn(d.kind,a.kind)>0;
        double back=b.answers?Grown(AttackOn(d.kind,a.kind),d.exp):0,guard=Info(a.kind).defence*(1+b.cover[0]);
        b.surrounded[0]=SurroundedAt(a,aAt,movedId,movedTo);
        b.surrounded[1]=SurroundedAt(d,d.at,movedId,movedTo);
        double aim=own;
        if(b.surrounded[0]){attack*=0.5;aim*=0.5;guard*=0.5;}
        if(b.surrounded[1]){back*=0.5;defence*=0.5;}
        b.attack[0]=attack;b.defence[1]=defence;
        b.attack[1]=back;b.defence[0]=guard;
        b.chance[0]=ChanceNow(Hit(aim,defence),a.who,d.who);
        b.chance[1]=b.answers?ChanceNow(Hit(back,guard),d.who,a.who):0;
        b.expected[0]=std::min<double>(d.count,(a.count+b.support)*b.chance[0]);
        b.expected[1]=b.answers?std::min<double>(a.count,d.count*b.chance[1]):0;
        if(!b.answers)b.own[1]=0;
        return b;
    }

    Battle Table::Preview(int attacker,int defender) const
    {
        const Unit* a=UnitById(attacker);
        const Unit* d=UnitById(defender);
        if(!a||!d)return {};
        return Fight(*a,a->at,*d,-1,-1);
    }

    // Each member of both shoots once, at the same time (those hit still shoot), and the
    // attacker's supporting fire after its own. Then the experience: each side left standing
    // gains for coming through it and for each member it destroyed, by how the other ranks
    // against it (a third from one well below it, up to three times from one well above).
    void Table::Shoot(Battle& b)
    {
        std::uniform_real_distribution<double> any(0,1);
        for(int side=0;side<2;++side)
        {
            const int other=side^1;
            if(side==1&&!b.answers)break;
            for(int i=0;i<b.before[side]+(side?0:b.support);++i)
            {
                const bool hit=any(rng)<b.chance[side]&&b.after[other]>0;
                b.shots[side].push_back(hit);
                if(hit)--b.after[other];
            }
        }
        for(int side=0;side<2;++side)
        {
            const int other=side^1;
            if(b.after[side]<=0)continue;
            const double above=std::clamp(Rank(b.kinds[other],b.exp[other])/Rank(b.kinds[side],b.exp[side]),0.3,3.0);
            b.gained[side]=static_cast<int>(std::lround(above*(1.5+b.before[other]-b.after[other])));
            if(above>0.95&&above<1.05){equalGained+=b.gained[side];++equalBattles;}
        }
    }

    void Table::Apply(Battle& b)
    {
        Unit* a=Find(b.attacker);
        Unit* d=Find(b.defender);
        const int byA=b.before[1]-b.after[1],byD=b.before[0]-b.after[0];
        a->exp+=b.gained[0];
        d->exp+=b.gained[1];
        a->count=b.after[0];
        d->count=b.after[1];
        // The command gauges fill with members destroyed, and lost.
        Fill(a->who,4*byA+3*byD);
        Fill(d->who,4*byD+3*byA);
        units.erase(std::remove_if(units.begin(),units.end(),[](const Unit& u){return u.count<=0;}),units.end());
    }

    void Table::Fill(int who,int by)
    {
        gauge[static_cast<size_t>(who)]=std::min(GaugeFull,gauge[static_cast<size_t>(who)]+by);
    }

    // ---- The commanders' orders ----

    int SkillCost(Skill s)
    {
        static const int cost[Skills]={50,60,40,60,30};
        return cost[static_cast<int>(s)];
    }

    bool Table::InForce(int who,Skill s) const
    {
        const size_t w=static_cast<size_t>(who);
        switch(s)
        {
        case Skill::Barrage:return barrage[w];
        case Skill::Smoke:return smoke[w];
        case Skill::March:return march[w];
        default:return false;
        }
    }

    // A shot's chance with the orders in force: surer under a barrage, less sure into smoke.
    double Table::ChanceNow(double chance,int shooter,int target) const
    {
        if(barrage[static_cast<size_t>(shooter)])chance*=1.3;
        if(smoke[static_cast<size_t>(target)])chance*=0.6;
        return std::min(chance,0.95);
    }

    double Table::StrikeChance(int target) const
    {
        const Unit* d=UnitById(target);
        if(!d)return 0;
        return ChanceNow(Hit(StrikePower,Info(d->kind).defence*(1+Cover(At(d->at),d->kind))),d->who^1,d->who);
    }

    bool Table::CanCommand(int who,Skill s) const
    {
        const size_t w=static_cast<size_t>(who);
        if(phase!=(who==You?Phase::YourTurn:Phase::HerTurn)||used[w]||gauge[w]<SkillCost(s))return false;
        if(s==Skill::Repair)return std::any_of(units.begin(),units.end(),[&](const Unit& u){return u.who==who&&u.count<FullStrength;});
        if(s==Skill::Strike)return Alive(who^1)>0;
        return true;
    }

    bool Table::Command(int who,Skill s,int target)
    {
        if(!CanCommand(who,s))return false;
        const size_t w=static_cast<size_t>(who);
        Unit* t=nullptr;
        if(s==Skill::Strike)
        {
            t=Find(target);
            if(!t||t->who==who||t->count<=0)return false;
        }
        gauge[w]-=SkillCost(s);
        used[w]=true;
        last={};
        last.who=who;
        last.skill=static_cast<int>(s);
        switch(s)
        {
        case Skill::Barrage:barrage[w]=true;break;
        case Skill::Smoke:smoke[w]=true;break;
        case Skill::March:march[w]=true;break;
        case Skill::Repair:
            for(Unit& u:units)if(u.who==who)u.count=std::min(FullStrength,u.count+2);
            break;
        case Skill::Strike:
        {
            std::uniform_real_distribution<double> any(0,1);
            const double chance=StrikeChance(target);
            last.strikeHex=t->at;
            last.strikeKind=t->kind;
            last.strikeBefore=t->count;
            int hits=0;
            for(int i=0;i<StrikeShots;++i)if(any(rng)<chance)++hits;
            t->count=std::max(0,t->count-hits);
            last.strikeAfter=t->count;
            Fill(t->who,3*(last.strikeBefore-t->count));
            units.erase(std::remove_if(units.begin(),units.end(),[](const Unit& u){return u.count<=0;}),units.end());
            break;
        }
        }
        ++actions;
        CheckEnd();
        return true;
    }

    // Her orders: a strike when it would destroy much (most of all a whole unit), repairs when
    // many members are missing, a barrage when several units can attack this turn, smoke when
    // several stand where they could be hurt.
    bool Table::AutoCommand(int who)
    {
        if(used[static_cast<size_t>(who)])return false;
        const int other=who^1;
        if(CanCommand(who,Skill::Strike))
        {
            int best=-1;
            double worth=0;
            for(const Unit& e:units)
            {
                if(e.who!=other||e.count<=0)continue;
                const double kills=std::min<double>(e.count,StrikeShots*StrikeChance(e.id));
                const double w=kills*Info(e.kind).value+(kills>=e.count-0.5?3*Info(e.kind).value:0);
                if(w>worth){worth=w;best=e.id;}
            }
            if(best>=0&&worth>=8)return Command(who,Skill::Strike,best);
        }
        int missing=0,attackers=0,exposed=0;
        for(const Unit& u:units)
        {
            if(u.who!=who||u.count<=0)continue;
            missing+=(FullStrength-u.count)*Info(u.kind).value;
            bool reaches=false;
            for(const Route& r:ReachFrom(u,u.at))
            {
                for(int d=0;d<6&&!reaches;++d)
                    if(const int n=Next(r.to,d);n>=0)
                        if(const Unit* e=UnitAt(n);e&&e->who==other)reaches=true;
                if(reaches)break;
            }
            attackers+=reaches?1:0;
            exposed+=Danger(u,u.at)>2?1:0;
        }
        if(missing>=14&&CanCommand(who,Skill::Repair))return Command(who,Skill::Repair);
        if(attackers>=3&&CanCommand(who,Skill::Barrage))return Command(who,Skill::Barrage);
        if(exposed>=3&&attackers<2&&CanCommand(who,Skill::Smoke))return Command(who,Skill::Smoke);
        return false;
    }

    // ---- The game ----

    void Table::NewGame(int index,std::uint32_t seed,double c,bool swap)
    {
        swapped=swap;
        rng.seed(seed);
        care=c;
        map=std::clamp(index,0,Maps()-1);
        const MapInfo& m=Map(map);
        columns=m.columns;rows=m.rows;turns=m.turns;
        gauge={StartGauge,StartGauge};
        used={false,false};
        barrage={false,false};smoke={false,false};march={false,false};
        terrain.assign(static_cast<size_t>(columns*rows),Terrain::Plain);
        base={-1,-1};
        auto set=[&](int c,int r,char ch){
            Terrain t=Terrain::Plain;
            switch(ch)
            {
            case 'R':t=Terrain::Rough;break;
            case 'F':t=Terrain::Forest;break;
            case 'M':t=Terrain::Mountain;break;
            case 'W':t=Terrain::River;break;
            case '=':t=Terrain::Road;break;
            case '#':t=Terrain::Bridge;break;
            case 'C':t=Terrain::City;break;
            case 'K':t=Terrain::Factory;break;
            case 'H':t=Terrain::Foundation;break;
            case 'B':t=Terrain::Base;base[You]=Hex(c,r);break;
            case 'b':t=Terrain::Base;base[Her]=Hex(c,r);break;
            }
            terrain[static_cast<size_t>(Hex(c,r))]=t;
        };
        for(int r=0;r<static_cast<int>(m.terrain.size());++r)
            for(int c=0;c<columns;++c)
            {
                const char ch=m.terrain[static_cast<size_t>(r)][c];
                set(c,r,ch);
                // Turned round the middle: your base becomes hers.
                if(m.mirrored)set(columns-1-c,rows-1-r,ch=='B'?'b':ch=='b'?'B':ch);
            }
        units.clear();
        int id=1;
        for(int pass=0;pass<(m.mirrored?2:1);++pass)
            for(const auto& s:m.units)
            {
                Unit u;
                u.id=id++;
                u.kind=s.kind;
                u.who=pass?Her:s.who;
                u.at=pass?Hex(columns-1-s.column,rows-1-s.row):Hex(s.column,s.row);
                units.push_back(u);
            }
        if(swapped)
        {
            std::swap(base[You],base[Her]);
            for(Unit& u:units)u.who=1-u.who;
        }
        bridges.clear();
        factories.clear();
        for(int h=0;h<Hexes();++h)
            if(terrain[static_cast<size_t>(h)]==Terrain::Factory){Factory f;f.hex=h;factories.push_back(f);}
        phase=Phase::YourTurn;
        turn=1;
        winner=-1;
        ending=Ending::None;
        actions=0;
        last={};
        undo={};
        planned=false;
        StartTurn(You);
    }

    int Table::Facing(int who) const
    {
        const int from=base[who],to=base[1-who];
        if(from>=0&&to>=0&&from%columns!=to%columns)return to%columns>from%columns?1:-1;
        return who==You?1:-1;   // one above the other: as on the battle screen
    }

    void Table::StartTurn(int who)
    {
        for(Unit& u:units)
            if(u.who==who)u.moved=u.done=false;
        // Its factories: units of its own on them get members back; what they build comes
        // nearer, and out when done and the hex is free (to act from the side's next turn).
        for(Factory& f:factories)
        {
            if(f.owner!=who)continue;
            Unit* on=FindAt(f.hex);
            if(on&&on->who==who)on->count=std::min(FullStrength,on->count+RepairPerTurn(on->kind));
            if(f.building<0)continue;
            if(f.left>0)--f.left;
            if(f.left==0&&!on)
            {
                Unit u;
                int id=1;
                for(const Unit& o:units)id=std::max(id,o.id+1);
                u.id=id;
                u.kind=static_cast<Kind>(f.building);
                u.who=who;
                u.at=f.hex;
                u.moved=u.done=true;
                units.push_back(u);
                f.building=-1;
            }
        }
        undo={};
        planned=false;
        // Orders end: the side's own as its turn comes round again, the other's barrage and
        // march with the other's turn.
        const size_t w=static_cast<size_t>(who),o=static_cast<size_t>(who^1);
        used[w]=false;
        barrage[w]=smoke[w]=march[w]=false;
        barrage[o]=march[o]=false;
        skillThought=false;
    }

    void Table::CheckEnd()
    {
        if(phase==Phase::GameOver)return;
        for(int who:{You,Her})
        {
            const Unit* on=UnitAt(base[static_cast<size_t>(who^1)]);
            if(on&&on->who==who&&Info(on->kind).captures){phase=Phase::GameOver;winner=who;ending=Ending::Captured;return;}
        }
        for(int who:{You,Her})
            if(!Alive(who)){phase=Phase::GameOver;winner=who^1;ending=Ending::Destroyed;return;}
    }

    bool Table::Move(int id,int to)
    {
        Unit* u=Find(id);
        if(phase!=Phase::YourTurn||!u||u->who!=You)return false;
        const auto routes=Reach(id);
        const auto r=std::find_if(routes.begin(),routes.end(),[&](const Route& x){return x.to==to;});
        if(r==routes.end())return false;
        undo={id,u->at};
        last={};
        last.unit=id;last.who=You;
        if(to!=u->at)last.path=r->path;
        u->at=to;
        u->moved=true;
        Capture(*u);
        ++actions;
        CheckEnd();
        return true;
    }

    bool Table::TakeBack(int id)
    {
        Unit* u=Find(id);
        if(phase!=Phase::YourTurn||!u||undo.id!=id||u->done)return false;
        if(undo.factory>=0)factories[static_cast<size_t>(undo.factory)].owner=undo.owner;
        u->at=undo.from;
        u->moved=false;
        undo={};
        ++actions;
        last={};
        last.unit=id;last.who=You;
        return true;
    }

    bool Table::Attack(int attacker,int defender)
    {
        Unit* a=Find(attacker);
        if(phase!=Phase::YourTurn||!a||a->who!=You)return false;
        const auto targets=Targets(attacker);
        if(std::find(targets.begin(),targets.end(),defender)==targets.end())return false;
        Battle b=Preview(attacker,defender);
        Shoot(b);
        // (A move just before it was shown by itself.)
        last={};
        last.unit=attacker;last.who=You;
        last.fought=true;
        last.battle=b;
        a->moved=a->done=true;
        undo={};
        Apply(b);
        ++actions;
        CheckEnd();
        return true;
    }

    void Table::Finish(int id)
    {
        Unit* u=Find(id);
        if(!u||u->who!=You||phase!=Phase::YourTurn)return;
        u->moved=u->done=true;
        if(undo.id==id)undo={};
    }

    void Table::EndTurn()
    {
        if(phase!=Phase::YourTurn)return;
        for(Unit& u:units)if(u.who==You)u.moved=u.done=true;
        phase=Phase::HerTurn;
        StartTurn(Her);
    }

    void Table::Resign(int who)
    {
        if(phase==Phase::GameOver)return;
        phase=Phase::GameOver;
        winner=who^1;
        ending=Ending::Resigned;
    }

    // ---- Factories ----

    int Table::FactoryAt(int hex) const
    {
        for(size_t i=0;i<factories.size();++i)if(factories[i].hex==hex)return static_cast<int>(i);
        return -1;
    }

    std::vector<Kind> Table::Roster() const
    {
        std::vector<Kind> out;
        for(const auto& s:Map(map).units)
            if(std::find(out.begin(),out.end(),s.kind)==out.end())out.push_back(s.kind);
        std::sort(out.begin(),out.end());
        return out;
    }

    bool Table::CanBuild(int who,int factory,Kind k) const
    {
        if(factory<0||factory>=static_cast<int>(factories.size())||phase==Phase::GameOver)return false;
        if(phase!=(who==You?Phase::YourTurn:Phase::HerTurn))return false;
        const Factory& f=factories[static_cast<size_t>(factory)];
        if(f.owner!=who||f.building>=0)return false;
        const auto roster=Roster();
        if(std::find(roster.begin(),roster.end(),k)==roster.end())return false;
        int count=Alive(who);
        for(const Factory& o:factories)if(o.owner==who&&o.building>=0)++count;
        return count<MostUnits;
    }

    bool Table::Build(int who,int factory,Kind k)
    {
        if(!CanBuild(who,factory,k))return false;
        Factory& f=factories[static_cast<size_t>(factory)];
        f.building=static_cast<int>(k);
        f.left=BuildTurns(k);
        // (Counted as done, with nothing to show: not the move before it again.)
        last={};
        last.who=who;
        ++actions;
        return true;
    }

    void Table::Capture(Unit& u)
    {
        if(!Info(u.kind).captures)return;
        const int f=FactoryAt(u.at);
        if(f<0||factories[static_cast<size_t>(f)].owner==u.who)return;
        if(u.who==You&&phase==Phase::YourTurn){undo.factory=f;undo.owner=factories[static_cast<size_t>(f)].owner;}
        factories[static_cast<size_t>(f)].owner=u.who;
        last.captured=f;
    }

    // What to build: infantry while fewer than two of the side can take bases; against many
    // vehicles, anti-tank infantry; else the strongest vehicle of the map, a gun when it has
    // none; nothing that would not be done before the last turn.
    void Table::AutoBuild(int who)
    {
        const auto roster=Roster();
        auto has=[&](Kind k){return std::find(roster.begin(),roster.end(),k)!=roster.end();};
        int takers=0,guns=0,armour=0,air=0,against=0;
        for(const Unit& u:units)
        {
            if(u.count<=0)continue;
            if(u.who==who){takers+=Info(u.kind).captures?1:0;guns+=Ranged(u.kind)?1:0;against+=Info(u.kind).attackAir>=10?1:0;}
            else if(Info(u.kind).flies)++air;
            else armour+=Info(u.kind).armoured?1:0;
        }
        for(const Factory& o:factories)
            if(o.owner==who&&o.building>=0){const Kind b=static_cast<Kind>(o.building);takers+=Info(b).captures?1:0;guns+=Ranged(b)?1:0;}
        for(size_t i=0;i<factories.size();++i)
        {
            const Factory& f=factories[i];
            if(f.owner!=who||f.building>=0)continue;
            Kind k=Kind::Tank;
            if(takers<2)k=has(Kind::Bazooka)&&armour>=3?Kind::Bazooka:Kind::Infantry;
            else if(air>against&&(has(Kind::AntiAir)||has(Kind::Fighter)))k=has(Kind::AntiAir)&&against%2==0?Kind::AntiAir:has(Kind::Fighter)?Kind::Fighter:Kind::AntiAir;
            else if(has(Kind::Bazooka)&&armour>=4)k=Kind::Bazooka;
            else if(has(Kind::Attacker)&&armour>=3)k=Kind::Attacker;
            else if(has(Kind::Artillery)&&guns==0)k=Kind::Artillery;
            else if(has(Kind::Heavy))k=Kind::Heavy;
            if(turn+BuildTurns(k)>turns)continue;
            if(!has(k))k=Kind::Tank;
            if(Build(who,static_cast<int>(i),k)){takers+=Info(k).captures?1:0;guns+=Ranged(k)?1:0;against+=Info(k).attackAir>=10?1:0;}
        }
    }

    // ---- Bridges ----

    std::vector<int> Table::BridgeSpotsFrom(const Unit& u,int at) const
    {
        std::vector<int> out;
        if(!Map(map).bridging||!Info(u.kind).captures)return out;
        for(int d=0;d<6;++d)
        {
            const int n=Next(at,d);
            if(n>=0&&At(n)==Terrain::River)out.push_back(n);
        }
        return out;
    }

    std::vector<int> Table::BridgeSpots(int id) const
    {
        const Unit* u=UnitById(id);
        if(!u||u->count<=0||u->done||phase==Phase::GameOver)return {};
        return BridgeSpotsFrom(*u,u->at);
    }

    bool Table::Bridge(int id,int hex)
    {
        Unit* u=Find(id);
        if(phase!=Phase::YourTurn||!u||u->who!=You)return false;
        const auto spots=BridgeSpots(id);
        if(std::find(spots.begin(),spots.end(),hex)==spots.end())return false;
        terrain[static_cast<size_t>(hex)]=Terrain::Bridge;
        bridges.push_back(hex);
        u->moved=u->done=true;
        undo={};
        last={};
        last.unit=id;last.who=You;last.bridged=hex;
        ++actions;
        return true;
    }

    double Table::BridgeWorth(int who,int hex)
    {
        const int other=who^1;
        auto sum=[&](int side,const std::vector<double>& field){
            double s=0;
            for(const Unit& u:units)
                if(u.who==side&&u.count>0&&Info(u.kind).armoured)s+=std::min(field[static_cast<size_t>(u.at)],60.0);
            return s;
        };
        const auto toOther=Field(Kind::Tank,{base[static_cast<size_t>(other)]}),toMine=Field(Kind::Tank,{base[static_cast<size_t>(who)]});
        const double before=sum(who,toOther),theirs=sum(other,toMine);
        terrain[static_cast<size_t>(hex)]=Terrain::Bridge;
        const double after=sum(who,Field(Kind::Tank,{base[static_cast<size_t>(other)]})),theirsAfter=sum(other,Field(Kind::Tank,{base[static_cast<size_t>(who)]}));
        terrain[static_cast<size_t>(hex)]=Terrain::River;
        return (before-after)-0.7*(theirs-theirsAfter);
    }

    // ---- Her play ----

    // How much worse off u's side is likely to be at a hex next turn, from the other side's
    // units that could reach it then: each one's attack as if alone, what it would destroy less
    // what u's shots back would (in members' worth); the worst in full, the rest a little less.
    double Table::Danger(const Unit& u,int at) const
    {
        std::vector<double> losses;
        const double guard=Info(u.kind).defence*(1+Cover(At(at),u.kind));
        for(const Unit& e:units)
        {
            if(e.who==u.who||e.count<=0||!AttackOn(e.kind,u.kind))continue;
            const double back=Ranged(u.kind)||Ranged(e.kind)?0:Grown(AttackOn(u.kind,e.kind),u.exp);
            if(Ranged(e.kind))
            {
                // A gun fires from where it stands: two or three hexes off, nothing back.
                const int gap=Distance(e.at,at);
                if(gap<Info(e.kind).rangeMin||gap>Info(e.kind).rangeMax)continue;
            }
            else if(Distance(e.at,at)>Info(e.kind).move+1)continue;
            // Close enough by count; by the ground between, only when it can get next to it.
            bool reaches=Ranged(e.kind)||Distance(e.at,at)<=1;
            if(!reaches)
            {
                const auto known=std::find(threatIds.begin(),threatIds.end(),e.id);
                if(known!=threatIds.end())reaches=threats[static_cast<size_t>(known-threatIds.begin())][static_cast<size_t>(at)]!=0;
                else for(const Route& r:ReachFrom(e,e.at))if(Adjacent(r.to,at)){reaches=true;break;}
            }
            if(!reaches)continue;
            const double lost=std::min<double>(u.count,e.count*Hit(Grown(AttackOn(e.kind,u.kind),e.exp),guard));
            const double hurt=std::min<double>(e.count,u.count*Hit(back,Info(e.kind).defence));
            const double net=lost*Info(u.kind).value-hurt*Info(e.kind).value;
            if(net>0)losses.push_back(net);
        }
        std::sort(losses.begin(),losses.end(),std::greater<>());
        double sum=0;
        for(size_t i=0;i<losses.size();++i)sum+=i?0.7*losses[i]:losses[i];
        return sum;
    }

    // The worth of u going to a hex and attacking target there (-1: none), for its side.
    double Table::Score(const Unit& u,int to,int target) const
    {
        const int who=u.who,other=who^1;
        const KindInfo& me=Info(u.kind);
        double s=0;
        double left=u.count;
        if(target>=0)
        {
            const Unit* t=UnitById(target);
            const Battle b=Fight(u,to,*t,u.id,to);
            s+=1.2*b.expected[0]*Info(t->kind).value-b.expected[1]*me.value;
            if(b.expected[0]>=t->count-0.5)s+=2.0*Info(t->kind).value;
            // One that could take her base next turn is worth stopping at any cost.
            if(baseAtRisk&&Info(t->kind).captures&&Distance(t->at,base[static_cast<size_t>(who)])<=Info(t->kind).move+1)
                s+=6*b.expected[0]+(b.expected[0]>=t->count-0.5?30:0);
            left-=b.expected[1];
        }
        if(me.captures&&to==base[static_cast<size_t>(other)])return 1000;
        // Factories: infantry takes those not its side's (the more if one is building, or is
        // the other's); a worn unit goes to one of its own to be mended.
        if(const int f=FactoryAt(to);f>=0)
        {
            const Factory& fa=factories[static_cast<size_t>(f)];
            if(me.captures&&fa.owner!=who)s+=10+(fa.building>=0?6:0)+(fa.owner==other?4:0);
            if(fa.owner==who&&u.count<FullStrength)s+=(u.count<=6?0.6:0.2)*(FullStrength-u.count)*me.value;
        }
        if(me.captures)
        {
            int gap=99;
            for(const Factory& fa:factories)if(fa.owner!=who)gap=std::min(gap,Distance(to,fa.hex));
            if(gap<99)s-=0.15*gap;
        }
        // Not to be left where the other side can hurt it much.
        if(left>0)s-=0.8*Danger(u,to)*left/std::max(1,u.count);
        s+=0.3*Cover(At(to),u.kind)*me.value;
        // Forward, the more as the turns go by: infantry toward the other's base, everyone
        // toward the other side.
        const double press=std::min(1.6,1+0.05*turn);
        // (How far by the ground, when it has been worked out: round ridges, over bridges.)
        const size_t k=static_cast<size_t>(u.kind);
        const bool ground=k<toBase.size()&&!toBase[k].empty();
        const double home=ground?toBase[k][static_cast<size_t>(to)]:Distance(to,base[static_cast<size_t>(other)]);
        if(me.captures&&home<1e5)s-=0.45*press*home;
        double nearest=99;
        if(ground)nearest=toFoe[k][static_cast<size_t>(to)];
        else for(const Unit& e:units)if(e.who==other&&e.count>0)nearest=std::min<double>(nearest,Distance(to,e.at));
        if(Ranged(u.kind))
        {
            // A gun keeps off: in range of one of the other side, not next to any.
            bool next=false;
            double gap=99;
            for(const Unit& e:units)
                if(e.who==other&&e.count>0){gap=std::min<double>(gap,Distance(to,e.at));next=next||Distance(to,e.at)<=1;}
            if(next)s-=3*me.value;
            if(nearest<1e5)s-=0.3*press*std::max(0.0,std::min(nearest,40.0)-2);
        }
        else if(nearest<1e5)s-=(me.captures?0.1:0.5)*press*std::min(nearest,40.0);
        // Its own base held while the other's infantry could take it next turn; raiders
        // within two turns of it hunted down, the more the closer they are.
        if(baseAtRisk&&to==base[static_cast<size_t>(who)])s+=40;
        // One of its infantry within three turns of the base and none of ours on it: the unit
        // nearest the base goes back to hold it.
        if(u.id==guardId)s+=to==base[static_cast<size_t>(who)]?30:-1.5*Distance(to,base[static_cast<size_t>(who)]);
        for(const Unit& e:units)
        {
            if(e.who!=other||e.count<=0||!Info(e.kind).captures)continue;
            const int reach=2*Info(e.kind).move+2,gap=Distance(e.at,base[static_cast<size_t>(who)]);
            if(gap>=reach)continue;
            const double w=reach-gap;
            if(!me.captures)s-=0.25*w*Distance(to,e.at);
            if(target==e.id)s+=0.8*w;
        }
        return s;
    }

    // How far each hex is from the nearest of sources for a kind, in movement points over the
    // ground (units left out; 1e6 where it cannot get).
    std::vector<double> Table::Field(Kind kind,const std::vector<int>& sources) const
    {
        std::vector<double> dist(static_cast<size_t>(Hexes()),1e6);
        using Item=std::pair<double,int>;
        std::priority_queue<Item,std::vector<Item>,std::greater<>> open;
        for(int s:sources)if(s>=0&&dist[static_cast<size_t>(s)]>0){dist[static_cast<size_t>(s)]=0;open.push({0,s});}
        while(!open.empty())
        {
            const auto [cost,at]=open.top();
            open.pop();
            if(cost>dist[static_cast<size_t>(at)])continue;
            const int step=Info(kind).cost[static_cast<size_t>(At(at))];
            if(!step&&cost>0)continue;
            for(int d=0;d<6;++d)
            {
                const int n=Next(at,d);
                if(n<0||!Info(kind).cost[static_cast<size_t>(At(n))])continue;
                // Moving from n to at costs at's terrain: the field is walked backwards.
                const double c=cost+std::max(1,step);
                if(c<dist[static_cast<size_t>(n)]){dist[static_cast<size_t>(n)]=c;open.push({c,n});}
            }
        }
        return dist;
    }

    // The best order for her side (who) now: of every unit not done, every hex it may go to and
    // every attack from there; at less than full care now and then the next best of that unit.
    Order Table::Choose()
    {
        const int who=phase==Phase::HerTurn?Her:You;
        Order best;
        best.score=-1e18;
        std::vector<Order> options;
        // Where each of the other side could attack next turn, worked out once, and whether
        // one of them could take the base then.
        baseAtRisk=false;
        // How far each hex is by the ground for each kind, from the other's base and from the
        // other's units next to which it could stand.
        toBase.assign(Kinds,{});
        toFoe.assign(Kinds,{});
        for(int k=0;k<Kinds;++k)
        {
            // (Toward those it is good against: as good as half its best attack, or any.)
            const KindInfo& me=Info(static_cast<Kind>(k));
            const int best=std::max({me.attack,me.attackArmour,me.attackAir});
            std::vector<int> foes;
            for(int pass=0;pass<2&&foes.empty();++pass)
                for(const Unit& e:units)
                    if(e.who!=who&&e.count>0&&AttackOn(static_cast<Kind>(k),e.kind)*(pass?1:2)>=(pass?1:best))
                        for(int d=0;d<6;++d)if(const int n=Next(e.at,d);n>=0)foes.push_back(n);
            toBase[static_cast<size_t>(k)]=Field(static_cast<Kind>(k),{base[static_cast<size_t>(who^1)]});
            toFoe[static_cast<size_t>(k)]=Field(static_cast<Kind>(k),foes);
        }
        guardId=-1;
        {
            const int home=base[static_cast<size_t>(who)];
            const Unit* held=UnitAt(home);
            bool raided=false;
            for(const Unit& e:units)
                if(e.who!=who&&e.count>0&&Info(e.kind).captures&&Distance(e.at,home)<=3*Info(e.kind).move+1)raided=true;
            if(raided&&!(held&&held->who==who))
            {
                int nearest=1<<20;
                for(const Unit& u:units)
                    if(u.who==who&&!u.done&&u.count>0&&Distance(u.at,home)<nearest){nearest=Distance(u.at,home);guardId=u.id;}
            }
        }
        threatIds.clear();
        threats.clear();
        for(const Unit& e:units)
        {
            if(e.who==who||e.count<=0)continue;
            std::vector<char> hexes(static_cast<size_t>(Hexes()),0);
            for(const Route& r:ReachFrom(e,e.at))
            {
                for(int d=0;d<6;++d)if(const int n=Next(r.to,d);n>=0)hexes[static_cast<size_t>(n)]=1;
                // (It may not stand on the base now that a unit of ours does, but could if it left.)
                if(Info(e.kind).captures&&(r.to==base[static_cast<size_t>(who)]||Adjacent(r.to,base[static_cast<size_t>(who)])))baseAtRisk=true;
            }
            threatIds.push_back(e.id);
            threats.push_back(std::move(hexes));
        }
        std::map<int,double> worth;   // what bridging each river hex would be worth (worked out once)
        for(const Unit& u:units)
        {
            if(u.who!=who||u.done||u.count<=0)continue;
            const auto routes=ReachFrom(u,u.at);
            for(const Route& r:routes)
            {
                options.push_back({u.id,r.to,-1,Score(u,r.to,-1)});
                for(int e:TargetsFrom(u,r.to,r.to!=u.at))options.push_back({u.id,r.to,e,Score(u,r.to,e)});
                for(int h:BridgeSpotsFrom(u,r.to))
                {
                    auto known=worth.find(h);
                    if(known==worth.end())known=worth.emplace(h,BridgeWorth(who,h)).first;
                    if(known->second>2)options.push_back({u.id,r.to,-1,Score(u,r.to,-1)+std::min(30.0,1.5*known->second),h});
                }
            }
        }
        threatIds.clear();
        threats.clear();
        if(options.empty())return {};
        // The unit with the best thing to do goes first.
        const auto top=*std::max_element(options.begin(),options.end(),[](const Order& a,const Order& b){return a.score<b.score;});
        best=top;
        double careNow=care*(who==Her&&HerShare()>0.65?0.8:1.0);
        std::uniform_real_distribution<double> any(0,1);
        if(who==Her&&top.score<500&&any(rng)>careNow)
        {
            // The next best of the same unit, if not much worse.
            Order second;
            second.score=-1e18;
            for(const Order& o:options)
                if(o.unit==top.unit&&(o.to!=top.to||o.target!=top.target||o.bridge!=top.bridge)&&o.score>second.score)second=o;
            if(second.unit>=0&&second.score>top.score-2.5)best=second;
        }
        return best;
    }

    Order Table::HerNext()
    {
        if(phase!=Phase::HerTurn)return {};
        if(!planned){plan=Choose();planned=true;}
        return plan;
    }

    void Table::Step()
    {
        if(phase!=Phase::HerTurn)return;
        // Far behind: she gives up.
        if(Alive(Her)&&HerShare()<0.12&&std::none_of(units.begin(),units.end(),[](const Unit& u){return u.who==Her&&u.done;}))
        {
            Resign(Her);
            return;
        }
        // Her order for the turn, first (shown as a step of its own).
        if(!skillThought)
        {
            skillThought=true;
            AutoBuild(Her);
            if(AutoCommand(Her))return;
        }
        const Order o=HerNext();
        planned=false;
        if(o.unit<0)
        {
            // Her turn over: the next turn, or the game's end by the turns.
            ++turn;
            if(turn>turns)
            {
                phase=Phase::GameOver;
                ending=Ending::TurnLimit;
                const int h=Strength(Her),y=Strength(You);
                winner=h>y?Her:y>h?You:-1;
                return;
            }
            phase=Phase::YourTurn;
            StartTurn(You);
            return;
        }
        Unit* u=Find(o.unit);
        last={};
        last.unit=o.unit;last.who=u->who;
        if(o.to!=u->at)
            for(const Route& r:ReachFrom(*u,u->at))if(r.to==o.to){last.path=r.path;break;}
        u->at=o.to;
        u->moved=u->done=true;
        Capture(*u);
        if(o.bridge>=0)
        {
            terrain[static_cast<size_t>(o.bridge)]=Terrain::Bridge;
            bridges.push_back(o.bridge);
            last.bridged=o.bridge;
        }
        if(o.target>=0)
        {
            Battle b=Preview(o.unit,o.target);
            Shoot(b);
            last.fought=true;
            last.battle=b;
            Apply(b);
        }
        ++actions;
        CheckEnd();
    }

    void Table::PlayTurnForYou()
    {
        AutoBuild(You);
        AutoCommand(You);
        for(int guard=0;guard<100&&phase==Phase::YourTurn;++guard)
        {
            const Order o=Choose();
            if(o.unit<0)break;
            Unit* u=Find(o.unit);
            if(o.to!=u->at&&!Move(o.unit,o.to))break;
            if(o.bridge>=0&&phase==Phase::YourTurn)Bridge(o.unit,o.bridge);
            else if(o.target>=0&&phase==Phase::YourTurn)Attack(o.unit,o.target);
            else Finish(o.unit);
        }
        EndTurn();
    }

    bool Table::Consistent() const
    {
        for(size_t i=0;i<units.size();++i)
        {
            const Unit& u=units[i];
            if(u.count<=0||u.count>FullStrength||u.at<0||u.at>=Hexes())return false;
            if(!Info(u.kind).cost[static_cast<size_t>(At(u.at))])return false;
            for(size_t j=i+1;j<units.size();++j)if(units[j].at==u.at||units[j].id==u.id)return false;
        }
        return true;
    }

    // ---- Diagnostics ----

    std::wstring CheckRules()
    {
        std::wostringstream out,lines;
        int wrong=0;
        auto check=[&](bool ok,const wchar_t* what){if(!ok){++wrong;lines<<L"WRONG: "<<what<<L"\n";}else lines<<L"ok: "<<what<<L"\n";};
        Table t;
        t.NewGame(Maps()-1,1,1);
        // Directions: d and d+3 undo each other; distance 1 for every neighbour.
        bool geometry=true;
        for(int h=0;h<t.Hexes();++h)
            for(int d=0;d<6;++d)
            {
                const int n=t.Next(h,d);
                if(n<0)continue;
                if(t.Next(n,(d+3)%6)!=h||t.Distance(h,n)!=1)geometry=false;
            }
        check(geometry,L"hex directions and distances");
        check(t.Distance(t.Hex(0,0),t.Hex(3,0))==3&&t.Distance(t.Hex(0,0),t.Hex(0,4))==4,L"distances along a column and a row");
        // The map turned round its middle.
        bool mirrored=true;
        for(int h=0;h<t.Hexes();++h)
        {
            const int c=h%t.Columns(),r=h/t.Columns(),m=t.Hex(t.Columns()-1-c,t.Rows()-1-r);
            if(t.At(h)!=t.At(m))mirrored=false;
            for(int d=0;d<6;++d)
            {
                const int n=t.Next(h,d),nm=t.Next(m,(d+3)%6);
                if((n<0)!=(nm<0)||(n>=0&&t.Hex(t.Columns()-1-n%t.Columns(),t.Rows()-1-n/t.Columns())!=nm))mirrored=false;
            }
        }
        check(mirrored&&t.BaseOf(You)>=0&&t.BaseOf(Her)>=0&&t.Alive(You)==6&&t.Alive(Her)==6&&t.Consistent(),L"the plain is the same for both sides");
        for(int m=0;m<Maps();++m)
        {
            Table x;
            x.NewGame(m,1,1);
            bool rows=true;
            for(const char* line:Map(m).terrain)if(static_cast<int>(std::strlen(line))!=Map(m).columns)rows=false;
            check(rows&&x.BaseOf(You)>=0&&x.BaseOf(Her)>=0&&x.Alive(You)>0&&x.Alive(You)==x.Alive(Her)&&x.Consistent(),L"each map's rows, bases and armies");
            Table y;
            y.NewGame(m,1,1,true);
            bool sides=y.BaseOf(You)==x.BaseOf(Her)&&y.BaseOf(Her)==x.BaseOf(You)&&y.Consistent();
            for(size_t i=0;i<x.units.size();++i)sides=sides&&y.units[i].at==x.units[i].at&&y.units[i].who==1-x.units[i].who;
            check(sides,L"each map played from the other side");
        }

        // A bare board to set positions on.
        auto bare=[&](Table& x){
            x.NewGame(0,1,1);
            for(auto& c:x.terrain)c=Terrain::Plain;
            x.units.clear();
        };
        auto put=[&](Table& x,Kind k,int who,int c,int r){Unit u;u.id=static_cast<int>(x.units.size())+1;u.kind=k;u.who=who;u.at=x.Hex(c,r);x.units.push_back(u);return u.id;};
        auto reaches=[&](const Table& x,int id,int c,int r){for(const Route& q:x.Reach(id))if(q.to==x.Hex(c,r))return true;return false;};
        {
            Table x;bare(x);
            const int tank=put(x,Kind::Tank,You,7,5),inf=put(x,Kind::Infantry,You,13,2);
            check(x.Reach(tank).size()==91,L"a tank on open plain reaches 5 hexes round (91)");
            x.terrain[static_cast<size_t>(x.Hex(8,5))]=Terrain::Mountain;
            x.terrain[static_cast<size_t>(x.Hex(14,2))]=Terrain::Mountain;
            check(!reaches(x,tank,8,5),L"a tank cannot climb a mountain");
            check(reaches(x,inf,14,2),L"infantry climbs a mountain next to it");
            x.terrain[static_cast<size_t>(x.Hex(15,2))]=Terrain::Mountain;
            check(!reaches(x,inf,15,2),L"but not two (a third of its movement each)");
            x.terrain[static_cast<size_t>(x.Hex(13,3))]=Terrain::Rough;
            check(reaches(x,inf,13,4)&&!reaches(x,inf,13,5),L"rough costs infantry two");
            x.terrain[static_cast<size_t>(x.Hex(6,5))]=Terrain::Foundation;
            x.terrain[static_cast<size_t>(x.Hex(12,2))]=Terrain::Foundation;
            check(!reaches(x,tank,6,5)&&reaches(x,inf,12,2),L"foundations: closed to a tank, open to infantry");
        }
        {
            // Aircraft: over anything, shot at only by those with an attack on aircraft.
            Table x;bare(x);
            const int fighter=put(x,Kind::Fighter,You,3,5);
            for(int c=4;c<=7;++c)x.terrain[static_cast<size_t>(x.Hex(c,5))]=c%2?Terrain::River:Terrain::Foundation;
            x.terrain[static_cast<size_t>(x.Hex(8,5))]=Terrain::Mountain;
            check(reaches(x,fighter,8,5)&&reaches(x,fighter,9,5),L"a fighter flies over rivers, foundations and mountains");
            const int tank=put(x,Kind::Tank,Her,3,4);
            bool past=false;
            for(const Route& q:x.Reach(fighter))if(q.to==x.Hex(5,4))past=true;
            check(!past,L"a tank holds a fighter back too (no flying on past it)");
            const int inf=put(x,Kind::Infantry,Her,2,5);
            const auto byTank=x.TargetsFrom(x.units[1],x.units[1].at,false),byInf=x.TargetsFrom(x.units[2],x.units[2].at,false);
            check(byTank.empty()&&byInf.size()==1,L"a tank cannot shoot at a fighter; infantry can");
            check(x.Preview(fighter,inf).answers&&!x.Preview(fighter,tank).answers,L"infantry fires back at a fighter, a tank does not");
            // Caught between two tanks with no attack on aircraft, it is surrounded all the same.
            x.units[2].at=x.Hex(3,6);x.units[2].kind=Kind::Tank;
            check(x.Surrounded(fighter),L"a fighter between two tanks is surrounded");
        }
        {
            // Experience: for coming through a battle, and more against one that ranks above.
            Table x;bare(x);
            const int inf=put(x,Kind::Infantry,You,5,5),tank=put(x,Kind::Tank,Her,6,5);
            bool rule=true;
            for(int i=0;i<20;++i)
            {
                Battle b=x.Preview(inf,tank);
                x.Shoot(b);
                for(int side=0;side<2;++side)
                {
                    const double above=side?1.0/3:3.0;
                    const int want=b.after[side]>0?static_cast<int>(std::lround(above*(1.5+b.before[side^1]-b.after[side^1]))):0;
                    if(b.gained[side]!=want)rule=false;
                }
                if(b.after[0]>0&&b.gained[0]<5)rule=false;   // infantry coming through a tank's fire, even with none destroyed
            }
            check(rule,L"experience: 1.5 and the members destroyed, times the other's rank over its own (a third to three times)");
        }
        {
            // Support: an aircraft only from those that can shoot at aircraft.
            Table x;bare(x);
            const int attacker=put(x,Kind::Attacker,You,5,5),tank=put(x,Kind::Tank,Her,6,5);
            put(x,Kind::Tank,You,7,5);
            check(x.Preview(attacker,tank).support==0&&!x.Preview(attacker,tank).answers,L"an attack aircraft: no support from a tank, no fire back from one");
            put(x,Kind::AntiAir,You,6,4);
            check(x.Preview(attacker,tank).support>0,L"but support from a missile truck");
            Table y;bare(y);
            const int fighter=put(y,Kind::Fighter,You,5,5),other=put(y,Kind::Attacker,Her,6,5);
            const double alone=y.Preview(fighter,other).defence[1];
            put(y,Kind::Tank,Her,5,4);
            check(std::abs(y.Preview(fighter,other).defence[1]-alone)<1e-9,L"a tank adds no defence to an aircraft");
            check(y.TargetsFrom(y.units[1],y.units[1].at,false).empty(),L"an attack aircraft cannot shoot at a fighter");
        }
        {
            Table x;bare(x);
            const int tank=put(x,Kind::Tank,You,2,5);
            put(x,Kind::Tank,Her,5,5);
            // Next to her tank it stops: no route goes on through such a hex.
            bool stops=reaches(x,tank,4,5);
            for(const Route& q:x.Reach(tank))
                for(size_t i=1;i+1<q.path.size();++i)if(x.HeldBy(q.path[i],You))stops=false;
            check(stops,L"a hex next to the other side stops a unit");
            x.units[0].at=x.Hex(4,5);
            const auto r=x.Reach(tank);
            bool oneStep=!r.empty();
            for(const Route& q:r)if(x.Distance(q.to,x.Hex(4,5))>1)oneStep=false;
            check(oneStep,L"from next to the other side, one hex only");
            check(x.Targets(tank).size()==1,L"the one next to it may be attacked");
        }
        {
            Table x;bare(x);
            const int a=put(x,Kind::Tank,You,4,5),d=put(x,Kind::Tank,Her,5,5);
            const Battle alone=x.Preview(a,d);
            const int helper=put(x,Kind::Tank,You,5,6);   // next to the defender (below it)
            const Battle helped=x.Preview(a,d);
            check(helped.attack[0]>alone.attack[0]&&helped.defence[1]==alone.defence[1],L"a neighbour of the defender adds to the attack only");
            const int guard=put(x,Kind::Tank,Her,4,4);    // next to the attacker
            const Battle guarded=x.Preview(a,d);
            check(guarded.defence[1]>helped.defence[1]&&guarded.attack[1]==helped.attack[1],L"a neighbour of the attacker adds to the defence only");
            (void)guard;
            // Surrounded: attacker at (4,5) is upper left of (5,5)? Put one on the opposite side.
            const int dd=x.Hex(5,5);
            int toward=-1;
            for(int k=0;k<6;++k)if(x.Next(dd,k)==x.Hex(4,5))toward=k;
            x.units[static_cast<size_t>(helper-1)].at=x.Next(dd,(toward+3)%6);
            check(x.Surrounded(d)&&x.Preview(a,d).surrounded[1],L"two on opposite sides surround");
            x.units[static_cast<size_t>(helper-1)].at=x.Next(dd,(toward+2)%6);
            check(!x.Surrounded(d),L"two not opposite do not");
            put(x,Kind::Tank,You,0,0);
            x.units.back().at=x.Next(dd,(toward+4)%6);
            check(x.Surrounded(d),L"three with a hex between each surround");
            // One tank left, three full ones beside the infantry it attacks, surrounded: it
            // can destroy the whole unit.
            {
                Table y;bare(y);
                const int lone=put(y,Kind::Tank,You,4,5),target=put(y,Kind::Infantry,Her,5,5);
                y.units[0].count=1;
                const int dh=y.Hex(5,5);
                int k0=-1;
                for(int k=0;k<6;++k)if(y.Next(dh,k)==y.Hex(4,5))k0=k;
                for(int k:{2,3,4}){put(y,Kind::Tank,You,0,0);y.units.back().at=y.Next(dh,(k0+k)%6);}
                const Battle b=y.Preview(lone,target);
                check(b.support>=10&&b.expected[0]>=9.5,L"one member, well supported, can destroy a whole unit");
                int wiped=0;
                for(int n=0;n<20;++n){Battle c=b;y.Shoot(c);if(c.after[1]==0)++wiped;}
                check(wiped>=10,L"and does, more often than not");
            }
        }
        {
            Table x;bare(x);
            x.terrain[static_cast<size_t>(x.Hex(15,5))]=Terrain::Base;
            x.base[Her]=x.Hex(15,5);
            const int inf=put(x,Kind::Infantry,You,13,5);
            put(x,Kind::Tank,Her,0,0);
            check(x.Move(inf,x.Hex(15,5))&&x.Now()==Phase::GameOver&&x.Winner()==You&&x.HowEnded()==Ending::Captured,L"infantry on her base wins");
        }
        {
            Table x;bare(x);
            const int a=put(x,Kind::Tank,You,4,5),d=put(x,Kind::Infantry,Her,5,5);
            x.units[1].count=1;
            int tries=0;
            while(x.Now()!=Phase::GameOver&&tries<50)
            {
                x.units[0].done=x.units[0].moved=false;
                x.Attack(a,d);
                ++tries;
            }
            check(x.Now()==Phase::GameOver&&x.Winner()==You&&x.HowEnded()==Ending::Destroyed&&x.UnitById(a)->exp>=1,L"the last unit destroyed ends it, with experience");
        }
        {
            // The orders: each paid from the gauge, one a turn, each doing what it says.
            Table x;bare(x);
            const int mine=put(x,Kind::Tank,You,4,5),hers=put(x,Kind::Tank,Her,5,5);
            const double plain=x.Preview(mine,hers).chance[0];
            x.gauge={100,100};
            check(x.Command(You,Skill::Barrage)&&x.Gauge(You)==100-SkillCost(Skill::Barrage)&&x.Preview(mine,hers).chance[0]>plain*1.25,L"a barrage: surer shots, paid from the gauge");
            check(!x.Command(You,Skill::Smoke),L"one order a turn");
            x.units[1].at=x.Hex(15,10);   // (away, not to stop it)
            const size_t before=x.Reach(mine).size();
            x.used[You]=false;
            x.gauge={100,100};
            x.Command(You,Skill::March);
            check(x.Reach(mine).size()>before,L"a march: further");
            x.units[1].at=x.Hex(5,5);
            x.used[You]=false;
            x.gauge={100,100};
            x.units[0].count=5;
            x.Command(You,Skill::Repair);
            check(x.units[0].count==7,L"repairs: two members back");
            x.used[You]=false;
            x.gauge={100,100};
            check(x.Command(You,Skill::Strike,hers)&&x.LastAction().strikeBefore==10&&x.LastAction().strikeAfter<10,L"a strike destroys members");
            x.EndTurn();
            check(!x.InForce(You,Skill::Barrage)&&!x.InForce(You,Skill::March),L"a barrage and a march end with the turn");
            x.smoke[Her]=true;
            check(x.Preview(mine,hers).chance[0]<plain,L"smoke: shots at the side less sure");
        }
        {
            // The gun: two or three hexes off, not next to it, not after moving; never fired back at.
            Table x;bare(x);
            const int gun=put(x,Kind::Artillery,You,4,5);
            const int beside=put(x,Kind::Tank,Her,5,5),off=put(x,Kind::Tank,Her,7,5),beyond=put(x,Kind::Tank,Her,12,5);
            const auto aims=x.Targets(gun);
            auto aimsAt=[&](int id){return std::find(aims.begin(),aims.end(),id)!=aims.end();};
            check(!aimsAt(beside)&&aimsAt(off)&&!aimsAt(beyond),L"a gun fires two or three hexes off");
            const Battle b=x.Preview(gun,off);
            check(!b.answers&&b.expected[1]==0&&b.expected[0]>0,L"a gun's shells are not fired back at");
            check(!x.Preview(beside,gun).answers,L"a gun does not fire back");
            x.Attack(gun,off);
            check(x.LastAction().battle.shots[1].empty()&&x.UnitById(gun)->count==10,L"a gun's battle: no shots back");
            Table y;bare(y);
            const int moving=put(y,Kind::Artillery,You,4,5);
            put(y,Kind::Tank,Her,7,5);
            y.Move(moving,y.Hex(4,6));
            check(y.Targets(moving).empty(),L"a gun that moved does not fire");
        }
        {
            // Anti-tank infantry: much more against vehicles than against men; heavy tanks hold.
            Table x;bare(x);
            const int at=put(x,Kind::Bazooka,You,4,5),tank=put(x,Kind::Tank,Her,5,5);
            const double onTank=x.Preview(at,tank).chance[0];
            x.units[1].kind=Kind::Infantry;
            const double onMen=x.Preview(at,tank).chance[0];
            check(onTank>1.5*onMen,L"anti-tank infantry: strong on vehicles, weak on men");
            x.units[1].kind=Kind::Heavy;
            check(x.Preview(at,tank).chance[0]<onTank,L"a heavy tank holds better than a tank");
            x.units[0].kind=Kind::Scout;
            x.units[0].at=x.Hex(2,5);
            x.units[1].at=x.Hex(15,10);
            check(x.Reach(at).size()>x.Range(tank).size(),L"a scout car goes further than a heavy tank");
        }
        {
            // Bridges: on the first maps infantry bridges a river hex next to it (and is done);
            // tanks then cross. Not on maps without bridging, not by tanks.
            Table x;
            x.NewGame(0,1,1);
            for(auto& c:x.terrain)if(c!=Terrain::River)c=Terrain::Plain;
            x.units.clear();
            x.factories.clear();
            int river=-1;
            for(int h=0;h<x.Hexes()&&river<0;++h)
                if(x.At(h)==Terrain::River&&h%x.Columns()>1&&h%x.Columns()<x.Columns()-2)river=h;
            const int beside=x.Next(river,1)>=0&&x.At(x.Next(river,1))!=Terrain::River?x.Next(river,1):x.Next(river,4);
            const int inf=put(x,Kind::Infantry,You,beside%x.Columns(),beside/x.Columns());
            const int tank=put(x,Kind::Tank,You,0,0);
            put(x,Kind::Tank,Her,x.Columns()-1,x.Rows()-1);
            const auto spots=x.BridgeSpots(inf);
            check(std::find(spots.begin(),spots.end(),river)!=spots.end()&&x.BridgeSpots(tank).empty(),L"infantry may bridge the river next to it, tanks not");
            check(x.Bridge(inf,river)&&x.At(river)==Terrain::Bridge&&x.UnitById(inf)->done&&x.LastAction().bridged==river,L"a bridge built: the river crossable, the unit done");
            Table y;
            y.NewGame(2,1,1);
            bool any=false;
            for(const Unit& u:y.Units())if(u.who==You&&!y.BridgeSpots(u.id).empty())any=true;
            check(!any&&!Map(2).bridging&&Map(0).bridging&&Map(1).bridging,L"bridging only on the first two maps");
        }
        {
            // Factories: taken by infantry (given back with the move taken back), mending the
            // unit on them, building what the map's armies have; taken while building, what is
            // built is the taker's; out only onto a free hex.
            Table x;bare(x);
            const int fh=x.Hex(8,5);
            x.terrain[static_cast<size_t>(fh)]=Terrain::Factory;
            x.factories={Factory{fh}};
            const int inf=put(x,Kind::Infantry,You,7,5),tank=put(x,Kind::Tank,You,2,2);
            put(x,Kind::Tank,Her,15,10);
            check(x.Move(inf,fh)&&x.Factories()[0].owner==You&&x.LastAction().captured==0,L"infantry takes a factory");
            x.TakeBack(inf);
            check(x.Factories()[0].owner==-1,L"a move taken back gives the factory back");
            x.Move(inf,fh);
            x.Finish(inf);
            check(!x.Build(You,0,Kind::Heavy)&&x.Build(You,0,Kind::Tank)&&!x.Build(You,0,Kind::Infantry),L"building: only the map's kinds, one at a time");
            x.Find(inf)->count=4;
            const int before=static_cast<int>(x.units.size());
            x.EndTurn();
            while(x.Now()==Phase::HerTurn)x.Step();
            check(x.UnitById(inf)->count==4+RepairPerTurn(Kind::Infantry),L"a factory mends the unit on it");
            check(static_cast<int>(x.units.size())==before&&x.Factories()[0].left==BuildTurns(Kind::Tank)-1,L"what is built waits while the factory's hex is taken");
            (void)tank;
            // Taken by her while building: the tank comes out hers.
            Table y;bare(y);
            y.terrain[static_cast<size_t>(fh)]=Terrain::Factory;
            y.factories={Factory{fh,You,static_cast<int>(Kind::Tank),1}};
            put(y,Kind::Tank,You,2,2);
            const int hers=put(y,Kind::Infantry,Her,9,5);
            y.EndTurn();
            y.units[static_cast<size_t>(1)].at=fh;
            y.Capture(y.units[1]);
            check(y.Factories()[0].owner==Her,L"her infantry takes a factory of yours");
            y.units[1].at=y.Hex(10,6);
            y.phase=Phase::YourTurn;y.StartTurn(Her);
            const Unit* made=y.UnitAt(fh);
            check(made&&made->who==Her&&made->kind==Kind::Tank&&made->done,L"taken while building, what it builds is the taker's");
            (void)hers;
        }
        out<<wrong<<L" wrong\n"<<lines.str();
        return out.str();
    }

    std::wstring CheckPlaying(int count,std::uint32_t seed)
    {
        std::wostringstream out,lines;
        int wrong=0;
        double longest=0;
        // Her own play on every map offered, then against one who only waits on the first.
        std::vector<std::pair<int,int>> runs;
        for(int m=0;m<Maps();++m)if(Map(m).offered)runs.push_back({m,0});
        runs.push_back({0,1});
        for(const auto& [map,against]:runs)
        {
            int hers=0,yours=0,draws=0,turnsSum=0;
            std::wstring endings;
            for(int g=0;g<count;++g)
            {
                Table t;
                t.NewGame(map,seed+g*7919u+against*104729u,1,g%2==1);
                for(int step=0;step<5000&&t.Now()!=Phase::GameOver;++step)
                {
                    if(t.Now()==Phase::HerTurn)
                    {
                        const auto from=std::chrono::steady_clock::now();
                        t.Step();
                        longest=std::max(longest,std::chrono::duration<double>(std::chrono::steady_clock::now()-from).count());
                    }
                    else if(against==0)t.PlayTurnForYou();
                    else t.EndTurn();
                    if(!t.Consistent()){++wrong;break;}
                }
                if(t.Now()!=Phase::GameOver){++wrong;continue;}
                turnsSum+=t.Turn();
                const int w=t.Winner();
                (w==Her?hers:w==You?yours:draws)+=1;
                static const wchar_t* const how[]={L"-",L"destroyed",L"captured",L"resigned",L"turns"};
                endings+=std::wstring(endings.empty()?L"":L", ")+how[static_cast<int>(t.HowEnded())]+L" "+std::to_wstring(t.Turn())+L" "+std::to_wstring(t.Strength(You))+L"/"+std::to_wstring(t.Strength(Her));
            }
            lines<<L"map "<<map+1<<(against==0?L", against her own play: ":L", against one who only waits: ")<<count<<L" games, her "<<hers<<L", yours "<<yours<<L", draws "<<draws
                 <<L"; turns on average "<<(count?static_cast<double>(turnsSum)/count:0)<<L" ("<<endings<<L")\n";
        }
        out<<wrong<<L" wrong\n"<<lines.str()<<L"her longest order "<<static_cast<int>(longest*1000)<<L" ms\n";
        out<<L"experience from a battle with an equal, coming through it: "<<(equalBattles?equalGained/equalBattles:0)<<L" on average ("<<equalBattles<<L" battles)\n";
        return out.str();
    }
}
