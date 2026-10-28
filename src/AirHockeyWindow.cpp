#define NOMINMAX
#include "AirHockeyWindow.hpp"
#include "AirHockey.hpp"
#include "Text.hpp"
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>
#pragma comment(lib,"gdiplus.lib")

namespace MikuMikuDesktopPerch
{
    namespace
    {
        using namespace Gdiplus;
        using AirHockey::Vec;
        constexpr wchar_t ClassName[]=L"MikuMikuDesktopPerchAirHockey";
        constexpr UINT_PTR TimerId=1;

        // Where the table is in the window: its middle, and pixels per table unit (your end down).
        struct Layout
        {
            float cx=0,cy=0,ppu=1,s=1;
            PointF At(Vec p) const {return PointF(cx+p.x*ppu,cy-p.y*ppu);}
            Vec Table(float x,float y) const {return {(x-cx)/ppu,(cy-y)/ppu};}
        };

        struct State
        {
            HWND window=nullptr;
            ULONG_PTR gdiplus=0;
            AirHockeyOpponent her;
            std::function<void()> onEnd;
            std::unique_ptr<AirHockey::Game> game;
            std::unique_ptr<AirHockey::Brain> brain,autoplay;   // hers; yours in a test
            std::unique_ptr<Bitmap> canvas,table;   // the frame drawn, and the table under it (drawn once per size)
            Layout layout;
            LARGE_INTEGER last{};
            bool paused=false,asking=false,overTold=false;
            bool held=false;   // the cursor is kept over your half (ClipCursor)
            ULONGLONG speedShownUntil=0;   // GetTickCount64: until then, the speed just chosen is shown
            int frames=0;   // drawn, for the snapshot's report
            Vec target{};
        } state;

        std::uint32_t Seed()
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            return static_cast<std::uint32_t>(now.QuadPart)^0x9E3779B9u;
        }

        std::wstring HerName(){return state.her.name.empty()?std::wstring(T(L"相手",L"Her")):state.her.name;}

        // The puck's top speed chosen (1-4, AirHockey::PuckSpeeds), in words.
        std::wstring SpeedName(int level)
        {
            switch(level)
            {
            case 1:return T(L"ゆっくり",L"slow");
            case 2:return T(L"ふつう",L"normal");
            case 3:return T(L"はやい",L"fast");
            default:return T(L"とてもはやい",L"very fast");
            }
        }
        int SpeedLevel(const AirHockey::Game& game)
        {
            int best=1;
            for(int i=0;i<4;++i)if(std::abs(AirHockey::PuckSpeeds[i]-game.Rule().puckSpeed)<std::abs(AirHockey::PuckSpeeds[best-1]-game.Rule().puckSpeed))best=i+1;
            return best;
        }

        const FontFamily* Family()
        {
            static FontFamily gothic(L"Yu Gothic UI");
            return gothic.IsAvailable()?&gothic:FontFamily::GenericSansSerif();
        }

        void Text(Graphics& g,const std::wstring& text,const RectF& box,float size,Color color,bool bold=false,
                  StringAlignment across=StringAlignmentCenter,StringAlignment down=StringAlignmentCenter)
        {
            Font font(Family(),size,bold?FontStyleBold:FontStyleRegular,UnitPixel);
            SolidBrush paint(color);
            StringFormat format;
            format.SetAlignment(across);
            format.SetLineAlignment(down);
            format.SetFormatFlags(StringFormatFlagsNoWrap);
            g.DrawString(text.c_str(),static_cast<INT>(text.size()),&font,box,&format,&paint);
        }

        void RoundRect(GraphicsPath& path,RectF r,float radius)
        {
            const float d=radius*2;
            path.AddArc(r.X,r.Y,d,d,180,90);
            path.AddArc(r.X+r.Width-d,r.Y,d,d,270,90);
            path.AddArc(r.X+r.Width-d,r.Y+r.Height-d,d,d,0,90);
            path.AddArc(r.X,r.Y+r.Height-d,d,d,90,90);
            path.CloseFigure();
        }

