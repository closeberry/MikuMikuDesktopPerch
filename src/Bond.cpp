#define NOMINMAX
#include "Bond.hpp"
#include "Memory.hpp"
#include "Settings.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#pragma comment(lib,"bcrypt.lib")
#pragma comment(lib,"crypt32.lib")

namespace MikuMikuDesktopPerch
{
    namespace
    {
        constexpr wchar_t Extension[]=L".bond.dat";
        constexpr char Header[]="MMDPBOND1";
        constexpr std::uintmax_t MaxFileSize=64*1024;
        constexpr double PresenceSeconds=10*60;

        // Not secrets: anyone reading this can rewrite the file. They only keep it from being
        // edited in a text editor.
        constexpr unsigned char Scramble[]={0x5A,0x1F,0xC3,0x77,0x08,0xE4,0x92,0x3D,0x6B,0xA0,0x4E,0xD1,0x29,0x87,0xF5,0x13,
                                            0xB8,0x61,0x0C,0x9F,0x46,0xEA,0x35,0x7C,0xD3,0x58,0xAF,0x02,0x91,0x6E,0xC7,0x24};
        constexpr char SignKey[]="MikuMikuDesktopPerch/bond/2026";

        struct Rule {BondAction action;const wchar_t* name;int each,cap;};
        constexpr Rule Rules[]={{BondAction::Present,L"present",10,10},{BondAction::Talk,L"talk",2,10},{BondAction::Click,L"click",1,5},
                                {BondAction::Focus,L"focus",3,12},{BondAction::Todo,L"todo",2,6},{BondAction::Dungeon,L"dungeon",3,9},
                                {BondAction::Mahjong,L"mahjong",3,9},{BondAction::Hanafuda,L"hanafuda",3,9},{BondAction::Othello,L"othello",3,9},{BondAction::Gomoku,L"gomoku",3,9}};

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

