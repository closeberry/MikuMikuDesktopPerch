#pragma once
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch
{
    // What belongs to one character on the desktop: its model, its motions, its place and its
    // voice. Character 1 is kept in the [Mascot] section (where a single
    // mascot's settings always were), characters 2 to 4 in [Mascot2] .. [Mascot4].
    struct CharacterSettings
    {
        std::wstring modelPath;
        std::wstring motionPath;      // played once when opened, then now and then
        // Played while idle, one at a time in random order (empty: the generated standing
        // pose). Stored as one IdleMotion line, paths separated by '|'.
        std::vector<std::wstring> idleMotionPaths;
        // Played on a click while standing, one at random (empty: built-in reactions). Stored
        // like IdleMotion, as the ClickMotion line.
        std::vector<std::wstring> clickMotionPaths;
        // Walking and running in place (looped, made to be moved by 全ての親) to use instead of
        // the generated gait; empty: generated.
        std::wstring walkMotionPath,runMotionPath;
        int x=-1;                   // -1: place at the bottom right of the work area
        int voiceSpeaker=3;
        std::wstring voiceName;     // the voice's name as the engine gave it (for the settings window)
    };

    // MikuMikuDesktopPerch.ini next to the executable (UTF-16LE so Japanese paths survive).
    struct Settings
    {
        static constexpr int MaxCharacters=4;
        // At least one; the first is the one the app started with.
        std::vector<CharacterSettings> characters{CharacterSettings{}};
        std::wstring toonDir;       // folder holding toon01.bmp..toon10.bmp (MMD's Data folder)
        static constexpr int MinScale=50,MaxScale=600;
        int windowHeight=300;       // logical pixels at 96 DPI, at 100% scale
        int scale=100;              // percent, 10% steps from the wheel; the same for every character
        int fps=30;
        bool motionBlur=false;      // smear what moves, so low frame rates look smoother
        bool physics=true;
        int physicsFilter=3;        // smoothing of the simulated bones' trembling: 0 off, 1 weak, 2 medium, 3 strong
        int physicsFps=60;
        int physicsIterations=10;   // solver passes per physics step (more: long chains jitter less)
        int msaa=4;
        bool topmost=true;
        float edgeScale=1.0f;
        bool selfShadow=true;       // the model shades itself from the light (MMD's self-shadow)
        // Where the light (the sun) is, in degrees: around (0 in front of her, + to the
        // viewer's right, ±180 behind) and up from the horizon. The default: low in front, a
        // little to the right, so the shading falls softly across the body.
        int lightAzimuth=12,lightElevation=25;
        int maxTextureSize=1024;   // larger textures are downscaled at load (0 = keep original)
        bool blink=true;
        bool breath=true;
        bool lookAtMouse=true;
        bool autoBehavior=true;     // walk around on its own
        bool windowMode=false;      // go to the active window, sit or hang there
        // Talking: a local LLM server's OpenAI-compatible endpoint (Ollama's by default) and the
        // model it should use (empty until chosen).
        std::wstring chatEndpoint=L"http://127.0.0.1:11434/v1";
        std::wstring chatModel;
        std::wstring userName;      // what the mascot calls the user (empty: no name)
        bool chatMemory=true;       // remember things from conversations (per model, on this computer)
        bool chatInitiative=true;   // greet, welcome back and now and then say something first
        // With chatInitiative: about how often (minutes, varied by a third either way) one says a
        // word of her own, and two characters talk with each other (0: never).
        int chatterMinutes=45;
        int banterMinutes=30;
        bool updateCheck=true;      // look for a newer release on GitHub now and then
        std::wstring language;      // "ja", "en", or empty: Windows' display language (see Text.hpp)
        // Speaking aloud through a VOICEVOX-compatible engine on this computer, in one of its voices.
        bool voice=false;
        bool voiceDirect=false;     // VOICEVOX's files used directly (voiceFolder), not its engine
        std::wstring voiceFolder;   // where VOICEVOX is installed
        std::wstring voiceEndpoint=L"http://127.0.0.1:50021";
        std::wstring lastGreeting;  // the last time-of-day greeting (date and slot), so a restart does not repeat it

        static std::wstring DefaultPath();
        void Load(const std::wstring& path);
        void Save(const std::wstring& path) const;
    };

    // A file of the current model's character in %LOCALAPPDATA%\MikuMikuDesktopPerch\characters
    // (the profile is <name>.ini); create makes the folder. Empty when there is no such folder.
    std::wstring CharacterDataFile(const std::wstring& modelPath,const wchar_t* extension,bool create);

    // Who the mascot is when talking, kept per model (a model is its own character) in
    // %LOCALAPPDATA%\MikuMikuDesktopPerch\characters, not in the shared settings file.
    struct CharacterProfile
    {
        std::wstring name;      // what it is called (empty: the model's name)
        std::wstring persona;   // personality and way of speaking, written by the user
        static std::wstring DefaultPersona();
        // modelName (the name inside the PMX) is the name until one is written.
        static CharacterProfile Load(const std::wstring& modelPath,const std::wstring& modelName);
        bool Save(const std::wstring& modelPath) const;
    };
}
