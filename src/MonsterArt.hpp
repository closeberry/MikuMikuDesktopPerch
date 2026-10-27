#pragma once
// The dungeon's monsters as 3D shapes (docs/Dungeon.md): each kind built from a few simple
// solids (ellipsoids, tapered limbs, boxes, flat wings) in plain colours, turned to face her,
// and posed as it walks, strikes, flinches or gathers itself.
#include "Dungeon.hpp"
#include "MascotRenderer.hpp"
#include <vector>

namespace MikuMikuDesktopPerch
{
    // How a monster moves this frame.
    struct MonsterPose
    {
        float walk=0;        // its stride's phase (radians), running on while it walks
        float moving=0;      // how much it walks (0 standing .. 1 striding)
        float strike=-1;     // how far through its attack (0 .. 1), or -1 when not attacking
        float flinch=-1;     // how far through flinching from a blow (0 .. 1), or -1
        float time=0;        // seconds, for what it does all the time (a bat's wings, a ghost's drift)
        bool charging=false; // an orc gathering itself for a heavy blow
    };

    // Adds monster m standing at (x, z) in world units, facing yaw (radians about +Y; 0 faces
    // +Z), posed so, to out. Returns how tall it is (for what is shown above it).
    float AddMonster(std::vector<MascotRenderer::GeometryVertex>& out,const Dungeon::Monster& m,float x,float z,float yaw,const MonsterPose& pose={});
}
