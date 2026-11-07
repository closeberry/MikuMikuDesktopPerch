#pragma once
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch::Golf
{
    // Metres. x across the hole (+ to the right looking from the tee), y up, z along it (+
    // toward the green). The tee is at the origin.
    struct V3
    {
        float x=0,y=0,z=0;
        V3 operator+(const V3& o) const {return {x+o.x,y+o.y,z+o.z};}
        V3 operator-(const V3& o) const {return {x-o.x,y-o.y,z-o.z};}
        V3 operator*(float k) const {return {x*k,y*k,z*k};}
    };
    float Dot(const V3& a,const V3& b);
    V3 Cross(const V3& a,const V3& b);
    float Length(const V3& a);
    V3 Normalized(const V3& a);
    float Flat(const V3& a,const V3& b);   // the distance across the ground (x, z only)

    enum class Lie {Tee,Fairway,Rough,Bunker,Green,Water,Out};
    const wchar_t* LieName(Lie lie);

    constexpr float BallRadius=0.02135f;
    constexpr float CupRadius=0.054f;

    struct Club
    {
        const wchar_t *ja,*en;
        float loft;    // degrees: how high the ball leaves
        float speed;   // m/s the ball leaves at full power, from a good lie
        float spin;    // rpm of backspin at full power
    };
    constexpr int ClubCount=8,Driver=0,Wedge=6,Putter=7;
    const Club& ClubOf(int club);
    std::wstring ClubName(int club);

    // One hole, made from a seed: its line (a dogleg or straight), the fairway, the green with
    // its slope and the cup, bunkers, maybe a pond, trees along the sides; the ground's rise and
    // fall. Outside its bounds the ball is out of bounds.
    class Course
    {
    public:
        Course(std::uint32_t seed,int par);
        float Height(float x,float z) const;
        V3 Normal(float x,float z) const;
        Lie LieAt(float x,float z) const;
        // How much of each surface a point is (for drawing: the edges blend). Rough is the rest.
        void Surface(float x,float z,float& fairway,float& green,float& sand,float& water,float& tee) const;
        V3 Tee() const {return {0,Height(0,0),0};}
        V3 Pin() const {return {pin.x,Height(pin.x,pin.z),pin.z};}
        V3 GreenMiddle() const {return {green.x,Height(green.x,green.z),green.z};}
        int Par() const {return par;}
        float Length() const {return length;}   // tee to the green's middle, along the line
        float WaterLevel() const {return waterLevel;}
        bool HasWater() const {return pond.rx>0;}
        // Bounds of what is drawn (and in play), metres.
        float MinX() const {return minX;}
        float MaxX() const {return maxX;}
        float MinZ() const {return -20;}
        float MaxZ() const {return length+40;}
        float LineX(float z) const;   // the hole's middle line across at z
        struct Tree {float x,z,height,radius;int kind;};
        const std::vector<Tree>& Trees() const {return trees;}
        std::uint32_t Seed() const {return seed;}

    private:
        struct Ellipse {float x=0,z=0,rx=0,rz=0,turn=0;float At(float px,float pz) const;};   // < 1 inside
        float BaseHeight(float x,float z) const;
        std::uint32_t seed;
        int par;
        float length=360,bend=0,bendFrom=150,bendTo=260,fairwayFrom=80,fairwayWidth=30;
        Ellipse green,pond;
        V3 pin;
        float greenTilt[2]={0,0};      // the green's slope (rise per metre along x, z)
        float waterLevel=-100;
        std::vector<Ellipse> bunkers;
        std::vector<Tree> trees;
        float minX=-60,maxX=60;
        float hills[6]={};             // the ground's waves' phases
    };

    struct Wind {float speed=0,toward=0;};   // m/s; the way it blows (radians, 0 = down the hole, + to the right)
    V3 WindVector(const Wind& w);

    struct Shot
    {
        int club=Driver;
        float aim=0;        // radians from straight down the hole (+ to the right)
        float power=1;      // 0-1 (a little over 1: forced)
        float accuracy=0;   // the strike: 0 square, + early (it goes right and curves right), - late
        float range=0;      // the putter: the distance (m) power 1 sends it on level ground
    };

    // The ball's way from a lie, struck so: where it is each 1/60 s, where it stops, how it ended.
    struct Flight
    {
        std::vector<V3> path;
        V3 rest;
        Lie lie=Lie::Rough;
        bool holed=false,water=false,out=false;
        float carry=0;         // across the ground to where it first came down
        int landing=0;         // path index where it first came down
        V3 drop;               // in the water: the last place on land it crossed (dropped there)
    };
    // keepPath false: only where it ends (quicker; her thinking). step: seconds a step.
    Flight Fly(const Course& course,V3 from,Lie lie,const Shot& shot,const Wind& wind,bool keepPath=true,float step=1/60.0f);

    // The putter's range for a putt of this distance: the next of 3, 6, 10, 15, 20, 30, 45, 60 m
    // above it with room (power 1 rolls that far on level ground).
    float PutterRange(float distance);
    // Clubs that can be played from a lie: off the green every club but the putter (and the
    // putter near the green); on it, only the putter.
    std::vector<int> ClubsFor(Lie lie,float toPin);
    // The club for a distance (the one whose full carry is just past it).
    int ClubFor(Lie lie,float toPin,const Course& course,V3 ball,const Wind& wind);

    // Her choice for the ball at from: club, aim and power, by trying them out (wind and
    // slope counted), scored by how many more strokes it should take from where it stops.
    Shot Choose(const Course& course,V3 from,Lie lie,const Wind& wind);
    // Her strike as it comes off: a little off in power and timing, more so the less care she takes.
    void Execute(Shot& shot,float care,std::mt19937& random);
    // How many strokes from a ball at p (lie) to hole out, roughly (a putt from 1 m: 1).
    float StrokesLeft(const Course& course,V3 p,Lie lie);

    // Diagnostics (--golf-test): every club's carry and roll at full power on level ground with
    // no wind; holes made from seeds checked (the green and tee in bounds and dry, a par each);
    // her play from the tee to the hole on each, counted. First line "<n> wrong".
    std::wstring Check(int holes,std::uint32_t seed);
}
