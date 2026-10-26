#define NOMINMAX
#include "Voice.hpp"
#include "Text.hpp"
#include "HttpJson.hpp"
#include "LocalChat.hpp"
#include "VoicevoxCore.hpp"
#include "AudioDecode.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
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
            if(!Crack(endpoint,url)){error=T(T(L"音声エンジンの接続先が正しくありません。",L"The voice engine's address is not valid."),L"The voice engine's address is not valid.");return false;}
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
                if(!ok)error=Fmt(T(L"音声エンジンがエラーを返しました（{0}）。",L"The voice engine returned an error ({0})."),{std::to_wstring(status)});
            }
            else error=T(T(L"音声エンジンに接続できません。VOICEVOX などを起動しているか確かめてください。",L"Cannot connect to the voice engine. Check that VOICEVOX or the like is running."),L"Cannot connect to the voice engine. Check that VOICEVOX or the like is running.");
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
        // spoken is how long its syllables take (0 from an engine that leaves the lengths to
        // its voices, as AivisSpeech does).
        bool Timeline(const Json& query,VoiceClip& clip,double& spoken)
        {
            const Json* phrases=query.Member("accent_phrases");
            if(!phrases||phrases->type!=Json::Type::Array)return false;
            // The engine writes the settings in camelCase, VOICEVOX CORE in snake_case.
            auto setting=[&](const char* camel,const char* snake){const Json* j=query.Member(camel);return Number(j?j:query.Member(snake));};
            const double scale=setting("speedScale","speed_scale");
            const double speed=std::max(0.1,scale>0?scale:1.0);
            double t=setting("prePhonemeLength","pre_phoneme_length");
            spoken=0;
            auto mora=[&](const Json& m,bool pause){
                const double consonant=Number(m.Member("consonant_length")),vowel=Number(m.Member("vowel_length"));
                t+=consonant;
                const Json* text=m.Member("text");
                clip.morae.push_back({t/speed,pause?std::wstring(L"、"):Hiragana(text&&text->type==Json::Type::String?Wide(text->text):std::wstring())});
                t+=vowel;
                if(!pause)spoken+=consonant+vowel;
            };
            for(const auto& phrase:phrases->items)
            {
                if(const Json* moras=phrase.Member("moras");moras&&moras->type==Json::Type::Array)
                    for(const auto& m:moras->items)mora(m,false);
                if(const Json* pause=phrase.Member("pause_mora");pause&&pause->type==Json::Type::Object)mora(*pause,true);
            }
            t+=setting("postPhonemeLength","post_phoneme_length");
            clip.seconds=t/speed;
            return true;
        }

        // Where the sound is voiced (seconds, start to end): loud against its own loudest
        // parts, with gaps under 60 ms bridged. seconds is how long the whole sound is.
        std::vector<std::pair<double,double>> VoicedSpans(const std::string& wav,double& seconds)
        {
            WAVEFORMATEX format{};
            const std::int16_t* samples=nullptr;
            size_t count=0;
            if(!ReadPcm(wav,format,samples,count))return {};
            const size_t channels=format.nChannels,frames=count/channels;
            if(frames==0)return {};
            seconds=static_cast<double>(frames)/format.nSamplesPerSec;
            const size_t step=std::max<size_t>(1,format.nSamplesPerSec/100);   // 10 ms
            const double window=static_cast<double>(step)/format.nSamplesPerSec;
            std::vector<float> loud;
            for(size_t first=0;first<frames;first+=step)
            {
                const size_t last=std::min(frames,first+step);
                double sum=0;
                for(size_t i=first;i<last;++i)
                {
                    float v=0;
                    for(size_t c=0;c<channels;++c)v+=samples[i*channels+c];
                    v/=static_cast<float>(channels)*32768.0f;
                    sum+=static_cast<double>(v)*v;
                }
                loud.push_back(static_cast<float>(std::sqrt(sum/static_cast<double>(last-first))));
            }
            std::vector<float> sorted(loud);
            std::sort(sorted.begin(),sorted.end());
            const float threshold=0.1f*std::max(1e-4f,sorted[sorted.size()*95/100]);
            std::vector<std::pair<double,double>> spans;
            for(size_t f=0;f<loud.size();++f)
            {
                if(loud[f]<threshold)continue;
                const double at=static_cast<double>(f)*window;
                if(!spans.empty()&&at-spans.back().second<0.06)spans.back().second=at+window;
                else spans.push_back({at,at+window});
            }
            std::erase_if(spans,[](const auto& s){return s.second-s.first<0.03;});
            return spans;
        }

        // The syllables spread over the voiced parts of the sound, for an engine whose query
        // has no timings: the query's phrases (split at its pauses) go to the parts between the
        // longest silences, each syllable taking the same share of its phrase's voiced time, and
        // the mouth rests in every silence.
        void Spread(VoiceClip& clip,std::vector<std::pair<double,double>> spans)
        {
            if(spans.empty())spans.push_back({0.0,clip.seconds});
            std::vector<std::vector<std::wstring>> phrases(1);
            for(const auto& [at,mora]:clip.morae)
            {
                // AivisSpeech gives punctuation syllables of its own: they are pauses too.
                const bool kana=std::any_of(mora.begin(),mora.end(),[](wchar_t c){return c>=0x3041&&c<=0x30FF;});
                if(kana)phrases.back().push_back(mora);
                else if(!phrases.back().empty())phrases.emplace_back();
            }
            if(phrases.size()>1&&phrases.back().empty())phrases.pop_back();
            if(phrases.size()>spans.size())
            {
                std::vector<std::wstring> all;
                for(const auto& p:phrases)all.insert(all.end(),p.begin(),p.end());
                phrases={all};
            }
            // The phrases end at the longest silences.
            std::vector<size_t> ends(spans.size()-1);
            for(size_t i=0;i<ends.size();++i)ends[i]=i;
            auto gap=[&](size_t i){return spans[i+1].first-spans[i].second;};
            std::sort(ends.begin(),ends.end(),[&](size_t a,size_t b){return gap(a)>gap(b);});
            ends.resize(phrases.size()-1);
            std::sort(ends.begin(),ends.end());
            ends.push_back(spans.size()-1);

            clip.morae.clear();
            size_t first=0;
            for(size_t p=0;p<phrases.size();++p)
            {
                const size_t last=ends[p];
                double voiced=0;
                for(size_t i=first;i<=last;++i)voiced+=spans[i].second-spans[i].first;
                const auto& syllables=phrases[p];
                size_t span=first;
                double before=0;   // voiced time in this phrase's spans before span
                for(size_t k=0;k<syllables.size();++k)
                {
                    const double share=voiced*static_cast<double>(k)/static_cast<double>(syllables.size());
                    while(span<last&&share>=before+spans[span].second-spans[span].first)
                    {
                        before+=spans[span].second-spans[span].first;
                        ++span;
                    }
                    clip.morae.push_back({spans[span].first+share-before,syllables[k]});
                }
                for(size_t i=first;i<=last;++i)
                    if(i==last||gap(i)>=0.1)clip.morae.push_back({spans[i].second,L"、"});
                first=last+1;
            }
            std::stable_sort(clip.morae.begin(),clip.morae.end(),[](const auto& a,const auto& b){return a.first<b.first;});
        }
    }

    namespace
    {
        // An audio query, then its sound, from whichever source.
        std::wstring Query(const VoiceSource& source,const std::wstring& text,int speaker,std::string& query)
        {
            if(source.direct)return VoicevoxCore::AudioQuery(source.folder,text,speaker,query);
            std::wstring error;
            Request(source.endpoint,L"POST",L"/audio_query?text="+Escape(text)+L"&speaker="+std::to_wstring(speaker),{},15000,query,error);
            return error;
        }

        // An audio query with one of its settings (a number at the top level, which the
        // engine writes after accent_phrases) set to value.
        std::string WithSetting(std::string query,const std::string& key,double value)
        {
            char number[32];
            const auto end=std::to_chars(number,number+sizeof(number),value,std::chars_format::fixed,2).ptr;
            const std::string quoted="\""+key+"\"";
            const size_t at=query.rfind(quoted);
            if(at==std::string::npos)
            {
                const size_t close=query.rfind('}');
                if(close!=std::string::npos)query.insert(close,","+quoted+":"+std::string(number,end));
                return query;
            }
            size_t first=query.find(':',at+quoted.size());
            if(first==std::string::npos)return query;
            first=query.find_first_not_of(" \t\r\n",first+1);
            const size_t last=query.find_first_not_of("+-0123456789.eE",first);
            if(first==std::string::npos||last==std::string::npos)return query;
            return query.replace(first,last-first,number,static_cast<size_t>(end-number));
        }

        std::wstring Sound(const VoiceSource& source,std::string query,int speaker,std::string& wav)
        {
            std::wstring error;
            // AivisSpeech reads intonationScale as how strongly the style comes through.
            if(!source.direct&&source.styleStrength!=1.0&&IsAivisSpeech(source.endpoint))
                query=WithSetting(std::move(query),"intonationScale",source.styleStrength);
            if(source.direct)error=VoicevoxCore::Synthesis(source.folder,query,speaker,wav);
            else Request(source.endpoint,L"POST",L"/synthesis?speaker="+std::to_wstring(speaker),query,60000,wav,error);   // AivisSpeech loads a voice's model on its first phrase
            if(error.empty()&&(wav.size()<=44||wav.compare(0,4,"RIFF")!=0))error=T(T(L"音声の作成に失敗しました。",L"Could not make the speech."),L"Could not make the speech.");
            return error;
        }
    }

    bool IsAivisSpeech(const std::wstring& endpoint)
    {
        Url url;
        return Crack(endpoint,url)&&url.port==10101;
    }

    std::wstring ListVoices(const VoiceSource& source,std::vector<std::pair<std::wstring,int>>& voices)
    {
        if(source.direct)return VoicevoxCore::Voices(source.folder,voices);
        std::string body;
        std::wstring error;
        if(!Request(source.endpoint,L"GET",L"/speakers",{},5000,body,error))return error;
        Json j;
        if(!JsonParser(body).Parse(j)||j.type!=Json::Type::Array)return T(T(L"音声エンジンの返事を読めませんでした。",L"Could not read the voice engine's reply."),L"Could not read the voice engine's reply.");
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
        if(voices.empty())return T(T(L"音声エンジンに声がありません。",L"The voice engine has no voices."),L"The voice engine has no voices.");
        return {};
    }

    std::wstring SynthesizeVoice(const VoiceSource& source,int speaker,const std::wstring& text,std::string& wav)
    {
        std::string query;
        if(auto error=Query(source,text,speaker,query);!error.empty())return error;
        return Sound(source,query,speaker,wav);
    }

    std::wstring MakeVoiceClip(const VoiceSource& source,int speaker,const std::wstring& text,VoiceClip& clip)
    {
        std::string query,wav;
        if(auto error=Query(source,text,speaker,query);!error.empty())return error;
        Json parsed;
        double spoken=0;
        if(!JsonParser(query).Parse(parsed)||!Timeline(parsed,clip,spoken))return T(T(L"音声エンジンの返事を読めませんでした。",L"Could not read the voice engine's reply."),L"Could not read the voice engine's reply.");
        if(auto error=Sound(source,query,speaker,wav);!error.empty())return error;
        // The sound decides how long the phrase is; without timings in the query the syllables
        // follow the sound too.
        double seconds=clip.seconds;
        const auto spans=VoicedSpans(wav,seconds);
        clip.seconds=seconds;
        if(spoken<0.01)Spread(clip,spans);
        clip.wav=std::move(wav);
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

    int VoiceEngine::Speak(const VoiceSource& source,int speaker,const std::wstring& text)
    {
        std::lock_guard lock(mutex);
        const int id=nextId++;
        jobs.push_back({id,source,speaker,text});
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
            VoiceClip* result=MakeVoiceClip(job.source,job.speaker,job.text,*clip).empty()?clip.release():nullptr;
            if(!PostMessageW(notify,DoneMessage,reinterpret_cast<WPARAM>(result),job.id))delete result;
        }
    }
}
