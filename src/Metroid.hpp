#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// The ruins (docs/Metroidvania.md): a fixed map of tiles seen from the side, cut into rooms, and
// her body running and jumping through it. Tiles are a share of her height (she is about 2.5
// tiles tall); x runs right, y runs down, in tiles.
namespace MikuMikuDesktopPerch::Metroid
{
    constexpr int ScreenWidth=28,ScreenHeight=16;   // a screen's tiles: rooms are whole screens

    struct Area
    {
        int number=0;
        int look=0;                      // the walls' and floors' look (GeometryPS's themes)
        std::array<std::wstring,2> name;   // Japanese, English
    };

    struct Room
    {
        int area=0;
        int x=0,y=0,width=0,height=0;   // tiles, on the whole map's grid
        std::array<std::wstring,2> name;
        std::vector<std::string> rows;
        bool Contains(int tx,int ty) const {return tx>=x&&ty>=y&&tx<x+width&&ty<y+height;}
    };

    class Map
    {
    public:
        // The map's text (res/metroid/map.txt). False with the reason when it is not right.
        bool Parse(const std::string& utf8,std::wstring& error);
        // The one built into the exe.
        bool LoadBuiltIn(std::wstring& error);
        // The tile at (tx, ty): '#' outside every room.
        char At(int tx,int ty) const;
        int RoomAt(int tx,int ty) const;   // -1: none
        const std::vector<Room>& Rooms() const {return rooms;}
        const std::vector<Area>& Areas() const {return areas;}
        const Area* AreaOf(int number) const;
        float StartX() const {return startX;}
        float StartY() const {return startY;}
        // What is wrong with it, a line each (empty: nothing): rooms overlapping, an opening in a
        // room's edge with no room beyond it.
        std::wstring Check() const;

    private:
        std::vector<Room> rooms;
        std::vector<Area> areas;
        float startX=0,startY=0;
    };

    // The keys held this moment.
    struct Input
    {
        bool left=false,right=false,up=false,down=false,jump=false;
    };

    // How she moves: tiles and seconds.
    struct Moves
    {
        float run=6.0f;           // top speed on the ground
        float accelerate=60;      // to the top speed in 0.1 s
        float stop=75;            // from it to a stop in 0.08 s
        float airAccelerate=40;
        float gravity=62;
        float jumpHeight=3.3f;    // a full jump lifts her feet this high (a 3-tile ledge is in reach)
        float cut=0.45f;          // the rise kept when the jump key is let go early
        float fall=22;            // the fastest fall
        float coyote=0.1f;        // a jump still taken this long after running off an edge
        float buffer=0.1f;        // a jump pressed this long before landing is taken on landing
        float halfWidth=0.36f,height=2.2f;   // her body's box (feet in the middle of its foot)
    };

    // Her body: where her feet are (the middle of her soles), how fast, which way she faces.
    struct Body
    {
        float x=0,y=0,vx=0,vy=0;
        int facing=1;
        bool ground=true;
    };

    // What happened in a step, for the window (her motion, sounds, words).
    struct Events
    {
        bool jumped=false;
        float landed=0;        // the speed she came down at (0: did not land)
        bool roomChanged=false;
    };

    class Game
    {
    public:
        explicit Game(const Map& map);
        // Puts her at (x, y), standing still.
        void Place(float x,float y);
        // On by dt with these keys held (steps of at most 1/240 s inside).
        void Step(double dt,const Input& in);
        const Body& Hero() const {return body;}
        const Moves& Rule() const {return moves;}
        int Room() const {return room;}
        double Clock() const {return clock;}
        const std::vector<bool>& Visited() const {return visited;}
        // Since last taken.
        Events TakeEvents(){Events e=events;events={};return e;}

    private:
        bool Solid(int tx,int ty) const;
        bool StandOn(int tx,int ty,float fromY) const;   // a ledge counts coming down onto it
        void Tick(float h,const Input& in);

        const Map& map;
        Moves moves;
        Body body;
        int room=-1;
        std::vector<bool> visited;
        double clock=0;
        float coyote=0,buffer=0;
        bool rising=false,jumpHeld=false;
        Events events;
    };
}
