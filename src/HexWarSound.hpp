#pragma once
#include <string>

namespace MikuMikuDesktopPerch
{
    // The hex war's battle sounds: a tank's gun, an infantry rifle, a tank blown up (one take
    // of the gun, four of the others, WAV resources of type HEXWARSOUND, see the .rc). They are mixed into one output of
    // their own, each take a little different in pitch and loudness, placed left or right, and
    // held under a ceiling however many play at once (a limiter that comes down at once and
    // rises again slowly).
    // Tracks and Steps: a vehicle moving, men marching; loops, played for as long as asked.
    enum class HexWarSound {Cannon,Rifle,Blast,Tracks,Steps};
    // pan: -1 left .. +1 right; loud: 0..1; seconds: for a loop, how long it plays (faded in
    // and out), 0 for a sound played once through.
    void PlayHexWarSound(HexWarSound sound,float pan=0,float loud=1,double seconds=0);
    // Stops the sounds and closes the output (the window closing).
    void StopHexWarSounds();
    // Diagnostics (--hexwar-sounds <out.wav>): a battle's worth of sounds mixed as in play
    // (both sides rolling in, ten guns and ten rifles a shot each 0.2 s, six tanks blown up),
    // saved as a WAV.
    bool SaveHexWarSoundTest(const std::wstring& path);
}
