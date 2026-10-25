#define NOMINMAX
#include "Speech.hpp"
#include <windows.h>
#include <oleauto.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <mmsystem.h>

#pragma comment(lib,"winmm.lib")

namespace MikuMikuDesktopPerch
{
    namespace
    {
        // The IME's language interface (msime.h, not part of the Windows SDK).
        MIDL_INTERFACE("019F7152-E6DB-11d0-83C3-00C04FDDB82E")
        IFELanguage:public IUnknown
        {
        public:
            virtual HRESULT STDMETHODCALLTYPE Open()=0;
            virtual HRESULT STDMETHODCALLTYPE Close()=0;
            virtual HRESULT STDMETHODCALLTYPE GetJMorphResult(DWORD,DWORD,INT,const WCHAR*,DWORD*,void**)=0;
            virtual HRESULT STDMETHODCALLTYPE GetConversionModeCaps(DWORD*)=0;
            virtual HRESULT STDMETHODCALLTYPE GetPhonetic(BSTR text,LONG start,LONG length,BSTR* phonetic)=0;
            virtual HRESULT STDMETHODCALLTYPE GetConversion(BSTR text,LONG start,LONG length,BSTR* result)=0;
        };

        // Opened once, on the thread that first asks (the UI thread).
        IFELanguage* Ime()
        {
            static Microsoft::WRL::ComPtr<IFELanguage> ime;
            static bool tried=false;
            if(!tried)
            {
                tried=true;
                CLSID clsid{};
                if(SUCCEEDED(CLSIDFromProgID(L"MSIME.Japan",&clsid))&&
                   SUCCEEDED(CoCreateInstance(clsid,nullptr,CLSCTX_INPROC_SERVER|CLSCTX_LOCAL_SERVER,__uuidof(IFELanguage),reinterpret_cast<void**>(ime.GetAddressOf())))&&
                   FAILED(ime->Open()))
                    ime.Reset();
            }
            return ime.Get();
        }

        bool IsKana(wchar_t c){return (c>=0x3041&&c<=0x309F)||(c>=0x30A0&&c<=0x30FF);}

        // Without the IME: kana and punctuation as they are, anything else two syllables
        // whose vowels vary with the character.
        std::wstring Guess(const std::wstring& text)
        {
            static constexpr const wchar_t* Syllables[]={L"か",L"き",L"く",L"け",L"こ"};
            std::wstring out;
            for(const wchar_t c:text)
            {
                if(IsKana(c)||std::iswspace(c)||std::wstring(L"、。！？!?,.ー〜").find(c)!=std::wstring::npos){out+=c;continue;}
                if(c>=L'0'&&c<=L'9'){out+=L"いち";continue;}
                if((c>=L'A'&&c<=L'Z')||(c>=L'a'&&c<=L'z')){out+=Syllables[c%5];continue;}
                if(c>=0x4E00&&c<=0x9FFF){out+=Syllables[c%5];out+=Syllables[(c/5)%5];}
            }
            return out;
        }

        // A curve's value at a frame: its keys interpolated linearly as MMD does between morph
        // keys (0 before the first, the last value after the last).
        float Sample(const std::vector<std::pair<float,float>>& curve,float frame)
        {
            if(curve.empty()||frame<curve.front().first)return 0;
            if(frame>=curve.back().first)return curve.back().second;
            const auto next=std::upper_bound(curve.begin(),curve.end(),frame,[](float f,const auto& key){return f<key.first;});
            const auto prev=next-1;
            const float t=(frame-prev->first)/std::max(1e-3f,next->first-prev->first);
            return prev->second+(next->second-prev->second)*t;
        }

