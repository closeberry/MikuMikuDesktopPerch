#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch
{
    // Pictures of moments with the characters (docs/Album.md), saved under
    // %LOCALAPPDATA%\MikuMikuDesktopPerch\album with their words in album.txt. Made from what
    // the app drew itself (the characters' frames, the bubbles' bitmaps), never from the
    // screen, so nothing behind them is ever in a picture.

    // An image placed on the screen: premultiplied BGRA rows, rect's width by height.
    struct AlbumLayer
    {
        RECT rect{};
        std::vector<std::uint8_t> pixels;
    };

    struct AlbumShot
    {
        bool automatic=false;              // a day or a stage reached (auto\), or taken by the user (manual\)
        std::wstring label;                // the file name's end ("一緒に100日"); manual ones are named by the time
        std::vector<AlbumLayer> characters,bubbles;
        std::vector<std::pair<std::wstring,std::wstring>> words;   // who said what, in the bubbles
    };

    // Saves the picture with its bubbles and one without (both on a soft background, cropped
    // to what is drawn), and a line in album.txt. Returns the first file, or empty (error says why).
    std::wstring SaveAlbumShot(const AlbumShot& shot,std::wstring& error);

    // The album's folder (made when create is set).
    std::wstring AlbumFolder(bool create);

    // A PNG from premultiplied BGRA rows.
    bool WritePng(const std::wstring& path,UINT width,UINT height,UINT pitch,const std::uint8_t* pixels);

    // The shutter's click (made here, no file needed), played without waiting.
    void PlayShutter();
}
