#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>

namespace Gdiplus {class Font;class Graphics;class StringFormat;}

namespace MikuMikuDesktopPerch
{
    // The font and size of the words in speech bubbles (the characters' on the desktop and hers
    // in the mini games), as the settings say. family: an installed font's name (empty: each
    // bubble's own); percent: the size, 100 as designed.
    void SetBubbleText(const std::wstring& family,int percent);
    std::wstring BubbleFamily();
    float BubbleTextScale();

    // A font of pixels in the family set if it is installed (and has the style), else in the
    // first of fallbacks installed, else the generic sans serif. The size set is the caller's
    // to apply (BubbleTextScale).
    std::unique_ptr<Gdiplus::Font> BubbleFont(float pixels,int style,std::initializer_list<const wchar_t*> fallbacks);

    // The fonts that can be set: the installed families GDI+ draws, by name, sorted.
    std::vector<std::wstring> InstalledFonts();

    // Diagnostics: words the mini games' window shots show in her bubble (--words).
    void SetSampleWords(const std::wstring& words);
    const std::wstring& SampleWords();

    // Her words in a mini game's window: a bubble as wide as the whole line needs (to most,
    // then wrapped), its tail out of its left side toward her.
    class WordsBubble
    {
    public:
        WordsBubble(Gdiplus::Graphics& g,const std::wstring& said,float s,float most);
        ~WordsBubble();
        float Width() const {return width;}
        float Height() const {return height;}
        // At (x,y), the tail pointing at height tailY, showing shown (the part said so far).
        void Draw(Gdiplus::Graphics& g,float x,float y,float tailY,const std::wstring& shown) const;

    private:
        std::unique_ptr<Gdiplus::Font> font;
        std::unique_ptr<Gdiplus::StringFormat> format;
        float s=1,pad=0,textWidth=0,textHeight=0,width=0,height=0;
    };
}