        // The 16-bit PCM in a WAV file: its format and samples (false when it is not that).
        bool ReadPcm(const std::string& wav,WAVEFORMATEX& format,const std::int16_t*& samples,size_t& count)
        {
            if(wav.size()<12||wav.compare(0,4,"RIFF")!=0||wav.compare(8,4,"WAVE")!=0)return false;
            bool haveFormat=false;
            for(size_t at=12;at+8<=wav.size();)
            {
                std::uint32_t size=0;
                std::memcpy(&size,wav.data()+at+4,4);
                const size_t body=at+8;
                if(body+size>wav.size())size=static_cast<std::uint32_t>(wav.size()-body);
                if(wav.compare(at,4,"fmt ")==0&&size>=16)
                {
                    std::memcpy(&format,wav.data()+body,16);
                    haveFormat=format.wFormatTag==WAVE_FORMAT_PCM&&format.wBitsPerSample==16&&format.nChannels>=1&&format.nSamplesPerSec>0;
                }
                else if(wav.compare(at,4,"data")==0&&haveFormat)
                {
                    samples=reinterpret_cast<const std::int16_t*>(wav.data()+body);
                    count=size/2;
                    return true;
                }
                at=body+size+(size&1);
            }
            return false;
        }

        bool IsPause(wchar_t c){return std::wstring(L"、。！？!?,.…\n").find(c)!=std::wstring::npos;}

        bool IsLatin(wchar_t c){return (c>=L'A'&&c<=L'Z')||(c>=L'a'&&c<=L'z');}

        // An English word as the mouth says it, roughly: each vowel sound (a vowel letter, a
        // common pair of them, or y after a consonant) becomes one kana with that vowel, and a
        // word ending in m, b or p closes the lips (ん). A silent final e is left out.
        std::wstring EnglishWord(std::wstring w)
        {
            for(auto& c:w)c=static_cast<wchar_t>(std::towlower(c));
            auto vowel=[](wchar_t c){return c==L'a'||c==L'e'||c==L'i'||c==L'o'||c==L'u';};
            if(w.size()>2&&w.back()==L'e'&&!vowel(w[w.size()-2])&&std::any_of(w.begin(),w.end()-2,vowel))w.pop_back();
            struct Sound {const wchar_t* letters;const wchar_t* kana;};
            static constexpr Sound Pairs[]={{L"oo",L"う"},{L"ou",L"あう"},{L"ow",L"あう"},{L"oi",L"おい"},{L"oy",L"おい"},{L"ai",L"えい"},
                                            {L"ay",L"えい"},{L"ee",L"い"},{L"ea",L"い"},{L"ie",L"い"},{L"ei",L"えい"},{L"ey",L"えい"},
                                            {L"oa",L"おう"},{L"au",L"お"},{L"aw",L"お"},{L"ue",L"う"},{L"ew",L"う"},{L"ui",L"う"}};
            std::wstring out;
            for(size_t i=0;i<w.size();)
            {
                const auto pair=std::find_if(std::begin(Pairs),std::end(Pairs),[&](const Sound& p){return w.compare(i,2,p.letters)==0;});
                if(pair!=std::end(Pairs)){out+=pair->kana;i+=2;continue;}
                const wchar_t c=w[i++];
                if(c==L'a')out+=L"あ";
                else if(c==L'e')out+=L"え";
                else if(c==L'i'||(c==L'y'&&i>1&&!vowel(w[i-2])))out+=L"い";
                else if(c==L'o')out+=L"お";
                else if(c==L'u')out+=L"う";
            }
            if(out.empty())return L"ん";   // hmm, shh
            if(const wchar_t last=w.back();last==L'm'||last==L'b'||last==L'p')out+=L"ん";
            return out;
        }

        // Japanese through the IME (or a guess without it).
        std::wstring JapaneseReading(const std::wstring& text)
        {
            if(text.empty())return {};
            if(IFELanguage* ime=Ime())
            {
                BSTR input=SysAllocStringLen(text.data(),static_cast<UINT>(text.size()));
                BSTR reading=nullptr;
                const HRESULT hr=ime->GetPhonetic(input,1,-1,&reading);
                SysFreeString(input);
                if(SUCCEEDED(hr)&&reading)
                {
                    std::wstring out(reading,SysStringLen(reading));
                    SysFreeString(reading);
                    if(!out.empty())return out;
                }
            }
            return Guess(text);
        }
    }

