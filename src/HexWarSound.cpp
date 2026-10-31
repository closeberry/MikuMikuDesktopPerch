#include "HexWarSound.hpp"
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>
#include <string>
#include <vector>

#pragma comment(lib,"winmm.lib")

namespace MikuMikuDesktopPerch
{
    namespace
    {
        constexpr int Rate=32000;
        constexpr int MostTakes=4;
        constexpr int Kinds=5;
        constexpr float Volume=0.55f;     // against the voice and the desktop's sounds
        constexpr float Ceiling=0.6f;     // the mix never goes above this
        constexpr float Release=1.0f/(0.25f*Rate);   // the limiter's way back up: about a quarter second
        constexpr size_t MostVoices=24;   // the oldest dropped beyond this
        constexpr int BlockFrames=640;    // 20 ms
        constexpr int Blocks=4;

        using Samples=std::vector<float>;

        // IMA ADPCM, as tools/hexwar/build_sounds.py writes it (4 bits a sample): blocks, each a
        // first sample and step index, then two samples a byte (low nibble first).
        void DecodeAdpcm(const unsigned char* data,size_t size,size_t block,size_t count,Samples& pcm)
        {
            static const int steps[89]={7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,
                                        107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,
                                        876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,
                                        5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,
                                        27086,29794,32767};
            static const int move[8]={-1,-1,-1,-1,2,4,6,8};
            pcm.clear();
            pcm.reserve(count);
            for(size_t at=0;at+4<=size&&pcm.size()<count;at+=block)
            {
                int value=static_cast<std::int16_t>(data[at]|data[at+1]<<8),index=std::min(88,static_cast<int>(data[at+2]));
                pcm.push_back(value/32768.0f);
                for(size_t i=at+4;i<std::min(at+block,size)&&pcm.size()<count;++i)
                    for(int code:{data[i]&15,data[i]>>4})
                    {
                        const int step=steps[index];
                        int change=step>>3;
                        if(code&4)change+=step;
                        if(code&2)change+=step>>1;
                        if(code&1)change+=step>>2;
                        value=std::clamp(code&8?value-change:value+change,-32768,32767);
                        index=std::clamp(index+move[code&7],0,88);
                        if(pcm.size()<count)pcm.push_back(value/32768.0f);
                    }
            }
        }

        // A take (a WAV resource, 32 kHz mono, 16-bit or IMA ADPCM); empty when missing or of
        // another kind.
        Samples Load(const wchar_t* name)
        {
            Samples pcm;
            const HRSRC info=FindResourceW(nullptr,name,L"HEXWARSOUND");
            const HGLOBAL data=info?LoadResource(nullptr,info):nullptr;
            if(!data)return pcm;
            const auto* bytes=static_cast<const unsigned char*>(LockResource(data));
            const size_t size=SizeofResource(nullptr,info);
            auto u16=[&](size_t at){return static_cast<std::uint32_t>(bytes[at]|bytes[at+1]<<8);};
            auto u32=[&](size_t at){return u16(at)|u16(at+2)<<16;};
            if(size<12||std::memcmp(bytes,"RIFF",4)!=0||std::memcmp(bytes+8,"WAVE",4)!=0)return pcm;
            bool right=false,adpcm=false;
            size_t block=0,count=0;
            for(size_t at=12;at+8<=size;)
            {
                const size_t length=std::min<size_t>(u32(at+4),size-at-8);
                if(std::memcmp(bytes+at,"fmt ",4)==0&&length>=16)
                {
                    adpcm=u16(at+8)==0x11;
                    block=u16(at+20);
                    right=(u16(at+8)==1&&u16(at+22)==16||adpcm&&u16(at+22)==4&&block>4)&&u16(at+10)==1&&u32(at+12)==Rate;
                }
                else if(std::memcmp(bytes+at,"fact",4)==0&&length>=4)count=u32(at+8);
                else if(std::memcmp(bytes+at,"data",4)==0&&right)
                {
                    if(adpcm)DecodeAdpcm(bytes+at+8,length,block,count?count:(length/block)*((block-4)*2+1),pcm);
                    else
                    {
                        pcm.resize(length/2);
                        for(size_t i=0;i<pcm.size();++i)pcm[i]=static_cast<std::int16_t>(u16(at+8+i*2))/32768.0f;
                    }
                    break;
                }
                at+=8+length+(length&1);
            }
            return pcm;
        }

        // The takes of a kind (those built in: the gun has one, the others four).
        const std::vector<Samples>& Takes(HexWarSound sound)
        {
            static const std::array<std::vector<Samples>,Kinds> all=[]{
                std::array<std::vector<Samples>,Kinds> a;
                const wchar_t* names[Kinds]={L"CANNON",L"RIFLE",L"BLAST",L"TRACKS",L"STEPS"};
                for(int s=0;s<Kinds;++s)
                    for(int k=1;k<=MostTakes;++k)
                        if(Samples pcm=Load((names[s]+std::to_wstring(k)).c_str());!pcm.empty())a[s].push_back(std::move(pcm));
                return a;
            }();
            return all[static_cast<int>(sound)];
        }

