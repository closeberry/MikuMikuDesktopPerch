#define NOMINMAX
#include "Memory.hpp"
#include "Settings.hpp"
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace MikuMikuDesktopPerch
{
    namespace
    {
        constexpr wchar_t Extension[]=L".memory.txt";
        constexpr std::uintmax_t MaxFileSize=1u<<20;   // a hand-edited or damaged file is not read whole

        std::wstring FromUtf8(const std::string& text)
        {
            if(text.empty())return {};
            const int n=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
            std::wstring out(static_cast<size_t>(n),L'\0');
            MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),out.data(),n);
            return out;
        }

        std::string ToUtf8(const std::wstring& text)
        {
            if(text.empty())return {};
            const int n=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
            std::string out(static_cast<size_t>(n),'\0');
            WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),out.data(),n,nullptr,nullptr);
            return out;
        }

        // One line, no tabs, trimmed, at most MaxLength characters.
        std::wstring Tidy(std::wstring text)
        {
            for(auto& c:text)if(c==L'\t'||c==L'\r'||c==L'\n')c=L' ';
            const size_t a=text.find_first_not_of(L" 　"),b=text.find_last_not_of(L" 　");
            text=a==std::wstring::npos?std::wstring():text.substr(a,b-a+1);
            if(text.size()>CharacterMemory::MaxLength)text.resize(CharacterMemory::MaxLength);
            return text;
        }

        bool IsDate(const std::wstring& d)
        {
            return d.size()==10&&d[4]==L'-'&&d[7]==L'-'&&
                   std::all_of(d.begin(),d.end(),[](wchar_t c){return c==L'-'||(c>=L'0'&&c<=L'9');});
        }

        // Pairs of neighbouring characters, ignoring spaces and punctuation: Japanese has no
        // spaces between words, and shared pairs are a fair measure of a shared topic.
        std::set<std::wstring> Bigrams(const std::wstring& text)
        {
            std::wstring letters;
            for(const wchar_t c:text)
                if(!std::iswspace(c)&&!std::iswpunct(c)&&std::wstring(L"、。！？「」『』（）・ー〜…").find(c)==std::wstring::npos)
                    letters+=c;
            std::set<std::wstring> out;
            for(size_t i=0;i+1<letters.size();++i)out.insert(letters.substr(i,2));
            return out;
        }
    }

    void SavePendingExchanges(const std::wstring& modelPath,const std::vector<std::pair<bool,std::wstring>>& exchanges)
    {
        const std::wstring file=CharacterDataFile(modelPath,L".pending.txt",!exchanges.empty());
        if(file.empty())return;
        if(exchanges.empty()){DeleteFileW(file.c_str());return;}
        std::string data;
        for(const auto& [user,text]:exchanges)
        {
            std::wstring line=text;
            for(auto& c:line)if(c==L'\t'||c==L'\r'||c==L'\n')c=L' ';
            data+=(user?"U\t":"A\t")+ToUtf8(line)+"\n";
        }
        std::ofstream out(std::filesystem::path(file),std::ios::binary|std::ios::trunc);
        out.write(data.data(),static_cast<std::streamsize>(data.size()));
    }

    std::vector<std::pair<bool,std::wstring>> TakePendingExchanges(const std::wstring& modelPath)
    {
        std::vector<std::pair<bool,std::wstring>> out;
        const std::wstring file=CharacterDataFile(modelPath,L".pending.txt",false);
        std::error_code ec;
        if(file.empty()||!std::filesystem::is_regular_file(file,ec))return out;
        if(std::filesystem::file_size(file,ec)<=MaxFileSize)
        {
            std::ifstream in(std::filesystem::path(file),std::ios::binary);
            std::string line;
            while(std::getline(in,line)&&out.size()<48)
                if(line.size()>2&&(line[0]=='U'||line[0]=='A')&&line[1]=='\t')
                    out.push_back({line[0]=='U',FromUtf8(line.substr(2))});
        }
        DeleteFileW(file.c_str());
        return out;
    }

    std::wstring TodayDate()
    {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t text[16]{};
        swprintf_s(text,L"%04d-%02d-%02d",now.wYear,now.wMonth,now.wDay);
        return text;
    }

    void CharacterMemory::Load(const std::wstring& path)
    {
        modelPath=path;
        entries.clear();
        const std::wstring file=CharacterDataFile(modelPath,Extension,false);
        std::error_code ec;
        if(file.empty()||!std::filesystem::is_regular_file(file,ec)||std::filesystem::file_size(file,ec)>MaxFileSize)return;
        std::ifstream in(std::filesystem::path(file),std::ios::binary);
        std::string line;
        while(std::getline(in,line))
        {
            if(line.size()>=3&&static_cast<unsigned char>(line[0])==0xEF&&static_cast<unsigned char>(line[1])==0xBB&&static_cast<unsigned char>(line[2])==0xBF)
                line.erase(0,3);
            const std::wstring w=FromUtf8(line);
            const size_t tab=w.find(L'\t');
            MemoryEntry e;
            e.date=tab==std::wstring::npos?std::wstring():w.substr(0,tab);
            e.text=Tidy(tab==std::wstring::npos?w:w.substr(tab+1));
            if(!IsDate(e.date))e.date.clear();
            if(e.text.empty())continue;
            e.id=nextId++;
            entries.push_back(std::move(e));
        }
        if(entries.size()>MaxEntries)entries.erase(entries.begin(),entries.end()-MaxEntries);
    }

    bool CharacterMemory::Save() const
    {
        if(modelPath.empty())return false;
        const std::wstring file=CharacterDataFile(modelPath,Extension,true);
        if(file.empty())return false;
        std::string data;
        for(const auto& e:entries)data+=ToUtf8(e.date)+"\t"+ToUtf8(e.text)+"\n";
        // Written beside the file and moved over it, so a crash never leaves half a file.
        const std::wstring temp=file+L".tmp";
        {
            std::ofstream out(std::filesystem::path(temp),std::ios::binary|std::ios::trunc);
            if(!out)return false;
            out.write(data.data(),static_cast<std::streamsize>(data.size()));
            if(!out)return false;
        }
        return MoveFileExW(temp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
    }

    bool CharacterMemory::Add(std::wstring text,const std::wstring& date)
    {
        text=Tidy(std::move(text));
        if(text.empty())return false;
        if(std::any_of(entries.begin(),entries.end(),[&](const MemoryEntry& e){return e.text==text;}))return false;
        entries.push_back({nextId++,IsDate(date)?date:TodayDate(),text});
        if(entries.size()>MaxEntries)entries.erase(entries.begin());
        return true;
    }

    void CharacterMemory::Remove(int id)
    {
        entries.erase(std::remove_if(entries.begin(),entries.end(),[&](const MemoryEntry& e){return e.id==id;}),entries.end());
    }

    void CharacterMemory::Clear()
    {
        entries.clear();
    }

    std::wstring CharacterMemory::Recall(const std::wstring& query,size_t maxChars) const
    {
        auto line=[](const MemoryEntry& e){
            std::wstring when;
            if(e.date.size()==10)when=std::to_wstring(std::stoi(e.date.substr(0,4)))+L"年"+
                                      std::to_wstring(std::stoi(e.date.substr(5,2)))+L"月"+
                                      std::to_wstring(std::stoi(e.date.substr(8,2)))+L"日：";
            return L"- "+when+e.text+L"\n";
        };
        size_t total=0;
        for(const auto& e:entries)total+=line(e).size();
        std::vector<size_t> chosen;
        if(total<=maxChars)
        {
            for(size_t i=0;i<entries.size();++i)chosen.push_back(i);
        }
        else
        {
            // Most shared word pairs first; ties (and nothing shared) go to the newer note.
            const auto words=Bigrams(query);
            std::vector<std::pair<int,size_t>> ranked;
            for(size_t i=0;i<entries.size();++i)
            {
                int shared=0;
                for(const auto& b:Bigrams(entries[i].text))shared+=words.count(b)?1:0;
                ranked.push_back({shared,i});
            }
            std::sort(ranked.begin(),ranked.end(),[](const auto& a,const auto& b){return a.first!=b.first?a.first>b.first:a.second>b.second;});
            size_t used=0;
            for(const auto& [shared,i]:ranked)
            {
                const size_t n=line(entries[i]).size();
                if(used+n>maxChars)continue;
                used+=n;
                chosen.push_back(i);
            }
            std::sort(chosen.begin(),chosen.end());
        }
        std::wstring out;
        for(const size_t i:chosen)out+=line(entries[i]);
        return out;
    }
}
