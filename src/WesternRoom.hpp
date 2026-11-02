#pragma once
#include "MascotRenderer.hpp"
#include <vector>

namespace MikuMikuDesktopPerch
{
    // The Western parlour round a table (chess and the card games): parquet and a rug, wallpaper
    // over panelling, curtained windows and a fireplace behind her, a painting, a bookcase and a
    // chandelier. Sizes follow the character: upper is the length of her upper body.
    struct WesternRoomSize
    {
        float upper=10,hips=10;
        float floor=-5;      // the floor's height
        float tableTop=0;    // the table's height
        float lampZ=0;       // where over the table the chandelier hangs
    };
    // Adds the room to vertices, and its soft lights (windows, candles, the fire) to glows
    // (emptied first).
    void BuildWesternRoom(std::vector<MascotRenderer::GeometryVertex>& vertices,std::vector<MascotRenderer::Particle>& glows,const WesternRoomSize& size);
    // Those lights as they are this moment (the flames flicker, the glass twinkles), added to shown. clock: seconds.
    void ShowRoomGlows(std::vector<MascotRenderer::Particle>& shown,const std::vector<MascotRenderer::Particle>& glows,double clock);
}
