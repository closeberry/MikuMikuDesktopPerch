#define NOMINMAX
#include "Settings.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <algorithm>

namespace MikuMikuDesktopPerch
{
    namespace
    {
        constexpr wchar_t Section[]=L"Mascot";

        // Paths separated by '|' (never part of a Windows path).
        std::vector<std::wstring> ReadList(const std::wstring& path,const wchar_t* key);
        void WriteList(const std::wstring& path,const wchar_t* key,const std::vector<std::wstring>& list);

        std::wstring ReadString(const std::wstring& path,const wchar_t* key,const std::wstring& fallback)
        {
            // Room for a long IdleMotion list.
            std::vector<wchar_t> buffer(32768);
            GetPrivateProfileStringW(Section,key,fallback.c_str(),buffer.data(),static_cast<DWORD>(buffer.size()),path.c_str());
            return buffer.data();
        }
        int ReadInt(const std::wstring& path,const wchar_t* key,int fallback)
        {
            return static_cast<int>(GetPrivateProfileIntW(Section,key,fallback,path.c_str()));
        }
        void WriteString(const std::wstring& path,const wchar_t* key,const std::wstring& value)
        {
            WritePrivateProfileStringW(Section,key,value.c_str(),path.c_str());
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
        std::vector<std::wstring> ReadList(const std::wstring& path,const wchar_t* key)
        {
            const std::wstring list=ReadString(path,key,{});
            std::vector<std::wstring> paths;
            for(size_t start=0;start<=list.size();)
            {
                const size_t end=std::min(list.find(L'|',start),list.size());
                if(end>start)paths.push_back(list.substr(start,end-start));
                start=end+1;
            }
            return paths;
        }

        void WriteList(const std::wstring& path,const wchar_t* key,const std::vector<std::wstring>& list)
        {
            std::wstring joined;
            for(const auto& p:list)joined+=(joined.empty()?L"":L"|")+p;
            WriteString(path,key,joined);
        }
    }

    void Settings::Load(const std::wstring& path)
    {
        modelPath=ReadString(path,L"Model",modelPath);
        motionPath=ReadString(path,L"Motion",motionPath);
        idleMotionPaths=ReadList(path,L"IdleMotion");
        clickMotionPaths=ReadList(path,L"ClickMotion");
        toonDir=ReadString(path,L"ToonDir",toonDir);
        windowHeight=std::clamp(ReadInt(path,L"Height",windowHeight),100,4000);
        // Version 1 files were written when the default size was twice today's: halve them once.
        if(ReadInt(path,L"SizeVersion",1)<2&&GetPrivateProfileIntW(Section,L"Height",-1,path.c_str())>0)
            windowHeight=std::max(windowHeight/2,100);
        scale=std::clamp(ReadInt(path,L"Scale",scale),MinScale,MaxScale);
        x=ReadInt(path,L"X",x);
        y=ReadInt(path,L"Y",y);
        fps=std::clamp(ReadInt(path,L"Fps",fps),5,120);
        physics=ReadInt(path,L"Physics",physics?1:0)!=0;
        physicsFps=std::clamp(ReadInt(path,L"PhysicsFps",physicsFps),30,240);
        msaa=ReadInt(path,L"Msaa",msaa);
        if(msaa!=1&&msaa!=2&&msaa!=4&&msaa!=8)msaa=4;
        topmost=ReadInt(path,L"Topmost",topmost?1:0)!=0;
        maxTextureSize=std::clamp(ReadInt(path,L"MaxTextureSize",maxTextureSize),0,16384);
        blink=ReadInt(path,L"Blink",blink?1:0)!=0;
        breath=ReadInt(path,L"Breath",breath?1:0)!=0;
        lookAtMouse=ReadInt(path,L"LookAtMouse",lookAtMouse?1:0)!=0;
        autoBehavior=ReadInt(path,L"AutoBehavior",autoBehavior?1:0)!=0;
        windowMode=ReadInt(path,L"WindowMode",windowMode?1:0)!=0;
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
        WriteString(path,L"Model",modelPath);
        WriteString(path,L"Motion",motionPath);
        WriteList(path,L"IdleMotion",idleMotionPaths);
        WriteList(path,L"ClickMotion",clickMotionPaths);
        WriteString(path,L"ToonDir",toonDir);
        WriteString(path,L"Height",std::to_wstring(windowHeight));
        WriteString(path,L"SizeVersion",L"2");
        WriteString(path,L"Scale",std::to_wstring(scale));
        WriteString(path,L"X",std::to_wstring(x));
        WriteString(path,L"Y",std::to_wstring(y));
        WriteString(path,L"Fps",std::to_wstring(fps));
        WriteString(path,L"Physics",physics?L"1":L"0");
        WriteString(path,L"PhysicsFps",std::to_wstring(physicsFps));
        WriteString(path,L"Msaa",std::to_wstring(msaa));
        WriteString(path,L"Topmost",topmost?L"1":L"0");
        WriteString(path,L"MaxTextureSize",std::to_wstring(maxTextureSize));
        WriteString(path,L"Blink",blink?L"1":L"0");
        WriteString(path,L"Breath",breath?L"1":L"0");
        WriteString(path,L"LookAtMouse",lookAtMouse?L"1":L"0");
        WriteString(path,L"AutoBehavior",autoBehavior?L"1":L"0");
        WriteString(path,L"WindowMode",windowMode?L"1":L"0");
        wchar_t edge[32]{};
        swprintf_s(edge,L"%.2f",edgeScale);
        WriteString(path,L"EdgeScale",edge);
    }
}
