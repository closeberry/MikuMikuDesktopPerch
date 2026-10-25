#pragma once
#include <string>

namespace MikuMikuDesktopPerch
{
    // A sound file in memory (MP3, AAC / M4A, WAV, FLAC, and whatever else the codecs of this
    // Windows read) made into a 16-bit PCM WAV file, mono or stereo, through Media Foundation.
    // Up to 10 minutes. Returns an explanation on failure (empty on success).
    std::wstring DecodeAudio(const std::string& bytes,std::string& wav);
}
