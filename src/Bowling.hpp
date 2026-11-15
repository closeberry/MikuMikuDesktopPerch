#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch::Bowling
{
    // Ten-pin bowling (docs/Bowling.md), without the window: the lane, the ball and the ten pins,
    // the score of a game of ten frames for two, and how she throws. In metres: x across the lane
    // (to the bowler's right), y along it (the foul line at 0, the head pin at PinDeckY), z up
    // (the lane's surface at 0).
    struct Vec3 {float x=0,y=0,z=0;};
    struct Quat {float x=0,y=0,z=0,w=1;};

    constexpr float LaneHalf=0.527f;        // the lane's half width (41.5 in)
    constexpr float GutterWidth=0.235f,GutterDepth=0.06f;
    constexpr float PinDeckY=18.288f;       // the foul line to the head pin
    constexpr float PinSpacing=0.3048f;     // between pins' middles (12 in)
    constexpr float DeckEnd=PinDeckY+0.86f*1.25f;   // where the pin deck drops into the pit
    constexpr float BallRadius=0.1085f,BallMass=6.8f;
    constexpr float PinHeight=0.381f,PinMass=1.53f,PinCentre=0.145f;   // its middle of mass over its base
    constexpr float OilLength=12.2f;        // the oiled length of the lane: little grip, then more
    constexpr float ApproachLength=4.6f;    // behind the foul line

    enum Side {You=0,Her=1};

    // Where pin n (0 the head pin .. 9 the 10 pin) stands: rows of 1, 2, 3, 4, the 7 pin on the left.
    Vec3 PinSpot(int n);

    // One throw: let go at x across (on the foul line), at angle (radians, + to the right), at
    // speed (metres a second), with spin -1..1 (+: a right-hander's hook, curling left late on).
    struct Throw
    {
        float x=0,angle=0,speed=7.5f,spin=0;
    };

    // The ball as it goes down the lane (rolls, skids, hooks) on its own, with no pins: what she
    // aims by. x across where it comes to y (false: into the gutter first).
    bool Predict(const Throw& t,float y,float& x,float* entry=nullptr);
    // Its path down the lane (a point every step metres along it), up to the pins or the gutter.
    std::vector<Vec3> Path(const Throw& t,float step);

    // The physics of one throw: the ball on the lane (its own skid and hook, as Predict), then
    // the pins and the ball together on the deck (rigid bodies).
    struct Pin
    {
        Vec3 p{};          // its middle of mass
        Quat q{};
        bool there=true;   // on the deck at the start of this throw (not swept off)
    };
    struct Event
    {
        enum Kind {BallPin,PinPin,PinWall,Gutter,Pit} kind=BallPin;
        float strength=0;  // how hard (0-1)
        Vec3 at{};
    };
    class Roll
    {
    public:
        Roll();
        ~Roll();
        // The pins as they stand (only those there take part).
        void Start(const Throw& t,const std::array<Pin,10>& pins);
        void Step(double dt);
        bool Done() const;
        double Clock() const {return clock;}
        Vec3 Ball() const {return ball;}
        Vec3 BallVelocity() const {return velocity;}
        Quat BallTurn() const {return turn;}
        bool InGutter() const {return gutter;}
        bool OnDeck() const {return deck;}
        const std::array<Pin,10>& Pins() const {return pins;}
        // Standing now (upright on the deck where it can be hit again).
        bool Standing(int n) const;
        std::vector<Event> TakeEvents(){return std::move(events);}
        // After the throw: the standing pins put back where they stand (the others taken off).
        std::array<Pin,10> Left() const;
    private:
        struct World;
        std::unique_ptr<World> world;
        std::array<Pin,10> pins{};
        Vec3 ball{},velocity{},spin{};
        Quat turn{};
        bool gutter=false,deck=false,stopped=false;
        double clock=0,deckAt=0,quietFor=0;
        std::vector<Event> events;
        void LaneStep(float h);
        void ToDeck();
    };

    // The score: ten frames for each side, you first in each frame.
    class Game
    {
    public:
        void New();
        // Whose ball, which frame (0-9), which ball in it (0-2).
        int Turn() const {return turn;}
        int Frame() const {return frame;}
        int BallNo() const {return ball;}
        bool Over() const {return frame>=10;}
        // The pins knocked down by this ball (of those standing before it). Returns what
        // happened: 0 nothing special, 1 a strike, 2 a spare.
        int Record(int pins);
        // Whether the next ball is thrown at a full rack.
        bool FreshRack() const;
        // A side's throws in a frame (pins each), and its running total to that frame (-1: not yet known).
        const std::vector<int>& Throws(int side,int f) const {return rolls[side][f];}
        int Total(int side,int f) const;
        int Score(int side) const;   // so far, counting what is known
        int Winner() const;          // -1 even or not over
        int Strikes(int side) const {return strikes[side];}
        int Spares(int side) const {return spares[side];}
    private:
        std::array<std::array<std::vector<int>,10>,2> rolls{};
        int turn=You,frame=0,ball=0;
        std::array<int,2> strikes{},spares{};
        bool FrameDone(int side,int f) const;
    };

    // How she bowls: where she stands, her line and speed, and how far she misses it, by her
    // strength (1-3: TableTennis's keys likewise).
    struct Style
    {
        float aim=0.010f;     // radians her angle is off by (one sigma)
        float speed=0.25f;    // metres a second off
        float stand=0.03f;    // metres off where she lets go
        float spin=0.55f;     // her hook
    };
    Style LevelStyle(int level);
    // Her throw at the pins standing (true: there).
    Throw Choose(const std::array<bool,10>& standing,const Style& style,std::mt19937& random);

    // Tests (--bowling-test): the scoring, throws at the pocket and at spares, her games.
    std::wstring Check(int games,std::uint32_t seed);
}