    // English words are read from their letters (the IME would spell them out); the rest, in
    // between, through the IME.
    std::wstring HiraganaReading(const std::wstring& text)
    {
        std::wstring out,other;
        auto flush=[&]{out+=JapaneseReading(other);other.clear();};
        for(size_t i=0;i<text.size();)
        {
            if(!IsLatin(text[i])){other+=text[i++];continue;}
            size_t end=i;
            while(end<text.size()&&(IsLatin(text[end])||(text[end]==L'\''&&end+1<text.size()&&IsLatin(text[end+1]))))++end;
            flush();
            out+=EnglishWord(text.substr(i,end-i));
            i=end;
        }
        flush();
        return out;
    }

    // One WAV file at a time on a waveOut device of its own (the system mixes them).
    struct Speech::Player
    {
        HWAVEOUT device=nullptr;
        WAVEHDR header{};
        std::string data;

        ~Player(){Stop();}

        void Stop()
        {
            if(!device)return;
            waveOutReset(device);
            waveOutUnprepareHeader(device,&header,sizeof(header));
            waveOutClose(device);
            device=nullptr;
        }

        // A RIFF WAVE file in memory (PCM, as the voice engines make it).
        void Play(const std::string& wav)
        {
            Stop();
            if(wav.size()<12||wav.compare(0,4,"RIFF")!=0||wav.compare(8,4,"WAVE")!=0)return;
            WAVEFORMATEX format{};
            bool haveFormat=false;
            for(size_t at=12;at+8<=wav.size();)
            {
                std::uint32_t size=0;
                std::memcpy(&size,wav.data()+at+4,4);
                const size_t body=at+8;
                if(body+size>wav.size())size=static_cast<std::uint32_t>(wav.size()-body);
                if(wav.compare(at,4,"fmt ")==0&&size>=16)
                {
                    std::memcpy(&format,wav.data()+body,16);
                    format.cbSize=0;
                    haveFormat=true;
                }
                else if(wav.compare(at,4,"data")==0&&haveFormat)
                {
                    data.assign(wav,body,size);
                    if(waveOutOpen(&device,WAVE_MAPPER,&format,0,0,CALLBACK_NULL)!=MMSYSERR_NOERROR){device=nullptr;return;}
                    header={};
                    header.lpData=data.data();
                    header.dwBufferLength=static_cast<DWORD>(data.size());
                    if(waveOutPrepareHeader(device,&header,sizeof(header))!=MMSYSERR_NOERROR||
                       waveOutWrite(device,&header,sizeof(header))!=MMSYSERR_NOERROR)Stop();
                    return;
                }
                at=body+size+(size&1);
            }
        }
    };

    Speech::Speech()=default;
    Speech::Speech(Speech&&) noexcept=default;
    Speech& Speech::operator=(Speech&&) noexcept=default;
    Speech::~Speech()=default;   // the player stops what it plays

    void Speech::Reset()
    {
        auto keep=std::move(voice);
        *this=Speech();
        voice=std::move(keep);
    }

    void Speech::Add(const std::wstring& text)
    {
        pending+=text;
        // Read in phrases: up to the last pause, or a long run without one.
        size_t cut=std::wstring::npos;
        for(size_t i=pending.size();i>0;--i)
            if(IsPause(pending[i-1])){cut=i;break;}
        if(cut==std::wstring::npos&&pending.size()>=24)cut=pending.size();
        if(cut==std::wstring::npos)return;
        const std::wstring chunk=pending.substr(0,cut);
        pending.erase(0,cut);
        Flush(chunk);
    }

    void Speech::Finish()
    {
        if(!pending.empty()){const std::wstring chunk=pending;pending.clear();Flush(chunk);}
        finished=true;
    }

    void Speech::Flush(const std::wstring& chunk)
    {
        // With a voice the phrase waits for its sound (in order); a phrase with nothing to say
        // (punctuation alone) goes straight on.
        const bool sayable=std::any_of(chunk.begin(),chunk.end(),[](wchar_t c){return !IsPause(c)&&!std::iswspace(c);});
        if(voice&&sayable)
        {
            waiting.push_back({voice(chunk),chunk,false,nullptr});
            return;
        }
        if(!waiting.empty())waiting.push_back({0,chunk,true,nullptr});   // keeps its place behind the voiced ones
        else Place(chunk,nullptr);
    }

