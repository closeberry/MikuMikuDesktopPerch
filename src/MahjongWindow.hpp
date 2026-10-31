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
        std::function<void()> onGameOver; // each game played to its end (not one given up)
        // Not used now (her lines come by scene, from res/mahjong/lines.txt): asks the LLM about
        // situation (a stage direction), the line coming back through MahjongHeard. False when it
        // cannot now.
        std::function<bool(const std::wstring& situation)> talk;
        // Something those watching may speak of (観戦者の解説): what happened, told as to one
        // watching, how much it matters (1-9), and a line to say without the LLM.
        std::function<void(const std::wstring& facts,int weight,const std::wstring& fallback)> onEvent;
        // Her lines by scene asked for ahead, a scene at a time: sends ask to the LLM, the lines
        // coming back through MahjongLines. False when it cannot now.
        std::function<bool(const std::wstring& ask)> prefetch;
        // Her lines in her own words are kept in this file (empty: not kept), under this key
        // (her name and persona: a change asks again).
        std::wstring linesFile,styleKey;
        // The user's own lines (JSON; docs/GameLines.md), when there is such a file.
        std::wstring ownLinesFile;
        // Her line said aloud in her voice (the voice engine), when speaking aloud is on; an empty
        // line stops what she is saying.
        std::function<void(const std::wstring& line)> speak;
    };

    // Opens it (or brings it forward when open). onEnd: once, when the window closes.
    void ShowMahjongWindow(HWND owner,const MahjongOpponent& her,std::function<void()> onEnd);
    bool MahjongWindowOpen();
    // The mahjong window, and where it is on the screen while it shows (not minimized).
    HWND MahjongWindowHandle();
    bool MahjongWindowRect(RECT& r);

    // After the window closes: what to say on the game played to its end (a stage direction for
    // the LLM, and her own line without it). False when none was (or it was given up).
    bool MahjongAfterword(std::wstring& situation,std::wstring& fallback);
    // The lines asked for through MahjongOpponent::prefetch: the LLM's answer, whole (empty: none).
    void MahjongLines(const std::wstring& answer);
    // Her line asked for through MahjongOpponent::talk, as it grows; done: it is all there (empty:
    // none came, her own line is shown instead).
    void MahjongHeard(const std::wstring& line,bool done);

    // Every frame while open: her model, as it is posed now, drawn at the table (she sits).
    void TickMahjongWindow(MascotModel& model,double dt);

    // Diagnostics (--snapshot with --mahjong): the table's 3D view as last drawn, to a PNG, and
    // a game played on by a simple player of yours for moves steps first.
    bool SaveMahjongView(const std::wstring& path);
    // Diagnostics: where her feet are against the floor (world units), for the snapshot's report.
    std::wstring DescribeMahjongFeet();
    void PlayMahjongMoves(int moves);
    // Diagnostics (--mahjong-camera): the camera turned this way (its tilt in radians, how far,
    // where it looks, raised and moved back from the default, in her upper body's lengths, and
    // how far round her in radians).
    void SetMahjongCamera(float pitch,float distance,float y,float z,float yaw);
    // Diagnostics (--mahjong-reach [far]): once seated, she touches a tile of hers as at a discard,
    // and her newest river tile fades in again. whole (far): her hand goes the whole way out to the tile
    // (leaning in), not the light pat in front of her.
    void ReachMahjongForTest(bool whole=false);
    // Diagnostics (--mahjong-solid-lift x): the table's solids (MascotModel::SetSolids) raised by
    // x of her upper body's lengths above the table drawn, to see her stop short of them.
    void LiftMahjongSolidsForTest(float lift);
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
