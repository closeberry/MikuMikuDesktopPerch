#define NOMINMAX
#include "Speech.hpp"
#include <windows.h>
#include <oleauto.h>
#include <wrl/client.h>
#include <algorithm>

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

        bool IsPause(wchar_t c){return std::wstring(L"、。！？!?,.…\n").find(c)!=std::wstring::npos;}
    }

    std::wstring HiraganaReading(const std::wstring& text)
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

    void Speech::Reset()
    {
        *this=Speech();
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
        using namespace MotionTraceEditor::LipSyncCore;
        // Speaking goes on from where it is: a phrase that arrives late starts now.
        nextFrame=std::max(nextFrame,static_cast<int>(clock*30.0)+1);
        Chunk c;
        c.chars=chunk.size();
        c.start=nextFrame/30.0;
        // About 7.5 syllables a second; a comma pauses a little, a full stop longer.
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
            nextFrame+=4;
        }
        c.end=std::max(c.start+0.05,nextFrame/30.0);
        chunks.push_back(c);
        dirty=true;
    }

    void Speech::Tick(double dt)
    {
        if(!Active())return;
        clock+=dt;
        if(!dirty)return;
        dirty=false;
        for(auto& curve:curves)curve.clear();
        for(const auto& p:MotionTraceEditor::LipSyncCore::GenerateMorphPoints(events,0.8f))
            curves[static_cast<size_t>(p.vowel)].push_back({static_cast<float>(p.frame),p.value});
    }

    bool Speech::Speaking() const
    {
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
        // Each shape's keys, interpolated linearly as MMD does between morph keys.
        std::array<float,6> weights{};
        const float frame=static_cast<float>(clock*30.0);
        for(size_t channel=0;channel<curves.size();++channel)
        {
            const auto& curve=curves[channel];
            if(curve.empty()||frame<curve.front().first)continue;
            if(frame>=curve.back().first){weights[channel]=curve.back().second;continue;}
            const auto next=std::upper_bound(curve.begin(),curve.end(),frame,[](float f,const auto& key){return f<key.first;});
            const auto prev=next-1;
            const float t=(frame-prev->first)/std::max(1e-3f,next->first-prev->first);
            weights[channel]=prev->second+(next->second-prev->second)*t;
        }
        return weights;
    }
}
