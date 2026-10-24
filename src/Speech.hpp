#pragma once
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "LipSyncCore.hpp"
#include "Voice.hpp"

namespace MikuMikuDesktopPerch
{
    // The reading of Japanese text in hiragana, through the Windows Japanese IME's conversion
    // engine (MSIME.Japan). Where that is unavailable, kana are kept and every other character
    // becomes a guess of two syllables, which is enough to move a mouth. English words become
    // their vowel sounds in kana (spelling read roughly: enough for the mouth, not a voice).
    std::wstring HiraganaReading(const std::wstring& text);

    // A reply spoken aloud: the text is read into syllables (morae) as it streams in, the
    // syllables are placed on a timeline at a talking pace with pauses at punctuation, and
    // MotionTraceEditor's lip-sync turns them into mouth shapes. The bubble shows the text as
    // far as it has been "said". With a voice, each phrase is made into speech first and its
    // syllables follow the sound (a phrase the voice fails on is said silently).
    class Speech
    {
    public:
        Speech();
        Speech(Speech&&) noexcept;
        Speech& operator=(Speech&&) noexcept;
        ~Speech();   // stops a voice still playing (its sound lives here)
        void Reset();
        // voice(text) asks for a phrase to be voiced and returns the request id (empty: silent).
        void SetVoice(std::function<int(const std::wstring&)> voice){this->voice=std::move(voice);}
        void Voiced(int id,VoiceClip* clip);  // a phrase came back from the voice (nullptr: failed)
        void Add(const std::wstring& text);   // more of the reply (as shown in the bubble)
        void Finish();                        // the reply is complete
        void Tick(double dt);

        bool Active() const {return !events.empty()||!pending.empty()||!waiting.empty();}
        bool Speaking() const;                // still moving the mouth
        double RevealedChars() const;         // how much of the text has been said
        // Weights for the あ い う え お ん morphs now.
        std::array<float,6> Mouth() const;

    private:
        void Flush(const std::wstring& chunk);
        void Place(const std::wstring& chunk,VoiceClip* clip);

        std::function<int(const std::wstring&)> voice;
        struct Waiting {int id=0;std::wstring text;bool done=false;std::unique_ptr<VoiceClip> clip;};
        std::vector<Waiting> waiting;                      // phrases being voiced, in order
        struct Sound {std::string wav;double start=0,end=0;bool started=false;};
        std::vector<Sound> sounds;                         // voiced phrases, played at their time
        // Its own output (not PlaySound, which the whole program shares): two characters can
        // speak at once, and one stopping does not cut the other off.
        struct Player;
        std::unique_ptr<Player> player;

        struct Chunk {size_t chars=0;double start=0,end=0;};
        std::vector<Chunk> chunks;
        std::vector<MotionTraceEditor::LipSyncCore::LyricEvent> events;
        std::array<std::vector<std::pair<float,float>>,6> curves;   // per mouth shape: (frame, value)
        std::wstring pending;
        double clock=0;      // seconds since the reply started
        int nextFrame=0;     // where the next syllable goes, in 30 fps frames
        bool dirty=false,finished=false;
    };
}
