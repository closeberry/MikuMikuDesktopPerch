#pragma once
// Small helpers shared by the clients of local servers (the LLM, the voice engine): UTF-8,
// just enough JSON to read their replies, and URL checks.
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace MikuMikuDesktopPerch::Net
{
    inline std::string Utf8(const std::wstring& text)
    {
        if(text.empty())return {};
        const int size=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
        std::string out(static_cast<size_t>(size),'\0');
        WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),out.data(),size,nullptr,nullptr);
        return out;
    }

    inline std::wstring Wide(std::string_view text)
    {
        if(text.empty())return {};
        const int size=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
        std::wstring out(static_cast<size_t>(size),L'\0');
        MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),out.data(),size);
        return out;
    }

    inline std::string JsonString(const std::wstring& text)
    {
        std::string out="\"";
        for(const unsigned char c:Utf8(text))
        {
            switch(c)
            {
            case '"':out+="\\\"";break;
            case '\\':out+="\\\\";break;
            case '\n':out+="\\n";break;
            case '\r':out+="\\r";break;
            case '\t':out+="\\t";break;
            default:
                if(c<0x20){char hex[8];sprintf_s(hex,"\\u%04x",c);out+=hex;}
                else out+=static_cast<char>(c);
            }
        }
        return out+"\"";
    }

    // Just enough JSON to read the server's replies: values are parsed fully (with a depth
    // limit), strings come back as UTF-8.
    struct Json
    {
        enum class Type {Null,Bool,Number,String,Array,Object} type=Type::Null;
        std::string text;
        std::vector<Json> items;
        std::vector<std::pair<std::string,Json>> members;

        const Json* Member(std::string_view name) const
        {
            for(const auto& [key,value]:members)if(key==name)return &value;
            return nullptr;
        }
        const Json* Item(size_t index) const {return index<items.size()?&items[index]:nullptr;}
    };

    class JsonParser
    {
    public:
        explicit JsonParser(std::string_view text):s(text){}
        bool Parse(Json& out){Space();if(!Value(out,0))return false;Space();return p==s.size();}

    private:
        std::string_view s;
        size_t p=0;

        void Space(){while(p<s.size()&&(s[p]==' '||s[p]=='\t'||s[p]=='\n'||s[p]=='\r'))++p;}
        bool Literal(std::string_view word){if(s.substr(p,word.size())!=word)return false;p+=word.size();return true;}

        static void Append(std::string& out,unsigned code)
        {
            if(code<0x80)out+=static_cast<char>(code);
            else if(code<0x800){out+=static_cast<char>(0xC0|(code>>6));out+=static_cast<char>(0x80|(code&0x3F));}
            else if(code<0x10000){out+=static_cast<char>(0xE0|(code>>12));out+=static_cast<char>(0x80|((code>>6)&0x3F));out+=static_cast<char>(0x80|(code&0x3F));}
            else{out+=static_cast<char>(0xF0|(code>>18));out+=static_cast<char>(0x80|((code>>12)&0x3F));out+=static_cast<char>(0x80|((code>>6)&0x3F));out+=static_cast<char>(0x80|(code&0x3F));}
        }

        bool Hex4(unsigned& code)
        {
            if(p+4>s.size())return false;
            code=0;
            for(int i=0;i<4;++i)
            {
                const char c=s[p++];
                code<<=4;
                if(c>='0'&&c<='9')code|=c-'0';
                else if(c>='a'&&c<='f')code|=c-'a'+10;
                else if(c>='A'&&c<='F')code|=c-'A'+10;
                else return false;
            }
            return true;
        }

        bool String(std::string& out)
        {
            if(p>=s.size()||s[p]!='"')return false;
            ++p;
            while(p<s.size())
            {
                const char c=s[p++];
                if(c=='"')return true;
                if(c!='\\'){out+=c;continue;}
                if(p>=s.size())return false;
                const char e=s[p++];
                switch(e)
                {
                case '"':out+='"';break;
                case '\\':out+='\\';break;
                case '/':out+='/';break;
                case 'b':out+='\b';break;
                case 'f':out+='\f';break;
                case 'n':out+='\n';break;
                case 'r':out+='\r';break;
                case 't':out+='\t';break;
                case 'u':
                {
                    unsigned code=0;
                    if(!Hex4(code))return false;
                    if(code>=0xD800&&code<0xDC00&&p+6<=s.size()&&s[p]=='\\'&&s[p+1]=='u')
                    {
                        p+=2;
                        unsigned low=0;
                        if(!Hex4(low))return false;
                        if(low>=0xDC00&&low<0xE000)code=0x10000+((code-0xD800)<<10)+(low-0xDC00);
                    }
                    Append(out,code);
                    break;
                }
                default:return false;
                }
            }
            return false;
        }

        bool Value(Json& out,int depth)
        {
            if(depth>32||p>=s.size())return false;
            const char c=s[p];
            if(c=='{')
            {
                out.type=Json::Type::Object;
                ++p;Space();
                if(p<s.size()&&s[p]=='}'){++p;return true;}
                for(;;)
                {
                    Space();
                    std::string key;
                    if(!String(key))return false;
                    Space();
                    if(p>=s.size()||s[p++]!=':')return false;
                    Space();
                    Json value;
                    if(!Value(value,depth+1))return false;
                    out.members.emplace_back(std::move(key),std::move(value));
                    Space();
                    if(p<s.size()&&s[p]==','){++p;continue;}
                    if(p<s.size()&&s[p]=='}'){++p;return true;}
                    return false;
                }
            }
            if(c=='[')
            {
                out.type=Json::Type::Array;
                ++p;Space();
                if(p<s.size()&&s[p]==']'){++p;return true;}
                for(;;)
                {
                    Space();
                    Json value;
                    if(!Value(value,depth+1))return false;
                    out.items.push_back(std::move(value));
                    Space();
                    if(p<s.size()&&s[p]==','){++p;continue;}
                    if(p<s.size()&&s[p]==']'){++p;return true;}
                    return false;
                }
            }
            if(c=='"'){out.type=Json::Type::String;return String(out.text);}
            if(Literal("true")){out.type=Json::Type::Bool;out.text="true";return true;}
            if(Literal("false")){out.type=Json::Type::Bool;out.text="false";return true;}
            if(Literal("null")){out.type=Json::Type::Null;return true;}
            const size_t start=p;
            while(p<s.size()&&(std::isdigit(static_cast<unsigned char>(s[p]))||s[p]=='-'||s[p]=='+'||s[p]=='.'||s[p]=='e'||s[p]=='E'))++p;
            if(p==start)return false;
            out.type=Json::Type::Number;
            out.text=std::string(s.substr(start,p-start));
            return true;
        }
    };

    inline const Json* Path(const Json* j,std::initializer_list<std::string_view> keys)
    {
        for(const auto key:keys)
        {
            if(!j)return nullptr;
            if(key=="0")j=j->Item(0);
            else j=j->Member(key);
        }
        return j;
    }

    struct Url
    {
        bool secure=false;
        std::wstring host,path;
        INTERNET_PORT port=0;
    };

    inline bool Crack(const std::wstring& text,Url& url)
    {
        URL_COMPONENTS parts{sizeof(parts)};
        wchar_t host[256]{},path[1024]{};
        parts.lpszHostName=host;parts.dwHostNameLength=static_cast<DWORD>(std::size(host));
        parts.lpszUrlPath=path;parts.dwUrlPathLength=static_cast<DWORD>(std::size(path));
        if(!WinHttpCrackUrl(text.c_str(),static_cast<DWORD>(text.size()),0,&parts))return false;
        if(parts.nScheme!=INTERNET_SCHEME_HTTP&&parts.nScheme!=INTERNET_SCHEME_HTTPS)return false;
        url.secure=parts.nScheme==INTERNET_SCHEME_HTTPS;
        url.host=host;
        url.path=path;
        url.port=parts.nPort;
        while(!url.path.empty()&&url.path.back()==L'/')url.path.pop_back();
        return true;
    }

    inline bool IsLocalHost(std::wstring host)
    {
        std::transform(host.begin(),host.end(),host.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});
        return host==L"localhost"||host==L"127.0.0.1"||host==L"::1"||host==L"[::1]";
    }

}