        void Disc(Graphics& g,PointF at,float radius,const Brush& brush)
        {
            g.FillEllipse(&brush,at.X-radius,at.Y-radius,radius*2,radius*2);
        }

        // The table in a w x h picture (s: the screen's scale): its size, and where it is.
        Layout Place(int w,int h,float s,const AirHockey::Rules& rules)
        {
            Layout l;
            l.s=s;
            const float top=58*s,margin=18*s,rim=0.07f;
            l.ppu=std::min((w-2*margin)/(rules.width+2*rim),(h-top-margin)/(rules.length+2*rim));
            l.cx=w/2.0f;
            l.cy=top+(h-top-margin)/2.0f;
            return l;
        }

        // What does not move: the room's floor, the table's rim and white top, its lines, the
        // goals' slots and the air holes.
        void DrawTable(Graphics& g,int w,int h,const Layout& l,const AirHockey::Rules& rules)
        {
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.Clear(Color(255,38,44,58));
            const float W=rules.width/2,L=rules.length/2,rim=0.07f*l.ppu,u=l.ppu;
            const PointF corner=l.At({-W,L});
            const RectF top(corner.X,corner.Y,rules.width*u,rules.length*u);
            {
                GraphicsPath frame;
                RoundRect(frame,RectF(top.X-rim,top.Y-rim,top.Width+2*rim,top.Height+2*rim),rim*1.2f);
                SolidBrush wood(Color(255,58,92,150));
                g.FillPath(&wood,&frame);
                Pen edge(Color(255,120,160,214),1.5f*l.s);
                g.DrawPath(&edge,&frame);
            }
            LinearGradientBrush surface(top,Color(255,236,243,250),Color(255,214,228,242),LinearGradientModeVertical);
            g.FillRectangle(&surface,top);
            // The air holes, faint.
            SolidBrush hole(Color(60,90,120,160));
            const float step=0.05f;
            for(float y=-L+step/2;y<L;y+=step)
                for(float x=-W+step/2;x<W;x+=step)Disc(g,l.At({x,y}),std::max(0.8f,0.004f*u),hole);
            // The centre line and circle, the goals' creases.
            Pen red(Color(200,214,70,84),std::max(2.0f,0.012f*u)),blue(Color(170,70,110,200),std::max(1.5f,0.008f*u));
            g.DrawLine(&red,l.At({-W,0}),l.At({W,0}));
            const float circle=0.16f*u;
            const PointF mid=l.At({0,0});
            g.DrawEllipse(&blue,mid.X-circle,mid.Y-circle,circle*2,circle*2);
            for(float s:{-1.0f,1.0f})
            {
                const PointF g0=l.At({0,s*L});
                const float crease=rules.goal*0.75f*u;
                g.DrawArc(&blue,g0.X-crease,g0.Y-crease,crease*2,crease*2,s>0?0.0f:180.0f,180.0f);
                // The goal: a dark slot in the rim.
                SolidBrush slot(Color(255,18,20,28));
                const PointF a=l.At({-rules.goal/2,s*L});
                g.FillRectangle(&slot,a.X,s>0?a.Y-rim:a.Y,rules.goal*u,rim);
            }
        }

        void DrawMallet(Graphics& g,PointF at,float radius,Color body,Color ring)
        {
            SolidBrush shadow(Color(60,0,0,0));
            Disc(g,PointF(at.X+radius*0.12f,at.Y+radius*0.16f),radius,shadow);
            SolidBrush outer(ring),inner(body);
            Disc(g,at,radius,outer);
            Disc(g,at,radius*0.78f,inner);
            SolidBrush knob(ring);
            Disc(g,at,radius*0.42f,knob);
            SolidBrush shine(Color(110,255,255,255));
            Disc(g,PointF(at.X-radius*0.15f,at.Y-radius*0.15f),radius*0.16f,shine);
        }

