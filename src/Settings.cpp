#define NOMINMAX
#include "Settings.hpp"
#include "Text.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cwctype>

namespace MikuMikuDesktopPerch
{
    namespace
    {
        constexpr wchar_t Section[]=L"Mascot";

        // Paths separated by '|' (never part of a Windows path).
        std::vector<std::wstring> ReadList(const std::wstring& path,const wchar_t* key,const wchar_t* section=Section);
        void WriteList(const std::wstring& path,const wchar_t* key,const std::vector<std::wstring>& list,const wchar_t* section=Section);

        std::wstring ReadString(const std::wstring& path,const wchar_t* key,const std::wstring& fallback,const wchar_t* section=Section)
        {
            // Room for a long IdleMotion list.
            std::vector<wchar_t> buffer(32768);
            GetPrivateProfileStringW(section,key,fallback.c_str(),buffer.data(),static_cast<DWORD>(buffer.size()),path.c_str());
            return buffer.data();
        }
        int ReadInt(const std::wstring& path,const wchar_t* key,int fallback,const wchar_t* section=Section)
        {
            return static_cast<int>(GetPrivateProfileIntW(section,key,fallback,path.c_str()));
        }
        void WriteString(const std::wstring& path,const wchar_t* key,const std::wstring& value,const wchar_t* section=Section)
        {
            WritePrivateProfileStringW(section,key,value.c_str(),path.c_str());
        }

        // Character 1 in [Mascot], the others in [Mascot2] .. [Mascot4].
        std::wstring CharacterSection(size_t index)
        {
            return index==0?std::wstring(Section):std::wstring(Section)+std::to_wstring(index+1);
        }
    }

