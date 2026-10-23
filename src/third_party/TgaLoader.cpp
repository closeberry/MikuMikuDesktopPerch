#define NOMINMAX
// Extracted from MotionTraceEditor PreviewTextureFileLoader.cpp (TGA decoder only).
#include "TgaLoader.hpp"

#include <filesystem>
#include <fstream>

namespace MotionTraceEditor::PreviewTextureFileLoader
{
    bool LoadTgaPixels(
        const std::wstring& path,
        std::vector<std::uint8_t>& rgba,
        UINT& width,UINT& height)
    {
        std::ifstream file(
            std::filesystem::path(path),
            std::ios::binary);
        if(!file) return false;

        std::uint8_t header[18]{};
        file.read(
            reinterpret_cast<char*>(header),
            sizeof(header));
        if(file.gcount()!=sizeof(header))
            return false;

        const std::uint8_t idLength=header[0];
        const std::uint8_t colorMapType=header[1];
        const std::uint8_t imageType=header[2];
        const std::uint16_t imageWidth=
            static_cast<std::uint16_t>(
                header[12]|
                (header[13]<<8));
        const std::uint16_t imageHeight=
            static_cast<std::uint16_t>(
                header[14]|
                (header[15]<<8));
        const std::uint8_t bitsPerPixel=header[16];
        const std::uint8_t descriptor=header[17];

        if(colorMapType!=0||
           (imageType!=2&&imageType!=10)||
           (bitsPerPixel!=24&&bitsPerPixel!=32)||
           imageWidth==0||imageHeight==0)
            return false;

        file.seekg(idLength,std::ios::cur);
        if(!file) return false;

        width=imageWidth;
        height=imageHeight;
        const size_t pixelCount=
            static_cast<size_t>(width)*
            static_cast<size_t>(height);
        rgba.assign(pixelCount*4,0);

        const int sourceBytes=bitsPerPixel/8;
        const bool topOrigin=(descriptor&0x20)!=0;
        const bool rightOrigin=(descriptor&0x10)!=0;

        size_t decoded=0;
        auto writePixel=[&](const std::uint8_t* source)
        {
            if(decoded>=pixelCount)
                return false;

            const size_t sourceX=decoded%width;
            const size_t sourceY=decoded/width;
            const size_t x=rightOrigin
                ?static_cast<size_t>(width-1)-sourceX
                :sourceX;
            const size_t y=topOrigin
                ?sourceY
                :static_cast<size_t>(height-1)-sourceY;
            const size_t destination=(y*width+x)*4;

            rgba[destination+0]=source[2];
            rgba[destination+1]=source[1];
            rgba[destination+2]=source[0];
            rgba[destination+3]=sourceBytes==4?source[3]:255;
            ++decoded;
            return true;
        };

        if(imageType==2)
        {
            std::uint8_t pixel[4]{};
            while(decoded<pixelCount)
            {
                file.read(
                    reinterpret_cast<char*>(pixel),
                    sourceBytes);
                if(!file||!writePixel(pixel))
                    return false;
            }
        }
        else
        {
            std::uint8_t pixel[4]{};
            while(decoded<pixelCount)
            {
                std::uint8_t packet=0;
                file.read(reinterpret_cast<char*>(&packet),1);
                if(!file) return false;

                const size_t count=
                    static_cast<size_t>(packet&0x7Fu)+1;
                if(packet&0x80u)
                {
                    file.read(
                        reinterpret_cast<char*>(pixel),
                        sourceBytes);
                    if(!file) return false;
                    for(size_t index=0;index<count;++index)
                    {
                        if(!writePixel(pixel))
                            return false;
                    }
                }
                else
                {
                    for(size_t index=0;index<count;++index)
                    {
                        file.read(
                            reinterpret_cast<char*>(pixel),
                            sourceBytes);
                        if(!file||!writePixel(pixel))
                            return false;
                    }
                }
            }
        }

        return decoded==pixelCount;
    }
}