        std::string Sign(const std::string& data)
        {
            BCRYPT_ALG_HANDLE algorithm=nullptr;
            BCRYPT_HASH_HANDLE hash=nullptr;
            unsigned char digest[32]{};
            bool ok=false;
            if(BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,BCRYPT_ALG_HANDLE_HMAC_FLAG))&&
               BCRYPT_SUCCESS(BCryptCreateHash(algorithm,&hash,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<char*>(SignKey)),sizeof(SignKey)-1,0))&&
               BCRYPT_SUCCESS(BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),static_cast<ULONG>(data.size()),0))&&
               BCRYPT_SUCCESS(BCryptFinishHash(hash,digest,sizeof(digest),0)))
                ok=true;
            if(hash)BCryptDestroyHash(hash);
            if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);
            if(!ok)return {};
            std::string hex;
            for(const unsigned char b:digest){char two[3]{};sprintf_s(two,"%02x",b);hex+=two;}
            return hex;
        }

        void Mix(std::string& data)
        {
            for(size_t i=0;i<data.size();++i)data[i]=static_cast<char>(data[i]^Scramble[i%sizeof(Scramble)]^static_cast<unsigned char>(i*131));
        }

        std::string ToBase64(const std::string& data)
        {
            DWORD n=0;
            const auto bytes=reinterpret_cast<const BYTE*>(data.data());
            if(!CryptBinaryToStringA(bytes,static_cast<DWORD>(data.size()),CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,nullptr,&n))return {};
            std::string out(n,'\0');
            if(!CryptBinaryToStringA(bytes,static_cast<DWORD>(data.size()),CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,out.data(),&n))return {};
            out.resize(n);
            return out;
        }

        bool FromBase64(const std::string& text,std::string& data)
        {
            DWORD n=0;
            if(!CryptStringToBinaryA(text.data(),static_cast<DWORD>(text.size()),CRYPT_STRING_BASE64,nullptr,&n,nullptr,nullptr))return false;
            data.assign(n,'\0');
            if(!CryptStringToBinaryA(text.data(),static_cast<DWORD>(text.size()),CRYPT_STRING_BASE64,reinterpret_cast<BYTE*>(data.data()),&n,nullptr,nullptr))return false;
            data.resize(n);
            return true;
        }

        // The file's body ("key=value" lines), if it is whole and signed; empty otherwise.
        bool ReadSigned(const std::wstring& file,std::string& body)
        {
            std::error_code ec;
            if(!std::filesystem::is_regular_file(file,ec)||std::filesystem::file_size(file,ec)>MaxFileSize)return false;
            std::ifstream in(std::filesystem::path(file),std::ios::binary);
            std::string header,text,signature;
            std::getline(in,header);
            std::getline(in,text);
            std::getline(in,signature);
            for(auto* s:{&header,&text,&signature})if(!s->empty()&&s->back()=='\r')s->pop_back();
            if(header!=Header||text.empty()||signature.empty()||Sign(text)!=signature)return false;
            if(!FromBase64(text,body))return false;
            Mix(body);
            return true;
        }

        bool IsDate(const std::wstring& d)
        {
            return d.size()==10&&d[4]==L'-'&&d[7]==L'-'&&
                   std::all_of(d.begin(),d.end(),[](wchar_t c){return c==L'-'||(c>=L'0'&&c<=L'9');});
        }

        // The highest of 7, 30, 100, 200, ... (days) or 1000, 3000, 5000, 10000, 20000, ...
        // (points) reached, or 0.
        long long Milestone(long long value,std::initializer_list<long long> first,long long then)
        {
            long long reached=0;
            for(const long long m:first)if(value>=m)reached=m;
            if(value>=then)reached=value/then*then;
            return reached;
        }
    }

    void Bond::Load(const std::wstring& path,std::wstring& problem)
    {
        *this=Bond{};
        modelPath=path;
        const std::wstring file=CharacterDataFile(modelPath,Extension,false);
        if(file.empty())return;
        std::string body;
        std::error_code ec;
        const bool there=std::filesystem::exists(file,ec);
        if(!ReadSigned(file,body))
        {
            const bool backup=ReadSigned(file+L".bak",body);
            if(there||backup)problem=backup?L"the file could not be read: its backup is used":L"the file and its backup could not be read: starting again";
            if(!backup)return;
        }
        fresh=false;
        std::istringstream lines(body);
        std::string line;
        while(std::getline(lines,line))
        {
            const size_t eq=line.find('=');
            if(eq==std::string::npos)continue;
            const std::wstring key=FromUtf8(line.substr(0,eq)),value=FromUtf8(line.substr(eq+1));
            try
            {
                if(key==L"points")points=std::max(0LL,std::stoll(value));
                else if(key==L"days")days=std::max(0,std::stoi(value));
                else if(key==L"first"&&IsDate(value))first=value;
                else if(key==L"last"&&IsDate(value))last=value;
                else if(key==L"today"&&IsDate(value))today=value;
                else if(key==L"toldOn"&&IsDate(value))toldOn=value;
                else if(key.rfind(L"count.",0)==0)counts[key.substr(6)]=std::max(0,std::stoi(value));
                else if(key==L"told")
                {
                    std::wistringstream keys(value);
                    for(std::wstring k;std::getline(keys,k,L',');)if(!k.empty())told.insert(k);
                }
            }
            catch(...){}
        }
    }

    bool Bond::Save()
    {
        if(modelPath.empty())return false;
        const std::wstring file=CharacterDataFile(modelPath,Extension,true);
        if(file.empty())return false;
        std::wstring text=L"points="+std::to_wstring(points)+L"\ndays="+std::to_wstring(days)+L"\nfirst="+first+L"\nlast="+last+
                          L"\ntoday="+today+L"\ntoldOn="+toldOn+L"\ntold=";
        for(const auto& k:told)text+=k+L",";
        text+=L"\n";
        for(const auto& [name,n]:counts)text+=L"count."+name+L"="+std::to_wstring(n)+L"\n";
        std::string body=ToUtf8(text);
        Mix(body);
        const std::string encoded=ToBase64(body),signature=Sign(encoded);
        if(encoded.empty()||signature.empty())return false;
        const std::string data=std::string(Header)+"\n"+encoded+"\n"+signature+"\n";
        // Written beside the file and moved over it (the old one kept as .bak), so a crash
        // never leaves half a file and a damaged one can be recovered from.
        const std::wstring temp=file+L".tmp",backup=file+L".bak";
        {
            std::ofstream out(std::filesystem::path(temp),std::ios::binary|std::ios::trunc);
            if(!out)return false;
            out.write(data.data(),static_cast<std::streamsize>(data.size()));
            if(!out)return false;
        }
        std::string current;
        if(ReadSigned(file,current))CopyFileW(file.c_str(),backup.c_str(),FALSE);
        return MoveFileExW(temp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
    }

    void Bond::CarryOver(const std::wstring& since)
    {
        const std::wstring now=TodayDate();
        int elapsed=0;
        if(IsDate(since)&&since<now)
        {
            using namespace std::chrono;
            auto daysOf=[](const std::wstring& d){
                return sys_days(year_month_day(year(std::stoi(d.substr(0,4))),month(static_cast<unsigned>(std::stoi(d.substr(5,2)))),
                                               day(static_cast<unsigned>(std::stoi(d.substr(8,2))))));
            };
            elapsed=static_cast<int>((daysOf(now)-daysOf(since)).count());
            first=since;
        }
        // As if every day since had been a usual one (20 points), and friends at least.
        points=std::max<long long>(StagePoints[2],20LL*elapsed);
        days=elapsed;
        for(int s=1;s<=Stage();++s)told.insert(L"stage"+std::to_wstring(s));
        if(const long long d=Milestone(days,{7,30,100},100);d>0)told.insert(L"days"+std::to_wstring(d));
        if(const long long p=Milestone(points,{1000,3000,5000,10000},10000);p>0)told.insert(L"points"+std::to_wstring(p));
        fresh=false;
    }

    void Bond::Roll()
    {
        const std::wstring now=TodayDate();
        if(today==now)return;
        today=now;
        counts.clear();
        presentSeconds=0;
    }

    bool Bond::Add(BondAction action)
    {
        Roll();
        const Rule* rule=std::find_if(std::begin(Rules),std::end(Rules),[&](const Rule& r){return r.action==action;});
        int total=0;
        for(const auto& [name,n]:counts)total+=n;
        int& mine=counts[rule->name];
        const int add=std::min({rule->each,rule->cap-mine,DailyCap-total});
        if(add<=0)return false;
        mine+=add;
        points+=add;
        return true;
    }

    bool Bond::AddPresence(double seconds)
    {
        Roll();
        if(last==today)return false;
        presentSeconds+=seconds;
        if(presentSeconds<PresenceSeconds)return false;
        // The day counts together even when the day's points are used up already.
        ++days;
        last=today;
        if(first.empty())first=today;
        Add(BondAction::Present);
        return true;
    }

    int Bond::Stage() const
    {
        int stage=0;
        for(int i=0;i<Stages;++i)if(points>=StagePoints[i])stage=i;
        return stage;
    }

    std::wstring Bond::DueCelebration() const
    {
        const std::wstring now=TodayDate();
        if(toldOn==now)return {};
        std::vector<std::wstring> due;
        // A year (or more) since the first day, on its date.
        if(first.size()==10&&first.substr(5)==now.substr(5)&&now>first)
            due.push_back(L"year"+std::to_wstring(std::stoi(now.substr(0,4))-std::stoi(first.substr(0,4))));
        if(const long long d=Milestone(days,{7,30,100},100);d>0)due.push_back(L"days"+std::to_wstring(d));
        if(const int s=Stage();s>0)due.push_back(L"stage"+std::to_wstring(s));
        if(const long long p=Milestone(points,{1000,3000,5000,10000},10000);p>0)due.push_back(L"points"+std::to_wstring(p));
        for(const auto& key:due)if(!told.count(key))return key;
        return {};
    }

    void Bond::Told(const std::wstring& key)
    {
        told.insert(key);
        toldOn=TodayDate();
    }
}
