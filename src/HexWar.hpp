#pragma once
#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch::HexWar
{
    // The rules of the hex war game (docs/HexWar.md): your army against hers on a map of
    // hexagons, a turn at a time, each side moving all of its units in its turn.
    //
    // The map is drawn with flat-topped hexagons in columns, every odd column half a hex lower
    // than the even ones; a hex is numbered Columns * row + column.
    constexpr int You=0,Her=1;

    // City: ruined buildings. Factory: one of the factories (see Factory). Foundation: what is
    // left standing of a large building, closed to vehicles.
    enum class Terrain : std::uint8_t {Plain,Rough,Forest,Mountain,River,Road,Base,Bridge,City,Factory,Foundation};
    constexpr int Terrains=11;
    // Kinds of unit (docs/HexWar.md): the tank, infantry, anti-tank infantry (a launcher on the
    // shoulder: strong against vehicles, weak against men), the scout car (fast, light), the
    // self-propelled gun (fires from two or three hexes off, not after moving, and is never
    // fired back at), the heavy tank (slow, strong), and in the air the fighter (made to bring
    // down aircraft) and the attack aircraft (made to strike the ground), with the anti-aircraft
    // missile truck against them on the ground.
    enum class Kind : std::uint8_t {Tank,Infantry,Bazooka,Scout,Artillery,Heavy,Fighter,Attacker,AntiAir};
    constexpr int Kinds=9;

    // How a kind moves: its movement points, and what each terrain costs it (0: it cannot go).
    struct KindInfo
    {
        int move;
        int attack;          // against men (units not armoured)
        int attackArmour;    // against armoured units
        int defence;
        int value;           // how much one of its members is worth (her judgement)
        bool captures;       // takes the other's base by moving onto it
        bool armoured;       // a vehicle
        int rangeMin,rangeMax;   // the hexes off it can attack at (1, 1: next to it)
        std::array<int,Terrains> cost;   // by Terrain
        int attackAir;       // against aircraft (0: it cannot shoot at them)
        bool flies;          // an aircraft: over any terrain, no cover, shot at only by those
                             // with an attack against aircraft, supported only by such (held
                             // back and surrounded by any unit, as one on the ground)
    };
    const KindInfo& Info(Kind k);
    // a's attack against d (0: it cannot attack it).
    int AttackOn(Kind a,Kind d);
    // Whether a kind fires from afar (and so never back at those next to it).
    inline bool Ranged(Kind k){return Info(k).rangeMin>1;}
    // How much a terrain adds to the defence of a unit on it (0.4: 40%); an aircraft has none.
    double Cover(Terrain t);
    inline double Cover(Terrain t,Kind k){return Info(k).flies?0:Cover(t);}

    constexpr int FullStrength=10;

    struct Unit
    {
        int id=0;
        Kind kind=Kind::Tank;
        int who=You;
        int at=0;
        int count=FullStrength;   // members left (0: destroyed)
        int exp=0;                // members of the other side it has destroyed
        bool moved=false;         // moved this turn (it may still attack)
        bool done=false;          // finished this turn
    };
    // A factory: whose it is (-1: no one's yet; infantry moving onto it takes it), and the unit
    // being built there (kind, turns left; -1: none). A unit of its side standing on it gets
    // members back at the start of each of its side's turns; one being built comes out on it
    // when done and the hex is free. Taken while building, what it builds is the taker's.
    struct Factory
    {
        int hex=-1;
        int owner=-1;
        int building=-1;   // a Kind, or -1
        int left=0;        // turns until it is done
    };
    // Turns a kind takes to build (the stronger, the longer), and members a factory gives back
    // to one of its kind a turn (the stronger, the fewer).
    int BuildTurns(Kind k);
    int RepairPerTurn(Kind k);
    constexpr int MostUnits=14;   // a side builds no more than this many (with those being built)

    // The level its experience gives it (0-3): its attack grows by a third a level (twice at the
    // top); its defence stays.
    int Level(int exp);
    // How a unit ranks (its kind's worth, raised by its level): experience from a battle goes
    // by the other's rank over its own.
    double Rank(Kind k,int exp);

    // A battle, as it went: the members of both shoot at the same time, one shot each (those
    // hit still shoot). Strengths are after support, terrain, level and encirclement.
    struct Battle
    {
        int attacker=-1,defender=-1;           // unit ids
        Kind kinds[2]{};                       // [0] attacker, [1] defender
        int who[2]{};
        int at[2]{};                           // their hexes
        int before[2]{},after[2]{};            // members before and after
        int exp[2]{};                          // experience before
        double attack[2]{},defence[2]{};       // their strengths in this battle
        double cover[2]{};                     // the terrain's share of each one's defence
        bool surrounded[2]{};
        double chance[2]{};                    // the chance of each shot hitting
        int own[2]{};                          // shots of each one's own members
        int support=0;                         // the attacker's supporting fire, in shots
        bool answers=true;                     // the defender fires back (not at a gun's shells)
        std::vector<bool> shots[2];            // each shot: hit or not (the attacker's own, then its support's)
        double expected[2]{};                  // members each would expect to destroy
        int gained[2]{};                       // experience each gains (none when destroyed)
    };

    // One map: its terrain as text (P plain, R rough, F forest, M mountain, W river, = road,
    // # bridge, B your base, b hers), the units on it at the start (hers placed by turning the
    // map round its middle when mirrored), how many turns a game lasts, and its picture (a
    // resource of type HEXWARART drawn under the hexes; null: the hexes are painted).
    struct MapInfo
    {
        const wchar_t* nameJa;
        const wchar_t* nameEn;
        int columns,rows;
        std::vector<const char*> terrain;      // rows, or the first half and the middle row when mirrored
        bool mirrored;
        struct Start {Kind kind;int column,row;int who;};
        std::vector<Start> units;              // yours (and hers, when not mirrored)
        int turns;
        const wchar_t* picture=nullptr;
        bool offered=true;                     // among those to choose from
        bool bridging=false;                   // infantry may bridge its rivers
    };
    int Maps();
    const MapInfo& Map(int index);

    // A move of a unit: where it ends, and the hexes it goes through (from its start).
    struct Route {int to=-1;int cost=0;std::vector<int> path;};

    // What one of her units does: moves to a hex (its own: stays), then perhaps attacks.
    struct Order {int unit=-1;int to=-1;int target=-1;double score=0;int bridge=-1;};

    // The commanders' orders (one a turn, paid for from the command gauge, which fills as
    // members are destroyed on either side): Barrage, every shot of the side surer this turn;
    // Strike, a shelling of one unit of the other's; Smoke, shots at the side less sure until
    // its next turn; Repair, members back to every unit; March, a hex more for every unit.
    enum class Skill {Barrage,Strike,Smoke,Repair,March};
    constexpr int Skills=5;
    int SkillCost(Skill s);
    constexpr int GaugeFull=100,StartGauge=20;

    enum class Phase {YourTurn,HerTurn,GameOver};
    enum class Ending {None,Destroyed,Captured,Resigned,TurnLimit};

    // What was done last, for the window to show: a unit moved along a path, then perhaps a
    // battle. Counted so that each is shown once.
    struct Action
    {
        int unit=-1,who=You;
        std::vector<int> path;    // from where it was (empty: it did not move)
        bool fought=false;
        Battle battle;
        // An order given (skill >= 0) instead: whose, and for a strike, where and what it did.
        int skill=-1;
        int strikeHex=-1;
        Kind strikeKind=Kind::Tank;
        int strikeBefore=0,strikeAfter=0;
        int captured=-1;          // a factory taken by the move (its index), or -1
        int bridged=-1;           // a river hex bridged by the unit, or -1
    };

    class Table
    {
    public:
        // care: how carefully she plays (1: her best; less: now and then a looser order).
        // swapped: you take the map's other side (its second base and army).
        void NewGame(int map,std::uint32_t seed,double care,bool swapped=false);
        bool Swapped() const {return swapped;}
        // Which way a side's units face: +1 right, -1 left (toward the other base).
        int Facing(int who) const;

        Phase Now() const {return phase;}
        int MapIndex() const {return map;}
        int Columns() const {return columns;}
        int Rows() const {return rows;}
        int Hexes() const {return columns*rows;}
        Terrain At(int hex) const {return terrain[static_cast<size_t>(hex)];}
        int BaseOf(int who) const {return base[static_cast<size_t>(who)];}
        int Turn() const {return turn;}            // 1 at the start
        int TurnLimit() const {return turns;}
        const std::vector<Unit>& Units() const {return units;}
        const Unit* UnitAt(int hex) const;
        const Unit* UnitById(int id) const;
        int Alive(int who) const;
        int Strength(int who) const;              // members times their worth

        // Hex geometry.
        int Hex(int column,int row) const {return row*columns+column;}
        bool Inside(int column,int row) const {return column>=0&&column<columns&&row>=0&&row<rows;}
        // The hex next to hex in direction d (0-5, going round; d and d+3 are opposite), or -1.
        int Next(int hex,int d) const;
        int Distance(int a,int b) const;
        bool Adjacent(int a,int b) const {return Distance(a,b)==1;}
        // Next to a unit of the other side (it stops there; aircraft too, whatever the unit).
        bool HeldBy(int hex,int who) const;

        // Where a unit may move now (none when it has moved or is done).
        std::vector<Route> Reach(int id) const;
        // Where it could move in its side's turn, from where it is.
        std::vector<Route> Range(int id) const;
        // The units of the other side it may attack from where it is (none when done).
        std::vector<int> Targets(int id) const;
        // Whom u could attack standing at a hex (moved: having moved there this turn).
        std::vector<int> TargetsFrom(const Unit& u,int at,bool moved) const;
        // The battle that attacking would give, with expected losses but nothing shot.
        Battle Preview(int attacker,int defender) const;
        // Whether a unit is surrounded by the other side: two of them on opposite sides of it,
        // or three with one hex between each.
        bool Surrounded(int id) const;

        // Your turn: a unit of yours moves (to its own hex: it stays and may attack), attacks,
        // or is finished; a move not yet followed by an attack may be taken back.
        bool Move(int id,int to);
        bool Attack(int attacker,int defender);
        void Finish(int id);
        bool TakeBack(int id);
        void EndTurn();
        void Resign(int who);

        // The commanders' orders: the gauge, whether one may be given now (strike: at a unit of
        // the other side), giving one (in your turn; hers are given in Step), and those in force.
        int Gauge(int who) const {return gauge[static_cast<size_t>(who)];}
        bool CanCommand(int who,Skill s) const;
        bool Command(int who,Skill s,int target=-1);
        bool Commanded(int who) const {return used[static_cast<size_t>(who)];}
        bool InForce(int who,Skill s) const;
        // What a strike at a unit would be: the shot's chance and how many shots.
        double StrikeChance(int target) const;

        // The factories, the one on a hex (its index, or -1), the kinds this map's armies have
        // (all a factory may build), and building one (in your turn, at a factory of yours not
        // building; hers are set in Step).
        const std::vector<Factory>& Factories() const {return factories;}
        int FactoryAt(int hex) const;
        std::vector<Kind> Roster() const;
        bool CanBuild(int who,int factory,Kind k) const;
        bool Build(int who,int factory,Kind k);

        // Bridging (on maps that allow it): the river hexes next to a unit of infantry not done
        // that it may bridge, and bridging one (in your turn; the unit is done). The bridges
        // built so far.
        std::vector<int> BridgeSpots(int id) const;
        bool Bridge(int id,int hex);
        const std::vector<int>& Bridges() const {return bridges;}

        // Her turn: what she will do next (none: her turn is over), and doing it.
        Order HerNext();
        void Step();
        // Her whole turn played for you as she would play it (diagnostics).
        void PlayTurnForYou();

        int Winner() const {return winner;}
        Ending HowEnded() const {return ending;}
        int Actions() const {return actions;}
        const Action& LastAction() const {return last;}
        // Her view of the game: her strength over both (0-1).
        double HerShare() const;

        bool Consistent() const;
        // Diagnostics: the gauge filled by ten.
        void FillForTest(int who){Fill(who,10);}

    private:
        std::mt19937 rng;
        double care=1;
        int map=0,columns=0,rows=0,turns=0;
        std::vector<Terrain> terrain;
        std::array<int,2> base{-1,-1};
        std::vector<Unit> units;
        Phase phase=Phase::YourTurn;
        int turn=1;
        int winner=-1;
        Ending ending=Ending::None;
        int actions=0;
        Action last;
        struct Undo {int id=-1,from=-1,factory=-1,owner=-1;} undo;
        bool planned=false;
        bool swapped=false;
        Order plan;
        // While she chooses: the hexes each unit of the other side could attack next turn.
        mutable std::vector<int> threatIds;
        mutable std::vector<std::vector<char>> threats;
        mutable bool baseAtRisk=false;
        std::vector<std::vector<double>> toBase,toFoe;   // while she chooses: ground distances by kind
        std::vector<double> Field(Kind kind,const std::vector<int>& sources) const;
        std::vector<Factory> factories;
        std::vector<int> bridges;
        std::vector<int> BridgeSpotsFrom(const Unit& u,int at) const;
        // What a bridge at hex would be worth to who: how much nearer its vehicles come to the
        // other's base, less how much nearer the other's come to its own.
        double BridgeWorth(int who,int hex);
        // A capturing unit arriving on a factory not its side's takes it.
        void Capture(Unit& u);
        // What who's factories build, chosen as she would (each one idle and allowed).
        void AutoBuild(int who);
        std::array<int,2> gauge{StartGauge,StartGauge};
        std::array<bool,2> used{},barrage{},smoke{},march{};
        bool skillThought=false;              // her order for this turn decided
        double ChanceNow(double chance,int shooter,int target) const;
        // An order for who's side chosen as she would and given (false: none now).
        bool AutoCommand(int who);
        int guardId=-1;                        // the unit sent back to hold the base (-1: none)

        Unit* Find(int id);
        Unit* FindAt(int hex);
        int Cost(const Unit& u,int hex) const;
        bool Occupied(int hex) const {return UnitAt(hex)!=nullptr;}
        std::vector<Route> ReachFrom(const Unit& u,int from) const;
        bool SurroundedAt(const Unit& u,int at,int movedId=-1,int movedTo=-1) const;
        Battle Fight(const Unit& a,int aAt,const Unit& d,int movedId,int movedTo) const;
        void Shoot(Battle& b);
        void Apply(Battle& b);
        void Fill(int who,int by);
        void CheckEnd();
        void StartTurn(int who);
        double Score(const Unit& u,int to,int target) const;
        double Danger(const Unit& u,int at) const;
        Order Choose();
        friend std::wstring CheckRules();
    };

    // Diagnostics (--hexwar-test): the rules checked on set positions; the first line counts
    // the failures.
    std::wstring CheckRules();
    // Diagnostics (--hexwar-play-test): whole games of her play against itself and against a
    // side that only waits.
    std::wstring CheckPlaying(int count,std::uint32_t seed);
}