        // How loud each kind is against the others.
        constexpr float KindLoud[Kinds]={0.8f,0.55f,1.0f,0.55f,0.75f};
        constexpr double FadeIn=0.08,FadeOut=0.3;   // a loop's, in seconds

        struct Voice
        {
            const Samples* pcm=nullptr;
            double at=0,step=1;   // where it is, and how fast it goes (its pitch)
            float left=0,right=0;
            // A loop: frames it plays in all (0: once through, not a loop), and played so far.
            double frames=0,played=0;
        };

        // The voices and the limiter, mixed a block at a time.
        struct Mixer
        {
            std::mutex lock;
            std::vector<Voice> voices;
            float gain=1;
            std::mt19937 rng{std::random_device{}()};

            void Add(HexWarSound sound,float pan,float loud,double seconds)
            {
                std::lock_guard guard(lock);
                const auto& takes=Takes(sound);
                if(takes.empty())return;
                const Samples& pcm=takes[std::uniform_int_distribution<size_t>(0,takes.size()-1)(rng)];
                Voice v;
                v.pcm=&pcm;
                v.frames=seconds*Rate;
                // (A loop starts anywhere in its take.)
                if(v.frames>0)v.at=std::uniform_real_distribution<double>(0,static_cast<double>(pcm.size()-2))(rng);
                v.step=std::uniform_real_distribution<double>(0.94,1.06)(rng);
                const float k=Volume*KindLoud[static_cast<int>(sound)]*std::clamp(loud,0.0f,1.0f)*std::uniform_real_distribution<float>(0.85f,1.0f)(rng);
                const float angle=(std::clamp(pan,-1.0f,1.0f)+1)*0.25f*3.14159265f;   // equal power
                v.left=k*std::cos(angle)*1.41421356f;
                v.right=k*std::sin(angle)*1.41421356f;
                if(voices.size()>=MostVoices)voices.erase(voices.begin());
                voices.push_back(v);
            }

            // frames of stereo 16-bit samples.
            void Mix(std::int16_t* out,int frames)
            {
                std::lock_guard guard(lock);
                for(int i=0;i<frames;++i)
                {
                    float l=0,r=0;
                    for(Voice& v:voices)
                    {
                        if(!v.pcm)continue;
                        if(v.frames>0&&v.played>=v.frames){v.pcm=nullptr;continue;}
                        if(static_cast<size_t>(v.at)+1>=v.pcm->size())
                        {
                            if(v.frames<=0){v.pcm=nullptr;continue;}
                            v.at-=static_cast<double>(v.pcm->size()-1);   // round the loop
                        }
                        const size_t at=static_cast<size_t>(v.at);
                        const float f=static_cast<float>(v.at-at);
                        float x=(*v.pcm)[at]*(1-f)+(*v.pcm)[at+1]*f;
                        if(v.frames>0)
                        {
                            x*=static_cast<float>(std::min({1.0,v.played/(FadeIn*Rate),(v.frames-v.played)/(FadeOut*Rate)}));
                            v.played+=1;
                        }
                        l+=x*v.left;
                        r+=x*v.right;
                        v.at+=v.step;
                    }
                    const float peak=std::max(std::abs(l),std::abs(r));
                    const float want=peak*gain>Ceiling?Ceiling/peak:1.0f;
                    gain=want<gain?want:std::min(1.0f,gain+Release);
                    out[i*2]=static_cast<std::int16_t>(std::clamp(l*gain,-1.0f,1.0f)*32767);
                    out[i*2+1]=static_cast<std::int16_t>(std::clamp(r*gain,-1.0f,1.0f)*32767);
                }
                voices.erase(std::remove_if(voices.begin(),voices.end(),[](const Voice& v){return !v.pcm;}),voices.end());
            }
        };

        Mixer mixer;

        // The output: a few blocks kept queued, refilled by a thread of its own as each plays out.
        struct Output
        {
            HWAVEOUT device=nullptr;
            HANDLE done=nullptr;
            std::array<WAVEHDR,Blocks> headers{};
            std::array<std::vector<std::int16_t>,Blocks> blocks;
            std::thread thread;
            std::atomic<bool> running{false};
            // The program ending with the window still open: the thread stopped, not left running.
            ~Output()
            {
                running=false;
                if(done)SetEvent(done);
                if(thread.joinable())thread.join();
            }
        };
        Output output;
        std::mutex outputLock;

        void Feed(WAVEHDR& h)
        {
            mixer.Mix(reinterpret_cast<std::int16_t*>(h.lpData),BlockFrames);
            waveOutWrite(output.device,&h,sizeof(h));
        }

