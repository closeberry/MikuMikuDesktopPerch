#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch
{
    // The mahjong mini game's window (docs/Mahjong.md): you against her, drawn in 2D for now.
    // Click a tile (or ←→ and Enter) to discard; the buttons above your hand (or their number
    // keys, Space to let a tile go) win, call, kan and riichi. Esc gives up. One at a time.
    struct MahjongOpponent
    {
        std::wstring name;
        std::vector<std::uint8_t> face;   // premultiplied BGRA, faceSize square (empty: her initial)
        int faceSize=0;
        int bond=0;                       // how close she and the user are (0-4): she wins a little less
    };

    // Opens it (or brings it forward when open). onEnd: once, when the window closes.
    void ShowMahjongWindow(HWND owner,const MahjongOpponent& her,std::function<void()> onEnd);
    bool MahjongWindowOpen();

    // Diagnostics (--mahjong-shot): a game played by a simple player for moves steps (or to the
    // hand's end), drawn as the window would be, to a PNG.
    bool SaveMahjongShot(const std::wstring& path,int moves,std::uint32_t seed);
    // Diagnostics (--mahjong-tiles): every tile's face, at the hand's size and the river's, to a PNG.
    bool SaveMahjongTiles(const std::wstring& path);
}
