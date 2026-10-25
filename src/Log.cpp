#include "Log.hpp"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>

namespace MikuMikuDesktopPerch
{
    namespace
    {
        std::mutex lock;
        std::wstring path;
        bool full=false;
    }

    void OpenLog()
    {
        wchar_t base[MAX_PATH]{};
        if(!GetEnvironmentVariableW(L"LOCALAPPDATA",base,MAX_PATH))return;
        const std::filesystem::path folder=std::filesystem::path(base)/L"MikuMikuDesktopPerch";
        std::error_code ec;
        std::filesystem::create_directories(folder,ec);
        const std::filesystem::path file=folder/L"log.txt";
        if(std::filesystem::exists(file,ec))std::filesystem::rename(file,folder/L"log.old.txt",ec);
        std::lock_guard guard(lock);
        path=file.wstring();
        full=false;
    }

    void Log(const wchar_t* format,...)
    {
        wchar_t text[1024]{};
        va_list args;
        va_start(args,format);
        _vsnwprintf_s(text,_TRUNCATE,format,args);
        va_end(args);
        SYSTEMTIME now{};
        GetLocalTime(&now);
        std::lock_guard guard(lock);
        if(path.empty()||full)return;
        FILE* f=nullptr;
        if(_wfopen_s(&f,path.c_str(),L"a, ccs=UTF-8")!=0||!f)return;
        fseek(f,0,SEEK_END);
        if(ftell(f)>=LogLimitBytes)
        {
            fwprintf(f,L"(the log is full: nothing more is written this run)\n");
            full=true;
        }
        else fwprintf(f,L"%02u:%02u:%02u %s\n",now.wHour,now.wMinute,now.wSecond,text);
        fclose(f);
    }
}
