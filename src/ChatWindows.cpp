#define NOMINMAX
#include <algorithm>
#include "ChatWindows.hpp"
#include "Text.hpp"
#include "BubbleText.hpp"
#include <commctrl.h>
#include <objidl.h>
// GDI+ uses min and max unqualified; give it the std ones instead of the windows.h macros.
namespace Gdiplus {using std::min;using std::max;}
#include <gdiplus.h>
#include <cmath>
#include <memory>

#pragma comment(lib,"gdiplus.lib")

namespace MikuMikuDesktopPerch
{
    namespace
    {
        constexpr wchar_t BubbleClass[]=L"MikuMikuDesktopPerchBubble";
        constexpr wchar_t InputClass[]=L"MikuMikuDesktopPerchInput";
        constexpr wchar_t ChoiceClass[]=L"MikuMikuDesktopPerchChoice";

        float Scale(){return static_cast<float>(GetDpiForSystem())/96.0f;}

        bool Register(const wchar_t* name,WNDPROC proc,HBRUSH background)
        {
            WNDCLASSEXW wc{sizeof(wc)};
            if(GetClassInfoExW(GetModuleHandleW(nullptr),name,&wc))return true;
            wc.lpfnWndProc=proc;
            wc.hInstance=GetModuleHandleW(nullptr);
            wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
            wc.hbrBackground=background;
            wc.lpszClassName=name;
            return RegisterClassExW(&wc)!=0;
        }

        // In the font set, else the first of these installed.
        std::unique_ptr<Gdiplus::Font> SpeechFont(float pixels,INT style)
        {
            return BubbleFont(pixels,style,{L"BIZ UDPGothic",L"Yu Gothic UI",L"Meiryo UI",L"MS UI Gothic"});
        }

        void RoundRect(Gdiplus::GraphicsPath& path,Gdiplus::RectF r,float radius)
        {
            const float d=radius*2;
            path.AddArc(r.X,r.Y,d,d,180,90);
            path.AddArc(r.X+r.Width-d,r.Y,d,d,270,90);
            path.AddArc(r.X+r.Width-d,r.Y+r.Height-d,d,d,0,90);
            path.AddArc(r.X,r.Y+r.Height-d,d,d,90,90);
            path.CloseFigure();
        }
    }

    SpeechBubble::~SpeechBubble()
    {
        if(window)DestroyWindow(window);
        if(gdiplus)Gdiplus::GdiplusShutdown(gdiplus);
    }

