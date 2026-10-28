#define NOMINMAX
#include <windows.h>
#include "MahjongSound.hpp"
#include <mmsystem.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <initializer_list>
#include <vector>
#pragma comment(lib,"winmm.lib")

namespace MikuMikuDesktopPerch
{
    namespace
    {
        constexpr int Rate=44100;
        constexpr double Pi=3.14159265358979;
        constexpr double Volume=0.45;   // all the made ones, against the voice and the desktop's sounds
        constexpr double FileVolume=0.8;   // and the tiles' (theirs peak at about -5 dB)
        using Pcm=std::vector<std::int16_t>;
        constexpr int Sounds=static_cast<int>(MahjongSound::Deal)+1;

        // seconds long, each sample from f(t).
        template<class F>
        Pcm Make(double seconds,F f)
        {
            Pcm pcm(static_cast<size_t>(seconds*Rate));
            for(size_t i=0;i<pcm.size();++i)
            {
                const double t=static_cast<double>(i)/Rate;
                // A short fade at the end, so nothing clicks off.
                const double tail=std::min(1.0,(seconds-t)/0.01);
                pcm[i]=static_cast<std::int16_t>(std::clamp(f(t)*Volume*tail,-1.0,1.0)*32767);
            }
            return pcm;
        }

        double Noise(std::uint32_t& s)
        {
            s^=s<<13;s^=s>>17;s^=s<<5;
            return (s&0xFFFF)/32768.0-1.0;
        }

        // A sound of the tiles (a WAV resource of type MAHJONGSOUND, 44.1 kHz mono 16-bit, see
        // the .rc), made a little quieter; empty when it is missing or of another kind.
        Pcm File(const wchar_t* name)
        {
            Pcm pcm;
            const HRSRC info=FindResourceW(nullptr,name,L"MAHJONGSOUND");
            const HGLOBAL data=info?LoadResource(nullptr,info):nullptr;
            if(!data)return pcm;
            const auto* bytes=static_cast<const unsigned char*>(LockResource(data));
            const size_t size=SizeofResource(nullptr,info);
            auto u16=[&](size_t at){return static_cast<std::uint32_t>(bytes[at]|bytes[at+1]<<8);};
            auto u32=[&](size_t at){return u16(at)|u16(at+2)<<16;};
            if(size<12||std::memcmp(bytes,"RIFF",4)!=0||std::memcmp(bytes+8,"WAVE",4)!=0)return pcm;
            bool right=false;
            for(size_t at=12;at+8<=size;)
            {
                const size_t length=std::min<size_t>(u32(at+4),size-at-8);
                if(std::memcmp(bytes+at,"fmt ",4)==0&&length>=16)
                    right=u16(at+8)==1&&u16(at+10)==1&&u32(at+12)==Rate&&u16(at+22)==16;
                else if(std::memcmp(bytes+at,"data",4)==0&&right)
                {
                    pcm.resize(length/2);
                    for(size_t i=0;i<pcm.size();++i)
                        pcm[i]=static_cast<std::int16_t>(static_cast<std::int16_t>(u16(at+8+i*2))*FileVolume);
                    break;
                }
                at+=8+length+(length&1);
            }
            return pcm;
        }

        // A bell at f Hz struck u seconds ago, dying away over decay seconds: a little attack,
        // the tone, and an inharmonic overtone that dies sooner (a chime).
        double Bell(double u,double f,double decay)
        {
            if(u<0)return 0;
            const double attack=std::min(1.0,u/0.003);
            return attack*(std::sin(2*Pi*f*u)*std::exp(-u/decay)+0.35*std::sin(2*Pi*2.76*f*u)*std::exp(-u/(decay*0.35))+
                           0.15*std::sin(2*Pi*2*f*u)*std::exp(-u/(decay*0.6)));
        }

        // A drum: the head's pitch falling from high to low as it rings, a slap of noise.
        double Drum(double u,std::uint32_t& n,double low,double rise,double decay)
        {
            if(u<0)return 0;
            // The phase of a pitch low+rise*e^(-u/0.03), integrated.
            const double phase=2*Pi*(low*u+rise*0.03*(1-std::exp(-u/0.03)));
            return std::sin(phase)*std::exp(-u/decay)+0.35*Noise(n)*std::exp(-u/0.008);
        }

        // Noise through a band of frequencies that can move as it plays (a state-variable
        // filter, one sample at a time): the hiss of something swishing past.
        struct Swish
        {
            double low=0,band=0;
            double operator()(double in,double centre,double q=0.6)
            {
                const double f=2*std::sin(Pi*std::min(centre,Rate*0.2)/Rate);
                low+=f*band;
                const double high=in-low-q*band;
                band+=f*high;
                return band;
            }
        };

