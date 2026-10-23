#pragma once
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
    bool LoadImageRgba8(const std::wstring& path,ImageRgba8& image,unsigned maxSize=0);
    void GenerateMips(ImageRgba8& image);
}
