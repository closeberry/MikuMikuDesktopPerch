#pragma once
// The dungeon's things as 3D shapes (docs/Dungeon.md): chests, gold, keys, the goods in a
// shop, campfires, the stairs down, doors, traps and crystals, built from simple solids
// (ShapeBuilder) in wood, metal, stone and glowing colours. Positions are world units: the
// middle of the thing's tile, on the floor (y 0). k dims a thing remembered but out of sight.
#include "Dungeon.hpp"
#include "MascotRenderer.hpp"
#include <vector>

namespace MikuMikuDesktopPerch
{
    using Geometry=std::vector<MascotRenderer::GeometryVertex>;

    void AddChest(Geometry& out,float x,float z,float k);
    void AddGold(Geometry& out,float x,float z,int amount,float k);
    void AddKey(Geometry& out,float x,float z,float k);
    // A shop's goods: the mat she stands on to buy, the thing on it.
    void AddGoods(Geometry& out,float x,float z,const Dungeon::Item& item,float k);
    void AddCampfire(Geometry& out,float x,float z,bool burning,float k);
    // A burning campfire's flames at time (seconds), drawn every frame: they flicker.
    void AddFlames(Geometry& out,float x,float z,float time,float k);
    // Steps down into the floor from a landing at the tile's middle (where she stands).
    void AddStairs(Geometry& out,float x,float z,float k);
    // A door filling its tile up to height (alongZ: the passage runs along z through it).
    void AddDoor(Geometry& out,float x,float z,float height,bool alongZ,float k);
    void AddTrap(Geometry& out,float x,float z,Dungeon::TrapKind kind,float k);
    // A few crystals growing from the floor at (x, z), tilted as seed says.
    void AddCrystals(Geometry& out,float x,float z,float height,int seed,float k);
}