        double Note(int semitonesFromA4){return 440.0*std::pow(2.0,semitonesFromA4/12.0);}

        struct Tone {double at,f,gain,decay;};
        double Bells(double t,std::initializer_list<Tone> tones)
        {
            double v=0;
            for(const auto& b:tones)v+=b.gain*Bell(t-b.at,b.f,b.decay);
            return v;
        }

        const Pcm& Sound(MahjongSound which)
        {
            // C6, E6, G6, C7 and their neighbours, from A4.
            const double C6=Note(15),D6=Note(17),E6=Note(19),G6=Note(22),A6=Note(24),C7=Note(27),E7=Note(31),G7=Note(34),A5=Note(12),G5=Note(10),E5=Note(7);
            static std::array<Pcm,Sounds> made;
            Pcm& pcm=made[static_cast<size_t>(which)];
            if(!pcm.empty())return pcm;
            std::uint32_t n=0x2545F491u+static_cast<std::uint32_t>(which)*7919u;
            switch(which)
            {
            case MahjongSound::Place:
            {
                // Three of them by turns, so a run of discards does not sound the same each time.
                static const std::array<Pcm,3> places={File(L"PLACE1"),File(L"PLACE2"),File(L"PLACE3")};
                static size_t next=0;
                next=(next+1)%places.size();
                return places[next];
            }
            case MahjongSound::PlaceHard:pcm=File(L"PLACESTRONG");break;
            case MahjongSound::Draw:pcm=File(L"DRAW");break;
            case MahjongSound::Riichi:pcm=File(L"RIICHI");break;
            case MahjongSound::Pon:pcm=File(L"PON");break;
            case MahjongSound::Kan:pcm=File(L"KAN");break;
            case MahjongSound::Shuffle:pcm=File(L"SHUFFLE");break;
            case MahjongSound::Deal:pcm=File(L"DEAL");break;
            case MahjongSound::Vanish:
            {
                // Whisked away: a quick swish, high falling to low.
                Swish swish;
                pcm=Make(0.3,[&](double t){
                    const double envelope=std::min(1.0,t/0.006)*std::exp(-t/0.06);
                    return 0.7*envelope*swish(Noise(n),6500*std::exp(-t/0.09)+900);});
                break;
            }
            case MahjongSound::Appear:
            {
                // Whisked in: a swish rising and stopping short, then the tile set down softly on
                // the felt.
                Swish swish;
                pcm=Make(0.4,[&](double t){
                    const double u=std::min(t/0.16,1.0),envelope=u*u*std::exp(-std::max(t-0.16,0.0)/0.02);
                    return 0.65*envelope*swish(Noise(n),1200+6000*u);});
                const Pcm soft=File(L"PLACESOFT");
                pcm.insert(pcm.end(),soft.begin(),soft.end());
                break;
            }
            case MahjongSound::Win:
                // Ron, tsumo: a deep drum and a bright chord over it.
                pcm=Make(1.3,[&](double t){
                    return Drum(t,n,70,110,0.2)+0.28*Bells(t,{{0.05,C6,1,0.7},{0.05,E6,0.9,0.7},{0.05,G6,0.9,0.7},{0.05,C7,0.8,0.6}});});
                break;
            case MahjongSound::YouWon:
                pcm=Make(1.7,[&](double t){
                    return 0.3*Bells(t,{{0,C6,1,0.35},{0.12,E6,1,0.35},{0.24,G6,1,0.35},{0.36,C7,1,0.9},{0.36,E6,0.6,0.9},{0.36,G6,0.6,0.9}});});
                break;
            case MahjongSound::YouLost:
                // Gently down, as if "oh well".
                pcm=Make(1.5,[&](double t){
                    return 0.28*Bells(t,{{0,G6,0.9,0.4},{0.16,E6,0.9,0.4},{0.32,C6,0.9,0.45},{0.5,A5,0.8,0.9},{0.5,E5,0.5,0.9}});});
                break;
            case MahjongSound::Drawn:
                pcm=Make(1.0,[&](double t){return 0.26*Bells(t,{{0,E6,0.9,0.45},{0.2,D6,0.9,0.7}});});
                break;
            case MahjongSound::GameWon:
                pcm=Make(2.4,[&](double t){
                    return 0.28*Bells(t,{{0,C6,1,0.3},{0.11,E6,1,0.3},{0.22,G6,1,0.3},{0.33,C7,1,0.3},{0.44,E7,1,0.3},
                                         {0.6,G7,0.9,1.2},{0.6,C7,0.8,1.2},{0.6,E6,0.6,1.2},{0.6,C6,0.6,1.2}});});
                break;
            case MahjongSound::GameLost:
                pcm=Make(2.0,[&](double t){
                    return 0.26*Bells(t,{{0,A6,0.8,0.4},{0.18,G6,0.8,0.4},{0.36,E6,0.8,0.4},{0.54,D6,0.8,0.45},
                                         {0.75,C6,0.9,1.1},{0.75,G5,0.6,1.1},{0.75,E5,0.5,1.1}});});
                break;
            }
            return pcm;
        }

