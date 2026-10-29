#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace MikuMikuDesktopPerch::AirHockey
{
    // The air hockey game itself (docs/AirHockey.md), without the window: the table, the puck,
    // the two mallets and the score, and how the other side plays. In table units: x across
    // (-width/2 .. width/2, to your right), y along it (-length/2 your end .. length/2 hers).
    struct Vec
    {
        float x=0,y=0;
    };

    enum Side {You=0,Her=1};

    struct Rules
    {
        float width=1.0f,length=1.8f;
        // The goal's mouth (across the middle of each end), the radii: as Game::SetPieceSize(1.5)
        // makes them, larger than a real table's, to be easy to see and hit.
        float goal=0.45f;
        float puck=0.0525f;
        std::array<float,2> mallet{0.09f,0.09f};
        std::array<float,2> malletSpeed{7.0f,3.4f};   // the fastest each mallet moves (units per second)
        float herAcceleration=40.0f;    // how quickly her mallet gets up to speed
        float puckSpeed=2.6f;           // the fastest the puck goes (PuckSpeeds: the window's choices)
        float wallBounce=0.9f,malletBounce=0.85f;
        float drag=0.12f;               // the puck slows by this share a second (the air keeps it going)
        int toWin=7;
    };

    // The puck's top speeds to choose from (the settings' 1 to 4): from a slow one to see
    // and reach easily, to one that crosses the table in under half a second.
    constexpr std::array<float,4> PuckSpeeds{2.0f,2.6f,3.2f,4.0f};
    // The puck's and mallets' sizes to choose from (the settings' 1 to 4), against a real
    // table's (1); the goal widens with them.
    constexpr std::array<float,4> PieceSizes{1.0f,1.25f,1.5f,1.75f};

    enum class Phase {Serve,Play,Over};

    // What happened in a step, for the sounds: the puck against a wall or a mallet (strength:
    // its speed then, against the fastest), a goal.
    struct Event
    {
        enum Kind {Wall,Hit,Goal} kind=Wall;
        float strength=0;
        int side=0;        // Hit: whose mallet; Goal: who scored
    };

    class Game
    {
    public:
        explicit Game(const Rules& rules={});
        void NewGame(int serving=You);
        // A new top speed for the puck, from now on (a puck going faster slows to it).
        void SetPuckSpeed(float speed);
        // The puck and mallets scale times a real table's (PieceSizes), the goal with them. A
        // puck in play is served again on its half.
        void SetPieceSize(float scale);
        // The mallet on side held by a hand (hers, drawn in 3D): over the next Step it goes
        // straight to at (kept on its half), at the speed that takes, instead of toward its target.
        void HoldMallet(int side,Vec at);
        // dt seconds on: each mallet moves toward its target (kept on its own half), the puck
        // moves and bounces, a goal is counted. The puck stopped on her side for long is put in
        // front of you (on yours it is yours to play, however long it rests).
        void Step(double dt,Vec yourTarget,Vec herTarget);

        const Rules& Rule() const {return rules;}
        Phase Now() const {return phase;}
        Vec Puck() const {return puck;}
        Vec PuckVelocity() const {return puckVelocity;}
        Vec Mallet(int side) const {return mallet[side];}
        Vec MalletVelocity(int side) const {return malletVelocity[side];}
        int Score(int side) const {return score[side];}
        int Winner() const {return winner;}
        double ServeLeft() const {return serveLeft;}   // seconds before the puck may be played
        int ServingSide() const {return serving;}
        int LastScorer() const {return lastScorer;}
        bool GoalServe() const {return goalServe;}      // this serve follows a goal (not a puck put back)
        int Rally() const {return rally;}               // hits since the last serve
        int LongestRally() const {return longestRally;}
        float FastestPuck() const {return fastest;}
        double Clock() const {return clock;}
        std::vector<Event> TakeEvents(){auto e=std::move(events);events.clear();return e;}
        // Diagnostics: counted against what must not happen (the puck off the table or inside
        // a mallet after a step).
        int OffTable() const {return offTable;}
        int Overlaps() const {return overlaps;}
        int Restarts() const {return restarts;}         // a stopped puck put back

    private:
        Rules rules;
        Phase phase=Phase::Serve;
        Vec puck{},puckVelocity{};
        std::array<Vec,2> mallet{},malletVelocity{};
        std::array<bool,2> held{};            // HoldMallet: moved by a hand this Step
        std::array<Vec,2> heldFrom{},heldAt{};
        std::array<int,2> score{};
        int winner=-1,serving=You,lastScorer=-1;
        double serveLeft=0,clock=0,still=0;
        bool goalServe=false;
        int rally=0,longestRally=0,lastHitter=-1;
        float fastest=0;
        int offTable=0,overlaps=0,restarts=0;
        std::vector<Event> events;
        void Serve(int side,bool afterGoal=false);
        void MoveMallet(int side,Vec target,float dt);
        void MovePuck(float dt);
        void Collide(int side);
        void Bounce(Vec& p,Vec& v,float radius,bool goals,float bounce);
        Vec Home(int side) const;
    };

    // How the other side plays (docs/AirHockey.md, キャラクターの打ち方), from what it sees of the
    // puck a moment late: in front of its goal when the puck comes, a strike at it when it is
    // in reach and slow enough, back to its goal while the puck is away.
    struct Style
    {
        float reaction=0.18f;   // seconds late it sees the puck
        float aimError=0.08f;   // how far its aim strays (table units, at the far goal)
        float boldness=0.5f;    // 0 waits in front of its goal and returns; 1 comes out to strike
        float reach=1.0f;       // the share of its half it can reach toward the middle
    };

    class Brain
    {
    public:
        Brain(int side,const Style& style,std::uint32_t seed);
        // Where its mallet should go now.
        Vec Think(const Game& game,double dt);
        const Style& Manner() const {return style;}

    private:
        struct Seen {double at=0;Vec p{},v{};};
        int side;
        Style style;
        std::uint32_t random;
        std::vector<Seen> seen;
        Vec aim{};             // where it means to send the puck (the far end)
        double aimFor=0;       // seconds the aim holds before another is chosen
        float strike=0;        // seconds into a strike (0: none)
        Vec strikeFrom{};
        float Random01();
        Seen Late(const Game& game) const;
        // Where the puck seen at p moving at v crosses y (bouncing off the side walls), and in
        // how many seconds (negative: it does not).
        float CrossAt(const Game& game,Vec p,Vec v,float y,float& seconds) const;
    };
}
