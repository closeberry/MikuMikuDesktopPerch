#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch
{
    // Straight-alpha RGBA8 image with a full CPU-generated mip chain.
    struct ImageRgba8
    {
        struct Level { unsigned width=0,height=0; std::vector<std::uint8_t> pixels; };
        std::vector<Level> levels;
        bool hasAlpha=false;
    };

    // bmp/png/jpg/gif/tif through WIC, tga through the copied MTE decoder.
    // maxSize > 0 drops top mip levels until both sides fit, saving GPU memory.
    // uvTriangles (3 UVs per triangle, the model's faces using this texture) marks the texels
    // the model really uses: transparent padding outside them is filled from the nearest used
    // texel, so filtering and smaller mips do not blend see-through padding into UV seams.
    bool LoadImageRgba8(const std::wstring& path,ImageRgba8& image,unsigned maxSize=0,
                        const std::vector<std::array<float,2>>* uvTriangles=nullptr);
    void GenerateMips(ImageRgba8& image);
}
