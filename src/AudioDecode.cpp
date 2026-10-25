#define NOMINMAX
#include "AudioDecode.hpp"
#include "Text.hpp"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmreg.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <climits>
#include <cstdint>
#include <cstring>

#pragma comment(lib,"mfplat.lib")
#pragma comment(lib,"mfreadwrite.lib")
#pragma comment(lib,"mfuuid.lib")
#pragma comment(lib,"shlwapi.lib")

using Microsoft::WRL::ComPtr;

namespace MikuMikuDesktopPerch
{
    namespace
    {
        constexpr double MaxSeconds=600;

        // What the bytes are, from their first bytes: Media Foundation picks its reader by
        // content type when there is no file name to go by.
        const wchar_t* ContentType(const std::string& b)
        {
            auto at=[&](size_t i,const char* s){return b.size()>=i+std::strlen(s)&&b.compare(i,std::strlen(s),s)==0;};
            if(at(0,"RIFF")&&at(8,"WAVE"))return L"audio/wav";
            if(at(0,"fLaC"))return L"audio/flac";
            if(at(0,"OggS"))return L"audio/ogg";
            if(at(4,"ftyp"))return L"audio/mp4";
            if(at(0,"ID3"))return L"audio/mpeg";
            if(b.size()>=2&&static_cast<unsigned char>(b[0])==0xFF&&(static_cast<unsigned char>(b[1])&0xE0)==0xE0)
                return (static_cast<unsigned char>(b[1])&0x06)==0?L"audio/aac":L"audio/mpeg";   // layer 0: ADTS AAC
            return nullptr;
        }

        void Put32(std::string& s,std::uint32_t v){s.append(reinterpret_cast<const char*>(&v),4);}
        void Put16(std::string& s,std::uint16_t v){s.append(reinterpret_cast<const char*>(&v),2);}
    }

    std::wstring DecodeAudio(const std::string& bytes,std::string& wav)
    {
        const std::wstring unreadable=T(L"音声を読み込めませんでした（MP3、AAC、WAV、FLAC などに対応）。",
                                        L"Could not read the sound (MP3, AAC, WAV, FLAC and the like can be read).");
        static const bool started=SUCCEEDED(MFStartup(MF_VERSION,MFSTARTUP_LITE));
        if(!started)return T(L"音声の再生機能（Media Foundation）を使えません。",L"Media Foundation (for sound) is not available.");
        if(bytes.empty()||bytes.size()>UINT_MAX)return unreadable;

        ComPtr<IStream> stream;
        stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()),static_cast<UINT>(bytes.size())));
        ComPtr<IMFByteStream> byteStream;
        if(!stream||FAILED(MFCreateMFByteStreamOnStream(stream.Get(),&byteStream)))return unreadable;
        if(const wchar_t* type=ContentType(bytes))
            if(ComPtr<IMFAttributes> attributes;SUCCEEDED(byteStream.As(&attributes)))attributes->SetString(MF_BYTESTREAM_CONTENT_TYPE,type);
        ComPtr<IMFSourceReader> reader;
        if(FAILED(MFCreateSourceReaderFromByteStream(byteStream.Get(),nullptr,&reader)))return unreadable;
        constexpr DWORD Audio=static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS),FALSE);
        if(FAILED(reader->SetStreamSelection(Audio,TRUE)))return unreadable;

        // Decoded to 16-bit PCM at the sound's own rate.
        ComPtr<IMFMediaType> wanted;
        MFCreateMediaType(&wanted);
        wanted->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio);
        wanted->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_PCM);
        wanted->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16);
        ComPtr<IMFMediaType> actual;
        if(FAILED(reader->SetCurrentMediaType(Audio,nullptr,wanted.Get()))||FAILED(reader->GetCurrentMediaType(Audio,&actual)))return unreadable;
        WAVEFORMATEX* format=nullptr;
        UINT32 formatSize=0;
        if(FAILED(MFCreateWaveFormatExFromMFMediaType(actual.Get(),&format,&formatSize))||!format)return unreadable;
        const WORD channels=format->nChannels,bits=format->wBitsPerSample;
        const DWORD rate=format->nSamplesPerSec;
        CoTaskMemFree(format);
        if(bits!=16||channels<1||channels>2||rate<8000||rate>192000)
            return T(L"この音声の形式には対応していません（モノラルかステレオのみ）。",L"This sound's format is not supported (mono or stereo only).");

        const std::uint32_t blockAlign=channels*2u,bytesPerSecond=rate*blockAlign;
        std::string pcm;
        for(;;)
        {
            DWORD flags=0;
            ComPtr<IMFSample> sample;
            if(FAILED(reader->ReadSample(Audio,0,nullptr,&flags,nullptr,&sample)))return unreadable;
            if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;
            if(!sample)continue;
            ComPtr<IMFMediaBuffer> buffer;
            if(FAILED(sample->ConvertToContiguousBuffer(&buffer)))return unreadable;
            BYTE* data=nullptr;
            DWORD length=0;
            if(SUCCEEDED(buffer->Lock(&data,nullptr,&length)))
            {
                pcm.append(reinterpret_cast<const char*>(data),length);
                buffer->Unlock();
            }
            if(pcm.size()>MaxSeconds*bytesPerSecond)
                return T(L"音声が長すぎます（10分まで）。",L"The sound is too long (10 minutes at most).");
        }
        pcm.resize(pcm.size()/blockAlign*blockAlign);
        if(pcm.empty())return unreadable;

        wav.clear();
        wav.reserve(pcm.size()+44);
        wav+="RIFF";
        Put32(wav,static_cast<std::uint32_t>(36+pcm.size()));
        wav+="WAVEfmt ";
        Put32(wav,16);
        Put16(wav,WAVE_FORMAT_PCM);
        Put16(wav,channels);
        Put32(wav,rate);
        Put32(wav,bytesPerSecond);
        Put16(wav,static_cast<std::uint16_t>(blockAlign));
        Put16(wav,16);
        wav+="data";
        Put32(wav,static_cast<std::uint32_t>(pcm.size()));
        wav+=pcm;
        return {};
    }
}
