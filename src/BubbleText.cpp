#define NOMINMAX
#include <algorithm>
#include <cmath>
#include "BubbleText.hpp"
#include <objidl.h>
// GDI+ uses min and max unqualified; give it the std ones instead of the windows.h macros.
namespace Gdiplus {using std::min;using std::max;}
#include <gdiplus.h>
#include <mutex>

#pragma comment(lib,"gdiplus.lib")

namespace MikuMikuDesktopPerch
{
    namespace
    {
        // Set from the main thread, read by the windows' own threads as they draw.
        std::mutex lock;
        std::wstring family;
        int percent=100;
        std::wstring sample;
    }

    void SetBubbleText(const std::wstring& name,int size)
    {
        std::lock_guard guard(lock);
        family=name;
        percent=std::clamp(size,50,300);
    }

    std::wstring BubbleFamily()
    {
        std::lock_guard guard(lock);
        return family;
    }

    float BubbleTextScale()
    {
        std::lock_guard guard(lock);
        return static_cast<float>(percent)/100.0f;
    }

    std::unique_ptr<Gdiplus::Font> BubbleFont(float pixels,int style,std::initializer_list<const wchar_t*> fallbacks)
    {
        using namespace Gdiplus;
        if(const std::wstring chosen=BubbleFamily();!chosen.empty())
        {
            FontFamily f(chosen.c_str());
            if(f.IsAvailable())
            {
                // A family without this style (a bold-only one, say) gives an unusable font.
                auto font=std::make_unique<Font>(&f,pixels,style,UnitPixel);
                if(font->IsAvailable())return font;
            }
        }
        for(const wchar_t* name:fallbacks)
        {
            FontFamily f(name);
            if(f.IsAvailable())return std::make_unique<Font>(&f,pixels,style,UnitPixel);
        }
        return std::make_unique<Font>(FontFamily::GenericSansSerif(),pixels,style,UnitPixel);
    }

    std::vector<std::wstring> InstalledFonts()
    {
        using namespace Gdiplus;
        std::vector<std::wstring> names;
        // GDI+ may not be up yet on this thread's account: its start-ups are counted.
        ULONG_PTR token=0;
        GdiplusStartupInput input;
        if(GdiplusStartup(&token,&input,nullptr)!=Ok)return names;
        {
            InstalledFontCollection installed;
            const INT count=installed.GetFamilyCount();
            std::vector<FontFamily> families(static_cast<size_t>(std::max(count,0)));
            INT found=0;
            installed.GetFamilies(count,families.data(),&found);
            for(INT i=0;i<found;++i)
            {
                wchar_t name[LF_FACESIZE]{};
                if(families[static_cast<size_t>(i)].GetFamilyName(name)==Ok&&name[0])names.push_back(name);
            }
        }
        GdiplusShutdown(token);
        std::sort(names.begin(),names.end());
        names.erase(std::unique(names.begin(),names.end()),names.end());
        return names;
    }

    void SetSampleWords(const std::wstring& words){sample=words;}
    const std::wstring& SampleWords(){return sample;}

    WordsBubble::WordsBubble(Gdiplus::Graphics& g,const std::wstring& said,float scale,float most)
        :font(BubbleFont(16*scale*BubbleTextScale(),Gdiplus::FontStyleRegular,{L"Yu Gothic UI"})),format(std::make_unique<Gdiplus::StringFormat>()),s(scale),pad(14*scale)
    {
        using namespace Gdiplus;
        RectF one,need;
        g.MeasureString(said.c_str(),static_cast<INT>(said.size()),font.get(),PointF(0,0),format.get(),&one);
        g.MeasureString(said.c_str(),static_cast<INT>(said.size()),font.get(),RectF(0,0,most-2*pad,600*s*BubbleTextScale()),format.get(),&need);
        textWidth=std::min(one.Width,most-2*pad)+2;
        textHeight=need.Height;
        width=textWidth+2*pad;
        height=textHeight+2*pad*0.8f;
    }

    WordsBubble::~WordsBubble()=default;

    void WordsBubble::Draw(Gdiplus::Graphics& g,float x,float y,float tailY,const std::wstring& shown,bool tailRight) const
    {
        using namespace Gdiplus;
        const Color paper(240,255,252,246),line(255,214,170,120);
        const float e=tailRight?x+width:x,o=tailRight?-1.0f:1.0f;
        const PointF tail[3]={PointF(e+o*2*s,tailY-9*s),PointF(e-o*16*s,tailY+4*s),PointF(e+o*2*s,tailY+9*s)};
        SolidBrush fill(paper);
        Pen edge(line,1.5f*s);
        g.FillPolygon(&fill,tail,3);
        g.DrawLine(&edge,tail[0],tail[1]);
        g.DrawLine(&edge,tail[1],tail[2]);
        GraphicsPath body;
        const float d=14*s*2;
        body.AddArc(x,y,d,d,180,90);
        body.AddArc(x+width-d,y,d,d,270,90);
        body.AddArc(x+width-d,y+height-d,d,d,0,90);
        body.AddArc(x,y+height-d,d,d,90,90);
        body.CloseFigure();
        g.FillPath(&fill,&body);
        g.DrawPath(&edge,&body);
        SolidBrush ink(Color(255,60,44,36));
        g.DrawString(shown.c_str(),static_cast<INT>(shown.size()),font.get(),RectF(x+pad,y+pad*0.8f,textWidth+4,textHeight+4),format.get(),&ink);
    }

    void WordsBubble::DrawPointing(Gdiplus::Graphics& g,float x,float y,float tipX,float tipY,const std::wstring& shown) const
    {
        using namespace Gdiplus;
        const PointF tip(tipX,tipY);
        const Color paper(240,255,252,246),line(255,214,170,120);
        // The tail's foot on the top edge, under the tip as far as the corners allow; its point
        // toward the tip, stopping short of it.
        const float foot=std::clamp(tip.X,x+24*s,x+width-24*s);
        const float dx=tip.X-foot,dy=tip.Y-y,dist=std::sqrt(dx*dx+dy*dy);
        const float reach=std::clamp(dist-10*s,8*s,46*s);
        const PointF point=dist>1?PointF(foot+dx/dist*reach,y+dy/dist*reach):PointF(foot,y-reach);
        const PointF tail[3]={PointF(foot-10*s,y+2*s),point,PointF(foot+10*s,y+2*s)};
        SolidBrush fill(paper);
        Pen edge(line,1.5f*s);
        GraphicsPath body;
        const float d=14*s*2;
        body.AddArc(x,y,d,d,180,90);
        body.AddArc(x+width-d,y,d,d,270,90);
        body.AddArc(x+width-d,y+height-d,d,d,0,90);
        body.AddArc(x,y+height-d,d,d,90,90);
        body.CloseFigure();
        g.FillPolygon(&fill,tail,3);
        g.DrawLine(&edge,tail[0],tail[1]);
        g.DrawLine(&edge,tail[1],tail[2]);
        g.FillPath(&fill,&body);
        g.DrawPath(&edge,&body);
        // (The edge under the tail's foot hidden, so the two read as one.)
        g.FillRectangle(&fill,foot-8.5f*s,y-0.5f*s,17*s,3*s);
        SolidBrush ink(Color(255,60,44,36));
        g.DrawString(shown.c_str(),static_cast<INT>(shown.size()),font.get(),RectF(x+pad,y+pad*0.8f,textWidth+4,textHeight+4),format.get(),&ink);
    }
}