        // Outputs of their own, one sound each; one that has finished is opened again.
        struct Channel {HWAVEOUT device=nullptr;WAVEHDR header{};};
        std::array<Channel,8> channels;
        size_t nextSteal=0;

        void Close(Channel& c)
        {
            if(!c.device)return;
            waveOutReset(c.device);
            waveOutUnprepareHeader(c.device,&c.header,sizeof(c.header));
            waveOutClose(c.device);
            c.device=nullptr;
        }
    }

    void PlayMahjongSound(MahjongSound sound)
    {
        const Pcm& pcm=Sound(sound);
        if(pcm.empty())return;
        Channel* free=nullptr;
        for(auto& c:channels)
        {
            if(c.device&&(c.header.dwFlags&WHDR_DONE))Close(c);
            if(!c.device&&!free)free=&c;
        }
        if(!free){free=&channels[nextSteal];nextSteal=(nextSteal+1)%channels.size();Close(*free);}
        WAVEFORMATEX format{};
        format.wFormatTag=WAVE_FORMAT_PCM;
        format.nChannels=1;
        format.nSamplesPerSec=Rate;
        format.wBitsPerSample=16;
        format.nBlockAlign=2;
        format.nAvgBytesPerSec=Rate*2;
        if(waveOutOpen(&free->device,WAVE_MAPPER,&format,0,0,CALLBACK_NULL)!=MMSYSERR_NOERROR){free->device=nullptr;return;}
        free->header={};
        // The samples are made once and kept: the output reads them where they are.
        free->header.lpData=reinterpret_cast<LPSTR>(const_cast<std::int16_t*>(pcm.data()));
        free->header.dwBufferLength=static_cast<DWORD>(pcm.size()*sizeof(std::int16_t));
        if(waveOutPrepareHeader(free->device,&free->header,sizeof(free->header))!=MMSYSERR_NOERROR||
           waveOutWrite(free->device,&free->header,sizeof(free->header))!=MMSYSERR_NOERROR)Close(*free);
    }

    void StopMahjongSounds()
    {
        for(auto& c:channels)Close(c);
    }

    bool SaveMahjongSounds(const std::wstring& folder)
    {
        static const wchar_t* names[Sounds]={L"place",L"place-hard",L"draw",L"vanish",L"appear",L"pon",L"kan",L"riichi",L"win",
                                             L"you-won",L"you-lost",L"drawn",L"game-won",L"game-lost",L"shuffle",L"deal"};
        for(int i=0;i<Sounds;++i)
        {
            const Pcm& pcm=Sound(static_cast<MahjongSound>(i));
            std::ofstream out(folder+L"\\"+names[i]+L".wav",std::ios::binary);
            if(!out)return false;
            const std::uint32_t data=static_cast<std::uint32_t>(pcm.size()*2),riff=36+data,fmt=16,rate=Rate,bytes=Rate*2;
            const std::uint16_t pcmTag=1,mono=1,align=2,bits=16;
            out.write("RIFF",4);out.write(reinterpret_cast<const char*>(&riff),4);out.write("WAVEfmt ",8);
            out.write(reinterpret_cast<const char*>(&fmt),4);out.write(reinterpret_cast<const char*>(&pcmTag),2);out.write(reinterpret_cast<const char*>(&mono),2);
            out.write(reinterpret_cast<const char*>(&rate),4);out.write(reinterpret_cast<const char*>(&bytes),4);
            out.write(reinterpret_cast<const char*>(&align),2);out.write(reinterpret_cast<const char*>(&bits),2);
            out.write("data",4);out.write(reinterpret_cast<const char*>(&data),4);
            out.write(reinterpret_cast<const char*>(pcm.data()),static_cast<std::streamsize>(data));
        }
        return true;
    }
}
