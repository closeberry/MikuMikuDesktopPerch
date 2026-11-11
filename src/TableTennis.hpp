#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch::TableTennis
{
    // The table tennis game itself (docs/TableTennis.md), without the window: the table, the
    // ball, the two rackets and the score, and how the other side plays. In metres: x across
    // (to your right), y along the table (your end -length/2, hers length/2, the net at 0),
    // z above the table's top.
    struct Vec
    {
        float x=0,y=0;
    };
    struct Vec3
    {
        float x=0,y=0,z=0;
    };

    enum Side {You=0,Her=1};
    // A drive (topspin: dips and kicks on), a cut (backspin: slow, floats and holds up), a
    // smash (fast and flat: over the net only from a high ball).
    enum class Stroke {Drive,Cut,Smash};

    struct Rules
    {
        float width=1.525f,length=2.74f,net=0.1525f;
        float height=0.76f;            // the top above the floor
        float ball=0.03f;              // its radius: half as big again as a real one, to be seen
        std::array<float,2> reach{0.27f,0.27f};   // how far from a racket's middle a ball is still hit (across the table)
        // How far a swinging racket is taken sideways to the ball by itself (yours: a racket's
        // width, the mouse being hard to place so exactly).
        std::array<float,2> assist{0.17f,0.0f};
        float gravity=9.8f;
        float bounce=0.86f;            // the speed kept off the table
        int toWin=11;                  // and two clear
    };

    struct Ball
    {
        Vec3 p{},v{};
        float spin=0;                  // 1 topspin .. -1 backspin
        float curve=0;                 // sidespin: it bends across the table this much (metres a second, each second)
    };

    enum class Phase {Serve,Rally,Point,Over};
    // Why a point was given: the striker's ball into the net, or not onto the other half; the
    // receiver did not get it back.
    enum class Reason {None,Net,Out,Missed};

    // What happened in a step, for the sounds and the words.
    struct Event
    {
        enum Kind {Hit,Bounce,Net,Floor,Point} kind=Hit;
        int side=0;                    // Hit: whose racket; Bounce: whose half; Point: who got it
        Stroke stroke=Stroke::Drive;   // Hit; Point: the last stroke played
        float strength=0;              // the ball's speed then (metres a second)
        Reason reason=Reason::None;    // Point
        int rally=0;                   // Point: strokes in the rally
    };

    // The ball's way on from now, for whoever is to play it: where it comes down on their half
    // (lands: it will), then where it is every 1/120 s after that until it comes down again.
    struct Path
    {
        bool lands=false;
        Vec bounce{};
        float bounceIn=0;              // seconds from now
        std::vector<Ball> after;
    };

    class Game
    {
    public:
        explicit Game(const Rules& rules={},std::uint32_t seed=1);
        void NewGame(int firstServer=You);
        // The fastest a racket moves (metres a second): yours as quick as the mouse, hers as an arm.
        void SetRacketSpeed(int side,float speed){racketSpeed[side]=speed;}
        // Whether side's racket is where the game has it (hers, in her hand: when her hand has
        // not got there, her swing misses the ball).
        void SetAble(int side,bool able){this->able[side]=able;}
        int Missed(int side) const {return missed[side];}   // swings that would have struck but for that
        // dt seconds on: each racket toward its target (kept behind its own half of the net),
        // the ball flies and bounces, a swing under way meets it, the point is counted.
        void Step(double dt,Vec yourTarget,Vec herTarget);
        // A swing: it meets the ball if that comes within reach during it (after its bounce on
        // the swinger's half), and serves when the swinger holds the ball. Where the ball goes
        // comes from where it meets the racket and how the racket is moving (the mouse).
        // False: too soon after the last swing.
        bool Swing(int side,Stroke stroke);
        // The same, sent toward target on the other half, error metres astray at most.
        bool SwingAimed(int side,Stroke stroke,Vec target,float error);

        const Rules& Rule() const {return rules;}
        Phase Now() const {return phase;}
        const Ball& TheBall() const {return ball;}
        Vec Racket(int side) const {return racket[side];}
        Vec RacketVelocity(int side) const {return racketVelocity[side];}
        float SwingAge(int side) const {return swings[side].age;}   // seconds since side last swung (large: long ago)
        Stroke SwingStroke(int side) const {return swings[side].stroke;}
        // How far side's racket was taken across to the ball at its last swing's stroke (metres).
        float SwingShift(int side) const {return swings[side].shift;}
        int Score(int side) const {return score[side];}
        int Winner() const {return winner;}
        int Server() const {return server;}
        double ServeLeft() const {return serveLeft;}    // seconds before the server may serve
        int LastHitter() const {return lastHitter;}
        // 0: a serve not yet down on the server's half; 1: on its way to the other half; 2: down
        // there, theirs to play.
        int Stage() const {return stage;}
        int Hits() const {return hits;}                 // strokes played in the game
        int Rally() const {return rally;}               // strokes in this rally
        int LongestRally() const {return longestRally;}
        int LastWinner() const {return lastWinner;}
        Reason LastReason() const {return lastReason;}
        Stroke LastStroke() const {return lastStroke;}
        bool Deuce() const {return score[0]>=rules.toWin-1&&score[1]>=rules.toWin-1;}
        double Clock() const {return clock;}
        // Where the ball in flight will come down on the receiver's half (false: it will not).
        bool Mark(Vec& at) const {at=mark;return marked;}
        Path Predict() const;
        std::vector<Event> TakeEvents(){auto e=std::move(events);events.clear();return e;}
        int Wrong() const {return wrong;}               // diagnostics: what must not happen (a ball lost, a rally without end)

    private:
        struct SwingState
        {
            float age=100;             // seconds since it began
            Stroke stroke=Stroke::Drive;
            bool live=false;           // it may still meet the ball
            bool aimed=false;
            Vec target{};
            float error=0;
            float nearest=1e9f;
            bool within=false;
            float shift=0;             // the racket taken this far across to meet the ball
            bool counted=false;        // a miss for want of the racket, counted once
        };
        Rules rules;
        Phase phase=Phase::Serve;
        Ball ball{};
        std::array<Vec,2> racket{},racketVelocity{},racketMove{};
        std::array<float,2> racketSpeed{40.0f,3.6f};
        std::array<SwingState,2> swings{};
        std::array<bool,2> able{true,true};
        std::array<int,2> missed{};
        std::array<int,2> score{};
        int winner=-1,server=You,firstServer=You,lastHitter=-1,stage=0;
        int hits=0,rally=0,longestRally=0,lastWinner=-1,wrong=0;
        Reason lastReason=Reason::None;
        Stroke lastStroke=Stroke::Drive;
        double serveLeft=0,pointLeft=0,clock=0,sinceHit=0;
        Vec mark{};
        bool marked=false;
        std::uint32_t random;
        std::vector<Event> events;

        float Random(float lo,float hi);
        Vec Keep(int side,Vec at) const;
        void MoveRacket(int side,Vec target,float dt);
        void StartServe();
        void HoldBall();
        void MoveBall(float h);
        void Meet(int side,float h);
        void Strike(int side,float quality);
        void Serve(int side);
        void Launch(int side,Stroke stroke,Vec target,float speed,float curve);
        void Award(int side,Reason reason);
        void SetMark();
    };

    // How the other side plays (docs/TableTennis.md, キャラクターの打ち方): it sees your stroke a
    // moment late, works out where the ball will come, takes its racket there and swings as
    // the ball arrives.
    struct Style
    {
        float reaction=0.24f;   // seconds after a stroke before it knows where the ball goes
        float speed=3.6f;       // its racket's fastest (metres a second)
        float error=0.20f;      // how far its aim strays (metres)
        float slip=0.05f;       // the share of balls it swings late at
        float smash=0.5f;       // the share of high balls it smashes
        float placing=0.5f;     // the share of strokes sent away from where you are
        bool plain=false;       // swings as the mouse does (Game::Swing), for the diagnostics
        float across=9;         // diagnostics: every stroke sent here across the table (9: as it chooses)
    };
    // The window's three strengths (1 easy .. 3 strong).
    Style LevelStyle(int level);

    class Brain
    {
    public:
        Brain(int side,const Style& style,std::uint32_t seed);
        // Where its racket should go now; it swings and serves by itself.
        Vec Think(Game& game,double dt);
        float Height() const {return height;}     // how high it means to meet the ball (metres over the table)
        // The ball it is going to: whether there is one, where and when (the game's clock), and how.
        bool Comes() const {return planned&&comes;}
        Vec Spot() const {return spot;}
        double HitAt() const {return hitAt;}
        Stroke Planned() const {return stroke;}
        double ServeAt() const {return serveAt;}   // when it means to serve (the game's clock; below 0: not serving)
        const Style& Manner() const {return style;}

    private:
        int side;
        Style style;
        std::uint32_t random;
        int seenHits=-1;
        double seeAt=0,hitAt=0,serveAt=-1;
        bool planned=false,comes=false,swung=false,wild=false;
        Vec spot{},aim{},home{};
        float height=0.2f;
        Stroke stroke=Stroke::Drive;
        float Random(float lo,float hi);
    };

    // Diagnostics (--tabletennis-sim): games between the opponent at each strength and a player
    // swinging as the mouse does, checked for what must not happen, as a report whose first
    // line is "<n> wrong".
    std::wstring CheckPlaying(int games,std::uint32_t seed);
}