    bool SpeechBubble::Create(HWND owner)
    {
        if(window)return true;
        Gdiplus::GdiplusStartupInput input;
        if(!gdiplus&&Gdiplus::GdiplusStartup(&gdiplus,&input,nullptr)!=Gdiplus::Ok)return false;
        if(!Register(BubbleClass,DefWindowProcW,nullptr))return false;
        window=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE,BubbleClass,L"",WS_POPUP,
                               0,0,1,1,owner,nullptr,GetModuleHandleW(nullptr),nullptr);
        return window!=nullptr;
    }

    void SpeechBubble::Show()
    {
        // Pop in again only when it was gone or going.
        if(!visible||fading){clock=0;placed={};}
        visible=true;
        fading=false;
        fade=0;
    }

    void SpeechBubble::Begin(const std::wstring& name)
    {
        Show();
        speaker=name;
        text.clear();
        revealed=0;
        externalReveal=-1;
        waiting=true;
        finished=notice=false;
    }

    void SpeechBubble::Append(const std::wstring& more)
    {
        if(!visible||fading)Begin(speaker);
        // Replies tend to start with a blank line; nothing to show for it.
        if(text.empty())
        {
            const size_t start=more.find_first_not_of(L" \t\r\n");
            if(start==std::wstring::npos)return;
            text=more.substr(start);
        }
        else text+=more;
        waiting=false;
    }

    void SpeechBubble::Finish()
    {
        while(!text.empty()&&(text.back()==L'\n'||text.back()==L' '||text.back()==L'\r'))text.pop_back();
        waiting=false;
        finished=true;
        // Long enough to read it again after the last character appears.
        holdLeft=std::clamp(3.0+0.09*static_cast<double>(text.size()),3.0,15.0);
        if(text.empty())Hide();
    }

    void SpeechBubble::ShowNotice(const std::wstring& message)
    {
        Show();
        text=message;
        revealed=static_cast<double>(text.size());
        externalReveal=-1;
        finished=notice=true;
        waiting=false;
        holdLeft=6.0+0.05*static_cast<double>(text.size());
    }

    void SpeechBubble::Hide()
    {
        if(visible)fading=true;
    }

    void SpeechBubble::Tick(double dt,POINT anchor,int headHalfWidth,const RECT& work,bool hidden)
    {
        if(!visible||!window)return;
        clock+=dt;
        const double total=static_cast<double>(text.size());
        if(fading)
        {
            fade+=dt/0.25;
            if(fade>=1)
            {
                visible=fading=waiting=finished=notice=false;
                ShowWindow(window,SW_HIDE);
                return;
            }
        }
        else if(revealed<total)
        {
            // As far as it has been said, or about 24 characters a second (faster when the
            // reply has run far ahead).
            const double behind=total-revealed;
            revealed=externalReveal>=0?std::clamp(externalReveal,revealed,total):std::min(total,revealed+dt*(behind>60?72.0:24.0));
        }
        else if(finished&&(holdLeft-=dt)<=0)fading=true;
        if(hidden){ShowWindow(window,SW_HIDE);return;}
        Render(anchor,headHalfWidth,work);
    }

    void SpeechBubble::Render(POINT anchor,int headHalfWidth,const RECT& work)
    {
        using namespace Gdiplus;
        // The part to show: revealed characters (not splitting a surrogate pair).
        size_t count=static_cast<size_t>(revealed);
        if(count>0&&count<text.size()&&IS_HIGH_SURROGATE(text[count-1]))--count;
        const std::wstring shown=text.substr(0,count);
        const bool dots=waiting&&shown.empty();

        const float s=Scale();
        // The words at the size set, the width they wrap at with them (inside the work area).
        const float k=BubbleTextScale();
        const float pad=14*s,padTop=18*s,radius=20*s,tail=18*s,border=2.5f*s,margin=14*s;
        const float maxWidth=std::max(120*s,std::min(300*s*k,static_cast<float>(work.right-work.left)-2*margin-2*pad-2));
        auto font=SpeechFont(15*s*k,FontStyleRegular);
        auto tagFont=SpeechFont(12*s,FontStyleBold);
        StringFormat format(StringFormat::GenericTypographic());
        format.SetFormatFlags(format.GetFormatFlags()&~StringFormatFlagsNoWrap);
        Bitmap probe(1,1,PixelFormat32bppPARGB);
        Graphics measure(&probe);
        measure.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        auto measureText=[&](const std::wstring& t,const Font* f,float width){
            RectF box;
            measure.MeasureString(t.c_str(),static_cast<INT>(t.size()),f,RectF(0,0,width,10000),&format,&box);
            return box;
        };
        const float lineHeight=font->GetHeight(&measure);
        // At most about eight lines: a longer reply shows its latest part.
        std::wstring display=shown;
        RectF box=dots?RectF(0,0,48*s,lineHeight):measureText(display.empty()?L" ":display,font.get(),maxWidth);
        for(size_t cut=0;box.Height>lineHeight*8.2f&&cut+10<shown.size();)
        {
            cut+=10;
            display=L"…"+shown.substr(cut);
            box=measureText(display,font.get(),maxWidth);
        }
        const std::wstring tag=notice?std::wstring(T(L"お知らせ",L"Notice")):speaker;
        const RectF tagBox=tag.empty()?RectF():measureText(tag,tagFont.get(),maxWidth);
        const float tagW=tag.empty()?0:std::min(tagBox.Width+20*s,maxWidth),tagH=tag.empty()?0:22*s;
        const float w=std::max({box.Width+pad*2+2,tagW+28*s,72*s}),h=std::max(box.Height,lineHeight)+pad*1.3f+padTop;

        // Above the head if it fits, else beside it (right, then left).
        enum class Side {Above,Right,Left} side=Side::Above;
        float bx=anchor.x-w/2,by=anchor.y-tail-h;
        if(by-tagH/2<work.top+margin)
        {
            side=Side::Right;
            bx=static_cast<float>(anchor.x+headHalfWidth)+tail;
            by=static_cast<float>(anchor.y);
            if(bx+w>work.right-margin){side=Side::Left;bx=static_cast<float>(anchor.x-headHalfWidth)-tail-w;}
        }
        // Over someone else (another character, an older bubble): slide sideways, keeping the
        // head under the bubble's bottom edge so the tail still starts above it. The place
        // with the least covered, nearest to centred, wins.
        const float baseHalf=11*s;
        if(side==Side::Above&&!avoid.empty())
        {
            auto covered=[&](float x){
                float area=0;
                for(const RECT& a:avoid)
                {
                    const float w0=std::min(x+w,static_cast<float>(a.right))-std::max(x,static_cast<float>(a.left));
                    const float h0=std::min(by+h,static_cast<float>(a.bottom))-std::max(by-tagH/2,static_cast<float>(a.top));
                    if(w0>0&&h0>0)area+=w0*h0;
                }
                return area;
            };
            const float lo=anchor.x-(w-radius-baseHalf),hi=anchor.x-radius-baseHalf;
            std::vector<float> candidates{bx,lo,hi};
            for(const RECT& a:avoid){candidates.push_back(a.left-margin-w);candidates.push_back(a.right+margin);}
            float best=bx,bestArea=covered(bx);
            for(float x:candidates)
            {
                x=std::clamp(x,std::min(lo,hi),std::max(lo,hi));
                const float area=covered(x);
                if(area<bestArea-1||(area<=bestArea+1&&std::abs(x-(anchor.x-w/2))<std::abs(best-(anchor.x-w/2)))){best=x;bestArea=area;}
            }
            bx=best;
        }
        bx=std::clamp(bx,work.left+margin,std::max(work.left+margin,work.right-margin-w));
        by=std::clamp(by,work.top+margin+tagH/2,std::max(work.top+margin+tagH/2,work.bottom-margin-h));
        placed={static_cast<LONG>(bx),static_cast<LONG>(by-tagH/2),static_cast<LONG>(bx+w),static_cast<LONG>(by+h)};

        // The tail: a curved comic tail from the nearest edge of the bubble to the head. Its
        // base reaches into the body so the fill hides the seam.
        PointF tip,a1,a2,b1,b2,base1,base2;
        if(side==Side::Above)
        {
            const float x=std::clamp(static_cast<float>(anchor.x),bx+radius+baseHalf,bx+w-radius-baseHalf);
            tip={x+(anchor.x-x)*0.6f+4*s,by+h+tail};
            base1={x-baseHalf,by+h-border*2};base2={x+baseHalf,by+h-border*2};
            a1={x-baseHalf*0.4f,by+h+tail*0.35f};a2={tip.X-2*s,tip.Y-tail*0.3f};
            b1={tip.X+1*s,tip.Y-tail*0.55f};b2={x+baseHalf*0.6f,by+h+tail*0.15f};
        }
        else
        {
            const float y=std::clamp(by+h*0.4f,by+radius+baseHalf,by+h-radius-baseHalf);
            const float dir=side==Side::Right?-1.0f:1.0f;
            const float edge=side==Side::Right?bx+border*2:bx+w-border*2;
            tip={edge+dir*(tail+border*2),y+tail*0.5f};
            base1={edge,y-baseHalf};base2={edge,y+baseHalf};
            a1={edge+dir*tail*0.4f,y-baseHalf*0.2f};a2={tip.X-dir*tail*0.2f,tip.Y-tail*0.25f};
            b1={tip.X-dir*tail*0.45f,tip.Y-tail*0.05f};b2={edge+dir*tail*0.15f,y+baseHalf*0.7f};
        }

        // Pop in (a little overshoot) around the tail's tip, fade at both ends.
        const float t=static_cast<float>(std::min(clock/0.22,1.0));
        const float pop=t>=1?1.0f:1.0f+0.12f*std::sin(t*3.1416f)-(1-t)*(1-t)*0.25f;
        const float alpha=static_cast<float>(std::min(clock/0.12,1.0)*(1.0-std::clamp(fade,0.0,1.0)));

        const float left=std::min(bx,tip.X)-margin,top=std::min(by-tagH/2,tip.Y)-margin;
        const float right=std::max(bx+w,tip.X)+margin,bottom=std::max(by+h,tip.Y)+margin;
        const int width=static_cast<int>(std::ceil(right-left))+2,height=static_cast<int>(std::ceil(bottom-top))+2;
        const float ox=-std::floor(left),oy=-std::floor(top);

        BITMAPINFO info{};
        info.bmiHeader={sizeof(BITMAPINFOHEADER),width,-height,1,32,BI_RGB};
        void* bits=nullptr;
        HDC screen=GetDC(nullptr);
        HDC memory=CreateCompatibleDC(screen);
        HBITMAP dib=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        if(!dib){DeleteDC(memory);ReleaseDC(nullptr,screen);return;}
        HGDIOBJ old=SelectObject(memory,dib);
        {
            Bitmap canvas(width,height,width*4,PixelFormat32bppPARGB,static_cast<BYTE*>(bits));
            Graphics g(&canvas);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
            g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            g.Clear(Color(0,0,0,0));
            g.TranslateTransform(ox,oy);
            g.TranslateTransform(tip.X,tip.Y);
            g.ScaleTransform(pop,pop);
            g.TranslateTransform(-tip.X,-tip.Y);

            const Color accentA=notice?Color(255,255,150,80):Color(255,57,201,187);
            const Color accentB=notice?Color(255,240,96,96):Color(255,255,111,152);
            GraphicsPath body,tailPath;
            RoundRect(body,RectF(bx,by,w,h),radius);
            tailPath.StartFigure();
            tailPath.AddLine(base2,base1);
            tailPath.AddBezier(base1,a1,a2,tip);
            tailPath.AddBezier(tip,b1,b2,base2);
            tailPath.CloseFigure();

            // Soft shadow: the shape spread out in faint layers, a little below.
            {
                Matrix down;
                down.Translate(0,4*s);
                GraphicsPath shadowBody,shadowTail;
                RoundRect(shadowBody,RectF(bx,by,w,h),radius);
                shadowBody.Transform(&down);
                shadowTail.AddPath(&tailPath,FALSE);
                shadowTail.Transform(&down);
                for(int i=6;i>=1;--i)
                {
                    Pen spread(Color(static_cast<BYTE>(10*alpha),20,30,60),i*2.2f*s);
                    spread.SetLineJoin(LineJoinRound);
                    g.DrawPath(&spread,&shadowBody);
                    g.DrawPath(&spread,&shadowTail);
                }
            }
            // Outline: both shapes stroked twice as wide, then filled over, so only the
            // outside of their union keeps the line.
            LinearGradientBrush edge(PointF(bx,by),PointF(bx+w,by+h),
                                     Color(static_cast<BYTE>(255*alpha),accentA.GetR(),accentA.GetG(),accentA.GetB()),
                                     Color(static_cast<BYTE>(255*alpha),accentB.GetR(),accentB.GetG(),accentB.GetB()));
            Pen outline(&edge,border*2);
            outline.SetLineJoin(LineJoinRound);
            g.DrawPath(&outline,&body);
            g.DrawPath(&outline,&tailPath);
            LinearGradientBrush paper(PointF(bx,by),PointF(bx,by+h+tail),Color(static_cast<BYTE>(252*alpha),255,255,255),
                                      Color(static_cast<BYTE>(252*alpha),236,247,255));
            g.FillPath(&paper,&body);
            g.FillPath(&paper,&tailPath);
            // A soft highlight along the top inside edge.
            {
                Pen gloss(Color(static_cast<BYTE>(150*alpha),255,255,255),1.5f*s);
                GraphicsPath shine;
                RoundRect(shine,RectF(bx+border*1.5f,by+border*1.5f,w-border*3,h*0.55f),radius-border);
                g.DrawPath(&gloss,&shine);
            }
            // A little sparkle on the top-right corner.
            {
                const float cx=bx+w-radius*0.55f,cy=by+radius*0.45f,r=6*s;
                const PointF star[8]={{cx,cy-r},{cx+r*0.22f,cy-r*0.22f},{cx+r,cy},{cx+r*0.22f,cy+r*0.22f},
                                      {cx,cy+r},{cx-r*0.22f,cy+r*0.22f},{cx-r,cy},{cx-r*0.22f,cy-r*0.22f}};
                SolidBrush sparkle(Color(static_cast<BYTE>(200*alpha),accentB.GetR(),accentB.GetG(),accentB.GetB()));
                g.FillPolygon(&sparkle,star,8);
            }
            // Name tag over the top edge.
            if(!tag.empty())
            {
                const RectF pill(bx+16*s,by-tagH/2,tagW,tagH);
                GraphicsPath tagPath;
                RoundRect(tagPath,pill,tagH/2);
                LinearGradientBrush tagFill(PointF(pill.X,pill.Y),PointF(pill.X+pill.Width,pill.Y),
                                            Color(static_cast<BYTE>(255*alpha),accentA.GetR(),accentA.GetG(),accentA.GetB()),
                                            Color(static_cast<BYTE>(255*alpha),accentB.GetR(),accentB.GetG(),accentB.GetB()));
                Pen ring(Color(static_cast<BYTE>(255*alpha),255,255,255),2*s);
                g.FillPath(&tagFill,&tagPath);
                g.DrawPath(&ring,&tagPath);
                SolidBrush white(Color(static_cast<BYTE>(255*alpha),255,255,255));
                StringFormat centred(StringFormat::GenericTypographic());
                centred.SetAlignment(StringAlignmentCenter);
                centred.SetLineAlignment(StringAlignmentCenter);
                centred.SetFormatFlags(centred.GetFormatFlags()|StringFormatFlagsNoWrap);
                centred.SetTrimming(StringTrimmingEllipsisCharacter);
                g.DrawString(tag.c_str(),static_cast<INT>(tag.size()),tagFont.get(),RectF(pill.X+6*s,pill.Y,pill.Width-12*s,pill.Height),&centred,&white);
            }
            if(dots)
            {
                // Three dots hopping in turn.
                for(int i=0;i<3;++i)
                {
                    const float phase=static_cast<float>(clock*6.0-i*0.9);
                    const float hop=std::max(0.0f,std::sin(phase))*5*s;
                    const Color c=i==1?accentB:accentA;
                    SolidBrush dot(Color(static_cast<BYTE>(230*alpha),c.GetR(),c.GetG(),c.GetB()));
                    const float d=8*s,cx=bx+w/2+(i-1)*14*s,cy=by+padTop+lineHeight*0.5f-hop;
                    g.FillEllipse(&dot,cx-d/2,cy-d/2,d,d);
                }
            }
            else
            {
                SolidBrush ink(Color(static_cast<BYTE>(255*alpha),51,54,77));
                g.DrawString(display.c_str(),static_cast<INT>(display.size()),font.get(),RectF(bx+pad,by+padTop,maxWidth+1,h),&format,&ink);
            }
        }
        POINT position{static_cast<LONG>(std::floor(left)),static_cast<LONG>(std::floor(top))};
        SIZE size{width,height};
        POINT origin{0,0};
        GdiFlush();
        picture.assign(static_cast<const std::uint8_t*>(bits),static_cast<const std::uint8_t*>(bits)+static_cast<size_t>(width)*height*4);
        pictureRect={position.x,position.y,position.x+width,position.y+height};
        BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        UpdateLayeredWindow(window,screen,&position,&size,memory,&origin,0,&blend,ULW_ALPHA);
        SelectObject(memory,old);
        DeleteObject(dib);
        DeleteDC(memory);
        ReleaseDC(nullptr,screen);
        if(!IsWindowVisible(window))SetWindowPos(window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    }

    ChoiceBox::~ChoiceBox(){Close();}

    void ChoiceBox::Open(HWND owner,POINT bottomCentre,const std::wstring& yes,const std::wstring& no,std::function<void(bool)> choose)
    {
        Close();
        onChoose=std::move(choose);
        if(!Register(ChoiceClass,Proc,reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1)))return;
        const float s=Scale();
        const int buttonW=static_cast<int>(120*s),buttonH=static_cast<int>(32*s),gap=static_cast<int>(8*s);
        const int w=buttonW*2+gap*3,h=buttonH+gap*2;
        window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE,ChoiceClass,L"",WS_POPUP|WS_BORDER,
                               0,0,w,h,owner,nullptr,GetModuleHandleW(nullptr),this);
        if(!window)return;
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0,GetDpiForSystem());
        metrics.lfMessageFont.lfHeight=static_cast<LONG>(metrics.lfMessageFont.lfHeight*1.15f);
        font=CreateFontIndirectW(&metrics.lfMessageFont);
        RECT client{};
        GetClientRect(window,&client);
        const int top=(client.bottom-buttonH)/2;
        for(int i=0;i<2;++i)
        {
            HWND button=CreateWindowExW(0,L"BUTTON",(i==0?yes:no).c_str(),WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                        gap+i*(buttonW+gap),top,buttonW,buttonH,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(i==0?IDYES:IDNO)),
                                        GetModuleHandleW(nullptr),nullptr);
            SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        }
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint(bottomCentre,MONITOR_DEFAULTTONEAREST),&monitor);
        const RECT& work=monitor.rcWork;
        const int x=std::clamp(static_cast<int>(bottomCentre.x)-w/2,static_cast<int>(work.left),static_cast<int>(work.right)-w);
        const int y=std::clamp(static_cast<int>(bottomCentre.y)-h,static_cast<int>(work.top),static_cast<int>(work.bottom)-h);
        SetWindowPos(window,HWND_TOPMOST,x,y,w,h,SWP_SHOWWINDOW|SWP_NOACTIVATE);
    }

    void ChoiceBox::Close()
    {
        if(window){HWND w=window;window=nullptr;DestroyWindow(w);}
        if(font){DeleteObject(font);font=nullptr;}
    }

    LRESULT CALLBACK ChoiceBox::Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
    {
        auto* self=reinterpret_cast<ChoiceBox*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        switch(message)
        {
        case WM_NCCREATE:
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
            break;
        case WM_COMMAND:
            if(self&&self->window==hwnd&&(LOWORD(wParam)==IDYES||LOWORD(wParam)==IDNO))
            {
                auto choose=std::move(self->onChoose);
                self->Close();
                if(choose)choose(LOWORD(wParam)==IDYES);
                return 0;
            }
            break;
        }
        return DefWindowProcW(hwnd,message,wParam,lParam);
    }

    ChatInput::~ChatInput(){Close();}

    void ChatInput::Open(HWND owner,POINT bottomCentre,std::function<void(const std::wstring&)> send)
    {
        onSend=std::move(send);
        const float s=Scale();
        const int w=static_cast<int>(340*s),h=static_cast<int>(34*s);
        if(!window)
        {
            if(!Register(InputClass,Proc,reinterpret_cast<HBRUSH>(COLOR_WINDOW+1)))return;
            window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST,InputClass,T(L"話しかける",L"Talk"),WS_POPUP|WS_BORDER,
                                   0,0,w,h,owner,nullptr,GetModuleHandleW(nullptr),this);
            if(!window)return;
            NONCLIENTMETRICSW metrics{sizeof(metrics)};
            SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0,GetDpiForSystem());
            metrics.lfMessageFont.lfHeight=static_cast<LONG>(metrics.lfMessageFont.lfHeight*1.2f);
            font=CreateFontIndirectW(&metrics.lfMessageFont);
            RECT client{};
            GetClientRect(window,&client);
            const int inset=static_cast<int>(6*s);
            edit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,inset,inset,client.right-inset*2,client.bottom-inset*2,
                                 window,nullptr,GetModuleHandleW(nullptr),nullptr);
            SendMessageW(edit,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
            SendMessageW(edit,EM_SETLIMITTEXT,500,0);
            SendMessageW(edit,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(T(L"話しかける（Enterで送る、Escで閉じる）",L"Say something (Enter to send, Esc to close)")));
            SetWindowLongPtrW(edit,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(this));
            editDefault=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(edit,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(EditProc)));
        }
        // Inside the screen the point is on.
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint(bottomCentre,MONITOR_DEFAULTTONEAREST),&monitor);
        const RECT& work=monitor.rcWork;
        const int x=std::clamp(static_cast<int>(bottomCentre.x)-w/2,static_cast<int>(work.left),static_cast<int>(work.right)-w);
        const int y=std::clamp(static_cast<int>(bottomCentre.y)-h,static_cast<int>(work.top),static_cast<int>(work.bottom)-h);
        SetWindowPos(window,HWND_TOPMOST,x,y,w,h,SWP_SHOWWINDOW);
        SetForegroundWindow(window);
        SetFocus(edit);
    }

    void ChatInput::Close()
    {
        if(window){HWND w=window;window=edit=nullptr;DestroyWindow(w);}
        if(font){DeleteObject(font);font=nullptr;}
    }

    LRESULT CALLBACK ChatInput::Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
    {
        auto* self=reinterpret_cast<ChatInput*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        switch(message)
        {
        case WM_NCCREATE:
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
            break;
        case WM_ACTIVATE:
            // Clicking elsewhere closes it.
            if(LOWORD(wParam)==WA_INACTIVE)PostMessageW(hwnd,WM_CLOSE,0,0);
            else if(self&&self->edit)SetFocus(self->edit);
            return 0;
        case WM_CLOSE:
            if(self&&self->window==hwnd){self->Close();return 0;}
            break;
        }
        return DefWindowProcW(hwnd,message,wParam,lParam);
    }

    LRESULT CALLBACK ChatInput::EditProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
    {
        auto* self=reinterpret_cast<ChatInput*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(!self)return DefWindowProcW(hwnd,message,wParam,lParam);
        const WNDPROC base=self->editDefault;
        if(message==WM_KEYDOWN&&wParam==VK_RETURN)
        {
            wchar_t buffer[512]{};
            GetWindowTextW(hwnd,buffer,static_cast<int>(std::size(buffer)));
            std::wstring line=buffer;
            const size_t start=line.find_first_not_of(L" \t\x3000");
            if(start!=std::wstring::npos)
            {
                SetWindowTextW(hwnd,L"");
                if(self->onSend)self->onSend(line.substr(start));
            }
            return 0;
        }
        if(message==WM_KEYDOWN&&wParam==VK_ESCAPE)
        {
            PostMessageW(GetParent(hwnd),WM_CLOSE,0,0);
            return 0;
        }
        if(message==WM_CHAR&&(wParam==L'\r'||wParam==0x1B))return 0;   // no beep
        return CallWindowProcW(base,hwnd,message,wParam,lParam);
    }
}
