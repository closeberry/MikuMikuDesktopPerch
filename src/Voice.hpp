#pragma once
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace MikuMikuDesktopPerch
{
    // A phrase made into speech by the voice engine: the WAV file and when each syllable
    // starts in it (seconds; L"、" for a pause), for the mouth.
    struct VoiceClip
    {
        std::string wav;
        double seconds=0;
        std::vector<std::pair<double,std::wstring>> morae;
    };

    // The voices the engine offers ("character（style）", style id), waiting a few seconds
    // at most. Returns an explanation on failure (empty on success).
    std::wstring ListVoices(const std::wstring& endpoint,std::vector<std::pair<std::wstring,int>>& voices);

    // Say text in a voice right away (waiting for it): the WAV file. Returns an explanation
    // on failure (empty on success).
    std::wstring SynthesizeVoice(const std::wstring& endpoint,int speaker,const std::wstring& text,std::string& wav);

    // Speech through a VOICEVOX-compatible engine (VOICEVOX, AivisSpeech, ...) on this computer
    // (POST /audio_query, then /synthesis). Phrases are made one at a time, in the order asked.
    class VoiceEngine
    {
    public:
        // Posted to the notify window when a phrase is done: wParam is a heap-allocated
        // VoiceClip* the receiver deletes (nullptr when it failed), lParam the request id.
        static constexpr UINT DoneMessage=WM_APP+22;

        explicit VoiceEngine(HWND notify);
        ~VoiceEngine();
        VoiceEngine(const VoiceEngine&)=delete;
        VoiceEngine& operator=(const VoiceEngine&)=delete;

        // Queue a phrase; returns its request id.
        int Speak(const std::wstring& endpoint,int speaker,const std::wstring& text);
        void CancelAll();   // phrases not yet made are dropped (results still on the way are ignored by id)

    private:
        struct Job {int id;std::wstring endpoint;int speaker;std::wstring text;};
        void Run();

        HWND notify;
        std::thread worker;
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<Job> jobs;
        bool quit=false;
        int nextId=1;
    };
}
