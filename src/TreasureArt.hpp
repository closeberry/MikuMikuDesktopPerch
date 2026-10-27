#pragma once
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <string>

namespace MikuMikuDesktopPerch
{
    // The dungeon's treasures as small pictures (docs/Treasures.md), drawn here with shapes,
    // gradients and a sparkle: no image files, nobody else's artwork. id: the treasure's id
    // (its English name); an unknown one gets a plain gem. Drawn within box, on a clear
    // background (GDI+ must be started).
    void PaintTreasure(Gdiplus::Graphics& g,const std::wstring& id,const Gdiplus::RectF& box);

    // A treasure's picture over the desktop in a small window of its own (click-through, never
    // taking the focus, above the characters), resting on a point (her palm) with a soft
    // shadow under it.
    class TreasurePicture
    {
    public:
        TreasurePicture()=default;
        TreasurePicture(const TreasurePicture&)=delete;
        TreasurePicture& operator=(const TreasurePicture&)=delete;
        ~TreasurePicture();
        // Its bottom middle at bottom (screen pixels), size pixels square, alpha 0..255; drawn
        // again only when the treasure or the size changes.
        void Place(const std::wstring& id,POINT bottom,int size,BYTE alpha);
        void Hide();   // the window and its bitmap go

    private:
        void Draw();
        HWND window=nullptr;
        HDC memory=nullptr;
        HBITMAP bitmap=nullptr;
        HGDIOBJ previous=nullptr;
        void* bits=nullptr;
        std::wstring id;
        int size=0;
    };
}
