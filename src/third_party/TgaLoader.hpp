#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace MotionTraceEditor::PreviewTextureFileLoader
{
    // Uncompressed / RLE 24/32-bit TGA to straight RGBA8, top-left origin.
    bool LoadTgaPixels(
        const std::wstring& path,
        std::vector<std::uint8_t>& rgba,
        UINT& width,UINT& height);
}
