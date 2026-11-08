#pragma once
#include <array>
#include <cstdint>
#include <memory>
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

    // A model of the course's (res/golf/*.bin, made by tools/golf/build_scene.py from the
    // user's PMX files): its pictures' resource names (type GOLFTEX), its materials, vertices
    // (x, y, z, normal, u, v: MMD units, 12.5 a metre), triangles and bones.
    struct SceneModel
    {
        struct Material
        {
            std::string name;
            int texture=-1;
            float rgb[3]{1,1,1};
            int kind=0;   // 0 drawn only, 1 rough, 2 fairway, 3 green, 4 fringe, 5 tee, 6 bunker, 7 cart path, 8 water, 9 left out, 10 the sky
            std::uint32_t first=0,count=0;
        };
        std::vector<std::string> textures;
        std::vector<Material> materials;
        std::vector<float> vertices;          // 8 a vertex
        std::vector<std::uint32_t> indices;
        std::vector<std::pair<std::string,V3>> bones;
    };
    // name: the resource (type GOLFMODEL): COURSE, SKY, CLUBHOUSE1-4.
    bool LoadScene(const wchar_t* name,SceneModel& model);

    // The course model's ground, for the ball: its surface triangles (metres) and what each is,
    // found quickly by a grid; the tee, the model's own cup, the clubhouse's place.
    struct Ground;
    // The course's holes (resources HOLE1-9), each its own model with the tee at the origin.
    constexpr int HoleCount=9;
    std::shared_ptr<const Ground> LoadGround(int hole=1);   // made once per hole (null: none)
    // A rainy day: the ground takes the ball softer (it bounces less and rolls shorter).
    void SetWet(bool wet);
    int HolePar(int hole);   // 1-9

    // A tree's shape (metres, at its own size): its leaves' middle height, reach across, half
    // their height; the trunk's radius.
    struct TreeShape {float middle,across,half,trunk;};
    constexpr int TreeKinds=5;
    const std::array<TreeShape,TreeKinds>& TreeShapes();

    // One hole on the course: the pin placed by a seed (0: the model's own cup).
    class Course
    {
    public:
        // hard: the pin near the green's edge (a hard-pin day).
        Course(std::shared_ptr<const Ground> ground,std::uint32_t seed,bool hard=false);
        float Height(float x,float z) const;
        V3 Normal(float x,float z) const;
        Lie LieAt(float x,float z) const;
        bool Hard(float x,float z) const;   // the cart path (it bounces high and rolls on)
        V3 Tee() const;
        V3 Pin() const {return pin;}
        V3 ModelCup() const;            // the model's own cup (filled in when the pin is elsewhere)
        V3 Clubhouse() const;           // where the clubhouse stands (its ground's middle)
        V3 GreenMiddle() const;
        int Hole() const;               // 1-9
        int Par() const;
        float Length() const;           // along the fairway's way to the pin, metres
        // The fairway's way from the tee to the green: the point so far along it, and where to aim
        // from p (about so far down the way from where p is beside it; the pin when it is nearer).
        V3 RouteAt(float metres) const;
        V3 AimPoint(V3 p,float ahead) const;
        // How far the pin is to play from p: straight, unless that line leaves the course; then
        // round by the fairway's way.
        float PlayDistance(V3 p) const;
        // Bounds of play (beyond them, and on the clubhouse's ground, out of bounds), metres.
        float MinX() const;
        float MaxX() const;
        float MinZ() const;
        float MaxZ() const;
        // The wood round the course: the trees out of bounds (where, turned, how big, which of
        // the five), the same for every hole.
        struct Tree {float x,y,z,yaw,scale;int kind;};
        const std::vector<Tree>& Trees() const {return wood->trees;}
        // A ball in the air at p: slowed in a tree's leaves, bounced off its trunk (false: none there).
        bool ThroughTrees(V3& p,V3& v,V3& spin,float step) const;
        std::uint32_t Seed() const {return seed;}

    private:
        std::shared_ptr<const Ground> ground;
        std::uint32_t seed;
        V3 pin;
        // The wood, planted once for the ground and shared by every hole on it.
        struct Wood
        {
            std::vector<Tree> trees;
            static constexpr float Cell=8;
            float x0=0,z0=0;
            int nx=0,nz=0;
            std::vector<std::vector<int>> cells;   // the trees whose leaves reach over each cell
        };
        static std::shared_ptr<const Wood> PlantTrees(const Ground& ground);
        std::shared_ptr<const Wood> wood;
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
        // Where on the ball the club meets it (-1 to 1): +x its right (it curves left), +y its
        // top (lower, less spin, rolls further); below, it climbs, flies a little further, stops.
        float spotX=0,spotY=0;
        // Special strikes: spin 1 the back-spin shot (it stops dead at the third bounce and
        // spins back), -1 the top-spin shot (it runs on a little after it would stop); special
        // 1 the boost (a little further), 2 the meteor (much further, and it stops where it lands).
        int spin=0,special=0;
        float meteor=0;     // (the meteor's speed, as Fly works it out: 0 not yet)
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
        bool beamed=false;     // drawn in by the light pillar
        int spunAt=-1;         // path index where a spin shot took hold (stopped and spun back, or ran on)
        int boomAt=-1;         // path index where a meteor shot came down
        int beamAt=0;          // path index where the beam took it
        V3 drop;               // in the water: the last place on land it crossed (dropped there)
    };
    // keepPath false: only where it ends (quicker; her thinking). step: seconds a step.
    Flight Fly(const Course& course,V3 from,Lie lie,const Shot& shot,const Wind& wind,bool keepPath=true,float step=1/60.0f);

    // The putter's range for a putt of this distance, rise metres uphill: the next of 3, 5, 8,
    // 12, 16, 20, 30, 45, 60 m with room (power 1 rolls that far on level ground; uphill counts
    // as the roll it costs).
    float PutterRange(float distance,float rise=0);
    // A club's carry at full power from a good lie on level ground with no wind (the gauge's
    // numbers), struck at that spot.
    // How fast the ball leaves the face (m/s), as Fly launches it (her swing's head is as fast there).
    float LaunchSpeed(const Shot& shot,Lie lie);
    float FlatCarry(int club,float spotX=0,float spotY=0);
    // The share of the club's full speed that power sends the ball with: power is a share of the
    // full carry on level ground (half power, half as far), not of the speed.
    float SpeedShare(int club,float power,float spotY=0);
    // Where it comes down on level ground with no wind, struck square at full power: x across
    // (aimed along +z), z ahead (metres).
    V3 FlatLanding(int club,float spotX=0,float spotY=0);
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
