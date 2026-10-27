#pragma once
// The dungeon's monsters as 3D shapes (docs/Dungeon.md): each kind built from a few simple
// solids (ellipsoids, tapered limbs, boxes, flat wings) in plain colours, turned to face her.
#include "Dungeon.hpp"
#include "MascotRenderer.hpp"
#include <vector>

namespace MikuMikuDesktopPerch
{
    // Adds monster m standing at (x, z) in world units, facing yaw (radians about +Y; 0 faces
    // +Z), to out. Returns how tall it is (for what is shown above it).
    float AddMonster(std::vector<MascotRenderer::GeometryVertex>& out,const Dungeon::Monster& m,float x,float z,float yaw);
}