    void Speech::Voiced(int id,VoiceClip* clip)
    {
        std::unique_ptr<VoiceClip> owned(clip);
        auto it=std::find_if(waiting.begin(),waiting.end(),[&](const Waiting& w){return w.id==id&&!w.done;});
        if(it==waiting.end())return;   // from a reply already reset
        it->done=true;
        it->clip=std::move(owned);
        // Phrases are said in order: place those at the front that are ready.
        while(!waiting.empty()&&waiting.front().done)
        {
            Place(waiting.front().text,waiting.front().clip.get());
            waiting.erase(waiting.begin());
        }
    }

    void Speech::Place(const std::wstring& chunk,VoiceClip* clip)
    {
        using namespace MotionTraceEditor::LipSyncCore;
        // Speaking goes on from where it is: a phrase that arrives late starts now.
        nextFrame=std::max(nextFrame,static_cast<int>(clock*30.0)+1);
        Chunk c;
        c.chars=chunk.size();
        c.start=nextFrame/30.0;
        if(clip)
        {
            // The syllables where the voice says them.
            const int start=nextFrame;
            for(const auto& [at,mora]:clip->morae)
                if(IsRestText(mora)||IsSupportedLyricToken(mora))events.push_back({start+static_cast<int>(at*30.0+0.5),mora});
            nextFrame=start+static_cast<int>(clip->seconds*30.0+0.999);
            sounds.push_back({std::move(clip->wav),start/30.0,start/30.0+clip->seconds,false});
            c.end=std::max(c.start+0.05,nextFrame/30.0);
            chunks.push_back(c);
            dirty=true;
            return;
        }
        // About 7.5 syllables a second (English about 5); a comma pauses a little, a full stop longer.
        const bool english=std::count_if(chunk.begin(),chunk.end(),IsLatin)*3>static_cast<std::ptrdiff_t>(chunk.size());
        for(const auto& mora:SplitMorae(HiraganaReading(chunk)))
        {
            if(IsRestText(mora))
            {
                events.push_back({nextFrame,mora});
                nextFrame+=mora==L"、"||mora==L","?4:8;
                continue;
            }
            if(!IsSupportedLyricToken(mora))continue;
            events.push_back({nextFrame,mora});
            nextFrame+=english?6:4;
        }
        c.end=std::max(c.start+0.05,nextFrame/30.0);
        chunks.push_back(c);
        dirty=true;
    }

