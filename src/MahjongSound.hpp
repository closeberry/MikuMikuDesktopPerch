#pragma once
#include <string>

namespace MikuMikuDesktopPerch
{
    // The mahjong table's sounds: a tile set down (hard, for a riichi), a tile drawn, a tile
    // vanishing and appearing (her touch), pon, kan, the riichi stick, a win declared, the jingles
    // of a hand's end and of the game's, and the tiles shuffled and dealt. The tiles' are WAV
    // files built in (res\mahjong\sounds), the rest are made here. Several play at once, each on
    // an output of its own (not PlaySound, which the whole program shares and cuts off).
    enum class MahjongSound {Place,PlaceHard,Draw,Vanish,Appear,Pon,Kan,Riichi,Win,YouWon,YouLost,Drawn,GameWon,GameLost,Shuffle,Deal};
    void PlayMahjongSound(MahjongSound sound);
    // Stops what is playing (the mahjong window closing).
    void StopMahjongSounds();
    // Diagnostics (--mahjong-sounds <folder>): each sound as a WAV file there (place.wav, ...).
    bool SaveMahjongSounds(const std::wstring& folder);
}