        // The moving things and the words over the table.
        void DrawPlay(Graphics& g,int w,const Layout& l,const AirHockey::Game& game,bool paused,bool asking,bool live,bool speedChosen=false)
        {
            using namespace AirHockey;
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            const auto& rules=game.Rule();
            const float u=l.ppu,s=l.s;
            DrawMallet(g,l.At(game.Mallet(Her)),rules.mallet[Her]*u,Color(255,236,96,134),Color(255,190,50,92));
            {
                const PointF p=l.At(game.Puck());
                const float r=rules.puck*u;
                SolidBrush shadow(Color(70,0,0,0)),body(Color(255,30,32,40)),rim(Color(255,224,180,60));
                Disc(g,PointF(p.X+r*0.15f,p.Y+r*0.2f),r,shadow);
                Disc(g,p,r,rim);
                Disc(g,p,r*0.8f,body);
            }
            DrawMallet(g,l.At(game.Mallet(You)),rules.mallet[You]*u,Color(255,90,170,236),Color(255,40,110,190));

            // The score: hers on the left, yours on the right.
            const Color white(255,240,242,248),soft(255,170,184,208),gold(255,246,210,120);
            Text(g,HerName(),RectF(16*s,8*s,w/2.0f-60*s,24*s),15*s,soft,false,StringAlignmentNear);
            Text(g,T(L"あなた",L"You"),RectF(w/2.0f+44*s,8*s,w/2.0f-60*s,24*s),15*s,soft,false,StringAlignmentFar);
            if(live)Text(g,T(L"パック：",L"Puck: ")+SpeedName(SpeedLevel(game))+T(L"（1〜4キー）",L" (keys 1-4)"),
                         RectF(w/2.0f+44*s,30*s,w/2.0f-60*s,22*s),12*s,soft,false,StringAlignmentFar);
            Text(g,std::to_wstring(game.Score(Her))+L"  -  "+std::to_wstring(game.Score(You)),RectF(0,4*s,static_cast<float>(w),46*s),30*s,white,true);
            Text(g,T(L"先に",L"First to ")+std::to_wstring(rules.toWin)+T(L"点",L""),RectF(16*s,30*s,w/2.0f-60*s,22*s),12*s,soft,false,StringAlignmentNear);

            auto banner=[&](const std::wstring& big,const std::wstring& note){
                const float bw=std::min(static_cast<float>(w)-40*s,420*s),bh=note.empty()?64*s:96*s;
                const RectF box(w/2.0f-bw/2,l.cy-bh/2,bw,bh);
                GraphicsPath path;
                RoundRect(path,box,12*s);
                SolidBrush back(Color(215,16,22,36));
                g.FillPath(&back,&path);
                Text(g,big,RectF(box.X,box.Y+8*s,box.Width,note.empty()?box.Height-16*s:48*s),26*s,gold,true);
                if(!note.empty())Text(g,note,RectF(box.X,box.Y+56*s,box.Width,30*s),14*s,white);
            };
            if(game.Now()==Phase::Over)
            {
                std::wstring note=std::to_wstring(game.Score(Her))+L" - "+std::to_wstring(game.Score(You));
                note+=T(L"　いちばん長い打ち合い ",L"   longest rally ")+std::to_wstring(game.LongestRally());
                if(live)note+=T(L"　Enterでもう一度・Escで閉じる",L"   Enter: again, Esc: close");
                banner(game.Winner()==You?T(L"あなたの勝ち！",L"You win!"):HerName()+T(L"の勝ち",L" wins"),note);
            }
            else if(asking)banner(T(L"やめますか？",L"Stop playing?"),T(L"Y：やめる　N：続ける",L"Y: stop   N: go on"));
            else if(speedChosen)banner(T(L"パックの速さ：",L"Puck speed: ")+SpeedName(SpeedLevel(game)),L"");
            else if(paused)banner(T(L"一時停止",L"Paused"),live?T(L"Spaceで再開",L"Space to go on"):L"");
            else if(game.Now()==Phase::Serve&&game.GoalServe()&&game.ServeLeft()>0.3)
                banner(game.LastScorer()==You?T(L"ゴール！",L"Goal!"):HerName()+T(L"のゴール",L" scores"),L"");
        }

