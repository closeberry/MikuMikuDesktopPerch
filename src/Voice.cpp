#define NOMINMAX
#include "Voice.hpp"
#include "HttpJson.hpp"
#include "LocalChat.hpp"
#include <cstdlib>

#pragma comment(lib,"winhttp.lib")

namespace MikuMikuDesktopPerch
{
    using namespace Net;

    namespace
    {
        constexpr size_t MaxBody=32u<<20;   // a long phrase is a few hundred KB of WAV

        // One request to the engine; false with an explanation when it fails.
        bool Request(const std::wstring& endpoint,const wchar_t* verb,const std::wstring& path,const std::string& body,
                     DWORD timeout,std::string& reply,std::wstring& error)
        {
            if(const auto problem=CheckLocalEndpoint(endpoint);!problem.empty()){error=problem;return false;}
            Url url;
            if(!Crack(endpoint,url)){error=L"音声エンジンの接続先が正しくありません。";return false;}
            HINTERNET session=WinHttpOpen(L"MikuMikuDesktopPerch",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
            HINTERNET connection=nullptr,request=nullptr;
            if(session)
            {
                WinHttpSetTimeouts(session,3000,3000,timeout,timeout);
                connection=WinHttpConnect(session,url.host.c_str(),url.port,0);
            }
            if(connection)
                request=WinHttpOpenRequest(connection,verb,(url.path+path).c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,url.secure?WINHTTP_FLAG_SECURE:0);
            bool ok=false;
            const wchar_t* headers=body.empty()?WINHTTP_NO_ADDITIONAL_HEADERS:L"Content-Type: application/json\r\n";
            if(request&&WinHttpSendRequest(request,headers,body.empty()?0:static_cast<DWORD>(-1),
                                           body.empty()?WINHTTP_NO_REQUEST_DATA:const_cast<char*>(body.data()),
                                           static_cast<DWORD>(body.size()),static_cast<DWORD>(body.size()),0)&&
               WinHttpReceiveResponse(request,nullptr))
            {
                DWORD status=0,size=sizeof(status);
                WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX);
                char buffer[16384];
                for(DWORD read=0;WinHttpReadData(request,buffer,sizeof(buffer),&read)&&read>0&&reply.size()<MaxBody;)reply.append(buffer,read);
                ok=status==200;
                if(!ok)error=L"音声エンジンがエラーを返しました（"+std::to_wstring(status)+L"）。";
            }
            else error=L"音声エンジンに接続できません。VOICEVOX などを起動しているか確かめてください。";
            if(request)WinHttpCloseHandle(request);
            if(connection)WinHttpCloseHandle(connection);
            if(session)WinHttpCloseHandle(session);
            return ok;
        }

        std::wstring Escape(const std::wstring& text)
        {
            std::wstring out;
            for(const unsigned char c:Utf8(text))
            {
                if(std::isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~'){out+=static_cast<wchar_t>(c);continue;}
                wchar_t hex[4]{};
                swprintf_s(hex,L"%%%02X",c);
                out+=hex;
            }
            return out;
        }

        double Number(const Json* j){return j&&j->type==Json::Type::Number?std::atof(j->text.c_str()):0.0;}

        // Katakana to hiragana, which the lip-sync reads.
        std::wstring Hiragana(std::wstring text)
        {
            for(auto& c:text)if(c>=0x30A1&&c<=0x30F6)c=static_cast<wchar_t>(c-0x60);
            return text;
        }

        // When each syllable of the audio query is said: the query's timings, at its speed.
        bool Timeline(const Json& query,VoiceClip& clip)
        {
            const Json* phrases=query.Member("accent_phrases");
            if(!phrases||phrases->type!=Json::Type::Array)return false;
            const double speed=std::max(0.1,Number(query.Member("speedScale"))>0?Number(query.Member("speedScale")):1.0);
            double t=Number(query.Member("prePhonemeLength"));
            auto mora=[&](const Json& m,bool pause){
                t+=Number(m.Member("consonant_length"));
                const Json* text=m.Member("text");
                clip.morae.push_back({t/speed,pause?std::wstring(L"、"):Hiragana(text&&text->type==Json::Type::String?Wide(text->text):std::wstring())});
                t+=Number(m.Member("vowel_length"));
            };
            for(const auto& phrase:phrases->items)
            {
                if(const Json* moras=phrase.Member("moras");moras&&moras->type==Json::Type::Array)
                    for(const auto& m:moras->items)mora(m,false);
                if(const Json* pause=phrase.Member("pause_mora");pause&&pause->type==Json::Type::Object)mora(*pause,true);
            }
            t+=Number(query.Member("postPhonemeLength"));
            clip.seconds=t/speed;
            return true;
        }
    }

