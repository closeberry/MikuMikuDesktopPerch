#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch
{
    class MascotModel;

    // The mahjong mini game's window (docs/Mahjong.md): you against her, she sitting across the
    // table in 3D with the rivers on it, your hand, the buttons and the words laid over it. Click a tile (or ←→ and
    // Enter) to discard; the buttons above your hand (or their number keys, Space to let a
    // tile go) win, call, kan and riichi. Esc gives up. One at a time.
    struct MahjongOpponent
    {
        std::wstring name;
        std::vector<std::uint8_t> face;   // premultiplied BGRA, faceSize square (empty: her initial)
        int faceSize=0;
        int bond=0;                       // how close she and the user are (0-4)
        // Her model, drawn sitting at the table (null: 2D only, her face at the top). It must
        // stay loaded while the window is open; TickMahjongWindow is given it every frame.
        MascotModel* model=nullptr;
        std::wstring toonDir;
        float edgeScale=1;
        int msaa=4;                       // MSAA samples and the textures' largest side
        unsigned maxTexture=1024;
        bool light=false;                 // fewer pixels drawn (the settings' light 3D)
    };

    // Opens it (or brings it forward when open). onEnd: once, when the window closes.
    void ShowMahjongWindow(HWND owner,const MahjongOpponent& her,std::function<void()> onEnd);
    bool MahjongWindowOpen();

    // Every frame while open: her model, as it is posed now, drawn at the table (she sits).
    void TickMahjongWindow(MascotModel& model,double dt);

    // Diagnostics (--snapshot with --mahjong): the table's 3D view as last drawn, to a PNG, and
    // a game played on by a simple player of yours for moves steps first.
    bool SaveMahjongView(const std::wstring& path);
    void PlayMahjongMoves(int moves);
    // Diagnostics (--mahjong-camera): the camera turned this way (its tilt in radians, how far,
    // where it looks, raised and moved back from the default, in her upper body's lengths, and
    // how far round her in radians).
    void SetMahjongCamera(float pitch,float distance,float y,float z,float yaw);
    // Diagnostics (--mahjong-reach): once seated, she touches a tile of hers as at a discard, and
    // her newest river tile fades in again.
    void ReachMahjongForTest();
    // Diagnostics (--mahjong-gesture name): once seated, she plays that gesture (think, clap,
    // cheeks, oops, ...).
    void GestureMahjongForTest(const std::wstring& name);

    // Diagnostics (--mahjong-shot): a game played by a simple player for moves steps (or to the
    // hand's end), drawn as the window would be, to a PNG. stop: L"riichi" stops instead where
    // you can first riichi, choosing the tile for it (the first that can go, pointed at);
    // L"tsumo" where you can first tsumo (playing for you without riichi).
    bool SaveMahjongShot(const std::wstring& path,int moves,std::uint32_t seed,const std::wstring& stop={});
    // Diagnostics (--mahjong-tiles): every tile's face, at the hand's size and the river's, to a PNG.
    bool SaveMahjongTiles(const std::wstring& path);
}
