#pragma once
#include <windows.h>
#include <array>
#include <map>
#include <random>
#include <string>
#include <vector>
#include "Speech.hpp"

namespace MikuMikuDesktopPerch
{
    // A text file read whatever its encoding: UTF-8 or UTF-16 (with or without a BOM) or
    // Shift-JIS. Returns an explanation on failure (empty on success); encoding: which it was.
    std::wstring ReadAnyText(const std::wstring& path,std::wstring& text,std::wstring* encoding=nullptr);

    // A line to say: its words (empty: the voice alone, no bubble) and a sound file of its own
    // (a full path; empty: none).
    struct GameLine
    {
        std::wstring text,voice;
        bool own=false;   // from the user's lines file
    };

    // A mini game's lines by scene (docs/GameLines.md). The built-in drafts (res/<game>/lines.txt:
    // "[scene] about" then a line each) are said again in the character's own words by the LLM,
    // a scene at a time, and kept per character. The user's own lines file (JSON, in the app's
    // lines folder) may give a scene lines of its own, said as written (with a voice file,
    // played in place of the voice engine), and how often it is said.
    class GameLines
    {
    public:
        struct Scene
        {
            std::wstring name,about;
            std::vector<std::wstring> draft,styled;
            bool asked=false;
            std::vector<GameLine> own;
            double chance=-1;   // from the user's file (-1: the game's own)
        };
        // How the LLM is asked to say the drafts again: the game's name and a rule of its own
        // (Japanese and English; the rule may be empty).
        struct Asking {std::wstring gameJa,gameEn,ruleJa,ruleEn;};

        // resource: the drafts' resource name (type LINES); game: the key of the game in the
        // user's file; ownFile: that file (empty or missing: none); stylesFile, styleKey: where
        // and under what her styled lines are kept (empty file: not kept).
        void Load(const wchar_t* resource,const std::wstring& game,const std::wstring& ownFile,
                  const std::wstring& stylesFile,const std::wstring& styleKey,bool english,const Asking& asking);

        // A line of the scene: the user's own, else in her own words when there are, else the
        // draft (none in English until they come); not one of the recent few if it can be helped.
        bool Pick(const wchar_t* scene,const std::vector<std::wstring>& recent,std::mt19937& random,GameLine& out) const;
        // Whether to speak in the scene now: the user's chance for it, else the game's.
        bool Roll(const wchar_t* scene,double chance,std::mt19937& random) const;

        // The next scene to put in her own words (no own lines, not yet asked): the ask for the
        // LLM, empty when none is left. Answered(answer) takes the reply.
        std::wstring NextAsk();
        void Answered(const std::wstring& answer);

        // What was read (for the diagnostics' report): the user's file, and what went wrong.
        const std::wstring& Report() const {return report;}
        // What went wrong with the user's file, for them (empty: nothing).
        const std::wstring& Problem() const {return problem;}
        bool Empty() const {return scenes.empty();}

    private:
        Scene* Named(const std::wstring& name);
        const Scene* Named(const std::wstring& name) const;
        std::wstring Key() const;
        void LoadStyled();
        void SaveStyled();
        void LoadOwn(const std::wstring& game,const std::wstring& file);

        std::vector<Scene> scenes;
        std::wstring stylesFile,styleKey,report,problem;
        bool english=false;
        Asking asking;
        int askingFor=-1;
    };

    // A message box telling the user what is wrong with their lines file (nothing when all is well).
    void ShowLinesProblem(HWND owner,const GameLines& lines);

    // A voice file of a line played, with the mouth following it and the words revealed over it.
    class GameVoice
    {
    public:
        // False (and nothing played) when the file cannot be read; why in error.
        bool Play(const GameLine& line,std::wstring& error);
        void Stop(){speech.Reset();playing=false;}
        void Tick(double dt){if(playing){speech.Tick(dt);if(!speech.Active())playing=false;}}
        bool Playing() const {return playing;}
        double Seconds() const {return seconds;}             // the sound's length
        double Revealed() const {return speech.RevealedChars();}
        std::array<float,6> Mouth() const {return speech.Mouth();}

    private:
        Speech speech;
        bool playing=false;
        double seconds=0;
        std::map<std::wstring,std::string> decoded;           // per file: as a 16-bit PCM WAV
    };
}