        void Paint(HDC dc)
        {
            RECT client{};
            GetClientRect(state.window,&client);
            const int w=std::max(1L,client.right),h=std::max(1L,client.bottom);
            const float s=static_cast<float>(GetDpiForWindow(state.window))/96.0f;
            const auto& rules=state.game->Rule();
            if(!state.canvas||static_cast<int>(state.canvas->GetWidth())!=w||static_cast<int>(state.canvas->GetHeight())!=h)
            {
                state.canvas=std::make_unique<Bitmap>(w,h,PixelFormat32bppPARGB);
                state.table=std::make_unique<Bitmap>(w,h,PixelFormat32bppPARGB);
                state.layout=Place(w,h,s,rules);
                Graphics t(state.table.get());
                DrawTable(t,w,h,state.layout,rules);
            }
            Graphics g(state.canvas.get());
            g.SetCompositingMode(CompositingModeSourceCopy);
            g.DrawImage(state.table.get(),0,0);
            g.SetCompositingMode(CompositingModeSourceOver);
            DrawPlay(g,w,state.layout,*state.game,state.paused,state.asking,true,GetTickCount64()<state.speedShownUntil);
            Graphics screen(dc);
            screen.DrawImage(state.canvas.get(),0,0);
            ++state.frames;
        }

        void NewGame()
        {
            state.game=std::make_unique<AirHockey::Game>();
            state.game->SetPuckSpeed(AirHockey::PuckSpeeds[std::clamp(state.her.puckSpeed,1,4)-1]);
            state.speedShownUntil=0;
            state.brain=std::make_unique<AirHockey::Brain>(AirHockey::Her,AirHockey::Style{},Seed());
            state.paused=state.asking=state.overTold=false;
            state.target=state.game->Mallet(AirHockey::You);
            QueryPerformanceCounter(&state.last);
        }

        // While you play, the cursor stays over your half (where your mallet's middle can go), so
        // the mallet follows it at once however far the mouse moves. Let go of whenever play
        // stops (paused, asked, over) or the window is left, resized or closed.
        bool CursorArea(RECT& area)
        {
            if(!state.window||!state.canvas||!state.game)return false;
            const auto& rules=state.game->Rule();
            const float R=rules.mallet[AirHockey::You],W=rules.width/2-R,L=rules.length/2-R;
            const PointF a=state.layout.At({-W,-R}),b=state.layout.At({W,-L});
            POINT lo{static_cast<LONG>(std::ceil(a.X)),static_cast<LONG>(std::ceil(a.Y))};
            POINT hi{static_cast<LONG>(std::floor(b.X)),static_cast<LONG>(std::floor(b.Y))};
            ClientToScreen(state.window,&lo);
            ClientToScreen(state.window,&hi);
            area={lo.x,lo.y,hi.x+1,hi.y+1};
            return true;
        }
        void HoldCursor(bool on)
        {
            RECT area{};
            if(on&&CursorArea(area))
            {
                RECT now{};
                if(!state.held||!GetClipCursor(&now)||!EqualRect(&now,&area))ClipCursor(&area);
                state.held=true;
            }
            else if(state.held)
            {
                ClipCursor(nullptr);
                state.held=false;
            }
        }

        void Finish()
        {
            if(!state.window)return;
            HoldCursor(false);
            KillTimer(state.window,TimerId);
            auto onEnd=std::move(state.onEnd);
            DestroyWindow(state.window);
            if(onEnd)onEnd();
        }

