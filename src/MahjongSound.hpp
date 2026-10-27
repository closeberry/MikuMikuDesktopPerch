#pragma once
#include <string>

namespace MikuMikuDesktopPerch
{
    // The mahjong table's sounds, made here (no sound files): a tile set down, a tile drawn, a
    // tile vanishing and appearing (her touch), a call (pon, kan), riichi, a win declared, and
    // the jingles of a hand's end and of the game's. Several play at once, each on an output of
    // its own (not PlaySound, which the whole program shares and cuts off).
    enum class MahjongSound {Place,Draw,Vanish,Appear,Call,Riichi,Win,YouWon,YouLost,Drawn,GameWon,GameLost};
    void PlayMahjongSound(MahjongSound sound);
    // Stops what is playing (the mahjong window closing).
    void StopMahjongSounds();
    // Diagnostics (--mahjong-sounds <folder>): each sound as a WAV file there (place.wav, ...).
    bool SaveMahjongSounds(const std::wstring& folder);
}