    std::wstring Settings::DefaultPath()
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr,exe,MAX_PATH);
        const std::filesystem::path dir=std::filesystem::path(exe).parent_path();
        const std::filesystem::path path=dir/L"MikuMikuDesktopPerch.ini";
        // Settings saved under the app's former name (DesktopMascot) carry over once.
        std::error_code ignored;
        if(!std::filesystem::exists(path,ignored)&&std::filesystem::exists(dir/L"DesktopMascot.ini",ignored))
            std::filesystem::rename(dir/L"DesktopMascot.ini",path,ignored);
        return path.wstring();
    }

    namespace
    {
        std::vector<std::wstring> ReadList(const std::wstring& path,const wchar_t* key,const wchar_t* section)
        {
            const std::wstring list=ReadString(path,key,{},section);
            std::vector<std::wstring> paths;
            for(size_t start=0;start<=list.size();)
            {
                const size_t end=std::min(list.find(L'|',start),list.size());
                if(end>start)paths.push_back(list.substr(start,end-start));
                start=end+1;
            }
            return paths;
        }

        void WriteList(const std::wstring& path,const wchar_t* key,const std::vector<std::wstring>& list,const wchar_t* section)
        {
            std::wstring joined;
            for(const auto& p:list)joined+=(joined.empty()?L"":L"|")+p;
            WriteString(path,key,joined,section);
        }

        void LoadCharacter(const std::wstring& path,const wchar_t* section,CharacterSettings& c)
        {
            c.modelPath=ReadString(path,L"Model",c.modelPath,section);
            c.motionPath=ReadString(path,L"Motion",c.motionPath,section);
            c.idleMotionPaths=ReadList(path,L"IdleMotion",section);
            c.clickMotionPaths=ReadList(path,L"ClickMotion",section);
            c.walkMotionPath=ReadString(path,L"WalkMotion",c.walkMotionPath,section);
            c.runMotionPath=ReadString(path,L"RunMotion",c.runMotionPath,section);
            c.x=ReadInt(path,L"X",c.x,section);
            c.voiceSpeaker=ReadInt(path,L"VoiceSpeaker",c.voiceSpeaker,section);
            c.voiceName=ReadString(path,L"VoiceName",c.voiceName,section);
        }

        void SaveCharacter(const std::wstring& path,const wchar_t* section,const CharacterSettings& c)
        {
            WriteString(path,L"Model",c.modelPath,section);
            WriteString(path,L"Motion",c.motionPath,section);
            WriteList(path,L"IdleMotion",c.idleMotionPaths,section);
            WriteList(path,L"ClickMotion",c.clickMotionPaths,section);
            WriteString(path,L"WalkMotion",c.walkMotionPath,section);
            WriteString(path,L"RunMotion",c.runMotionPath,section);
            WriteString(path,L"X",std::to_wstring(c.x),section);
            WriteString(path,L"VoiceSpeaker",std::to_wstring(c.voiceSpeaker),section);
            WriteString(path,L"VoiceName",c.voiceName,section);
        }
    }

    void Settings::Load(const std::wstring& path)
    {
        // Characters 2 to 4 go as far as the first without a model.
        characters.assign(1,CharacterSettings{});
        LoadCharacter(path,Section,characters[0]);
        for(size_t i=1;i<MaxCharacters;++i)
        {
            CharacterSettings c;
            LoadCharacter(path,CharacterSection(i).c_str(),c);
            if(c.modelPath.empty())break;
            characters.push_back(std::move(c));
        }
        toonDir=ReadString(path,L"ToonDir",toonDir);
        windowHeight=std::clamp(ReadInt(path,L"Height",windowHeight),100,4000);
        scale=std::clamp(ReadInt(path,L"Scale",scale),MinScale,MaxScale);
        // Version 1 files were written when the default size was twice today's: halve them once.
        if(ReadInt(path,L"SizeVersion",1)<2&&GetPrivateProfileIntW(Section,L"Height",-1,path.c_str())>0)
            windowHeight=std::max(windowHeight/2,100);
        fps=std::clamp(ReadInt(path,L"Fps",fps),5,120);
        motionBlur=ReadInt(path,L"MotionBlur",motionBlur?1:0)!=0;
        physics=ReadInt(path,L"Physics",physics?1:0)!=0;
        physicsFilter=std::clamp(ReadInt(path,L"PhysicsFilter",physicsFilter),0,3);
        physicsFps=std::clamp(ReadInt(path,L"PhysicsFps",physicsFps),30,240);
        physicsIterations=std::clamp(ReadInt(path,L"PhysicsIterations",physicsIterations),1,100);
        msaa=ReadInt(path,L"Msaa",msaa);
        if(msaa!=1&&msaa!=2&&msaa!=4&&msaa!=8)msaa=4;
        topmost=ReadInt(path,L"Topmost",topmost?1:0)!=0;
        maxTextureSize=std::clamp(ReadInt(path,L"MaxTextureSize",maxTextureSize),0,16384);
        blink=ReadInt(path,L"Blink",blink?1:0)!=0;
        breath=ReadInt(path,L"Breath",breath?1:0)!=0;
        lookAtMouse=ReadInt(path,L"LookAtMouse",lookAtMouse?1:0)!=0;
        selfShadow=ReadInt(path,L"SelfShadow",selfShadow?1:0)!=0;
        lightAzimuth=std::clamp(ReadInt(path,L"LightAzimuth",lightAzimuth),-180,180);
        lightElevation=std::clamp(ReadInt(path,L"LightElevation",lightElevation),5,90);
        autoBehavior=ReadInt(path,L"AutoBehavior",autoBehavior?1:0)!=0;
        windowMode=ReadInt(path,L"WindowMode",windowMode?1:0)!=0;
        chatEndpoint=ReadString(path,L"ChatEndpoint",chatEndpoint);
        chatModel=ReadString(path,L"ChatModel",chatModel);
        userName=ReadString(path,L"UserName",userName);
        chatMemory=ReadInt(path,L"ChatMemory",chatMemory?1:0)!=0;
        chatInitiative=ReadInt(path,L"ChatInitiative",chatInitiative?1:0)!=0;
        chatterMinutes=std::clamp(ReadInt(path,L"ChatterMinutes",chatterMinutes),5,600);
        banterMinutes=std::clamp(ReadInt(path,L"BanterMinutes",banterMinutes),0,600);
        updateCheck=ReadInt(path,L"UpdateCheck",updateCheck?1:0)!=0;
        language=ReadString(path,L"Language",language);
        if(language!=L"ja"&&language!=L"en")language.clear();
        voice=ReadInt(path,L"Voice",voice?1:0)!=0;
        voiceEndpoint=ReadString(path,L"VoiceEndpoint",voiceEndpoint);
        voiceDirect=ReadInt(path,L"VoiceDirect",voiceDirect?1:0)!=0;
        voiceFolder=ReadString(path,L"VoiceFolder",voiceFolder);
        lastGreeting=ReadString(path,L"LastGreeting",lastGreeting);
        edgeScale=std::clamp(static_cast<float>(_wtof(ReadString(path,L"EdgeScale",std::to_wstring(edgeScale)).c_str())),0.0f,10.0f);
    }

    void Settings::Save(const std::wstring& path) const
    {
        // Seed an empty UTF-16LE file so the profile API keeps Unicode text.
        if(!std::filesystem::exists(path))
        {
            std::ofstream file(std::filesystem::path(path),std::ios::binary);
            const unsigned char bom[]={0xFF,0xFE};
            file.write(reinterpret_cast<const char*>(bom),sizeof(bom));
        }
        for(size_t i=0;i<MaxCharacters;++i)
        {
            const std::wstring section=CharacterSection(i);
            if(i<characters.size())SaveCharacter(path,section.c_str(),characters[i]);
            else WritePrivateProfileStringW(section.c_str(),nullptr,nullptr,path.c_str());   // one taken off
        }
        WriteString(path,L"ToonDir",toonDir);
        WriteString(path,L"Height",std::to_wstring(windowHeight));
        WriteString(path,L"SizeVersion",L"2");
        WriteString(path,L"Scale",std::to_wstring(scale));
        WriteString(path,L"Fps",std::to_wstring(fps));
        WriteString(path,L"MotionBlur",motionBlur?L"1":L"0");
        WriteString(path,L"Physics",physics?L"1":L"0");
        WriteString(path,L"PhysicsFilter",std::to_wstring(physicsFilter));
        WriteString(path,L"PhysicsFps",std::to_wstring(physicsFps));
        WriteString(path,L"PhysicsIterations",std::to_wstring(physicsIterations));
        WriteString(path,L"Msaa",std::to_wstring(msaa));
        WriteString(path,L"Topmost",topmost?L"1":L"0");
        WriteString(path,L"MaxTextureSize",std::to_wstring(maxTextureSize));
        WriteString(path,L"Blink",blink?L"1":L"0");
        WriteString(path,L"Breath",breath?L"1":L"0");
        WriteString(path,L"LookAtMouse",lookAtMouse?L"1":L"0");
        WriteString(path,L"SelfShadow",selfShadow?L"1":L"0");
        WriteString(path,L"LightAzimuth",std::to_wstring(lightAzimuth));
        WriteString(path,L"LightElevation",std::to_wstring(lightElevation));
        WriteString(path,L"AutoBehavior",autoBehavior?L"1":L"0");
        WriteString(path,L"WindowMode",windowMode?L"1":L"0");
        WriteString(path,L"ChatEndpoint",chatEndpoint);
        WriteString(path,L"ChatModel",chatModel);
        WriteString(path,L"UserName",userName);
        WriteString(path,L"ChatMemory",chatMemory?L"1":L"0");
        WriteString(path,L"ChatInitiative",chatInitiative?L"1":L"0");
        WriteString(path,L"ChatterMinutes",std::to_wstring(chatterMinutes));
        WriteString(path,L"BanterMinutes",std::to_wstring(banterMinutes));
        WriteString(path,L"UpdateCheck",updateCheck?L"1":L"0");
        WriteString(path,L"Language",language);
        WriteString(path,L"Voice",voice?L"1":L"0");
        WriteString(path,L"VoiceEndpoint",voiceEndpoint);
        WriteString(path,L"VoiceDirect",voiceDirect?L"1":L"0");
        WriteString(path,L"VoiceFolder",voiceFolder);
        WriteString(path,L"LastGreeting",lastGreeting);
        wchar_t edge[32]{};
        swprintf_s(edge,L"%.2f",edgeScale);
        WriteString(path,L"EdgeScale",edge);
    }

    // characters\<model file name>-<hash of its full path><extension>: two copies of a model in
    // different folders are different characters, and the file name says which model it is.
    std::wstring CharacterDataFile(const std::wstring& modelPath,const wchar_t* extension,bool create)
    {
        wchar_t base[MAX_PATH]{};
        if(!GetEnvironmentVariableW(L"LOCALAPPDATA",base,MAX_PATH))return {};
        std::wstring key=modelPath;
        std::transform(key.begin(),key.end(),key.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});
        unsigned long long hash=1469598103934665603ull;
        for(const wchar_t c:key){hash^=static_cast<unsigned long long>(c);hash*=1099511628211ull;}
        wchar_t hex[20]{};
        swprintf_s(hex,L"%016llx",hash);
        const std::filesystem::path folder=std::filesystem::path(base)/L"MikuMikuDesktopPerch"/L"characters";
        if(create){std::error_code ec;std::filesystem::create_directories(folder,ec);}
        return (folder/(std::filesystem::path(modelPath).stem().wstring()+L"-"+hex+extension)).wstring();
    }

    namespace
    {
        std::wstring CharacterFile(const std::wstring& modelPath,bool create){return CharacterDataFile(modelPath,L".ini",create);}

        // One ini line holds the whole persona: line breaks and backslashes are escaped.
        std::wstring Escape(const std::wstring& text)
        {
            std::wstring out;
            for(const wchar_t c:text)
            {
                if(c==L'\\')out+=L"\\\\";
                else if(c==L'\n')out+=L"\\n";
                else if(c!=L'\r')out+=c;
            }
            return out;
        }

        std::wstring Unescape(const std::wstring& text)
        {
            std::wstring out;
            for(size_t i=0;i<text.size();++i)
            {
                if(text[i]==L'\\'&&i+1<text.size()){out+=text[i+1]==L'n'?L'\n':text[i+1];++i;}
                else out+=text[i];
            }
            return out;
        }
    }

    std::wstring CharacterProfile::DefaultPersona()
    {
        if(English())
            return L"A little mascot living on the desktop, chatting with whoever talks to her.\n"
                   L"Cheerful and friendly, she speaks casually and cares about the other person.";
        return L"デスクトップの上で暮らしている小さなマスコットとして、話しかけてくれた人とおしゃべりします。\n"
               L"明るく親しみやすい話し言葉で、相手のことを気にかけます。\n"
               L"一人称は「わたし」。";
    }

    CharacterProfile CharacterProfile::Load(const std::wstring& modelPath,const std::wstring& modelName)
    {
        CharacterProfile profile;
        const std::wstring file=CharacterFile(modelPath,false);
        if(!file.empty()&&std::filesystem::exists(file))
        {
            profile.name=ReadString(file,L"Name",{});
            profile.persona=Unescape(ReadString(file,L"Persona",{}));
        }
        if(profile.name.empty())profile.name=modelName.empty()?std::filesystem::path(modelPath).stem().wstring():modelName;
        if(profile.persona.empty())profile.persona=DefaultPersona();
        return profile;
    }

    bool CharacterProfile::Save(const std::wstring& modelPath) const
    {
        const std::wstring file=CharacterFile(modelPath,true);
        if(file.empty())return false;
        if(!std::filesystem::exists(file))
        {
            // UTF-16 with a BOM, so the profile APIs keep Japanese text intact.
            std::ofstream out(std::filesystem::path(file),std::ios::binary);
            const unsigned char bom[2]={0xFF,0xFE};
            out.write(reinterpret_cast<const char*>(bom),2);
        }
        WriteString(file,L"Name",name);
        WriteString(file,L"Persona",Escape(persona));
        return true;
    }
}
