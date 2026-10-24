#include "Text.hpp"
#include <windows.h>

namespace MikuMikuDesktopPerch
{
    namespace
    {
        bool english=false;
    }

    void SetLanguage(const std::wstring& setting)
    {
        if(setting==L"ja")english=false;
        else if(setting==L"en")english=true;
        else english=PRIMARYLANGID(GetUserDefaultUILanguage())!=LANG_JAPANESE;
    }

    bool English(){return english;}

    std::wstring Fmt(std::wstring pattern,std::initializer_list<std::wstring> args)
    {
        size_t index=0;
        for(const auto& arg:args)
        {
            const std::wstring key=L"{"+std::to_wstring(index++)+L"}";
            for(size_t at=pattern.find(key);at!=std::wstring::npos;at=pattern.find(key,at+arg.size()))pattern.replace(at,key.size(),arg);
        }
        return pattern;
    }
}
