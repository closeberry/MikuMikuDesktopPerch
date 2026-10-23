#pragma once
#include <string>
#include <utility>
#include <vector>

// Speech straight from the VOICEVOX installation's own files, without starting its engine:
// voicevox_core.dll (VOICEVOX CORE 0.16), its ONNX Runtime, the voice models (model\*.vvm) and
// the Open JTalk dictionary, all loaded from the folder the user names. Nothing of VOICEVOX
// ships with this program. Calls are serialized; every function returns an explanation on
// failure (empty on success).
namespace MikuMikuDesktopPerch::VoicevoxCore
{
    // The folder holding voicevox_core.dll: folder itself or its vv-engine (VOICEVOX's layout).
    // Empty when neither has it.
    std::wstring FindCoreFolder(const std::wstring& folder);

    // The voices in the models ("character（style）", style id).
    std::wstring Voices(const std::wstring& folder,std::vector<std::pair<std::wstring,int>>& voices);
    // An audio query (JSON, as the engine's /audio_query gives) for text in a voice; the voice's
    // model is loaded the first time.
    std::wstring AudioQuery(const std::wstring& folder,const std::wstring& text,int style,std::string& json);
    // The WAV file for an audio query.
    std::wstring Synthesis(const std::wstring& folder,const std::string& json,int style,std::string& wav);
}
