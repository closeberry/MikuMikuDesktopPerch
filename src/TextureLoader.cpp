#define NOMINMAX
#include "TextureLoader.hpp"
#include "TgaLoader.hpp"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
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

    namespace
    {
        // Mark the texels whose centre lies inside a triangle (UV wraps), plus the texels under
        // the corners so thin triangles keep their own colour. Border texels just outside the
        // edges stay unmarked: filtering reads them, so they are filled like the padding.
        std::vector<std::uint8_t> Coverage(unsigned w,unsigned h,const std::vector<std::array<float,2>>& uv)
        {
            std::vector<std::uint8_t> mask(static_cast<size_t>(w)*h,0);
            auto mark=[&](long x,long y){
                x%=static_cast<long>(w);if(x<0)x+=w;
                y%=static_cast<long>(h);if(y<0)y+=h;
                mask[static_cast<size_t>(y)*w+x]=1;
            };
            for(size_t t=0;t+2<uv.size();t+=3)
            {
                // Texel space, shifted so the triangle starts in the first tile.
                const float shiftU=std::floor(std::min({uv[t][0],uv[t+1][0],uv[t+2][0]}));
                const float shiftV=std::floor(std::min({uv[t][1],uv[t+1][1],uv[t+2][1]}));
                float px[3],py[3];
                for(int k=0;k<3;++k){px[k]=(uv[t+k][0]-shiftU)*w;py[k]=(uv[t+k][1]-shiftV)*h;}
                const float area=(px[1]-px[0])*(py[2]-py[0])-(px[2]-px[0])*(py[1]-py[0]);
                const long x0=static_cast<long>(std::floor(std::min({px[0],px[1],px[2]})))-1,x1=static_cast<long>(std::ceil(std::max({px[0],px[1],px[2]})))+1;
                const long y0=static_cast<long>(std::floor(std::min({py[0],py[1],py[2]})))-1,y1=static_cast<long>(std::ceil(std::max({py[0],py[1],py[2]})))+1;
                if(x1-x0>static_cast<long>(4*w)||y1-y0>static_cast<long>(4*h))continue;   // broken UVs
                for(long y=y0;y<=y1;++y)
                for(long x=x0;x<=x1;++x)
                {
                    // Texel centre inside the triangle.
                    const float cx=x+0.5f,cy=y+0.5f;
                    bool inside=true;
                    for(int e=0;e<3&&inside;++e)
                    {
                        const int a=e,b=(e+1)%3;
                        const float ex=px[b]-px[a],ey=py[b]-py[a];
                        const float len=std::sqrt(ex*ex+ey*ey);
                        if(len<1e-6f)continue;
                        float d=(ex*(cy-py[a])-ey*(cx-px[a]))/len;
                        if(area<0)d=-d;
                        inside=d>=0.0f;
                    }
                    if(inside)mark(x,y);
                }
                for(int k=0;k<3;++k)mark(static_cast<long>(std::floor(px[k])),static_cast<long>(std::floor(py[k])));
            }
            return mask;
        }

        // Fill every unused texel from the nearest used one.
        void FillPadding(ImageRgba8::Level& level,const std::vector<std::uint8_t>& used)
        {
            const unsigned w=level.width,h=level.height;
            std::vector<std::int32_t> source(static_cast<size_t>(w)*h,-1);
            std::vector<std::uint32_t> frontier,next;
            for(std::uint32_t i=0;i<w*h;++i)if(used[i]){source[i]=static_cast<std::int32_t>(i);frontier.push_back(i);}
            if(frontier.empty())return;
            while(!frontier.empty())
            {
                next.clear();
                for(std::uint32_t i:frontier)
                {
                    const unsigned x=i%w,y=i/w;
                    const std::uint32_t around[4]={y*w+(x+w-1)%w,y*w+(x+1)%w,((y+h-1)%h)*w+x,((y+1)%h)*w+x};
                    for(std::uint32_t n:around)
                        if(source[n]<0){source[n]=source[i];next.push_back(n);}
                }
                frontier.swap(next);
            }
            for(std::uint32_t i=0;i<w*h;++i)
                if(!used[i]&&source[i]>=0)
                    std::copy_n(&level.pixels[static_cast<size_t>(source[i])*4],4,&level.pixels[static_cast<size_t>(i)*4]);
        }
    }

    bool LoadImageRgba8(const std::wstring& path,ImageRgba8& image,unsigned maxSize,const std::vector<std::array<float,2>>* uvTriangles)
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
        // All of the unused area: a model drawn small samples the small mips, where one texel
        // averages a wide block, and any transparent padding left in it shows as lines.
        if(image.hasAlpha&&uvTriangles&&!uvTriangles->empty())FillPadding(level,Coverage(level.width,level.height,*uvTriangles));
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