    void Speech::Say(const std::wstring& text,std::string wav)
    {
        WAVEFORMATEX format{};
        const std::int16_t* samples=nullptr;
        size_t count=0;
        if(!ReadPcm(wav,format,samples,count)){Add(text);return;}   // said silently, as without a voice
        const size_t channels=format.nChannels,frames=count/channels;
        if(frames==0){Add(text);return;}
        const double seconds=static_cast<double>(frames)/format.nSamplesPerSec;
        nextFrame=std::max(nextFrame,static_cast<int>(clock*30.0)+1);
        const int start=nextFrame;

        // Per 1/30 s: how loud (RMS) and how bright (zero crossings a second, high for s, sh, f).
        const size_t step=std::max<size_t>(1,format.nSamplesPerSec/30);
        std::vector<float> loud,bright;
        for(size_t first=0;first<frames;first+=step)
        {
            const size_t last=std::min(frames,first+step);
            double sum=0;
            int crossings=0;
            float previous=0;
            for(size_t i=first;i<last;++i)
            {
                float v=0;
                for(size_t c=0;c<channels;++c)v+=samples[i*channels+c];
                v/=static_cast<float>(channels)*32768.0f;
                sum+=static_cast<double>(v)*v;
                if(i>first&&(v>=0)!=(previous>=0))++crossings;
                previous=v;
            }
            loud.push_back(static_cast<float>(std::sqrt(sum/static_cast<double>(last-first))));
            bright.push_back(static_cast<float>(crossings*static_cast<double>(format.nSamplesPerSec)/static_cast<double>(last-first)));
        }
        // Loudness against the sound's own loudest parts (whatever its level): quiet gaps close
        // the mouth. Each syllable stands out from its neighbours: the level is taken against
        // its average over the last quarter second, so a run of loud syllables still opens and
        // closes. Quick to open, slower to close.
        std::vector<float> sorted(loud);
        std::sort(sorted.begin(),sorted.end());
        const float reference=std::max(1e-4f,sorted[std::min(sorted.size()-1,sorted.size()*97/100)]);
        float open=0,average=0;   // keys go after those of an earlier sound (it has ended by start)
        for(size_t f=0;f<loud.size();++f)
        {
            const float relative=std::min(1.0f,loud[f]/reference);
            average+=(relative-average)*0.25f;
            const float level=loud[f]<reference*0.08f?0.0f:std::clamp(std::pow(relative,1.2f)*(0.55f+0.9f*std::max(0.0f,relative-0.6f*average)),0.0f,1.0f);
            open+=(level-open)*(level>open?0.75f:0.4f);
            const float b=std::clamp((bright[f]-800.0f)/2500.0f,0.0f,1.0f);
            const float frame=static_cast<float>(start+static_cast<int>(f));
            soundCurves[0].push_back({frame,0.8f*open*(1.0f-0.6f*b)});   // あ
            soundCurves[1].push_back({frame,0.8f*open*0.7f*b});          // い
            soundCurves[4].push_back({frame,0.8f*open*0.3f*(1.0f-b)});   // お: a little rounding
        }
        for(auto& curve:soundCurves)curve.push_back({static_cast<float>(start+static_cast<int>(loud.size())),0.0f});

        nextFrame=start+static_cast<int>(seconds*30.0+0.999);
        sounds.push_back({std::move(wav),start/30.0,start/30.0+seconds,false});
        chunks.push_back({text.size(),start/30.0,std::max(start/30.0+0.05,nextFrame/30.0)});
        soundEnd=chunks.back().end;
    }

    void Speech::Tick(double dt)
    {
        if(!Active())return;
        clock+=dt;
        // A voiced phrase starts playing at its time (the one before it has ended by then).
        for(auto& s:sounds)
            if(!s.started&&clock>=s.start)
            {
                s.started=true;
                if(!player)player=std::make_unique<Player>();
                player->Play(s.wav);
            }
        // Finished sounds can go (the one playing, and those after it, stay).
        while(sounds.size()>1&&sounds.front().started&&sounds[1].started)sounds.erase(sounds.begin());
        if(!dirty)return;
        dirty=false;
        for(auto& curve:curves)curve.clear();
        for(const auto& p:MotionTraceEditor::LipSyncCore::GenerateMorphPoints(events,0.8f))
            curves[static_cast<size_t>(p.vowel)].push_back({static_cast<float>(p.frame),p.value});
    }

    bool Speech::Speaking() const
    {
        if(!waiting.empty()||clock<soundEnd)return true;
        if(events.empty())return !pending.empty();
        return clock*30.0<events.back().frame+8||!pending.empty();
    }

    double Speech::RevealedChars() const
    {
        double shown=0;
        for(const auto& c:chunks)
        {
            if(clock>=c.end)shown+=static_cast<double>(c.chars);
            else if(clock>c.start)shown+=static_cast<double>(c.chars)*(clock-c.start)/(c.end-c.start);
        }
        return shown;
    }

    std::array<float,6> Speech::Mouth() const
    {
        // Each shape's keys (from the syllables, and from the loudness of a sound of its own).
        std::array<float,6> weights{};
        const float frame=static_cast<float>(clock*30.0);
        for(size_t channel=0;channel<curves.size();++channel)
            weights[channel]=std::max(Sample(curves[channel],frame),Sample(soundCurves[channel],frame));
        return weights;
    }
}