    std::wstring ListVoices(const std::wstring& endpoint,std::vector<std::pair<std::wstring,int>>& voices)
    {
        std::string body;
        std::wstring error;
        if(!Request(endpoint,L"GET",L"/speakers",{},5000,body,error))return error;
        Json j;
        if(!JsonParser(body).Parse(j)||j.type!=Json::Type::Array)return L"音声エンジンの返事を読めませんでした。";
        for(const auto& speaker:j.items)
        {
            const Json* name=speaker.Member("name");
            const Json* styles=speaker.Member("styles");
            if(!name||name->type!=Json::Type::String||!styles||styles->type!=Json::Type::Array)continue;
            for(const auto& style:styles->items)
            {
                const Json* styleName=style.Member("name");
                const Json* id=style.Member("id");
                if(!id||id->type!=Json::Type::Number)continue;
                voices.push_back({Wide(name->text)+L"（"+(styleName&&styleName->type==Json::Type::String?Wide(styleName->text):std::wstring())+L"）",
                                  std::atoi(id->text.c_str())});
            }
        }
        if(voices.empty())return L"音声エンジンに声がありません。";
        return {};
    }

    std::wstring SynthesizeVoice(const std::wstring& endpoint,int speaker,const std::wstring& text,std::string& wav)
    {
        const std::wstring voice=L"speaker="+std::to_wstring(speaker);
        std::string query;
        std::wstring error;
        if(!Request(endpoint,L"POST",L"/audio_query?text="+Escape(text)+L"&"+voice,{},15000,query,error))return error;
        if(!Request(endpoint,L"POST",L"/synthesis?"+voice,query,30000,wav,error))return error;
        if(wav.size()<=44||wav.compare(0,4,"RIFF")!=0)return L"音声エンジンの返事を読めませんでした。";
        return {};
    }

    VoiceEngine::VoiceEngine(HWND notify):notify(notify)
    {
        worker=std::thread([this]{Run();});   // started last: it uses the other members
    }

    VoiceEngine::~VoiceEngine()
    {
        {
            std::lock_guard lock(mutex);
            quit=true;
            jobs.clear();
        }
        wake.notify_all();
        worker.join();
    }

    int VoiceEngine::Speak(const std::wstring& endpoint,int speaker,const std::wstring& text)
    {
        std::lock_guard lock(mutex);
        const int id=nextId++;
        jobs.push_back({id,endpoint,speaker,text});
        wake.notify_all();
        return id;
    }

    void VoiceEngine::CancelAll()
    {
        std::lock_guard lock(mutex);
        jobs.clear();
    }

    void VoiceEngine::Run()
    {
        for(;;)
        {
            Job job;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock,[&]{return quit||!jobs.empty();});
                if(quit)return;
                job=std::move(jobs.front());
                jobs.pop_front();
            }
            auto clip=std::make_unique<VoiceClip>();
            std::string query,wav;
            std::wstring error;
            const std::wstring speaker=L"speaker="+std::to_wstring(job.speaker);
            Json parsed;
            const bool ok=Request(job.endpoint,L"POST",L"/audio_query?text="+Escape(job.text)+L"&"+speaker,{},15000,query,error)&&
                          JsonParser(query).Parse(parsed)&&Timeline(parsed,*clip)&&
                          Request(job.endpoint,L"POST",L"/synthesis?"+speaker,query,30000,wav,error)&&
                          wav.size()>44&&wav.compare(0,4,"RIFF")==0;
            if(ok)clip->wav=std::move(wav);
            VoiceClip* result=ok?clip.release():nullptr;
            if(!PostMessageW(notify,DoneMessage,reinterpret_cast<WPARAM>(result),job.id))delete result;
        }
    }
}
