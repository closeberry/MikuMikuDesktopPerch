#define NOMINMAX
#include "Album.hpp"
#include "Text.hpp"
#include <wincodec.h>
#include <wrl/client.h>
#include <mmsystem.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#pragma comment(lib,"windowscodecs.lib")
#pragma comment(lib,"winmm.lib")

using Microsoft::WRL::ComPtr;

namespace MikuMikuDesktopPerch
{
    namespace
    {
        constexpr int Margin=24;                        // pixels of background around what is drawn
        constexpr long long MaxPixels=64LL*1024*1024;   // a picture spanning every monitor stays sane

        std::string ToUtf8(const std::wstring& text)
        {
            if(text.empty())return {};
            const int n=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
            std::string out(static_cast<size_t>(n),'\0');
            WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),out.data(),n,nullptr,nullptr);
            return out;
        }

        // Where a layer has anything drawn (alpha above a trace), in screen pixels; false if nowhere.
        bool DrawnBounds(const AlbumLayer& layer,RECT& out)
        {
            const int w=layer.rect.right-layer.rect.left,h=layer.rect.bottom-layer.rect.top;
            if(w<=0||h<=0||layer.pixels.size()<static_cast<size_t>(w)*h*4)return false;
            int left=w,top=h,right=-1,bottom=-1;
            for(int y=0;y<h;++y)
            {
                const std::uint8_t* row=layer.pixels.data()+static_cast<size_t>(y)*w*4;
                for(int x=0;x<w;++x)
                    if(row[x*4+3]>8){left=std::min(left,x);right=std::max(right,x);top=std::min(top,y);bottom=std::max(bottom,y);}
            }
            if(right<0)return false;
            out={layer.rect.left+left,layer.rect.top+top,layer.rect.left+right+1,layer.rect.top+bottom+1};
            return true;
        }

        // The layers over a soft background, cropped to what they draw; false if nothing is drawn.
        bool Compose(const std::vector<const AlbumLayer*>& layers,std::vector<std::uint8_t>& canvas,int& width,int& height)
        {
            RECT area{};
            bool any=false;
            for(const auto* layer:layers)
                if(RECT r{};DrawnBounds(*layer,r))
                {
                    if(!any)area=r;
                    else UnionRect(&area,&area,&r);
                    any=true;
                }
            if(!any)return false;
            InflateRect(&area,Margin,Margin);
            width=area.right-area.left;
            height=area.bottom-area.top;
            if(static_cast<long long>(width)*height>MaxPixels)return false;
            canvas.assign(static_cast<size_t>(width)*height*4,0);
            // A pale sky, lighter at the top.
            for(int y=0;y<height;++y)
            {
                const float t=height>1?static_cast<float>(y)/(height-1):0;
                const std::uint8_t b=static_cast<std::uint8_t>(252-14*t),g=static_cast<std::uint8_t>(248-18*t),r=static_cast<std::uint8_t>(244-22*t);
                for(int x=0;x<width;++x)
                {
                    std::uint8_t* p=canvas.data()+(static_cast<size_t>(y)*width+x)*4;
                    p[0]=b;p[1]=g;p[2]=r;p[3]=255;
                }
            }
            for(const auto* layer:layers)
            {
                const int w=layer->rect.right-layer->rect.left,h=layer->rect.bottom-layer->rect.top;
                if(w<=0||h<=0||layer->pixels.size()<static_cast<size_t>(w)*h*4)continue;
                for(int y=0;y<h;++y)
                {
                    const int cy=layer->rect.top+y-area.top;
                    if(cy<0||cy>=height)continue;
                    for(int x=0;x<w;++x)
                    {
                        const int cx=layer->rect.left+x-area.left;
                        if(cx<0||cx>=width)continue;
                        const std::uint8_t* s=layer->pixels.data()+(static_cast<size_t>(y)*w+x)*4;
                        if(s[3]==0)continue;
                        std::uint8_t* d=canvas.data()+(static_cast<size_t>(cy)*width+cx)*4;
                        // Premultiplied "over".
                        for(int c=0;c<4;++c)d[c]=static_cast<std::uint8_t>(std::min(255,s[c]+d[c]*(255-s[3])/255));
                    }
                }
            }
            return true;
        }

        std::wstring SafeName(std::wstring name)
        {
            for(auto& c:name)if(c<32||std::wstring(L"\\/:*?\"<>|").find(c)!=std::wstring::npos)c=L'_';
            return name;
        }

        // folder\name.png, or name (2).png and so on when it is taken.
        std::filesystem::path FreeFile(const std::filesystem::path& folder,const std::wstring& name)
        {
            std::error_code ec;
            std::filesystem::path file=folder/(name+L".png");
            for(int n=2;std::filesystem::exists(file,ec)&&n<1000;++n)file=folder/(name+L" ("+std::to_wstring(n)+L").png");
            return file;
        }

        std::wstring chosenFolder;   // SetAlbumFolder (empty: the default)

        std::wstring OneLine(std::wstring text)
        {
            for(auto& c:text)if(c==L'\t'||c==L'\r'||c==L'\n')c=L' ';
            return text;
        }
    }

    std::wstring DefaultAlbumFolder()
    {
        wchar_t base[MAX_PATH]{};
        if(!GetEnvironmentVariableW(L"LOCALAPPDATA",base,MAX_PATH))return {};
        return (std::filesystem::path(base)/L"MikuMikuDesktopPerch"/L"album").wstring();
    }

    void SetAlbumFolder(const std::wstring& folder){chosenFolder=folder;}

    std::wstring AlbumFolder(bool create)
    {
        const std::wstring folder=chosenFolder.empty()?DefaultAlbumFolder():chosenFolder;
        if(create&&!folder.empty()){std::error_code ec;std::filesystem::create_directories(folder,ec);}
        return folder;
    }

    int CopyAlbum(const std::wstring& from,const std::wstring& to,std::wstring& error)
    {
        namespace fs=std::filesystem;
        std::error_code ec;
        if(!fs::is_directory(from,ec))return 0;
        int copied=0;
        for(auto it=fs::recursive_directory_iterator(from,ec);!ec&&it!=fs::recursive_directory_iterator();it.increment(ec))
        {
            const fs::path relative=fs::relative(it->path(),from,ec);
            if(ec)break;
            const fs::path target=fs::path(to)/relative;
            if(it->is_directory(ec)){fs::create_directories(target,ec);continue;}
            if(!it->is_regular_file(ec))continue;
            if(relative==L"album.txt")
            {
                // The lines (pictures) the new one lacks, in their order.
                auto lines=[](const fs::path& file){
                    std::vector<std::string> out;
                    std::ifstream in(file,std::ios::binary);
                    for(std::string line;std::getline(in,line);)if(!line.empty())out.push_back(line);
                    return out;
                };
                const auto have=lines(target);
                std::ofstream out(target,std::ios::binary|std::ios::app);
                for(const auto& line:lines(it->path()))
                    if(std::find(have.begin(),have.end(),line)==have.end())out<<line<<"\n";
                continue;
            }
            if(fs::exists(target,ec))continue;   // never over a file already there
            fs::create_directories(target.parent_path(),ec);
            if(!fs::copy_file(it->path(),target,fs::copy_options::skip_existing,ec)){error=L"could not copy "+it->path().wstring();return -1;}
            ++copied;
        }
        if(ec){error=L"could not read "+from;return -1;}
        return copied;
    }

    bool WritePng(const std::wstring& path,UINT width,UINT height,UINT pitch,const std::uint8_t* pixels)
    {
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID format=GUID_WICPixelFormat32bppPBGRA;
        return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic)))&&
            SUCCEEDED(wic->CreateStream(&stream))&&SUCCEEDED(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE))&&
            SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder))&&SUCCEEDED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache))&&
            SUCCEEDED(encoder->CreateNewFrame(&frame,nullptr))&&SUCCEEDED(frame->Initialize(nullptr))&&
            SUCCEEDED(frame->SetSize(width,height))&&SUCCEEDED(frame->SetPixelFormat(&format))&&
            SUCCEEDED(frame->WritePixels(height,pitch,pitch*height,const_cast<BYTE*>(pixels)))&&SUCCEEDED(frame->Commit())&&SUCCEEDED(encoder->Commit());
    }

    std::wstring SaveAlbumShot(const AlbumShot& shot,std::wstring& error)
    {
        const std::wstring root=AlbumFolder(true);
        if(root.empty()){error=L"no LOCALAPPDATA";return {};}
        const std::filesystem::path folder=std::filesystem::path(root)/(shot.automatic?L"auto":L"manual");
        std::error_code ec;
        std::filesystem::create_directories(folder,ec);

        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t stamp[40]{};
        if(shot.automatic)swprintf_s(stamp,L"%04d-%02d-%02d ",now.wYear,now.wMonth,now.wDay);
        else swprintf_s(stamp,L"%04d-%02d-%02d %02d-%02d-%02d",now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond);
        const std::wstring name=SafeName(stamp+(shot.automatic?shot.label:std::wstring()));

        std::vector<const AlbumLayer*> plain,full;
        for(const auto& c:shot.characters){plain.push_back(&c);full.push_back(&c);}
        for(const auto& b:shot.bubbles)full.push_back(&b);
        std::vector<std::uint8_t> canvas;
        int width=0,height=0;
        if(!Compose(full,canvas,width,height)){error=L"nothing drawn to take";return {};}
        const std::filesystem::path file=FreeFile(folder,name);
        if(!WritePng(file.wstring(),width,height,width*4,canvas.data())){error=L"could not write "+file.wstring();return {};}
        // Without the bubbles too (for sharing: what she says may be personal).
        if(!shot.bubbles.empty()&&Compose(plain,canvas,width,height))
        {
            const std::filesystem::path bare=FreeFile(folder,file.stem().wstring()+T(L"（吹き出しなし）",L" (no bubble)"));
            WritePng(bare.wstring(),width,height,width*4,canvas.data());
        }

        wchar_t when[32]{};
        swprintf_s(when,L"%04d-%02d-%02d %02d:%02d:%02d",now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond);
        std::wstring who,said;
        for(const auto& [speaker,words]:shot.words)
        {
            who+=(who.empty()?L"":L", ")+speaker;
            said+=(said.empty()?L"":L" / ")+(shot.words.size()>1?speaker+L": ":L"")+words;
        }
        const std::wstring line=std::wstring(when)+L"\t"+(shot.automatic?L"auto":L"manual")+L"\t"+
                                std::filesystem::relative(file,root,ec).wstring()+L"\t"+OneLine(who)+L"\t"+OneLine(said)+L"\n";
        std::ofstream log(std::filesystem::path(root)/L"album.txt",std::ios::binary|std::ios::app);
        const std::string bytes=ToUtf8(line);
        log.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
        return file.wstring();
    }

    void PlayShutter()
    {
        // Two short clicks, the shutter opening and closing: noise and a ring, quickly dying out.
        static const std::vector<std::uint8_t> wave=[]{
            constexpr int Rate=44100;
            const int samples=Rate*16/100;
            std::vector<std::int16_t> pcm(static_cast<size_t>(samples));
            std::uint32_t noise=0x1234567u;
            for(int i=0;i<samples;++i)
            {
                const double t=static_cast<double>(i)/Rate;
                double v=0;
                for(const auto& [at,gain]:{std::pair{0.0,1.0},std::pair{0.075,0.75}})
                {
                    if(t<at)continue;
                    const double u=t-at;
                    noise^=noise<<13;noise^=noise>>17;noise^=noise<<5;
                    const double n=(noise&0xFFFF)/32768.0-1.0;
                    v+=gain*std::exp(-u/0.006)*(0.7*n+0.5*std::sin(2*3.14159265*2300*u));
                }
                pcm[static_cast<size_t>(i)]=static_cast<std::int16_t>(std::clamp(v*0.35,-1.0,1.0)*32767);
            }
            const std::uint32_t data=static_cast<std::uint32_t>(pcm.size()*2);
            std::vector<std::uint8_t> out(44+data);
            auto put32=[&](size_t at,std::uint32_t v){for(int k=0;k<4;++k)out[at+k]=static_cast<std::uint8_t>(v>>(8*k));};
            auto put16=[&](size_t at,std::uint16_t v){out[at]=static_cast<std::uint8_t>(v);out[at+1]=static_cast<std::uint8_t>(v>>8);};
            std::copy_n("RIFF",4,out.begin());put32(4,36+data);std::copy_n("WAVEfmt ",8,out.begin()+8);
            put32(16,16);put16(20,1);put16(22,1);put32(24,Rate);put32(28,Rate*2);put16(32,2);put16(34,16);
            std::copy_n("data",4,out.begin()+36);put32(40,data);
            std::memcpy(out.data()+44,pcm.data(),data);
            return out;
        }();
        PlaySoundW(reinterpret_cast<LPCWSTR>(wave.data()),nullptr,SND_MEMORY|SND_ASYNC|SND_NODEFAULT);
    }
}
