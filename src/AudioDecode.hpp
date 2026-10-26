#pragma once
#include <windows.h>
#include <mmreg.h>
#include <cstdint>
#include <string>

namespace MikuMikuDesktopPerch
{
    // A sound file in memory (MP3, AAC / M4A, WAV, FLAC, and whatever else the codecs of this
    // Windows read) made into a 16-bit PCM WAV file, mono or stereo, through Media Foundation.
    // Up to 10 minutes. Returns an explanation on failure (empty on success).
    std::wstring DecodeAudio(const std::string& bytes,std::string& wav);

    // The 16-bit PCM in a WAV file: its format and samples (false when it is not that).
    bool ReadPcm(const std::string& wav,WAVEFORMATEX& format,const std::int16_t*& samples,size_t& count);
}
