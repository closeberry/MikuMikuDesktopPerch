#define NOMINMAX
#include "TextureLoader.hpp"
#include "TgaLoader.hpp"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>

namespace MikuMikuDesktopPerch
{
    using Microsoft::WRL::ComPtr;

    namespace
    {
        bool LoadWic(const std::wstring& path,ImageRgba8::Level& level)
        {
            ComPtr<IWICImagingFactory> factory;
            if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))))
                return false;
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            ComPtr<IWICFormatConverter> converter;
            UINT width=0,height=0;
            if(FAILED(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder))||
               FAILED(decoder->GetFrame(0,&frame))||FAILED(frame->GetSize(&width,&height))||
               !width||!height||width>16384||height>16384||
               FAILED(factory->CreateFormatConverter(&converter))||
               FAILED(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))
                return false;
            level.width=width;
            level.height=height;
            level.pixels.resize(static_cast<size_t>(width)*height*4);
            return SUCCEEDED(converter->CopyPixels(nullptr,width*4,static_cast<UINT>(level.pixels.size()),level.pixels.data()));
        }
    }

    bool LoadImageRgba8(const std::wstring& path,ImageRgba8& image,unsigned maxSize)
    {
        auto extension=std::filesystem::path(path).extension().wstring();
        std::transform(extension.begin(),extension.end(),extension.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});
        ImageRgba8::Level level;
        bool loaded=false;
        if(extension==L".tga")
        {
            UINT width=0,height=0;
            loaded=MotionTraceEditor::PreviewTextureFileLoader::LoadTgaPixels(path,level.pixels,width,height);
            level.width=width;
            level.height=height;
        }
        else
        {
            loaded=LoadWic(path,level);
        }
        if(!loaded)return false;
        image.hasAlpha=false;
        for(size_t i=3;i<level.pixels.size();i+=4)
            if(level.pixels[i]!=255){image.hasAlpha=true;break;}
        image.levels.clear();
        image.levels.push_back(std::move(level));
        GenerateMips(image);
        size_t drop=0;
        while(maxSize>0&&drop+1<image.levels.size()&&(image.levels[drop].width>maxSize||image.levels[drop].height>maxSize))++drop;
        image.levels.erase(image.levels.begin(),image.levels.begin()+static_cast<std::ptrdiff_t>(drop));
        return true;
    }

    void GenerateMips(ImageRgba8& image)
    {
        image.levels.resize(1);
        while(image.levels.back().width>1||image.levels.back().height>1)
        {
            const auto& source=image.levels.back();
            ImageRgba8::Level next;
            next.width=std::max(1u,source.width/2);
            next.height=std::max(1u,source.height/2);
            next.pixels.resize(static_cast<size_t>(next.width)*next.height*4);
            for(unsigned y=0;y<next.height;++y)
            for(unsigned x=0;x<next.width;++x)
            {
                const unsigned x0=std::min(x*2,source.width-1),x1=std::min(x*2+1,source.width-1);
                const unsigned y0=std::min(y*2,source.height-1),y1=std::min(y*2+1,source.height-1);
                const std::uint8_t* p[4]={
                    &source.pixels[(static_cast<size_t>(y0)*source.width+x0)*4],&source.pixels[(static_cast<size_t>(y0)*source.width+x1)*4],
                    &source.pixels[(static_cast<size_t>(y1)*source.width+x0)*4],&source.pixels[(static_cast<size_t>(y1)*source.width+x1)*4]};
                auto* out=&next.pixels[(static_cast<size_t>(y)*next.width+x)*4];
                for(int c=0;c<4;++c)
                    out[c]=static_cast<std::uint8_t>((p[0][c]+p[1][c]+p[2][c]+p[3][c]+2)/4);
            }
            image.levels.push_back(std::move(next));
        }
    }
}
