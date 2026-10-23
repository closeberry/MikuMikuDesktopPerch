#pragma once
#include <array>
#include <string>
#include <vector>
#include "LipSyncCore.hpp"

namespace MikuMikuDesktopPerch
{
    // The reading of Japanese text in hiragana, through the Windows Japanese IME's conversion
    // engine (MSIME.Japan). Where that is unavailable, kana are kept and every other character
    // becomes a guess of two syllables, which is enough to move a mouth.
    std::wstring HiraganaReading(const std::wstring& text);

    // A reply spoken aloud, without sound: the text is read into syllables (morae) as it
    // streams in, the syllables are placed on a timeline at a talking pace with pauses at
    // punctuation, and MotionTraceEditor's lip-sync turns them into mouth shapes. The bubble
    // shows the text as far as it has been "said".
    class Speech
    {
    public:
        void Reset();
        void Add(const std::wstring& text);   // more of the reply (as shown in the bubble)
        void Finish();                        // the reply is complete
        void Tick(double dt);

        bool Active() const {return !events.empty()||!pending.empty();}
        bool Speaking() const;                // still moving the mouth
        double RevealedChars() const;         // how much of the text has been said
        // Weights for the あ い う え お ん morphs now.
        std::array<float,6> Mouth() const;

    private:
        void Flush(const std::wstring& chunk);

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
