#pragma once
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch
{
    // MikuMikuDesktopPerch.ini next to the executable (UTF-16LE so Japanese paths survive).
    struct Settings
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
        std::wstring toonDir;       // folder holding toon01.bmp..toon10.bmp (MMD's Data folder)
        static constexpr int MinScale=50,MaxScale=200;
        int windowHeight=300;       // logical pixels at 96 DPI, at 100% scale
        int scale=100;              // percent, 10% steps from the menu
        int x=-1,y=-1;              // -1: place at bottom-right of the work area
        int fps=30;
        bool physics=true;
        int physicsFps=60;
        int msaa=4;
        bool topmost=true;
        float edgeScale=1.0f;
        bool selfShadow=true;       // the model shades itself from the light (MMD's self-shadow)
        // Where the light (the sun) is, in degrees: around (0 in front of her, + to the
        // viewer's right, ±180 behind) and up from the horizon. The default is MMD's light.
        int lightAzimuth=45,lightElevation=55;
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
        // Speaking aloud through a VOICEVOX-compatible engine on this computer, in one of its voices.
        bool voice=false;
        std::wstring voiceEndpoint=L"http://127.0.0.1:50021";
        int voiceSpeaker=3;
        std::wstring voiceName;     // the voice's name as the engine gave it (for the settings window)
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