        bool Open()
        {
            if(output.running)return true;
            WAVEFORMATEX format{};
            format.wFormatTag=WAVE_FORMAT_PCM;
            format.nChannels=2;
            format.nSamplesPerSec=Rate;
            format.wBitsPerSample=16;
            format.nBlockAlign=4;
            format.nAvgBytesPerSec=Rate*4;
            output.done=CreateEventW(nullptr,FALSE,FALSE,nullptr);
            if(waveOutOpen(&output.device,WAVE_MAPPER,&format,reinterpret_cast<DWORD_PTR>(output.done),0,CALLBACK_EVENT)!=MMSYSERR_NOERROR)
            {
                output.device=nullptr;
                CloseHandle(output.done);
                output.done=nullptr;
                return false;
            }
            for(int i=0;i<Blocks;++i)
            {
                output.blocks[i].assign(static_cast<size_t>(BlockFrames)*2,0);
                output.headers[i]={};
                output.headers[i].lpData=reinterpret_cast<LPSTR>(output.blocks[i].data());
                output.headers[i].dwBufferLength=static_cast<DWORD>(output.blocks[i].size()*sizeof(std::int16_t));
                waveOutPrepareHeader(output.device,&output.headers[i],sizeof(WAVEHDR));
            }
            output.running=true;
            for(auto& h:output.headers)Feed(h);
            output.thread=std::thread([]{
                while(output.running)
                {
                    WaitForSingleObject(output.done,100);
                    for(auto& h:output.headers)
                        if(output.running&&(h.dwFlags&WHDR_DONE))Feed(h);
                }
            });
            return true;
        }
    }

    void PlayHexWarSound(HexWarSound sound,float pan,float loud,double seconds)
    {
        {
            std::lock_guard guard(outputLock);
            if(!Open())return;
        }
        mixer.Add(sound,pan,loud,seconds);
    }

    void StopHexWarSounds()
    {
        std::lock_guard guard(outputLock);
        if(!output.running)return;
        output.running=false;
        SetEvent(output.done);
        if(output.thread.joinable())output.thread.join();
        waveOutReset(output.device);
        for(auto& h:output.headers)waveOutUnprepareHeader(output.device,&h,sizeof(WAVEHDR));
        waveOutClose(output.device);
        output.device=nullptr;
        CloseHandle(output.done);
        output.done=nullptr;
        std::lock_guard voices(mixer.lock);
        mixer.voices.clear();
        mixer.gain=1;
    }

    bool SaveHexWarSoundTest(const std::wstring& path)
    {
        // When each sound starts (seconds), as a battle of two full tank units would play.
        struct Cue {double at;HexWarSound sound;float pan;};
        std::vector<Cue> cues;
        for(int i=0;i<10;++i)
        {
            cues.push_back({0.7+0.2*i,HexWarSound::Cannon,-0.6f});
            cues.push_back({0.8+0.2*i,HexWarSound::Rifle,0.6f});
        }
        for(int i=0;i<6;++i)cues.push_back({0.92+0.4*i,HexWarSound::Blast,i%2?-0.6f:0.6f});
        cues.push_back({0.0,HexWarSound::Tracks,-0.6f});
        cues.push_back({0.0,HexWarSound::Steps,0.6f});
        std::sort(cues.begin(),cues.end(),[](const Cue& a,const Cue& b){return a.at<b.at;});
        const int frames=static_cast<int>(7.0*Rate);
        std::vector<std::int16_t> pcm(static_cast<size_t>(frames)*2);
        size_t next=0;
        for(int at=0;at<frames;at+=BlockFrames/4)
        {
            while(next<cues.size()&&cues[next].at*Rate<=at){mixer.Add(cues[next].sound,cues[next].pan,1,cues[next].sound>=HexWarSound::Tracks?0.8:0);++next;}
            mixer.Mix(pcm.data()+static_cast<size_t>(at)*2,std::min(BlockFrames/4,frames-at));
        }
        std::ofstream out(path,std::ios::binary);
        if(!out)return false;
        const std::uint32_t data=static_cast<std::uint32_t>(pcm.size()*2),riff=36+data,fmt=16,rate=Rate,bytes=Rate*4;
        const std::uint16_t pcmTag=1,stereo=2,align=4,bits=16;
        out.write("RIFF",4);out.write(reinterpret_cast<const char*>(&riff),4);out.write("WAVEfmt ",8);
        out.write(reinterpret_cast<const char*>(&fmt),4);out.write(reinterpret_cast<const char*>(&pcmTag),2);out.write(reinterpret_cast<const char*>(&stereo),2);
        out.write(reinterpret_cast<const char*>(&rate),4);out.write(reinterpret_cast<const char*>(&bytes),4);
        out.write(reinterpret_cast<const char*>(&align),2);out.write(reinterpret_cast<const char*>(&bits),2);
        out.write("data",4);out.write(reinterpret_cast<const char*>(&data),4);
        out.write(reinterpret_cast<const char*>(pcm.data()),static_cast<std::streamsize>(data));
        return static_cast<bool>(out);
    }
}