        bool Playing(){return state.game&&state.game->Now()!=AirHockey::Phase::Over&&!state.paused&&!state.asking;}

        void Tick()
        {
            LARGE_INTEGER now,rate;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&rate);
            const double dt=std::min(0.05,static_cast<double>(now.QuadPart-state.last.QuadPart)/rate.QuadPart);
            state.last=now;
            if(GetForegroundWindow()!=state.window&&Playing()&&!state.autoplay)state.paused=true;   // away from the window: it waits
            HoldCursor(Playing()&&!state.autoplay&&GetForegroundWindow()==state.window&&!IsIconic(state.window));
            if(Playing()||state.game->Now()==AirHockey::Phase::Over)
            {
                // Your mallet goes where the mouse is over the table (kept on your half by the game).
                POINT cursor;
                if(state.autoplay)state.target=state.autoplay->Think(*state.game,dt);
                else if(GetCursorPos(&cursor)&&ScreenToClient(state.window,&cursor)&&Playing())
                    state.target=state.layout.Table(static_cast<float>(cursor.x),static_cast<float>(cursor.y));
                const Vec herTarget=state.brain->Think(*state.game,dt);
                state.game->Step(dt,state.target,herTarget);
                state.game->TakeEvents();
                if(state.game->Now()==AirHockey::Phase::Over&&!state.overTold)
                {
                    state.overTold=true;
                    if(state.her.onGameOver)state.her.onGameOver();
                }
            }
            InvalidateRect(state.window,nullptr,FALSE);
        }

        LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            switch(message)
            {
            case WM_TIMER:
                if(wParam==TimerId&&state.game)Tick();
                return 0;
            case WM_SETCURSOR:
                // Over the table while playing, the mallet is the cursor.
                if(LOWORD(lParam)==HTCLIENT&&Playing()){SetCursor(nullptr);return TRUE;}
                break;
            case WM_KEYDOWN:
                if(state.asking)
                {
                    if(wParam=='Y'){Finish();return 0;}
                    if(wParam=='N'||wParam==VK_ESCAPE){state.asking=false;QueryPerformanceCounter(&state.last);}
                    return 0;
                }
                if(wParam==VK_ESCAPE)
                {
                    if(state.game->Now()==AirHockey::Phase::Over)Finish();
                    else state.asking=true;
                }
                else if(wParam==VK_SPACE&&state.game->Now()!=AirHockey::Phase::Over)
                {
                    state.paused=!state.paused;
                    QueryPerformanceCounter(&state.last);
                }
                else if(wParam==VK_RETURN&&state.game->Now()==AirHockey::Phase::Over)NewGame();
                else if((wParam>='1'&&wParam<='4')||(wParam>=VK_NUMPAD1&&wParam<=VK_NUMPAD4))
                {
                    // The puck's top speed, from now on (and kept for the next time).
                    const int level=static_cast<int>(wParam>=VK_NUMPAD1?wParam-VK_NUMPAD1:wParam-'1')+1;
                    state.her.puckSpeed=level;
                    state.game->SetPuckSpeed(AirHockey::PuckSpeeds[level-1]);
                    state.speedShownUntil=GetTickCount64()+1200;
                    if(state.her.onPuckSpeed)state.her.onPuckSpeed(level);
                    InvalidateRect(hwnd,nullptr,FALSE);
                }
                return 0;
            case WM_LBUTTONDOWN:
                if(state.paused&&!state.asking){state.paused=false;QueryPerformanceCounter(&state.last);}
                return 0;
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC dc=BeginPaint(hwnd,&ps);
                if(state.game)Paint(dc);
                EndPaint(hwnd,&ps);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_ACTIVATE:
                if(LOWORD(wParam)==WA_INACTIVE)HoldCursor(false);
                break;
            case WM_MOVE:
                HoldCursor(false);   // held again where it now is, at the next tick
                return 0;
            case WM_SIZE:
                HoldCursor(false);
                state.canvas.reset();
                InvalidateRect(hwnd,nullptr,FALSE);
                return 0;
            case WM_CLOSE:
                Finish();
                return 0;
            case WM_DESTROY:
                HoldCursor(false);
                state.window=nullptr;
                state.canvas.reset();
                state.table.reset();
                return 0;
            }
            return DefWindowProcW(hwnd,message,wParam,lParam);
        }

        bool SavePng(Bitmap& image,const std::wstring& path)
        {
            UINT count=0,size=0;
            GetImageEncodersSize(&count,&size);
            std::vector<BYTE> buffer(size);
            auto* encoders=reinterpret_cast<ImageCodecInfo*>(buffer.data());
            GetImageEncoders(count,size,encoders);
            for(UINT i=0;i<count;++i)
                if(std::wstring(encoders[i].MimeType)==L"image/png")return image.Save(path.c_str(),&encoders[i].Clsid,nullptr)==Ok;
            return false;
        }
    }

    void ShowAirHockeyWindow(HWND owner,const AirHockeyOpponent& her,std::function<void()> onEnd)
    {
        if(state.window)
        {
            ShowWindow(state.window,SW_SHOWNORMAL);
            SetForegroundWindow(state.window);
            return;
        }
        if(!state.gdiplus){GdiplusStartupInput input;GdiplusStartup(&state.gdiplus,&input,nullptr);}
        static bool registered=false;
        const HINSTANCE instance=GetModuleHandleW(nullptr);
        if(!registered)
        {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc=Proc;
            wc.hInstance=instance;
            wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
            wc.lpszClassName=ClassName;
            wc.hIcon=owner?reinterpret_cast<HICON>(GetClassLongPtrW(owner,GCLP_HICON)):nullptr;
            RegisterClassExW(&wc);
            registered=true;
        }
        state.her=her;
        state.onEnd=std::move(onEnd);
        NewGame();

        // Upright, as the table is long: most of the screen's height.
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&info);
        const RECT work=info.rcWork;
        const int dpi=static_cast<int>(GetDpiForSystem());
        const int h=std::min(static_cast<int>(work.bottom-work.top)*9/10,MulDiv(900,dpi,96));
        const int w=std::min(static_cast<int>(work.right-work.left)*9/10,h*2/3);
        const std::wstring title=std::wstring(T(L"エアホッケー",L"Air hockey"))+L" - "+HerName();
        state.window=CreateWindowExW(0,ClassName,title.c_str(),WS_OVERLAPPEDWINDOW,
                                     work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,
                                     nullptr,nullptr,instance,nullptr);
        SetTimer(state.window,TimerId,10,nullptr);
        ShowWindow(state.window,SW_SHOWNORMAL);
        SetForegroundWindow(state.window);
    }

    bool AirHockeyWindowOpen(){return state.window!=nullptr;}

    void PlayAirHockeyForTest(){state.autoplay=std::make_unique<AirHockey::Brain>(AirHockey::You,AirHockey::Style{0.25f,0.12f,0.6f,1.0f},7);}

    bool SaveAirHockeyView(const std::wstring& path){return state.canvas&&SavePng(*state.canvas,path);}

    std::wstring DescribeAirHockey()
    {
        if(!state.game)return L"";
        const auto& g=*state.game;
        RECT area{},client{};
        CursorArea(area);
        if(state.window){GetClientRect(state.window,&client);MapWindowPoints(state.window,nullptr,reinterpret_cast<POINT*>(&client),2);}
        wchar_t held[160];
        swprintf_s(held,L"airhockey: cursor area %ld,%ld-%ld,%ld in the window's %ld,%ld-%ld,%ld\n",area.left,area.top,area.right,area.bottom,client.left,client.top,client.right,client.bottom);
        wchar_t text[200];
        swprintf_s(text,L"airhockey: her %d you %d, %.1f s, %s, rally %d (longest %d), frames %d, paused %d\n",g.Score(AirHockey::Her),g.Score(AirHockey::You),
                   g.Clock(),g.Now()==AirHockey::Phase::Over?L"over":g.Now()==AirHockey::Phase::Serve?L"serve":L"play",g.Rally(),g.LongestRally(),state.frames,state.paused?1:0);
        return std::wstring(held)+text;
    }

    std::wstring RunAirHockeySim(int games,std::uint32_t seed,int speed)
    {
        using namespace AirHockey;
        std::wstring lines;
        int wrong=0,goals=0,restarts=0,longest=0,wins[2]={};
        double seconds=0;
        float fastest=0;
        for(int i=0;i<games;++i)
        {
            Game game;
            if(speed>=1&&speed<=4)game.SetPuckSpeed(PuckSpeeds[speed-1]);
            game.NewGame(i%2);
            // Yours as a person plays it (a little slower to see, the mouse quick to move).
            Brain you(You,Style{0.25f,0.12f,0.6f,1.0f},seed*7919u+i*2+1),her(Her,Style{},seed*104729u+i*2+2);
            const double dt=1/120.0;
            while(game.Now()!=Phase::Over&&game.Clock()<900)
            {
                const Vec a=you.Think(game,dt),b=her.Think(game,dt);
                game.Step(dt,a,b);
                game.TakeEvents();
            }
            const bool ended=game.Now()==Phase::Over;
            const int bad=game.OffTable()+game.Overlaps()+(ended?0:1);
            wrong+=bad;
            if(ended)++wins[game.Winner()];
            goals+=game.Score(You)+game.Score(Her);
            restarts+=game.Restarts();
            longest=std::max(longest,game.LongestRally());
            fastest=std::max(fastest,game.FastestPuck());
            seconds+=game.Clock();
            wchar_t line[200];
            swprintf_s(line,L"game %d: her %d you %d, %.0f s, longest rally %d, fastest %.2f, restarts %d, off table %d, overlaps %d%s\n",
                       i+1,game.Score(Her),game.Score(You),game.Clock(),game.LongestRally(),game.FastestPuck(),game.Restarts(),
                       game.OffTable(),game.Overlaps(),ended?L"":L", NOT ENDED");
            lines+=line;
        }
        wchar_t head[240];
        swprintf_s(head,L"%d wrong\n%d games: her %d wins, you %d; %.1f goals and %.0f s a game, %.2f restarts a game, longest rally %d, fastest puck %.2f\n",
                   wrong,games,wins[Her],wins[You],games?static_cast<double>(goals)/games:0.0,games?seconds/games:0.0,
                   games?static_cast<double>(restarts)/games:0.0,longest,fastest);
        return head+lines;
    }

    bool SaveAirHockeyShot(const std::wstring& path,double seconds,std::uint32_t seed)
    {
        using namespace AirHockey;
        ULONG_PTR token=0;
        GdiplusStartupInput input;
        if(!state.gdiplus&&GdiplusStartup(&token,&input,nullptr)!=Ok)return false;
        bool saved=false;
        {
            Game game;
            Brain you(You,Style{0.25f,0.12f,0.6f,1.0f},seed*7919u+1),her(Her,Style{},seed*104729u+2);
            const double dt=1/60.0;
            for(double t=0;t<seconds&&game.Now()!=Phase::Over;t+=dt)
            {
                const Vec a=you.Think(game,dt),b=her.Think(game,dt);
                game.Step(dt,a,b);
                game.TakeEvents();
            }
            const int w=600,h=900;
            Bitmap image(w,h,PixelFormat32bppPARGB);
            Graphics g(&image);
            const Layout l=Place(w,h,1,game.Rule());
            DrawTable(g,w,h,l,game.Rule());
            DrawPlay(g,w,l,game,false,false,false);
            saved=SavePng(image,path);
        }
        if(token)GdiplusShutdown(token);
        return saved;
    }
}
