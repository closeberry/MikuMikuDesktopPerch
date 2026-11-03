#define NOMINMAX
#include "HexWarWindow.hpp"
#include "HexWar.hpp"
#include "MahjongSound.hpp"
#include "HexWarSound.hpp"
#include "Log.hpp"
#include "MascotModel.hpp"
#include "MascotRenderer.hpp"
#include "Text.hpp"
#include "BubbleText.hpp"
#include "GameLines.hpp"
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <shlwapi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <tuple>
#include <random>
#include <fstream>
#include <chrono>
#include <sstream>
#pragma comment(lib,"gdiplus.lib")
#pragma comment(lib,"shlwapi.lib")

namespace MikuMikuDesktopPerch
{
    namespace
    {
        using namespace Gdiplus;
        using HexWar::Phase;
        using HexWar::Kind;
        using HexWar::Terrain;
        using HexWar::You;
        using HexWar::Her;
        constexpr wchar_t ClassName[]=L"MikuMikuDesktopPerchHexWar";
        constexpr wchar_t ViewClassName[]=L"MikuMikuDesktopPerchHexWarView";
        constexpr UINT_PTR TimerId=1;
        // How carefully she plays (HexWar::Table::NewGame): a little less as you grow closer,
        // and after games you lost in a row.
        constexpr double Care=0.85,LessPerBond=0.03,LessPerLoss=0.08,LeastCare=0.4;
        // Her pace: before each of her units acts, before the game's end is shown.
        constexpr ULONGLONG HerStepWait=550,HerTurnWait=1500,EndWait=900;
        constexpr double BannerShown=1.6;   // seconds "Your turn" stays
        constexpr ULONGLONG SaidShown=1800;
        constexpr ULONGLONG EndWait3D=2200;        // at the 3D table: the end waits for her reaction
        // A unit moving goes a hex in this long (seconds).
        constexpr double HexGlide=0.11;
        // The battle scene's parts (seconds): the two sides come in, both shoot at once (each
        // member in turn, the defender's half a gap after the attacker's), the result held.
        // A battle's entrance: each member rolls (or runs) in for MoveIn seconds, the last
        // starting LastIn after the first; vehicles pitch forward for Pitch on stopping; Settle
        // after that the shooting begins (FightIn).
        constexpr double MoveIn=0.8,LastIn=0.34,Pitch=0.225,Settle=0.5;
        constexpr double FightIn=LastIn+MoveIn+Pitch+Settle,ShotGap=0.2,FightHold=1.6;
        constexpr double FightFlight=0.22;   // a round's time from the gun to its mark
        double ShotStart(int side){return FightIn+(side?ShotGap/2:0);}
        // Sizes in logical pixels (at 96 dpi, at the size the window opens).
        constexpr float BaseWidth=1240,BaseHeight=860;
        constexpr float HerColumn=0.25f;           // her column at the right, a share of the window's width
        constexpr float LeastRadius=26;            // a hex's radius at least (a larger map scrolls)

        struct State
        {
            HWND window=nullptr;
            ULONG_PTR gdiplus=0;
            HexWarOpponent her;
            std::unique_ptr<Bitmap> face;
            std::function<void()> onEnd;
            HexWar::Table table;
            int lost=0;                // games you lost in a row
            int map=0;                 // the map played
            // The maps played and won (by index), kept in her.progressFile; the map choice shown,
            // the one the keys are on, and its cards.
            std::vector<int> played,won;
            bool choosing=false;
            int choice=0;
            std::vector<RectF> cards;
            // The side you take: -1 by chance each game, 0 the map's first (its 'B' base), 1 the other.
            int side=-1;
            RectF sideButtons[3],startButton;
            bool gameOverTold=false;
            int hover=-1,cursor=-1;    // the hex under the mouse, and the keys' hex
            int selected=-1;           // the unit of yours chosen (its id)
            bool infoShown=false;      // the chosen unit's details shown (right click)
            bool overlayDue=false;     // (3D) the 2D layer to be drawn again with the next frame
            bool confirmResign=false;
            RectF quitButtons[2];
            RectF endButton;           // "end the turn"
            bool overEnd=false;
            ULONGLONG nextAt=0;
            ULONGLONG idleSince=0;     // when you were first left with nothing to do this turn (0: not)
            // A unit on its way along its path (the table has it at its end already), and the
            // battle shown after it (the table's counts are already after it).
            struct Glide {int unit=-1;std::vector<int> path;double start=0;} glide;
            struct Fight {bool on=false;HexWar::Battle b;double start=0,heard=0;} fight;
            bool fightNext=false;
            HexWar::Battle pending;
            int actionsSeen=0;
            // Whose turn it now is, shown across the map for a moment as each turn starts (or an
            // order given, with what it does below).
            std::wstring banner,bannerSub;
            double bannerAt=-100;
            int bannerFor=-1;          // the turn and side it was shown for
            bool bannerYours=true;
            // The commander's orders: the menu open, a strike waiting for its target, the menu's
            // rows and its button, and an order being shown (a strike's shells on their hex).
            bool orderMenu=false,aiming=false,overOrder=false;
            PointF hoverPoint;
            RectF orderButton,orderRows[HexWar::Skills];
            // The factory of yours whose menu of what to build is open (-1: none), its rows.
            int buildMenu=-1;
            std::vector<HexWar::Kind> buildKinds;
            std::vector<RectF> buildRows;
            struct Shown {bool on=false;HexWar::Action a;double start=0,heard=0;} order;
            // The map's view: its scroll (pixels), the hexes' radius, where the map's area is.
            float scrollX=0,scrollY=0,radius=30;
            RectF mapArea;
            bool panning=false;
            POINT panFrom{};
            std::wstring said;         // why a move cannot be made, in the big bubble
            ULONGLONG saidAt=0;
            // Her words in the bubble (as at mahjong).
            struct Words
            {
                std::wstring text,fallback;
                ULONGLONG since=0,until=0,asked=0;
                bool waiting=false;
                bool voiced=false;   // the words are said by a voice file of their own
                int weight=0;
                std::vector<std::wstring> recent;
                std::wstring revealing;
                double shown=0;
                ULONGLONG shownAt=0;
                std::wstring told;
            } words;
            // Her lines by scene (res/hexwar/lines.txt, said again her way by the LLM, and the
            // user's own lines file), and her voice for a line that has one.
            GameLines lines;
            GameVoice voice;
            bool prefetching=false;
            Expression tell=Expression::Neutral;   // her face while she is far ahead, or behind
            POINT headScreen{-1,-1};
            std::wstring afterword,afterwordLine;
            struct Seen
            {
                bool started=false,over=false;
                int actions=0,turn=0;
                bool sureWin=false,behind=false;   // said once each game
            } seen;
            std::unique_ptr<Bitmap> canvas;
            // Her at the table in 3D (none: 2D only).
            HWND view=nullptr;
            std::unique_ptr<MascotRenderer> renderer;
            int entrance=0;
            float herX=0,herZ=0,herAngle=0,entranceTime=0,chairZ=0;
            struct Camera {float yaw=0,pitch=0.12f,distance=3.0f,x=0,y=0,z=0;} camera;
            std::array<float,3> follow{};   // where the camera looks (her head, followed)
            bool following=false;
            POINT dragFrom{};
            int dragging=0;
            float upper=10,hips=10;
            float floorY=-5;
            std::vector<int> scene;
            double clock=0;
            struct Spark {std::array<float,3> at,velocity;float age,life,size,angle,spin;std::array<float,4> color;bool glow;};
            std::vector<Spark> sparks;
            std::vector<MascotRenderer::Particle> roomGlows;
            std::mt19937 sparkRandom{12345};
            double thinkFor=0;
            std::wstring endGesture;
            double endGestureUntil=0;
            bool seenOver=false;
        } state;

        double Seconds(){return static_cast<double>(GetTickCount64())/1000.0;}
        constexpr double OrderShown=1.8;   // seconds an order is shown before play goes on

        float Scale()
        {
            if(!state.window)return 1;
            const float dpi=static_cast<float>(GetDpiForWindow(state.window))/96.0f;
            RECT client{};
            GetClientRect(state.window,&client);
            const float grow=std::min(static_cast<float>(client.right)/(BaseWidth*dpi),static_cast<float>(client.bottom)/(BaseHeight*dpi));
            return dpi*std::clamp(grow,0.6f,2.2f);
        }

        void Redraw(){if(state.window)InvalidateRect(state.window,nullptr,FALSE);}

        // The brush hand of the mahjong result screen (res/mahjong/brush.ttf), null when it
        // cannot be loaded.
        const FontFamily* Brush()
        {
            static PrivateFontCollection fonts;
            static std::unique_ptr<FontFamily> family=[]{
                std::unique_ptr<FontFamily> loaded;
                const HRSRC info=FindResourceW(nullptr,L"BRUSH",L"MAHJONGFONT");
                const HGLOBAL data=info?LoadResource(nullptr,info):nullptr;
                if(!data||fonts.AddMemoryFont(LockResource(data),static_cast<INT>(SizeofResource(nullptr,info)))!=Ok||fonts.GetFamilyCount()<1)return loaded;
                loaded=std::make_unique<FontFamily>();
                INT found=0;
                fonts.GetFamilies(1,loaded.get(),&found);
                if(found<1)loaded.reset();
                return loaded;
            }();
            return family.get();
        }

        // Whether the brush hand has every character of text (it is cut down to what the games
        // write: a name, or a word it lacks, is written in the plain hand).
        bool BrushHas(const std::wstring& text)
        {
            static HFONT font=[]{
                HFONT made=nullptr;
                const FontFamily* family=Brush();
                const HRSRC info=FindResourceW(nullptr,L"BRUSH",L"MAHJONGFONT");
                const HGLOBAL data=info?LoadResource(nullptr,info):nullptr;
                DWORD added=0;
                WCHAR name[LF_FACESIZE]{};
                if(!family||!data||family->GetFamilyName(name)!=Ok||
                   !AddFontMemResourceEx(LockResource(data),SizeofResource(nullptr,info),nullptr,&added))return made;
                made=CreateFontW(-32,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,0,0,name);
                // Another font given in its place would have the characters: none then.
                if(made)
                {
                    const HDC dc=CreateCompatibleDC(nullptr);
                    const HGDIOBJ old=SelectObject(dc,made);
                    WCHAR face[LF_FACESIZE]{};
                    GetTextFaceW(dc,LF_FACESIZE,face);
                    SelectObject(dc,old);
                    DeleteDC(dc);
                    if(_wcsicmp(face,name)!=0){DeleteObject(made);made=nullptr;}
                }
                return made;
            }();
            if(!font||text.empty())return false;
            const HDC dc=CreateCompatibleDC(nullptr);
            const HGDIOBJ old=SelectObject(dc,font);
            std::vector<WORD> glyphs(text.size());
            const bool ok=GetGlyphIndicesW(dc,text.c_str(),static_cast<int>(text.size()),glyphs.data(),GGI_MARK_NONEXISTING_GLYPHS)!=GDI_ERROR&&
                          std::none_of(glyphs.begin(),glyphs.end(),[](WORD g){return g==0xFFFF;});
            SelectObject(dc,old);
            DeleteDC(dc);
            return ok;
        }

        const FontFamily* Family()
        {
            static FontFamily gothic(L"Yu Gothic UI");
            return gothic.IsAvailable()?&gothic:FontFamily::GenericSansSerif();
        }

        // brush: in the brush hand when it has the characters.
        void Text(Graphics& g,const std::wstring& text,const RectF& box,float size,Color color,bool bold=false,
                  StringAlignment across=StringAlignmentNear,StringAlignment down=StringAlignmentNear,bool brush=false)
        {
            const FontFamily* family=brush&&Brush()&&BrushHas(text)?Brush():Family();
            if(family!=Family()){bold=false;size*=1.08f;}
            Font font(family,size,bold?FontStyleBold:FontStyleRegular,UnitPixel);
            SolidBrush paint(color);
            StringFormat format;
            format.SetAlignment(across);
            format.SetLineAlignment(down);
            format.SetFormatFlags(StringFormatFlagsNoWrap);
            g.DrawString(text.c_str(),static_cast<INT>(text.size()),&font,box,&format,&paint);
        }

        float TextWidth(Graphics& g,const std::wstring& text,float size,bool bold=false,bool brush=false)
        {
            const FontFamily* family=brush&&Brush()&&BrushHas(text)?Brush():Family();
            if(family!=Family()){bold=false;size*=1.08f;}
            Font font(family,size,bold?FontStyleBold:FontStyleRegular,UnitPixel);
            RectF box;
            StringFormat format;
            format.SetFormatFlags(StringFormatFlagsMeasureTrailingSpaces);
            g.MeasureString(text.c_str(),static_cast<INT>(text.size()),&font,PointF(0,0),&format,&box);
            return box.Width;
        }

        void RoundRect(GraphicsPath& path,float x,float y,float w,float h,float r)
        {
            const float d=r*2;
            path.AddArc(x,y,d,d,180,90);
            path.AddArc(x+w-d,y,d,d,270,90);
            path.AddArc(x+w-d,y+h-d,d,d,0,90);
            path.AddArc(x,y+h-d,d,d,90,90);
            path.CloseFigure();
        }

        void FillRound(Graphics& g,const RectF& box,float r,Color fill,Color edge=Color(0,0,0,0),float edgeWidth=1)
        {
            GraphicsPath path;
            RoundRect(path,box.X,box.Y,box.Width,box.Height,r);
            SolidBrush brush(fill);
            g.FillPath(&brush,&path);
            if(edge.GetA()){Pen pen(edge,edgeWidth);g.DrawPath(&pen,&path);}
        }

        std::wstring HerName(){return state.her.name.empty()?std::wstring(T(L"相手",L"Her")):state.her.name;}
        double CharSeconds(){return English()?0.045:0.12;}
        bool Waiting(){return state.nextAt>GetTickCount64();}
        // A move or a battle still being shown (one done but not yet seen counts).
        bool Showing()
        {
            return state.actionsSeen!=state.table.Actions()||state.glide.unit>=0||state.fight.on||state.fightNext||state.order.on;
        }
        bool YourTurnNow(){return state.table.Now()==Phase::YourTurn&&!Showing();}
        // Whether none of your units can still do anything this turn: each finished, or moved
        // with no one to attack, or neither able to move nor to attack.
        bool NothingLeft()
        {
            const auto& t=state.table;
            for(const auto& u:t.Units())
            {
                if(u.who!=You||u.count<=0||u.done)continue;
                if(!t.Targets(u.id).empty()||!t.BridgeSpots(u.id).empty())return false;
                if(!u.moved)
                    for(const auto& q:t.Reach(u.id))
                        if(q.to!=u.at)return false;
            }
            return true;
        }
        constexpr ULONGLONG AutoEndWait=700;   // ms
        bool EndShown(){return state.table.Now()==Phase::GameOver&&!Waiting()&&!Showing();}

        LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam);

        bool OverHud(LPARAM lParam)
        {
            const int x=GET_X_LPARAM(lParam),y=GET_Y_LPARAM(lParam);
            if(!state.canvas||x<0||y<0||x>=static_cast<int>(state.canvas->GetWidth())||y>=static_cast<int>(state.canvas->GetHeight()))return false;
            Color c;
            state.canvas->GetPixel(x,y,&c);
            return c.GetA()>0;
        }

        LRESULT CALLBACK ViewProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            // The camera as at the mahjong table, over her column: the right button turns it,
            // the middle one (or Shift and the right) moves it, the wheel comes nearer; a right
            // double-click or F1 puts it back. A left click is the window's (a hex, a button), and
            // so are the right and middle buttons over the map (taking back, dragging the map).
            auto& c=state.camera;
            if(message==WM_LBUTTONDOWN||message==WM_LBUTTONDBLCLK)
            {
                SetFocus(state.window);
                return Proc(state.window,WM_LBUTTONDOWN,wParam,lParam);
            }
            if((message==WM_RBUTTONDOWN||message==WM_RBUTTONDBLCLK||message==WM_MBUTTONDOWN)&&!state.dragging&&
               state.mapArea.Contains(static_cast<float>(GET_X_LPARAM(lParam)),static_cast<float>(GET_Y_LPARAM(lParam))))
            {
                SetFocus(state.window);
                return Proc(state.window,message==WM_RBUTTONDBLCLK?WM_RBUTTONDOWN:message,wParam,lParam);
            }
            switch(message)
            {
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
                SetFocus(state.window);
                state.dragging=message==WM_MBUTTONDOWN||(wParam&MK_SHIFT)?2:1;
                state.dragFrom={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
                SetCapture(hwnd);
                return 0;
            case WM_MOUSEMOVE:
                if(!state.dragging)return Proc(state.window,message,wParam,lParam);
                else
                {
                    const float dx=static_cast<float>(GET_X_LPARAM(lParam)-state.dragFrom.x),dy=static_cast<float>(GET_Y_LPARAM(lParam)-state.dragFrom.y);
                    state.dragFrom={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
                    const float k=1/Scale();
                    if(state.dragging==1)
                    {
                        c.yaw-=dx*k*0.008f;
                        c.pitch=std::clamp(c.pitch+dy*k*0.006f,-0.35f,1.5f);
                    }
                    else
                    {
                        const float step=c.distance*0.0025f*k;
                        c.x-=std::cos(c.yaw)*dx*step;
                        c.z-=std::sin(c.yaw)*dx*step;
                        c.y+=dy*step;
                    }
                }
                return 0;
            case WM_RBUTTONUP:
            case WM_MBUTTONUP:
                state.dragging=0;
                ReleaseCapture();
                return 0;
            case WM_CAPTURECHANGED:
                state.dragging=0;
                return 0;
            case WM_MOUSEWHEEL:
                c.distance=std::clamp(c.distance*std::pow(0.9f,GET_WHEEL_DELTA_WPARAM(wParam)/120.0f),0.5f,4.5f);
                return 0;
            case WM_RBUTTONDBLCLK:
                c={};
                return 0;
            case WM_CONTEXTMENU:
                return 0;
            case WM_ERASEBKGND:
                return 1;
            }
            return DefWindowProcW(hwnd,message,wParam,lParam);
        }

        void PlaceView()
        {
            if(!state.view)return;
            RECT client{};
            GetClientRect(state.window,&client);
            MoveWindow(state.view,0,0,client.right,client.bottom,TRUE);
            std::wstring error;
            if(state.renderer&&!state.renderer->Resize(static_cast<unsigned>(std::max(1L,client.right)),static_cast<unsigned>(std::max(1L,client.bottom)),error))
                Log(L"hexwar: resize: %s",error.c_str());
            InvalidateRect(state.window,nullptr,FALSE);
        }

        void ReleaseView()
        {
            if(state.renderer){state.renderer->WaitIdle();state.renderer.reset();Log(L"hexwar: 3D view released");}
            state.view=nullptr;
        }

        // ---- The room (as the mahjong table's) ----

        constexpr int ScrollW=320,ScrollH=768;   // the hanging scroll's painting: the 3D table's texture

        float FloorY(){return state.floorY;}

        void ScrollSpan(float& h0,float& h1,float& width)
        {
            const float D=state.upper,floor=FloorY(),ceiling=floor+1.9f*(state.hips+state.upper),wain=floor+0.55f*state.hips;
            h0=wain+0.25f*D;h1=ceiling-0.6f*D;width=1.0f*D;
        }

        // The hanging scroll's painting: an ink moon over autumn grass, susuki bending, a red
        // seal at the bottom right.
        void PaintScroll(Graphics& g,int w,int h)
        {
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            const float W=static_cast<float>(w),H=static_cast<float>(h);
            LinearGradientBrush paper(PointF(0,0),PointF(0,H),Color(255,236,231,218),Color(255,224,218,202));
            g.FillRectangle(&paper,0.0f,0.0f,W,H);
            SolidBrush moon(Color(150,214,200,150));
            g.FillEllipse(&moon,W*0.38f,H*0.12f,W*0.42f,W*0.42f);
            // A low hill of ink fading upward, the grass rising out of it.
            GraphicsPath hill;
            hill.AddBezier(PointF(0,H*0.78f),PointF(W*0.3f,H*0.66f),PointF(W*0.7f,H*0.70f),PointF(W,H*0.64f));
            hill.AddLine(PointF(W,H),PointF(0,H));
            hill.CloseFigure();
            LinearGradientBrush ink(PointF(0,H*0.62f),PointF(0,H*0.95f),Color(200,40,40,44),Color(60,40,40,44));
            g.FillPath(&ink,&hill);
            std::mt19937 grass(17);
            std::uniform_real_distribution<float> any(0,1);
            for(int i=0;i<34;++i)
            {
                const float x=W*(0.03f+0.94f*any(grass)),base=H*(0.74f+0.06f*any(grass)),tall=H*(0.18f+0.22f*any(grass)),bend=W*(0.08f+0.2f*any(grass));
                Pen blade(Color(static_cast<BYTE>(150+80*any(grass)),36,34,30),W*(0.006f+0.006f*any(grass)));
                blade.SetEndCap(LineCapRound);
                g.DrawBezier(&blade,PointF(x,base),PointF(x+bend*0.2f,base-tall*0.5f),PointF(x+bend*0.6f,base-tall*0.85f),PointF(x+bend,base-tall*0.95f));
                if(i%3==0)
                {
                    Pen plume(Color(120,150,130,110),W*0.02f);
                    plume.SetEndCap(LineCapRound);
                    g.DrawLine(&plume,PointF(x+bend*0.85f,base-tall*0.93f),PointF(x+bend*1.1f,base-tall*0.82f));
                }
            }
            SolidBrush seal(Color(200,150,72,62));
            g.FillRectangle(&seal,W*0.78f,H*0.88f,W*0.09f,W*0.09f);
            Pen cut(Color(150,240,220,200),W*0.008f);
            g.DrawRectangle(&cut,W*0.795f,H*0.88f+W*0.015f,W*0.06f,W*0.06f);
        }


        // The 3D room's texture: the scroll's painting at the top left.
        constexpr int AtlasW=512,AtlasH=1024;

        ImageRgba8 ScrollImage()
        {
            ImageRgba8 image;
            Bitmap atlas(AtlasW,AtlasH,PixelFormat32bppARGB);
            {
                Graphics g(&atlas);
                g.Clear(Color(255,236,231,218));
                Bitmap painting(ScrollW,ScrollH,PixelFormat32bppARGB);
                {
                    Graphics p(&painting);
                    float h0,h1,width;
                    ScrollSpan(h0,h1,width);
                    const int tall=std::max(ScrollW,static_cast<int>(std::lround(ScrollW*(h1-h0)/std::max(width,0.001f))));
                    p.ScaleTransform(1.0f,static_cast<float>(ScrollH)/tall);
                    PaintScroll(p,ScrollW,tall);
                }
                g.DrawImage(&painting,0,0,ScrollW,ScrollH);
            }
            BitmapData bits{};
            Rect whole(0,0,AtlasW,AtlasH);
            if(atlas.LockBits(&whole,ImageLockModeRead,PixelFormat32bppARGB,&bits)!=Ok)return image;
            image.levels.resize(1);
            auto& level=image.levels[0];
            level.width=AtlasW;level.height=AtlasH;
            level.pixels.resize(static_cast<size_t>(AtlasW)*AtlasH*4);
            for(int y=0;y<AtlasH;++y)
            {
                const BYTE* from=static_cast<const BYTE*>(bits.Scan0)+static_cast<size_t>(y)*bits.Stride;
                BYTE* to=level.pixels.data()+static_cast<size_t>(y)*AtlasW*4;
                for(int x=0;x<AtlasW;++x,from+=4,to+=4){to[0]=from[2];to[1]=from[1];to[2]=from[0];to[3]=255;}
            }
            atlas.UnlockBits(&bits);
            GenerateMips(image);
            return image;
        }

        bool CreateView()
        {
            if(!state.her.model)return false;
            const HINSTANCE instance=GetModuleHandleW(nullptr);
            static bool registered=false;
            if(!registered)
            {
                WNDCLASSEXW wc{sizeof(wc)};
                wc.style=CS_DBLCLKS;
                wc.lpfnWndProc=ViewProc;
                wc.hInstance=instance;
                wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
                wc.lpszClassName=ViewClassName;
                RegisterClassExW(&wc);
                registered=true;
            }
            RECT client{};
            GetClientRect(state.window,&client);
            const unsigned w=static_cast<unsigned>(std::max(1L,client.right)),h=static_cast<unsigned>(std::max(1L,client.bottom));
            state.view=CreateWindowExW(0,ViewClassName,L"",WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,0,0,static_cast<int>(w),static_cast<int>(h),state.window,nullptr,instance,nullptr);
            if(!state.view)return false;
            auto renderer=std::make_unique<MascotRenderer>();
            renderer->SetShadowMapSize(16);
            renderer->SetSelfShadow(false);
            renderer->SetBackground(30/255.0f,24/255.0f,22/255.0f,1);
            std::wstring error;
            if(!renderer->Initialize(state.view,w,h,state.her.light?1:state.her.msaa,false,error)||
               !renderer->LoadModel(*state.her.model,state.her.toonDir,state.her.light?512:state.her.maxTexture,error))
            {
                Log(L"hexwar: no 3D view (%s)",error.c_str());
                renderer->WaitIdle();
                DestroyWindow(state.view);
                state.view=nullptr;
                return false;
            }
            renderer->SetLight(20,45);
            std::array<float,3> lo{},hi{},legLo{},legHi{},head{};
            if(state.her.model->StandardBoneBounds(lo,hi)&&state.her.model->LegBounds(legLo,legHi)&&state.her.model->HeadPosition(head)&&head[1]>legHi[1])
            {
                state.hips=std::max(legHi[1]-lo[1],0.1f);
                state.upper=head[1]-legHi[1];
            }
            // The floor below the seat, low enough for her feet as she sits (as at mahjong).
            state.floorY=-0.5f*state.hips;
            if(state.her.model->CanSit())
            {
                MascotModel& model=*state.her.model;
                model.SetActivity(Activity::Sit,0);
                for(int i=0;i<90;++i)model.Update(1/30.0);
                if(std::array<float,3> legLo{},legHi{};model.LegBounds(legLo,legHi))
                    state.floorY=std::min(state.floorY,legLo[1]-0.03f*state.hips);
                model.SetActivity(Activity::Idle);
                for(int i=0;i<90;++i)model.Update(1/30.0);
                model.ResetPhysics();
            }
            Log(L"hexwar: 3D view %ux%u on %s (hips %.1f, upper body %.1f, floor %.1f)",w,h,renderer->AdapterName().c_str(),state.hips,state.upper,state.floorY);
            if(!renderer->SetGeometryTexture(ScrollImage()))Log(L"hexwar: no texture for the 3D table");
            state.renderer=std::move(renderer);
            state.entrance=0;
            state.scene.clear();
            state.camera={};
            state.dragging=0;
            state.sparks.clear();
            state.thinkFor=0;
            state.endGesture.clear();
            state.seenOver=false;
            return true;
        }

        constexpr float Wood=0.10f;
        constexpr float Washi=0.08f;

        struct Mesh
        {
            using V=std::array<float,3>;
            std::vector<MascotRenderer::GeometryVertex> out;
            int grain=0;
            float grainScale=1;
            float clear=0;
            void Quad(const V& a,const V& b,const V& c,const V& d,const V& n,const float (&col)[4],const float* uv=nullptr)
            {
                const V* p[4]={&a,&b,&c,&d};
                for(int k:{0,1,2,0,2,3})
                {
                    const V& q=*p[k];
                    MascotRenderer::GeometryVertex v{{q[0],q[1],q[2]},{n[0],n[1],n[2]},{col[0],col[1],col[2],col[3]}};
                    if(col[3]==Wood||col[3]==Washi)
                    {
                        const float along=q[grain==0?0:grain==1?2:1],up=q[grain==2?0:1],across=q[grain==1?0:2];
                        v.wall[0]=along/grainScale;v.wall[1]=up/grainScale;v.wall[3]=across/grainScale;
                    }
                    if(uv){v.uv[0]=uv[k*2];v.uv[1]=uv[k*2+1];v.uv[2]=1;}
                    v.uv[3]=clear;
                    out.push_back(v);
                }
            }
            void Box(float x0,float y0,float z0,float x1,float y1,float z1,const float (&c)[4],bool topFace=true)
            {
                if(topFace)Quad({x0,y1,z0},{x1,y1,z0},{x1,y1,z1},{x0,y1,z1},{0,1,0},c);
                Quad({x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},{0,0,-1},c);
                Quad({x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1},{0,0,1},c);
                Quad({x0,y0,z0},{x0,y0,z1},{x0,y1,z1},{x0,y1,z0},{-1,0,0},c);
                Quad({x1,y0,z0},{x1,y0,z1},{x1,y1,z1},{x1,y1,z0},{1,0,0},c);
            }
        };

        float SeatDepth(){return 0.27f*state.hips+0.22f*state.upper;}
        float SeatFront(){return 0.27f*state.hips-SeatDepth()/2;}
        float SeatZ(){return 0.15f*state.upper+SeatDepth();}


        void ScrollUv(float (&uv)[8])
        {
            const float u0=1.0f/AtlasW,u1=(ScrollW-1.0f)/AtlasW,v0=1.0f/AtlasH,v1=(ScrollH-1.0f)/AtlasH;
            const float corners[8]={u0,v0,u1,v0,u1,v1,u0,v1};
            std::copy(std::begin(corners),std::end(corners),uv);
        }

        struct TableShape {float x0,x1,zNear,zFar,top,rw,frameTop,frameBottom,leg;};
        TableShape Table3D()
        {
            const float D=state.upper,top=0.12f*state.hips+0.30f*D;
            return {-1.3f*D,1.3f*D,-0.30f*D,-2.15f*D,top,0.11f*D,top+0.04f*D,top-0.11f*D,0.15f*D};
        }

        std::vector<MascotRenderer::GeometryVertex> BuildChair(float z)
        {
            const float D=state.upper,floor=FloorY(),cw=0.34f*D;
            z-=SeatFront();
            const float back=SeatDepth();
            const float frame[4]={0.34f,0.19f,0.10f,Wood},legs[4]={0.26f,0.14f,0.07f,Wood},cushion[4]={0.48f,0.10f,0.12f,1};
            Mesh m;
            m.grainScale=0.35f;
            m.grain=1;
            m.Box(-cw,-0.08f*D,z,cw,-0.03f*D,z+back,frame);
            m.Box(-cw+0.02f*D,-0.03f*D,z+0.02f*D,cw-0.02f*D,-0.005f*D,z+back-0.02f*D,cushion);
            m.grain=2;
            for(float lx:{-cw,cw-0.06f*D})
            {
                m.Box(lx,floor,z,lx+0.06f*D,-0.08f*D,z+0.06f*D,legs);
                m.Box(lx,floor,z+back-0.06f*D,lx+0.06f*D,0.75f*D,z+back,legs);
            }
            m.grain=0;
            for(float y:{0.30f*D,0.62f*D})m.Box(-cw,y,z+back-0.05f*D,cw,y+0.09f*D,z+back-0.01f*D,frame);
            return std::move(m.out);
        }

        // The table's wood and the room round it, as at the mahjong table (the chair is
        // BuildChair's).
        void BuildRoom(Mesh& m,float zc)
        {
            using V=std::array<float,3>;
            auto box=[&](float x0,float y0,float z0,float x1,float y1,float z1,const float (&c)[4]){m.Box(x0,y0,z0,x1,y1,z1,c);};
            auto quad=[&](const V& a,const V& b,const V& c,const V& d,const V& n,const float (&col)[4],const float* uv=nullptr){m.Quad(a,b,c,d,n,col,uv);};
            int& grain=m.grain;
            float& grainScale=m.grainScale;
            const float D=state.upper,top=0.12f*state.hips+0.30f*D,floor=FloorY();
            const float frame[4]={0.34f,0.19f,0.10f,Wood},skirt[4]={0.26f,0.14f,0.07f,Wood},planks[4]={0.46f,0.32f,0.20f,Wood};
            const float trim[4]={0.30f,0.18f,0.10f,Wood},beams[4]={0.20f,0.12f,0.07f,Wood};
            const float plaster[4]={0.80f,0.74f,0.62f,Washi},paper[4]={1.0f,0.93f,0.78f,0.25f};
            const float brass[4]={0.80f,0.62f,0.30f,1};
            const TableShape t=Table3D();
            const float x0=t.x0,x1=t.x1,zNear=t.zNear,zFar=t.zFar;
            grainScale=0.35f;
            const float rw=t.rw,frameTop=t.frameTop,frameBottom=t.frameBottom,leg=t.leg;
            grain=0;
            box(x0-rw,frameBottom,zFar-rw,x1+rw,frameTop,zFar,frame);
            box(x0-rw,frameBottom,zNear,x1+rw,frameTop,zNear+rw,frame);
            grain=1;
            box(x0-rw,frameBottom,zFar,x0,frameTop,zNear,frame);
            box(x1,frameBottom,zFar,x1+rw,frameTop,zNear,frame);
            grain=2;
            for(float lx:{x0-rw+0.02f*D,x1+rw-0.02f*D-leg})
                for(float lz:{zFar-rw+0.02f*D,zNear+rw-0.02f*D-leg})
                    box(lx,floor,lz,lx+leg,frameBottom,lz+leg,skirt);

            const float rx=6.0f*D,zBack=5.0f*D,zFront=-7.5f*D,ceiling=floor+1.9f*(state.hips+state.upper);
            grain=0;grainScale=1;
            quad({-rx,floor,zBack},{rx,floor,zBack},{rx,floor,zFront},{-rx,floor,zFront},{0,1,0},planks);
            quad({-rx,ceiling,zBack},{rx,ceiling,zBack},{rx,ceiling,zFront},{-rx,ceiling,zFront},{0,-1,0},beams);
            const float wain=floor+0.55f*state.hips,proud=0.04f*D;
            auto wall=[&](const V& a,const V& b,const V& n){
                const bool alongX=std::abs(a[0]-b[0])>std::abs(a[2]-b[2]);
                grain=alongX?0:1;grainScale=0.22f*D;
                quad({a[0],wain,a[2]},{b[0],wain,b[2]},{b[0],ceiling,b[2]},{a[0],ceiling,a[2]},n,plaster);
                grainScale=0.6f;
                const V in{n[0]*proud,0,n[2]*proud};
                quad({a[0]+in[0],floor,a[2]+in[2]},{b[0]+in[0],floor,b[2]+in[2]},{b[0]+in[0],wain,b[2]+in[2]},{a[0]+in[0],wain,a[2]+in[2]},n,trim);
                const float lo[3]={std::min(a[0],b[0]),0,std::min(a[2],b[2])},hi[3]={std::max(a[0],b[0]),0,std::max(a[2],b[2])};
                const float ox=n[0]*proud*1.6f,oz=n[2]*proud*1.6f;
                box(lo[0]+std::min(0.0f,ox),wain-0.02f*D,lo[2]+std::min(0.0f,oz),hi[0]+std::max(0.0f,ox),wain+0.04f*D,hi[2]+std::max(0.0f,oz),beams);
                box(lo[0]+std::min(0.0f,ox),floor,lo[2]+std::min(0.0f,oz),hi[0]+std::max(0.0f,ox),floor+0.10f*D,hi[2]+std::max(0.0f,oz),beams);
                box(lo[0]+std::min(0.0f,ox),ceiling-0.12f*D,lo[2]+std::min(0.0f,oz),hi[0]+std::max(0.0f,ox),ceiling,hi[2]+std::max(0.0f,oz),beams);
            };
            wall({-rx,floor,zBack},{rx,floor,zBack},{0,0,-1});
            wall({rx,floor,zFront},{-rx,floor,zFront},{0,0,1});
            wall({-rx,floor,zFront},{-rx,floor,zBack},{1,0,0});
            wall({rx,floor,zBack},{rx,floor,zFront},{-1,0,0});
            grain=2;grainScale=0.5f;
            const float pw=0.22f*D;
            for(float px:{-rx,-rx/3,rx/3,rx})
            {
                box(px-pw/2,floor,zBack-pw,px+pw/2,ceiling,zBack,trim);
                box(px-pw/2,floor,zFront,px+pw/2,ceiling,zFront+pw,trim);
            }
            for(float pz:{zFront+(zBack-zFront)/3,zFront+2*(zBack-zFront)/3})
            {
                box(-rx,floor,pz-pw/2,-rx+pw,ceiling,pz+pw/2,trim);
                box(rx-pw,floor,pz-pw/2,rx,ceiling,pz+pw/2,trim);
            }
            state.roomGlows.clear();
            for(float side:{-1.0f,1.0f})
            {
                const float cx=side*2*rx/3,sx=0.62f*(rx/3-pw/2),sy0=wain+0.08f*D,sy1=ceiling-0.5f*D,sz=zBack-0.06f*D;
                quad({cx-sx,sy0,sz},{cx+sx,sy0,sz},{cx+sx,sy1,sz},{cx-sx,sy1,sz},{0,0,-1},paper);
                grain=0;grainScale=0.4f;
                const float bar=0.025f*D,edge=0.07f*D;
                for(int i=0;i<=4;++i)
                {
                    const float y=sy0+(sy1-sy0)*i/4,b=i==0||i==4?edge:bar;
                    box(cx-sx-edge,y-b,sz-0.06f*D,cx+sx+edge,y+b,sz-0.005f*D,trim);
                }
                grain=2;
                for(int i=0;i<=6;++i)
                {
                    const float x=cx-sx+2*sx*i/6,b=i==0||i==6?edge:bar;
                    box(x-b,sy0,sz-0.06f*D,x+b,sy1,sz-0.005f*D,trim);
                }
                for(int j=0;j<2;++j)
                {
                    MascotRenderer::Particle g;
                    g.position={cx,sy0+(sy1-sy0)*(0.3f+0.4f*j),sz-0.1f*D};
                    g.size=1.9f*sx;
                    g.color={1.0f,0.88f,0.66f,0.38f};
                    g.glow=true;
                    state.roomGlows.push_back(g);
                }
            }
            {
                float h0,h1,paperWidth;
                ScrollSpan(h0,h1,paperWidth);
                const float wx=-rx+0.05f*D,sz=-0.6f*D;
                const float scroll[4]={1,1,1,1},cloth[4]={0.30f,0.36f,0.30f,1};
                quad({wx,h0-0.1f*D,sz-0.62f*D},{wx,h0-0.1f*D,sz+0.62f*D},{wx,h1+0.1f*D,sz+0.62f*D},{wx,h1+0.1f*D,sz-0.62f*D},{1,0,0},cloth);
                float uv[8];
                ScrollUv(uv);
                const float ruv[8]={uv[6],uv[7],uv[4],uv[5],uv[2],uv[3],uv[0],uv[1]};
                quad({wx+0.005f*D,h0,sz-0.5f*D},{wx+0.005f*D,h0,sz+0.5f*D},{wx+0.005f*D,h1,sz+0.5f*D},{wx+0.005f*D,h1,sz-0.5f*D},{1,0,0},scroll,ruv);
                grain=1;grainScale=0.35f;
                box(wx-0.04f*D,h0-0.16f*D,sz-0.7f*D,wx+0.08f*D,h0-0.08f*D,sz+0.7f*D,beams);
                box(wx-0.04f*D,h1+0.08f*D,sz-0.7f*D,wx+0.08f*D,h1+0.16f*D,sz+0.7f*D,beams);

                const float bx=rx-0.75f*D,bz0=-2.2f*D,bz1=0.2f*D,bt=floor+0.85f*state.hips;
                grain=1;
                box(bx,floor+0.08f*D,bz0,rx-0.02f*D,bt,bz1,skirt);
                box(bx-0.04f*D,bt,bz0-0.04f*D,rx-0.02f*D,bt+0.06f*D,bz1+0.04f*D,frame);
                for(int i=0;i<3;++i)
                {
                    const float z=bz0+(bz1-bz0)*(i+0.5f)/3;
                    box(bx-0.03f*D,bt-0.32f*D,z-0.03f*D,bx,bt-0.26f*D,z+0.03f*D,brass);
                }
            }
            {
                const float ly=top+2.4f*D,half=0.28f*D;
                grain=2;grainScale=0.35f;
                box(-0.012f*D,ly+0.36f*D,zc-0.012f*D,0.012f*D,ceiling,zc+0.012f*D,beams);
                const float glow[4]={1.0f,0.86f,0.60f,0.25f};
                box(-half,ly,zc-half,half,ly+0.36f*D,zc+half,glow);
                quad({-half,ly,zc-half},{half,ly,zc-half},{half,ly,zc+half},{-half,ly,zc+half},{0,-1,0},glow);
                grain=0;
                box(-half-0.02f*D,ly+0.33f*D,zc-half-0.02f*D,half+0.02f*D,ly+0.38f*D,zc+half+0.02f*D,frame);
                box(-half-0.02f*D,ly-0.02f*D,zc-half-0.02f*D,half+0.02f*D,ly+0.02f*D,zc+half+0.02f*D,frame);
                for(const auto& [size,alpha]:{std::pair{1.1f,0.95f},std::pair{2.0f,0.30f}})
                {
                    MascotRenderer::Particle g;
                    g.position={0,ly+0.18f*D,zc};
                    g.size=size*D;
                    g.color={1.0f,0.85f,0.58f,alpha};
                    g.glow=true;
                    state.roomGlows.push_back(g);
                }
            }
        }

        // ---- The table ----
        // The map is drawn flat over the window; the room's table only has its felt.

        float Top(){return 0.12f*state.hips+0.30f*state.upper;}
        float FieldZ(){return -1.10f*state.upper;}

        // What the table's geometry shows (nothing on it changes).
        std::vector<int> Scene(){return {1};}

        std::vector<MascotRenderer::GeometryVertex> BuildTable()
        {
            Mesh mesh;
            const float D=state.upper;
            const float felt[4]={0.09f,0.33f,0.22f,1};
            const TableShape t=Table3D();
            BuildRoom(mesh,FieldZ());
            mesh.Box(t.x0,t.top-0.05f*D,t.zFar,t.x1,t.top,t.zNear,felt);
            return std::move(mesh.out);
        }

        // ---- Names ----

        std::wstring KindName(Kind k)
        {
            switch(k)
            {
            case Kind::Tank:return T(L"戦車",L"Tank");
            case Kind::Bazooka:return T(L"対戦車兵",L"Anti-tank");
            case Kind::Scout:return T(L"装甲車",L"Scout car");
            case Kind::Artillery:return T(L"自走砲",L"SP gun");
            case Kind::Heavy:return T(L"重戦車",L"Heavy tank");
            case Kind::Fighter:return T(L"戦闘機",L"Fighter");
            case Kind::Attacker:return T(L"攻撃機",L"Attack aircraft");
            case Kind::AntiAir:return T(L"対空ミサイル車",L"AA missile truck");
            default:return T(L"歩兵",L"Infantry");
            }
        }
        // Its model name, shown with it (as on the battle screen).
        std::wstring KindCode(Kind k)
        {
            static const wchar_t* code[HexWar::Kinds]={L"T-4",L"I-1",L"AT-2",L"S-7",L"A-3",L"H-9",L"F-5",L"B-6",L"AA-8"};
            return code[static_cast<int>(k)];
        }
        bool Vehicle(Kind k){return HexWar::Info(k).armoured;}
        bool MovesOnFoot(Kind k){return k==Kind::Infantry||k==Kind::Bazooka;}
        bool Flies(Kind k){return HexWar::Info(k).flies;}
        // A unit of members moving: tracks or footsteps, as many at once as its members call for
        // (one for a few, four for a full unit), each a little apart in place and loudness.
        void PlayMoving(Kind k,float pan,float loud,double seconds,int members)
        {
            if(Flies(k))return;   // (no sound for aircraft yet)
            const int layers=std::clamp((members+2)/3,1,4);
            for(int i=0;i<layers;++i)
                PlayHexWarSound(MovesOnFoot(k)?HexWarSound::Steps:HexWarSound::Tracks,std::clamp(pan+0.15f*(i-0.5f*(layers-1)),-1.0f,1.0f),loud*0.75f,seconds);
        }
        // What a kind is good for, in a line or two.
        std::wstring KindAbout(Kind k)
        {
            switch(k)
            {
            case Kind::Tank:return T(L"攻守のそろった主力。山には入れず、荒地・森・廃墟は遅い。",L"The mainstay, strong both ways. No mountains; slow in rough, forest and ruins.");
            case Kind::Bazooka:return T(L"車両にとても強く、歩兵には弱い。歩兵と同じように動き、山も登れる。",L"Deadly to vehicles, weak against men. Moves as infantry, mountains too.");
            case Kind::Scout:return T(L"足が速く、平地と道路を一気に進む。守りは弱く、荒地は苦手。",L"Fast over open ground and roads; light, and slow in rough ground.");
            case Kind::Artillery:return T(L"2〜3マス先を撃ち、撃ち返されない。隣は撃てず、動いたターンは撃てない。守りは弱い。",L"Fires two or three hexes off and is never fired back at; not next to it, not after moving. Lightly protected.");
            case Kind::Heavy:return T(L"最も強く固いが遅い。廃墟はさらに遅い。",L"The strongest and toughest, but slow; slower still in ruins.");
            case Kind::Fighter:return T(L"空を飛び、地形に関係なく進む。飛行機を落とすのが得意で、地上は苦手。対空攻撃のできる味方だけが支援できる。",
                                        L"Flies over any ground. Made to bring down aircraft, poor against the ground. Only those that can shoot at aircraft support it.");
            case Kind::Attacker:return T(L"空を飛び、地形に関係なく進む。戦車や歩兵を上から撃つが、飛行機は撃てない。対空攻撃のできる相手にしか撃たれない。",
                                         L"Flies over any ground. Strikes tanks and men from above, cannot shoot at aircraft; only those that can shoot at aircraft hit it.");
            case Kind::AntiAir:return T(L"飛行機を撃ち落とすミサイル車。地上への攻撃は弱い。飛行機の戦いを支援できる。",
                                        L"A missile truck that brings down aircraft; weak against the ground. Supports battles in the air.");
            default:return T(L"弱いが安く早く作れる。工場と司令部を占領でき、山も登れる。",L"Weak but quick to build. Takes factories and bases, climbs mountains.");
            }
        }
        std::wstring TerrainName(Terrain t)
        {
            switch(t)
            {
            case Terrain::Rough:return T(L"荒地",L"Rough");
            case Terrain::Forest:return T(L"森",L"Forest");
            case Terrain::Mountain:return T(L"山",L"Mountain");
            case Terrain::River:return T(L"川",L"River");
            case Terrain::Road:return T(L"道路",L"Road");
            case Terrain::Bridge:return T(L"橋",L"Bridge");
            case Terrain::City:return T(L"廃墟",L"Ruins");
            case Terrain::Foundation:return T(L"建物跡",L"Foundations");
            case Terrain::Factory:return T(L"工場",L"Factory");
            case Terrain::Base:return T(L"司令部",L"Base");
            default:return T(L"平地",L"Plain");
            }
        }
        std::wstring SideName(int who){return who==You?std::wstring(T(L"あなた",L"You")):HerName();}
        std::wstring SkillName(HexWar::Skill s)
        {
            switch(s)
            {
            case HexWar::Skill::Barrage:return T(L"集中砲火",L"Barrage");
            case HexWar::Skill::Strike:return T(L"砲撃支援",L"Artillery strike");
            case HexWar::Skill::Smoke:return T(L"煙幕",L"Smoke screen");
            case HexWar::Skill::Repair:return T(L"補給",L"Resupply");
            default:return T(L"強行軍",L"Forced march");
            }
        }
        std::wstring SkillAbout(HexWar::Skill s)
        {
            switch(s)
            {
            case HexWar::Skill::Barrage:return T(L"このターン、味方の命中率が1.3倍",L"Your hit chance x1.3 this turn");
            case HexWar::Skill::Strike:return T(L"敵の部隊1つに砲撃（10発）",L"Ten shells on one enemy unit");
            case HexWar::Skill::Smoke:return T(L"次の自分のターンまで、味方が撃たれにくい（命中率0.6倍）",L"Shots at you x0.6 until your next turn");
            case HexWar::Skill::Repair:return T(L"味方の全部隊の数が2ずつ戻る",L"Two members back to every unit");
            default:return T(L"このターン、味方の移動力が1増える",L"One more hex of movement this turn");
            }
        }

        // ---- Playing ----

        const HexWar::Unit* Chosen(){return state.selected>=0?state.table.UnitById(state.selected):nullptr;}
        // The chosen unit when it is yours and may still act.
        const HexWar::Unit* Acting()
        {
            const HexWar::Unit* u=Chosen();
            return u&&u->who==You&&!u->done&&YourTurnNow()?u:nullptr;
        }

        // ---- The maps: which are open, and how far you have got ----

        void LoadProgress()
        {
            state.played.assign(static_cast<size_t>(HexWar::Maps()),0);
            state.won.assign(static_cast<size_t>(HexWar::Maps()),0);
            std::wifstream in(state.her.progressFile);
            std::wstring what;
            int map=0,n=0;
            while(in>>what>>map>>n)
                if(map>=0&&map<HexWar::Maps())(what==L"won"?state.won:state.played)[static_cast<size_t>(map)]=n;
        }

        void SaveProgress()
        {
            if(state.her.progressFile.empty())return;
            std::wofstream out(state.her.progressFile,std::ios::trunc);
            for(int m=0;m<HexWar::Maps();++m)
                out<<L"played "<<m<<L" "<<state.played[static_cast<size_t>(m)]<<L"\n"<<L"won "<<m<<L" "<<state.won[static_cast<size_t>(m)]<<L"\n";
        }

        // A map may be played once the one before it has been won, or played three times.
        constexpr int PlaysToOpen=3;
        bool Open(int m)
        {
            if(m<=0)return true;
            if(m>=HexWar::Maps()||!HexWar::Map(m).offered)return false;
            return Open(m-1)&&(state.won[static_cast<size_t>(m-1)]>0||state.played[static_cast<size_t>(m-1)]>=PlaysToOpen);
        }
        int Offered(){int n=0;for(int m=0;m<HexWar::Maps();++m)n+=HexWar::Map(m).offered?1:0;return n;}

        void Finish()
        {
            if(!state.window)return;
            KillTimer(state.window,TimerId);
            state.voice.Stop();
            if(state.her.model)
            {
                state.her.model->SetSolids({});
                state.her.model->SetMouth({},false);
            }
            if(state.her.model&&state.tell!=Expression::Neutral)state.her.model->SetExpression(Expression::Neutral,false);
            state.tell=Expression::Neutral;
            if(state.afterword.empty()&&state.seen.started&&state.table.Now()!=Phase::GameOver)
            {
                const auto& t=state.table;
                state.afterword=Fmt(T(L"いま、話し相手とヘックス戦線（ユニットを動かして戦う戦術ゲーム）をしていたが、途中でやめてデスクトップに戻ってきたところ（{0}ターン目まで進んでいた）。そのことを話し相手にひと言（1〜2文）",
                                      L"You were playing Hex Front (a war game of moving units) with them, but they stopped midway and you have come back to the desktop ({0} turns in). Say a word about it to them (one or two sentences)"),
                                    {std::to_wstring(t.Turn())});
                state.afterwordLine=T(L"また続き、やろうね。",L"Let's finish it another time.");
            }
            auto onEnd=std::move(state.onEnd);
            DestroyWindow(state.window);
            if(onEnd)onEnd();
        }

        void Prefetch();
        void Say(const wchar_t* name,int weight=2,double chance=1);

        void NewGame()
        {
            state.gameOverTold=false;
            std::random_device device;
            const bool swapped=state.side<0?(device()&1)!=0:state.side==1;
            state.table.NewGame(state.map,device(),std::max(LeastCare,Care-LessPerBond*state.her.bond-LessPerLoss*state.lost),swapped);
            state.hover=state.cursor=state.selected=-1;
            state.infoShown=false;
            state.confirmResign=false;
            state.said.clear();
            state.nextAt=GetTickCount64()+300;
            state.seen={};
            state.words.waiting=false;
            state.words.text.clear();
            state.afterword.clear();
            state.afterwordLine.clear();
            state.glide={};
            state.fight={};
            state.fightNext=false;
            state.actionsSeen=state.table.Actions();
            state.scrollX=state.scrollY=0;
            state.bannerFor=-1;
            state.orderMenu=state.aiming=false;
            state.order={};
            Prefetch();
        }

        void Tell(const std::wstring& why)
        {
            state.said=why;
            state.saidAt=GetTickCount64();
            Redraw();
        }

        void Choose(int id)
        {
            state.selected=id;
            state.infoShown=false;
            PlayMahjongSound(MahjongSound::CardPlace);
            Redraw();
        }

        // The chosen unit, moved but not attacking, is finished.
        void LetGo()
        {
            const HexWar::Unit* u=Acting();
            if(u&&u->moved)state.table.Finish(u->id);
            state.selected=-1;
            state.infoShown=false;
        }

        // A click on a hex in your turn: choose a unit (yours to act, hers to look at), move the
        // chosen one of yours to where it may go, or attack one of hers next to it.
        void OpenBuild(int hex);
        void Click(int hex)
        {
            if(!YourTurnNow()||hex<0)return;
            auto& t=state.table;
            const HexWar::Unit* there=t.UnitAt(hex);
            if(const HexWar::Unit* u=Acting())
            {
                const auto spots=t.BridgeSpots(u->id);
                if(std::find(spots.begin(),spots.end(),hex)!=spots.end())
                {
                    t.Bridge(u->id,hex);
                    state.selected=-1;
                    Redraw();
                    return;
                }
                if(there&&there->who==Her)
                {
                    const auto foes=t.Targets(u->id);
                    if(std::find(foes.begin(),foes.end(),there->id)!=foes.end())
                    {
                        const int id=u->id;
                        t.Attack(id,there->id);
                        state.selected=-1;
                        state.infoShown=false;
                        Redraw();
                        return;
                    }
                }
                if(!u->moved&&hex!=u->at)
                    for(const HexWar::Route& r:t.Reach(u->id))
                        if(r.to==hex)
                        {
                            const int id=u->id;
                            t.Move(id,hex);
                            // Nothing to attack from there: it is finished.
                            if(t.Now()==Phase::YourTurn&&t.Targets(id).empty()&&t.BridgeSpots(id).empty()){t.Finish(id);state.selected=-1;}
                            Redraw();
                            return;
                        }
                if(hex==u->at)
                {
                    // Clicked again: it stays (and is finished once it has moved).
                    LetGo();
                    Redraw();
                    return;
                }
            }
            if(there&&there->id!=state.selected){LetGo();Choose(there->id);return;}
            LetGo();
            if(!there)OpenBuild(hex);
            Redraw();
        }

        // What to build at a factory of yours (on hex), when it is not building.
        void OpenBuild(int hex)
        {
            const auto& t=state.table;
            const int f=t.FactoryAt(hex);
            if(f<0||!YourTurnNow())return;
            const auto& fa=t.Factories()[static_cast<size_t>(f)];
            if(fa.owner!=You)return;
            if(fa.building>=0){Tell(Fmt(T(L"生産中：{0} あと{1}ターン",L"Building: {0}, {1} turns left"),{KindName(static_cast<Kind>(fa.building)),std::to_wstring(fa.left)}));return;}
            state.orderMenu=state.aiming=false;
            state.buildMenu=f;
            state.buildKinds=t.Roster();
            state.buildRows.assign(state.buildKinds.size(),RectF());
        }

        void ChooseBuild(size_t i)
        {
            auto& t=state.table;
            if(state.buildMenu<0||i>=state.buildKinds.size())return;
            const int f=state.buildMenu;
            if(!t.CanBuild(You,f,state.buildKinds[i]))
            {
                Tell(T(L"これ以上は生産できません",L"No more can be built"));
                return;
            }
            t.Build(You,f,state.buildKinds[i]);
            state.buildMenu=-1;
            PlayMahjongSound(MahjongSound::Appear);
            Redraw();
        }

        // An order chosen from the menu: given at once, or for a strike, its target asked for.
        void ChooseOrder(int i)
        {
            const auto s=static_cast<HexWar::Skill>(i);
            if(!YourTurnNow()||!state.table.CanCommand(You,s))return;
            state.orderMenu=false;
            if(s==HexWar::Skill::Strike)
            {
                state.aiming=true;
                Tell(T(L"砲撃する敵の部隊をクリック",L"Click the enemy unit to shell"));
                return;
            }
            state.table.Command(You,s);
            Redraw();
        }

        // Esc or a right click: a move taken back, else the unit let go.
        bool Cancel()
        {
            if(state.orderMenu||state.aiming||state.buildMenu>=0){state.orderMenu=state.aiming=false;state.buildMenu=-1;Redraw();return true;}
            const HexWar::Unit* u=Acting();
            if(u&&u->moved&&state.table.TakeBack(u->id)){Redraw();return true;}
            if(state.selected>=0||state.infoShown){state.selected=-1;state.infoShown=false;Redraw();return true;}
            return false;
        }

        void EndTurn()
        {
            if(!YourTurnNow())return;
            LetGo();
            state.orderMenu=state.aiming=false;
            state.buildMenu=-1;
            state.table.EndTurn();
            state.nextAt=GetTickCount64()+HerTurnWait;
            Redraw();
        }

        void Continue()
        {
            auto& t=state.table;
            if(t.Now()==Phase::GameOver)
            {
                state.lost=t.Winner()==Her?state.lost+1:0;
                NewGame();
            }
            Redraw();
        }

        // The map chosen played (when it is open).
        void StartMap(int m)
        {
            if(!Open(m))
            {
                Tell(Fmt(T(L"前のマップで1回勝つか、{0}回遊ぶと開きます",L"Win the map before once, or play it {0} times, to open this"),{std::to_wstring(PlaysToOpen)}));
                return;
            }
            state.map=m;
            state.choosing=false;
            NewGame();
        }

        // How long a battle's scene lasts.
        double FightLength(const HexWar::Battle& b)
        {
            return ShotStart(1)+ShotGap*std::max(b.shots[0].size(),b.shots[1].size())+FightHold;
        }

        // What was done since the last look: the unit set gliding along its path, its battle
        // shown after; each shown to its end before the next.
        void Watch()
        {
            auto& t=state.table;
            const double now=Seconds();
            if(state.order.on&&state.order.a.skill==static_cast<int>(HexWar::Skill::Strike))
            {
                // A strike's shells bursting (as drawn: seven, 0.11 s apart from 0.3 s), placed by
                // where the hex is across the map.
                const double at=now-state.order.start,was=state.order.heard;
                state.order.heard=at;
                const float pan=0.8f*(2.0f*(state.order.a.strikeHex%t.Columns())/std::max(1,t.Columns()-1)-1);
                for(int i=0;i<7;++i)
                    if(const double burst=0.3+0.11*i;burst>was&&burst<=at)PlayHexWarSound(HexWarSound::Blast,pan,0.75f);
            }
            if(state.order.on&&now-state.order.start>=OrderShown)state.order.on=false;
            if(state.glide.unit<0&&!state.fight.on&&!state.fightNext&&!state.order.on&&t.Actions()!=state.actionsSeen)
            {
                state.actionsSeen=t.Actions();
                const HexWar::Action& a=t.LastAction();
                if(a.skill>=0)
                {
                    // An order: its name across the map, her word on it.
                    const auto s=static_cast<HexWar::Skill>(a.skill);
                    state.order={true,a,now};
                    state.banner=(a.who==You?std::wstring(T(L"作戦：",L"Order: ")):Fmt(T(L"{0}の作戦：",L"{0}'s order: "),{HerName()}))+SkillName(s);
                    state.bannerSub=SkillAbout(s);
                    state.bannerYours=a.who==You;
                    state.bannerAt=now;
                    PlayMahjongSound(MahjongSound::Appear);
                    if(a.who==Her)Say(L"作戦",3);
                    else Say(L"あなたの作戦",2,0.5);
                    return;
                }
                if(a.path.size()>1)
                {
                    state.glide={a.unit,a.path,now};
                    if(const HexWar::Unit* u=t.UnitById(a.unit))
                    {
                        const float x=static_cast<float>(a.path.back()%t.Columns())/std::max(1,t.Columns()-1);
                        PlayMoving(u->kind,1.2f*x-0.6f,1,HexGlide*(a.path.size()-1)+0.2,u->count);
                    }
                }
                if(a.fought){state.fightNext=true;state.pending=a.battle;}
                if(a.bridged>=0)
                {
                    Tell(a.who==You?std::wstring(T(L"橋を架けた",L"Bridge built")):Fmt(T(L"{0}が橋を架けた",L"{0} built a bridge"),{HerName()}));
                    PlayMahjongSound(MahjongSound::PlaceHard);
                }
                if(a.captured>=0)
                {
                    Tell(a.who==You?std::wstring(T(L"工場を占領した",L"Factory taken")):Fmt(T(L"{0}が工場を占領した",L"{0} took a factory"),{HerName()}));
                    PlayMahjongSound(MahjongSound::Appear);
                }
            }
            if(state.glide.unit>=0&&now>=state.glide.start+HexGlide*(state.glide.path.size()-1))
            {
                state.glide.unit=-1;
                PlayMahjongSound(MahjongSound::Place);
            }
            if(state.glide.unit<0&&state.fightNext)
            {
                state.fightNext=false;
                state.fight={true,state.pending,now};
                const int left=state.pending.who[0]==You?0:1;
                for(int side=0;side<2;++side)
                    PlayMoving(state.pending.kinds[side],side==left?-0.6f:0.6f,0.8f,LastIn+MoveIn+0.1,state.pending.before[side]);
            }
            if(state.fight.on)
            {
                // At each shot its gun (a tank's, or a rifle; supporting fire from further off), on
                // the shooter's side of the screen; where it lands on a member, a blast (a tank's loud).
                const auto& b=state.fight.b;
                const double at=now-state.fight.start,was=state.fight.heard;
                state.fight.heard=at;
                const int left=b.who[0]==You?0:1;   // as PaintFight puts them
                for(int side=0;side<2;++side)
                {
                    const double from=ShotStart(side);
                    const float pan=side==left?-0.6f:0.6f;
                    for(size_t i=0;i<b.shots[side].size();++i)
                    {
                        const double shot=from+ShotGap*i;
                        const bool supporting=side==0&&static_cast<int>(i)>=b.own[0];
                        if(shot>was&&shot<=at)
                            PlayHexWarSound(supporting||b.kinds[side]!=Kind::Infantry?HexWarSound::Cannon:HexWarSound::Rifle,supporting?pan*1.5f:pan,
                                            supporting?0.6f:b.kinds[side]==Kind::Bazooka?0.7f:1.0f);
                        if(const double lands=shot+FightFlight;b.shots[side][i]&&lands>was&&lands<=at)
                            PlayHexWarSound(HexWarSound::Blast,-pan,Vehicle(b.kinds[side^1])?0.85f:0.45f);
                    }
                }
                if(at>=FightLength(b))state.fight.on=false;
            }
        }

        // A battle being shown skipped to its end.
        void SkipFight()
        {
            state.glide.unit=-1;
            if(state.fightNext){state.fightNext=false;state.fight={true,state.pending,Seconds()};}
            state.fight.on=false;
            Redraw();
        }

        // ---- Her words and the watchers' (as at mahjong) ----

        constexpr ULONGLONG WordsHold=3500;
        constexpr ULONGLONG AskEvery=12000;
        constexpr ULONGLONG AnswerWait=9000;

        // Her words: shown in the bubble (none for a voice alone) and, with a voice file of
        // their own, played (the words revealed over it).
        void SayLine(const GameLine& line,int weight)
        {
            auto& w=state.words;
            const ULONGLONG now=GetTickCount64();
            state.voice.Stop();
            std::wstring error;
            w.voiced=!line.voice.empty()&&state.voice.Play(line,error);
            // In her voice (the voice engine) when she speaks aloud and the line has no sound of its own.
            if(state.her.speak)state.her.speak(w.voiced?std::wstring():line.text);
            if(!error.empty())w.told+=L"  voice: "+error+L"\n";
            w.text=line.text;
            w.waiting=false;
            w.weight=weight;
            w.since=now;
            w.told+=L"  she says: "+line.text+(line.voice.empty()?L"":L" ["+line.voice+L"]")+L"\n";
            const ULONGLONG reading=static_cast<ULONGLONG>(CharSeconds()*1000*line.text.size())+2600+90*static_cast<ULONGLONG>(line.text.size());
            w.until=now+(w.voiced?std::max(reading,static_cast<ULONGLONG>(state.voice.Seconds()*1000)+1800):reading);
            if(!line.text.empty()){w.recent.push_back(line.text);if(w.recent.size()>4)w.recent.erase(w.recent.begin());}
            Redraw();
        }

        void SayWords(const std::wstring& line,int weight){SayLine({line,{}},weight);}

        const std::wstring& Pick(const std::vector<std::wstring>& lines)
        {
            std::uniform_int_distribution<size_t> any(0,lines.size()-1);
            return lines[any(state.sparkRandom)];
        }

        // A line of the scene, said now and then (chance: how often, unless the user's lines
        // file says otherwise).
        void Say(const wchar_t* name,int weight,double chance)
        {
            if(!state.lines.Roll(name,chance,state.sparkRandom))return;
            GameLine line;
            if(state.lines.Pick(name,state.words.recent,state.sparkRandom,line))SayLine(line,weight);
        }

        // What she sees now, while the game goes on: 1 when she is far ahead in strength, -1
        // when she is far behind, else 0.
        int Certain()
        {
            const auto& t=state.table;
            if(t.Now()==Phase::GameOver||t.Turn()<2)return 0;
            const double share=t.HerShare();
            return share>=0.75?1:share<=0.3?-1:0;
        }

        void SetTell(Expression e)
        {
            if(e==state.tell)return;
            state.tell=e;
            if(state.her.model&&state.renderer)state.her.model->SetExpression(e,false);
        }

        // Her face: smiling when she is far ahead, glum when she is far behind.
        void UpdateTell()
        {
            const int certain=Certain();
            SetTell(certain>0?Expression::Smile:certain<0?Expression::Sad:Expression::Neutral);
        }

        // Something happened: she speaks of it, and those watching hear of it. facts: for her (she
        // is "you"); feel: how she is to feel about it; seen: the same for one watching; scene:
        // the scene of her lines (null: she says nothing); theirs: the watchers'.
        void Happened(int weight,const std::wstring& facts,const std::wstring& feel,const wchar_t* scene,
                      const std::wstring& seen,const std::vector<std::wstring>& theirs)
        {
            auto& t=state.table;
            state.words.told+=Fmt(L"  {0} s, weight {1}: {2}\n",{std::to_wstring(static_cast<int>(state.clock)),std::to_wstring(weight),seen});
            if(state.her.onEvent&&!theirs.empty())
                state.her.onEvent(seen+Fmt(T(L"（{0}ターン目）",L" (turn {0})"),{std::to_wstring(t.Turn())}),weight,Pick(theirs));
            if(!scene)return;
            auto& w=state.words;
            const ULONGLONG now=GetTickCount64();
            if((w.waiting||now-w.since<WordsHold)&&weight<w.weight)return;
            if(!state.lines.Roll(scene,1,state.sparkRandom))return;
            GameLine line;
            if(state.lines.Pick(scene,w.recent,state.sparkRandom,line))SayLine(line,weight);
        }

        // The next scene not yet in her own words, asked of the LLM.
        void Prefetch()
        {
            if(state.prefetching||!state.her.prefetch)return;
            const std::wstring ask=state.lines.NextAsk();
            if(!ask.empty())state.prefetching=state.her.prefetch(ask);
        }

        // How the game ended, in words (for the end screen and her afterword).
        std::wstring EndingText()
        {
            const auto& t=state.table;
            const std::wstring turns=std::to_wstring(std::min(t.Turn(),t.TurnLimit()));
            switch(t.HowEnded())
            {
            case HexWar::Ending::Destroyed:return Fmt(T(L"全滅（{0}ターン）",L"Wiped out (turn {0})"),{turns});
            case HexWar::Ending::Captured:return Fmt(T(L"司令部を占領（{0}ターン）",L"Base taken (turn {0})"),{turns});
            case HexWar::Ending::Resigned:return Fmt(T(L"降参（{0}ターン）",L"Gave up (turn {0})"),{turns});
            case HexWar::Ending::TurnLimit:return Fmt(T(L"{0}ターンが過ぎ、残った戦力で判定",L"{0} turns over: judged by the strength left"),{turns});
            default:return L"";
            }
        }

        // What is new in the game since the last tick, for words: its start, a unit destroyed
        // either way, your infantry near her base, the end.
        void NoticeWords()
        {
            auto& t=state.table;
            auto& seen=state.seen;
            auto& w=state.words;
            const ULONGLONG now=GetTickCount64();
            if(w.waiting&&now-w.since>AnswerWait)SayWords(w.fallback,w.weight);
            if(!w.text.empty()&&!w.waiting&&now>w.until){w.text.clear();Redraw();}
            if(w.text!=w.revealing)
            {
                if(w.revealing.empty()||w.revealing==L"…"||w.text.rfind(w.revealing,0)!=0)w.shown=0;
                w.revealing=w.text;
            }
            const size_t before=static_cast<size_t>(w.shown);
            w.shown=std::min(static_cast<double>(w.text.size()),w.voiced?state.voice.Revealed():w.shown+(now-std::min(now,w.shownAt))/1000.0/CharSeconds());
            w.shownAt=now;
            if(static_cast<size_t>(w.shown)!=before)Redraw();
            if(Showing()||state.choosing)return;   // a move or a battle is still being shown, or no game yet
            const std::wstring name=HerName();
            if(!seen.started)
            {
                seen.started=true;
                Happened(6,T(L"これから話し相手とヘックス戦線（ユニットを動かして戦う戦術ゲーム）の対戦が始まる。",L"A game of Hex Front (a war game of moving units) with them is about to begin. "),
                         T(L"わくわくする気持ち",L"how you look forward to it"),
                         L"始まり",
                         Fmt(T(L"{0}と、このパソコンの持ち主の戦術ゲームの対戦が始まる。",L"A war game between {0} and the owner of this computer is about to begin."),{name}),
                         {T(L"がんばれー！",L"Good luck, you two!"),T(L"どっちが勝つかな？",L"Who's going to win?")});
            }
            const bool over=t.Now()==Phase::GameOver;
            if(t.Actions()!=seen.actions)
            {
                seen.actions=t.Actions();
                const HexWar::Action& a=t.LastAction();
                if(!over&&a.fought&&a.battle.after[1]==0)
                {
                    if(a.who==You)
                        Happened(4,T(L"戦術ゲームで、話し相手にあなたのユニットを1つ全滅させられた。",L"At the war game, they have wiped out one of your units."),
                                 T(L"あせる気持ち",L"how alarmed you are"),
                                 L"危ない",
                                 Fmt(T(L"戦術ゲームで、持ち主が{0}のユニットを1つ倒した。",L"At the war game, the owner wiped out one of {0}'s units."),{name}),
                                 {T(L"おっ、撃破だ！",L"Ooh, got one!"),T(L"いい攻撃だね！",L"Nice attack!")});
                    else
                    {
                        Happened(3,L"",L"",nullptr,
                                 Fmt(T(L"戦術ゲームで、{0}が持ち主のユニットを1つ倒した。",L"At the war game, {0} wiped out one of the owner's units."),{name}),
                                 {Fmt(T(L"{0}、攻めてるね！",L"{0} is on the attack!"),{name})});
                        if(!w.waiting&&GetTickCount64()-w.since>WordsHold)Say(L"いい手",2,0.7);
                    }
                }
            }
            if(!over&&t.Now()==Phase::HerTurn&&seen.turn!=t.Turn())
            {
                seen.turn=t.Turn();
                // Your infantry close to her base, or now and then a word as she starts her turn.
                bool close=false;
                for(const auto& u:t.Units())
                    if(u.who==You&&HexWar::Info(u.kind).captures&&t.Distance(u.at,t.BaseOf(Her))<=HexWar::Info(u.kind).move)close=true;
                if(close)Say(L"危ない",4,0.8);
                else if(t.Turn()>=2&&!w.waiting&&GetTickCount64()-w.since>WordsHold&&state.lines.Roll(L"考える",0.2,state.sparkRandom))
                {
                    Say(L"考える",2);
                    state.thinkFor=1.6;
                }
            }
            // Far ahead or far behind: said once a game, and her face holds it (UpdateTell).
            const int certain=Certain();
            if(certain>0&&!seen.sureWin){seen.sureWin=true;Say(L"勝ち確定");}
            if(certain<0&&!seen.behind){seen.behind=true;Say(L"苦しい");}
            if(over&&!seen.over)
            {
                const int winner=t.Winner();
                const bool sheResigned=t.HowEnded()==HexWar::Ending::Resigned&&winner==You;
                const std::wstring how=EndingText();
                state.afterword=Fmt(T(L"いま、話し相手とヘックス戦線（ユニットを動かして戦う戦術ゲーム）を最後までして、デスクトップに戻ってきたところ。結果は{0}（{1}）。その対戦の感想を、話し相手にひと言（1〜2文）",
                                      L"You have just played Hex Front (a war game of moving units) to its end with them and come back to the desktop. {0} ({1}). Tell them what you thought of the game (one or two sentences)"),
                                    {winner==Her?T(L"あなたの勝ち",L"You won"):winner<0?T(L"引き分け",L"A draw"):T(L"あなたの負け",L"You lost"),how});
                state.afterwordLine=winner==Her?T(L"楽しかったね！また対戦しよう。",L"That was fun! Let's play again sometime."):T(L"くやしいなあ…次は勝つからね！",L"So close... I'll win next time!");
                if(winner==Her)
                    Happened(9,Fmt(T(L"戦術ゲームの対戦が終わって、あなたが勝った（{0}）。",L"The war game is over: you won ({0})."),{how}),
                             T(L"勝ってうれしい気持ち",L"how glad you are to have won"),
                             L"勝った",
                             Fmt(T(L"{0}と持ち主の戦術ゲームの対戦が終わり、{0}が勝った。",L"The war game between {0} and the owner is over: {0} won."),{name}),
                             {T(L"おつかれさま！いい勝負だったね。",L"Well played, both of you!"),Fmt(T(L"{0}の勝ちだ！",L"{0} wins!"),{name})});
                else Happened(9,winner<0?Fmt(T(L"戦術ゲームの対戦が終わって、引き分け（{0}）。",L"The war game is over: a draw ({0})."),{how}):
                                sheResigned?Fmt(T(L"戦術ゲームの対戦で、勝ち目がないのであなたが降参した。あなたの負け（{0}）。",L"At the war game, you saw no way to win and gave up: you lost ({0})."),{how}):
                                            Fmt(T(L"戦術ゲームの対戦が終わって、あなたの負け（{0}）。",L"The war game is over: you lost ({0})."),{how}),
                              winner<0?T(L"引き分けの気持ち",L"how you feel about a draw"):T(L"負けて悔しい気持ちか、話し相手をほめる気持ち",L"how sorry you are to have lost, or praise them"),
                              winner<0?L"引き分け":sheResigned?L"投了":L"負けた",
                              Fmt(T(L"{0}と持ち主の戦術ゲームの対戦が終わった。",L"The war game between {0} and the owner is over."),{name}),
                              {T(L"おつかれさま！いい勝負だったね。",L"Well played, both of you!"),T(L"おめでとう！",L"Congratulations!")});
            }
            seen.over=over;
        }

        void Tick()
        {
            auto& t=state.table;
            const ULONGLONG now=GetTickCount64();
            if(!state.said.empty()&&now-state.saidAt>SaidShown){state.said.clear();Redraw();}
            Watch();
            // A new turn, once what came before it has been shown: whose it is, across the map.
            if(t.Now()!=Phase::GameOver&&!Showing()&&!state.choosing)
            {
                const int key=t.Turn()*2+(t.Now()==Phase::HerTurn?1:0);
                if(key!=state.bannerFor)
                {
                    state.bannerFor=key;
                    state.banner=t.Now()==Phase::YourTurn?std::wstring(T(L"あなたのターン",L"Your turn")):Fmt(T(L"{0}のターン",L"{0}'s turn"),{HerName()});
                    state.bannerSub=Fmt(T(L"ターン {0} / {1}",L"Turn {0} / {1}"),{std::to_wstring(t.Turn()),std::to_wstring(t.TurnLimit())});
                    state.bannerYours=t.Now()==Phase::YourTurn;
                    state.bannerAt=Seconds();
                    if(t.Now()==Phase::YourTurn)PlayMahjongSound(MahjongSound::Appear);
                }
            }
            // Your turn ended for you a moment after nothing is left to do (no unit can move or
            // attack any more; an order not given does not hold it).
            if(YourTurnNow()&&!state.choosing&&!state.orderMenu&&!state.aiming&&state.buildMenu<0&&!state.confirmResign&&NothingLeft())
            {
                if(!state.idleSince)state.idleSince=now;
                else if(now-state.idleSince>=AutoEndWait){state.idleSince=0;EndTurn();}
            }
            else state.idleSince=0;
            // Her turn: one unit at a time, each shown before the next acts.
            if(t.Now()==Phase::HerTurn&&!Showing()&&now>=state.nextAt&&(!state.renderer||state.entrance==5))
            {
                t.Step();
                if(t.Now()==Phase::GameOver)state.nextAt=now+(state.renderer?EndWait3D:EndWait);
                else state.nextAt=now+HerStepWait;
            }
            UpdateTell();
            Redraw();
            if(!state.renderer)state.voice.Tick(0.04);   // (at the 3D table the model's frame ticks it)
            NoticeWords();
            if(t.Now()==Phase::GameOver&&!state.gameOverTold)
            {
                state.gameOverTold=true;
                // Counted for the maps that open.
                const size_t m=static_cast<size_t>(t.MapIndex());
                ++state.played[m];
                if(t.Winner()==You)++state.won[m];
                SaveProgress();
                // A game given up by you counts as played.
                if(state.her.onGameOver)state.her.onGameOver();
            }
        }

        // ---- Drawing ----

        struct Run {std::wstring text;bool brush=true;};
        void Runs(Graphics& g,const std::vector<Run>& runs,const RectF& box,float size,Color color)
        {
            float total=0;
            for(const Run& r:runs)total+=TextWidth(g,r.text,size,false,r.brush);
            float x=box.X+(box.Width-total)/2;
            for(const Run& r:runs)
            {
                Text(g,r.text,RectF(x,box.Y,box.Width,box.Height),size,color,false,StringAlignmentNear,StringAlignmentCenter,r.brush);
                x+=TextWidth(g,r.text,size,false,r.brush);
            }
        }

        // Text wrapped within box (from its top).
        void Wrapped(Graphics& g,const std::wstring& text,const RectF& box,float size,Color color)
        {
            Font font(Family(),size,FontStyleRegular,UnitPixel);
            SolidBrush paint(color);
            StringFormat format;
            g.DrawString(text.c_str(),static_cast<INT>(text.size()),&font,box,&format,&paint);
        }

        // ---- The map ----

        constexpr float Root3=1.7320508f;

        // Where things go: her column at the right (her upper body in 3D, or her face), the map
        // over the rest above a panel of what is pointed at. A map too large for its area at
        // LeastRadius scrolls.
        struct Layout {float her,area,panel;RectF map;float r,ox,oy,mw,mh;bool fitsX,fitsY;};
        Layout Place(float W,float H,float s)
        {
            Layout l;
            const auto& t=state.table;
            l.her=std::round(W*HerColumn);
            l.area=W-l.her;
            l.panel=std::round(124*s);
            l.map=RectF(0,0,l.area,H-l.panel);
            const float C=static_cast<float>(t.Columns()),R=static_cast<float>(t.Rows()),pad=10*s;
            const float fit=std::min((l.map.Width-2*pad)/(1.5f*(C-1)+2),(l.map.Height-2*pad)/(Root3*(R+0.5f)));
            l.r=std::max(fit,LeastRadius*s);
            l.mw=l.r*(1.5f*(C-1)+2);
            l.mh=Root3*l.r*(R+0.5f);
            l.fitsX=l.mw+2*pad<=l.map.Width+0.5f;
            l.fitsY=l.mh+2*pad<=l.map.Height+0.5f;
            state.scrollX=l.fitsX?0:std::clamp(state.scrollX,0.0f,l.mw+2*pad-l.map.Width);
            state.scrollY=l.fitsY?0:std::clamp(state.scrollY,0.0f,l.mh+2*pad-l.map.Height);
            l.ox=std::round(l.fitsX?l.map.X+(l.map.Width-l.mw)/2:l.map.X+pad-state.scrollX);
            l.oy=std::round(l.fitsY?l.map.Y+(l.map.Height-l.mh)/2:l.map.Y+pad-state.scrollY);
            return l;
        }

        // A hex's middle, from the map's top left corner.
        PointF HexMiddle(int hex,float r)
        {
            const int c=hex%state.table.Columns(),row=hex/state.table.Columns();
            return PointF(r+1.5f*r*c,Root3*r*(row+0.5f+0.5f*(c&1)));
        }
        PointF HexAt(const Layout& l,int hex){const PointF p=HexMiddle(hex,l.r);return PointF(l.ox+p.X,l.oy+p.Y);}

        void HexPath(GraphicsPath& path,PointF c,float r)
        {
            PointF p[6];
            for(int i=0;i<6;++i)p[i]=PointF(c.X+r*std::cos(i*DirectX::XM_PI/3),c.Y+r*std::sin(i*DirectX::XM_PI/3));
            path.AddPolygon(p,6);
        }
        void FillHex(Graphics& g,PointF c,float r,Color fill)
        {
            GraphicsPath path;
            HexPath(path,c,r);
            SolidBrush brush(fill);
            g.FillPath(&brush,&path);
        }
        void EdgeHex(Graphics& g,PointF c,float r,Color edge,float width)
        {
            GraphicsPath path;
            HexPath(path,c,r);
            Pen pen(edge,width);
            pen.SetLineJoin(LineJoinRound);
            g.DrawPath(&pen,&path);
        }

        Color SideColor(int who,int shade=0)
        {
            // 0: the body, 1: dark, 2: light.
            static const Color you[3]={Color(255,64,128,232),Color(255,28,58,120),Color(255,150,196,255)};
            static const Color her[3]={Color(255,226,70,58),Color(255,112,30,26),Color(255,255,160,140)};
            return (who==You?you:her)[shade];
        }

        // One hex's ground, at c (radius r): plain grass, the mountains' peaks, a base's
        // building and its side's flag, and the rest for maps to come.
        void PaintGround(Graphics& g,int hex,PointF c,float r)
        {
            const auto& t=state.table;
            std::mt19937 grain(static_cast<unsigned>(hex*2654435761u));
            std::uniform_real_distribution<float> any(-1,1);
            const Terrain kind=t.At(hex);
            const Color plain(255,146,170,96);
            switch(kind)
            {
            case Terrain::Mountain:FillHex(g,c,r,Color(255,118,108,94));break;
            case Terrain::Rough:FillHex(g,c,r,Color(255,160,136,96));break;
            case Terrain::Forest:FillHex(g,c,r,Color(255,74,122,62));break;
            case Terrain::River:FillHex(g,c,r,Color(255,72,124,196));break;
            case Terrain::City:FillHex(g,c,r,Color(255,128,126,122));break;
            case Terrain::Foundation:FillHex(g,c,r,Color(255,100,98,96));break;
            default:FillHex(g,c,r,plain);break;
            }
            if(kind==Terrain::Plain||kind==Terrain::Base||kind==Terrain::Road)
            {
                Pen blade(Color(110,92,122,58),std::max(1.0f,r*0.04f));
                for(int i=0;i<5;++i)
                {
                    const float x=c.X+any(grain)*0.55f*r,y=c.Y+any(grain)*0.5f*r;
                    g.DrawLine(&blade,x,y,x+0.05f*r,y-0.13f*r);
                }
            }
            if(kind==Terrain::Rough)
            {
                SolidBrush stone(Color(160,110,90,64));
                for(int i=0;i<7;++i)g.FillEllipse(&stone,c.X+any(grain)*0.55f*r,c.Y+any(grain)*0.5f*r,0.12f*r,0.08f*r);
            }
            if(kind==Terrain::Forest)
            {
                SolidBrush tree(Color(255,44,88,40)),lit(Color(255,92,146,70));
                for(const auto& [dx,dy]:{std::pair{-0.3f,0.05f},std::pair{0.25f,-0.2f},std::pair{0.2f,0.3f},std::pair{-0.05f,-0.35f}})
                {
                    g.FillEllipse(&tree,c.X+dx*r-0.22f*r,c.Y+dy*r-0.22f*r,0.44f*r,0.44f*r);
                    g.FillEllipse(&lit,c.X+dx*r-0.15f*r,c.Y+dy*r-0.2f*r,0.22f*r,0.2f*r);
                }
            }
            if(kind==Terrain::Road)FillHex(g,c,r*0.45f,Color(255,150,146,138));
            if(kind==Terrain::City||kind==Terrain::Foundation)
            {
                // Broken blocks: walls lit from the left.
                SolidBrush wall(Color(255,170,166,158)),shade(Color(255,88,86,84));
                for(const auto& [dx,dy,w,h]:{std::tuple{-0.4f,-0.3f,0.34f,0.4f},std::tuple{0.08f,-0.42f,0.3f,0.52f},std::tuple{-0.18f,0.12f,0.4f,0.3f}})
                {
                    g.FillRectangle(&wall,c.X+dx*r,c.Y+dy*r,w*r,h*r);
                    g.FillRectangle(&shade,c.X+(dx+w*0.6f)*r,c.Y+dy*r,w*0.4f*r,h*r);
                }
            }
            if(kind==Terrain::Mountain)
            {
                // Two peaks, lit from the left, their tops in snow.
                auto peak=[&](float px,float base,float w,float h){
                    const PointF top(c.X+px*r,c.Y+(base-h)*r),left(c.X+(px-w)*r,c.Y+base*r),right(c.X+(px+w)*r,c.Y+base*r),foot(c.X+(px+0.1f*w)*r,c.Y+base*r);
                    const PointF lit[3]={left,top,foot},shade[3]={foot,top,right};
                    SolidBrush light(Color(255,168,158,142)),dark(Color(255,96,88,78));
                    g.FillPolygon(&light,lit,3);
                    g.FillPolygon(&dark,shade,3);
                    const float k=0.3f;
                    const PointF snow[4]={top,PointF(top.X+(right.X-top.X)*k,top.Y+(right.Y-top.Y)*k),PointF(top.X+(foot.X-top.X)*k*0.8f,top.Y+(foot.Y-top.Y)*k*0.9f),
                                          PointF(top.X+(left.X-top.X)*k,top.Y+(left.Y-top.Y)*k)};
                    SolidBrush white(Color(255,236,236,240));
                    g.FillPolygon(&white,snow,4);
                };
                peak(-0.18f,0.5f,0.5f,1.0f);
                peak(0.32f,0.55f,0.38f,0.7f);
            }
            if(kind==Terrain::Base)
            {
                const int who=hex==t.BaseOf(You)?You:Her;
                SolidBrush wall(Color(255,206,204,196)),roof(Color(255,110,108,104)),window(Color(255,70,90,120));
                g.FillRectangle(&wall,c.X-0.5f*r,c.Y-0.1f*r,r,0.5f*r);
                g.FillRectangle(&roof,c.X-0.56f*r,c.Y-0.22f*r,1.12f*r,0.14f*r);
                for(int i=0;i<3;++i)g.FillRectangle(&window,c.X-0.38f*r+i*0.3f*r,c.Y+0.02f*r,0.16f*r,0.14f*r);
                Pen pole(Color(255,60,60,60),std::max(1.0f,0.05f*r));
                g.DrawLine(&pole,c.X+0.3f*r,c.Y-0.22f*r,c.X+0.3f*r,c.Y-0.78f*r);
                SolidBrush flag(SideColor(who));
                const PointF f[3]={PointF(c.X+0.3f*r,c.Y-0.78f*r),PointF(c.X+(who==You?0.72f:-0.12f)*r,c.Y-0.66f*r),PointF(c.X+0.3f*r,c.Y-0.54f*r)};
                g.FillPolygon(&flag,f,3);
            }
            EdgeHex(g,c,r,Color(110,40,48,30),std::max(1.0f,r*0.035f));
        }

        // A picture of the game's (res/hexwar, type HEXWARART), loaded once; null when it
        // cannot be.
        Bitmap* Art(const wchar_t* name)
        {
            static std::map<std::wstring,std::unique_ptr<Bitmap>> loaded;
            if(!name)return nullptr;
            auto found=loaded.find(name);
            if(found==loaded.end())
            {
                std::unique_ptr<Bitmap> picture;
                const HRSRC info=FindResourceW(nullptr,name,L"HEXWARART");
                const HGLOBAL data=info?LoadResource(nullptr,info):nullptr;
                if(IStream* stream=data?SHCreateMemStream(static_cast<const BYTE*>(LockResource(data)),SizeofResource(nullptr,info)):nullptr)
                {
                    std::unique_ptr<Bitmap> read(Bitmap::FromStream(stream));
                    stream->Release();
                    if(read&&read->GetLastStatus()==Ok)picture.reset(read->Clone(0,0,read->GetWidth(),read->GetHeight(),PixelFormat32bppPARGB));
                }
                found=loaded.emplace(name,std::move(picture)).first;
            }
            return found->second.get();
        }

        // A picture made w x h once, smoothly, and kept: drawn so again each frame it costs a
        // plain copy, not a high-quality shrink of the whole large picture.
        Bitmap* Scaled(Bitmap* p,int w,int h)
        {
            static std::map<std::tuple<Bitmap*,int,int>,std::unique_ptr<Bitmap>> made;
            if(!p||w<1||h<1)return nullptr;
            const auto key=std::tuple{p,w,h};
            auto found=made.find(key);
            if(found==made.end())
            {
                auto shrunk=std::make_unique<Bitmap>(w,h,PixelFormat32bppPARGB);
                Graphics g(shrunk.get());
                g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                g.SetPixelOffsetMode(PixelOffsetModeHalf);
                g.DrawImage(p,Rect(0,0,w,h),0,0,p->GetWidth(),p->GetHeight(),UnitPixel);
                found=made.emplace(key,std::move(shrunk)).first;
            }
            return found->second.get();
        }

        // The battle's view of a terrain (res/hexwar/backdrop1-8: rocky mountains, a road through
        // rocks, grassland, forest, dry rough ground, ruins, snow, a river), and the colour of the
        // ground at its foot (the mean of its bottom rows).
        Bitmap* Backdrop(Terrain t)
        {
            switch(t)
            {
            case Terrain::Mountain:return Art(L"BACKDROP1");
            case Terrain::Road:case Terrain::Base:case Terrain::Factory:return Art(L"BACKDROP2");
            case Terrain::Forest:return Art(L"BACKDROP4");
            case Terrain::Rough:return Art(L"BACKDROP5");
            case Terrain::City:case Terrain::Foundation:return Art(L"BACKDROP6");
            case Terrain::Bridge:case Terrain::River:return Art(L"BACKDROP8");
            default:return Art(L"BACKDROP3");
            }
        }
        // The ground under a battle's units (seen from above at a slant, far side at the top):
        // rough ground, grass, earth, cracked asphalt, stone paving.
        Bitmap* Floor(Terrain t)
        {
            switch(t)
            {
            case Terrain::Mountain:case Terrain::Rough:return Art(L"FLOOR1");
            case Terrain::Forest:return Art(L"FLOOR3");
            case Terrain::Road:case Terrain::Base:case Terrain::Bridge:case Terrain::Factory:return Art(L"FLOOR4");
            case Terrain::City:case Terrain::Foundation:return Art(L"FLOOR5");
            default:return Art(L"FLOOR2");
            }
        }
        Color BackdropGround(Bitmap* view)
        {
            static std::map<Bitmap*,Color> known;
            if(auto found=known.find(view);found!=known.end())return found->second;
            unsigned long sum[3]{},n=0;
            const UINT w=view->GetWidth(),h=view->GetHeight();
            for(UINT y=h>8?h-8:0;y<h;++y)
                for(UINT x=0;x<w;x+=3)
                {
                    Color c;
                    view->GetPixel(static_cast<INT>(x),static_cast<INT>(y),&c);
                    sum[0]+=c.GetR();sum[1]+=c.GetG();sum[2]+=c.GetB();++n;
                }
            const Color mean(255,static_cast<BYTE>(sum[0]/std::max(1ul,n)),static_cast<BYTE>(sum[1]/std::max(1ul,n)),static_cast<BYTE>(sum[2]/std::max(1ul,n)));
            known[view]=mean;
            return mean;
        }

        // A base on the map at c: its building, and its side's colour round its foot.
        void PaintBase(Graphics& g,PointF c,float r,int who)
        {
            Bitmap* building=Art(L"BASE");
            if(!building)return;
            SolidBrush ring(Color(150,SideColor(who).GetR(),SideColor(who).GetG(),SideColor(who).GetB()));
            g.FillEllipse(&ring,c.X-0.7f*r,c.Y+0.05f*r,1.4f*r,0.6f*r);
            const float h=1.45f*r,w=h*building->GetWidth()/building->GetHeight();
            g.DrawImage(building,RectF(c.X-w/2,c.Y+0.45f*r-h,w,h));
        }

        // The map's ground, drawn once for a radius (it does not change in a game): its picture
        // with the hexes' lines over it, or each hex painted when it has none; the bases.
        std::unique_ptr<Bitmap> groundImage;
        float groundRadius=0;
        int groundMap=-1;
        Bitmap& Ground(const Layout& l)
        {
            if(!groundImage||groundRadius!=l.r||groundMap!=state.table.MapIndex())
            {
                const auto& t=state.table;
                const int w=static_cast<int>(std::ceil(l.mw))+2,h=static_cast<int>(std::ceil(l.mh))+2;
                groundImage=std::make_unique<Bitmap>(w,h,PixelFormat32bppPARGB);
                Graphics g(groundImage.get());
                g.SetSmoothingMode(SmoothingModeAntiAlias);
                g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                g.Clear(Color(0,0,0,0));
                if(Bitmap* picture=Art(HexWar::Map(t.MapIndex()).picture))
                {
                    g.DrawImage(picture,RectF(0,0,l.mw,l.mh));
                    for(int hex=0;hex<t.Hexes();++hex)EdgeHex(g,HexMiddle(hex,l.r),l.r,Color(60,255,255,240),std::max(1.0f,l.r*0.025f));
                    // Foundations (closed to vehicles) hatched, as the picture's buildings don't
                    // keep to the hexes.
                    HatchBrush hatch(HatchStyleWideUpwardDiagonal,Color(85,255,150,120),Color(0,0,0,0));
                    for(int hex=0;hex<t.Hexes();++hex)
                        if(t.At(hex)==Terrain::Foundation)
                        {
                            GraphicsPath path;
                            HexPath(path,HexMiddle(hex,l.r),l.r*0.94f);
                            SolidBrush dark(Color(110,0,0,0));
                            g.FillPath(&dark,&path);
                            g.FillPath(&hatch,&path);
                            EdgeHex(g,HexMiddle(hex,l.r),l.r*0.94f,Color(150,240,120,90),std::max(1.0f,l.r*0.035f));
                        }
                    for(int who:{You,Her})PaintBase(g,HexMiddle(t.BaseOf(who),l.r),l.r,who);
                }
                else for(int hex=0;hex<t.Hexes();++hex)PaintGround(g,hex,HexMiddle(hex,l.r),l.r);
                groundRadius=l.r;
                groundMap=t.MapIndex();
            }
            return *groundImage;
        }

        // A picture drawn with its middle at c, w across, turned to face left when facing < 0.
        // tilt: degrees it leans forward, about its front at the bottom.
        void DrawArt(Graphics& g,Bitmap* art,PointF c,float w,float facing,float tilt=0)
        {
            const float h=w*art->GetHeight()/art->GetWidth();
            const GraphicsState saved=g.Save();
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            g.TranslateTransform(c.X,c.Y);
            if(facing<0)g.ScaleTransform(-1,1);
            if(tilt!=0)
            {
                g.TranslateTransform(0.3f*w,0.4f*h);
                g.RotateTransform(tilt);
                g.TranslateTransform(-0.3f*w,-0.4f*h);
            }
            g.DrawImage(art,RectF(-w/2,-h/2,w,h));
            g.Restore(saved);
        }

        // A unit's picture at c, u across: a tank seen from the side, or soldiers (a squad of
        // three on the map, one on the battle screen); facing: +1 right, -1 left.
        void DrawSprite(Graphics& g,PointF c,float u,Kind kind,int who,float facing,bool squad,float tilt=0)
        {
            // The pictures when there are (both sides alike; the side shows round the unit).
            static const wchar_t* names[HexWar::Kinds]={L"TANK",L"INFANTRY",L"BAZOOKA",L"SCOUT",L"ARTILLERY",L"HEAVY",L"FIGHTER",L"ATTACKER",L"ANTIAIR"};
            static const float wide[HexWar::Kinds]={2.3f,0,0,1.9f,2.2f,2.6f,2.4f,2.6f,1.3f};   // a vehicle's width, in u
            if(Bitmap* art=Art(names[static_cast<int>(kind)]))
            {
                if(Vehicle(kind))DrawArt(g,art,c,wide[static_cast<int>(kind)]*u,facing,tilt);
                else if(squad)
                {
                    DrawArt(g,art,PointF(c.X-0.42f*u,c.Y-0.12f*u),1.35f*u,facing);
                    DrawArt(g,art,PointF(c.X+0.42f*u,c.Y+0.12f*u),1.35f*u,facing);
                }
                else DrawArt(g,art,c,1.9f*u,facing);
                return;
            }
            const Color body=SideColor(who),dark=SideColor(who,1),light=SideColor(who,2);
            const float f=facing;
            if(kind==Kind::Tank)
            {
                SolidBrush track(Color(255,46,46,50)),wheel(Color(255,120,120,124)),hull(body),top(light),gun(dark);
                FillRound(g,RectF(c.X-0.9f*u,c.Y+0.12f*u,1.8f*u,0.46f*u),0.2f*u,Color(255,46,46,50));
                for(int i=0;i<4;++i)g.FillEllipse(&wheel,c.X-0.74f*u+i*0.42f*u,c.Y+0.2f*u,0.3f*u,0.3f*u);
                // The hull, its front sloping.
                const float front=0.62f*u,back=0.78f*u;
                const PointF h[4]={PointF(c.X-0.86f*u,c.Y+0.16f*u),PointF(c.X+0.86f*u,c.Y+0.16f*u),PointF(c.X+(f>0?front:back),c.Y-0.18f*u),PointF(c.X-(f>0?back:front),c.Y-0.18f*u)};
                g.FillPolygon(&hull,h,4);
                Pen barrel(dark,0.12f*u);
                barrel.SetEndCap(LineCapFlat);
                g.DrawLine(&barrel,c.X+0.2f*f*u,c.Y-0.36f*u,c.X+1.05f*f*u,c.Y-0.40f*u);
                g.FillEllipse(&top,c.X-0.42f*u-0.05f*f*u,c.Y-0.56f*u,0.78f*u,0.42f*u);
                Pen line(dark,std::max(1.0f,0.04f*u));
                g.DrawEllipse(&line,c.X-0.42f*u-0.05f*f*u,c.Y-0.56f*u,0.78f*u,0.42f*u);
                return;
            }
            auto soldier=[&](float x,float y,float k){
                SolidBrush coat(body),boots(dark),skin(Color(255,240,204,170)),helmet(dark);
                Pen leg(dark,0.1f*k);
                g.DrawLine(&leg,x-0.06f*k,y+0.2f*k,x-0.12f*k,y+0.5f*k);
                g.DrawLine(&leg,x+0.06f*k,y+0.2f*k,x+0.12f*k,y+0.5f*k);
                FillRound(g,RectF(x-0.13f*k,y-0.18f*k,0.26f*k,0.42f*k),0.06f*k,body);
                g.FillEllipse(&skin,x-0.1f*k,y-0.38f*k,0.2f*k,0.2f*k);
                g.FillPie(&helmet,x-0.12f*k,y-0.42f*k,0.24f*k,0.22f*k,180,180);
                Pen rifle(Color(255,60,48,40),0.07f*k);
                g.DrawLine(&rifle,x-0.1f*f*k,y+0.02f*k,x+0.42f*f*k,y-0.1f*k);
            };
            if(squad)
            {
                soldier(c.X-0.5f*u,c.Y+0.05f*u,u);
                soldier(c.X+0.5f*u,c.Y+0.05f*u,u);
                soldier(c.X,c.Y-0.12f*u,u);
            }
            else soldier(c.X,c.Y,1.6f*u);
        }

        // A unit on the map at c: its picture, its members left, and stars for its level; one
        // done for this turn darker.
        void DrawUnit(Graphics& g,PointF c,float r,Kind kind,int who,int count,int exp,bool done)
        {
            // Its side's colour under it (an aircraft's a ring, with its shadow, as it is above).
            const bool flies=Flies(kind);
            {
                const Color side=SideColor(who);
                SolidBrush plate(Color(flies?60:170,side.GetR(),side.GetG(),side.GetB()));
                Pen rim(SideColor(who,2),std::max(1.0f,(flies?0.06f:0.04f)*r));
                g.FillEllipse(&plate,c.X-0.66f*r,c.Y+0.02f*r,1.32f*r,0.56f*r);
                g.DrawEllipse(&rim,c.X-0.66f*r,c.Y+0.02f*r,1.32f*r,0.56f*r);
                if(flies)
                {
                    SolidBrush shadow(Color(110,0,0,0));
                    g.FillEllipse(&shadow,c.X-0.42f*r,c.Y+0.2f*r,0.84f*r,0.2f*r);
                }
            }
            const float above=flies?-0.3f*r+0.04f*r*static_cast<float>(std::sin(Seconds()*2.2+c.X*0.05)):-0.05f*r;
            DrawSprite(g,PointF(c.X,c.Y+above),(flies?0.5f:0.56f)*r,kind,who,static_cast<float>(state.table.Facing(who)),true);
            if(done)FillHex(g,c,r*0.92f,Color(110,20,20,24));
            const RectF tag(c.X+0.18f*r,c.Y+0.32f*r,0.5f*r,0.4f*r);
            FillRound(g,tag,0.08f*r,Color(230,16,16,24),SideColor(who,2),std::max(1.0f,0.03f*r));
            Text(g,std::to_wstring(count),tag,0.32f*r,Color(255,255,255,255),true,StringAlignmentCenter,StringAlignmentCenter);
            const int level=HexWar::Level(exp);
            for(int i=0;i<level;++i)Text(g,L"★",RectF(c.X-0.62f*r+i*0.22f*r,c.Y-0.72f*r,0.3f*r,0.3f*r),0.24f*r,Color(255,255,214,80),false);
        }

        // ---- The battle scene ----
        // The two units side by side, yours at the left facing right: each member a picture in
        // a formation; both sides' members shoot at once, one after another down the line.

        // The light layer (a quarter the size, premultiplied) added onto what g draws on, stretched
        // smoothly over rows top to bottom, within box (the layer's part drawn on).
        void AddLight(Graphics& g,Bitmap& layer,float scale,const int box[4],int top,int bottom)
        {
            Bitmap* canvas=state.canvas.get();
            if(!canvas)return;
            const int W=static_cast<int>(canvas->GetWidth()),H=static_cast<int>(canvas->GetHeight());
            const int lw=static_cast<int>(layer.GetWidth()),lh=static_cast<int>(layer.GetHeight());
            const int bx0=std::clamp(box[0],0,lw-1),by0=std::clamp(box[1],0,lh-1),bx1=std::clamp(box[2],0,lw-1),by1=std::clamp(box[3],0,lh-1);
            if(bx1<=bx0||by1<=by0)return;
            const int x0=std::max(0,static_cast<int>(bx0/scale)),x1=std::min(W,static_cast<int>(bx1/scale));
            const int y0=std::max({0,top,static_cast<int>(by0/scale)}),y1=std::min({H,bottom,static_cast<int>(by1/scale)});
            if(x1<=x0||y1<=y0)return;
            g.Flush(FlushIntentionSync);
            BitmapData to{},from{};
            Rect area(x0,y0,x1-x0,y1-y0),whole(0,0,lw,lh);
            if(canvas->LockBits(&area,ImageLockModeRead|ImageLockModeWrite,PixelFormat32bppPARGB,&to)!=Ok)return;
            if(layer.LockBits(&whole,ImageLockModeRead,PixelFormat32bppPARGB,&from)!=Ok){canvas->UnlockBits(&to);return;}
            const auto* src=static_cast<const BYTE*>(from.Scan0);
            for(int y=y0;y<y1;++y)
            {
                const float sy=std::clamp((y+0.5f)*scale-0.5f,0.0f,static_cast<float>(lh-1));
                const int ry=std::min(static_cast<int>(sy),lh-2);
                const float fy=sy-ry;
                const BYTE* row0=src+static_cast<size_t>(ry)*from.Stride;
                const BYTE* row1=row0+from.Stride;
                BYTE* out=static_cast<BYTE*>(to.Scan0)+static_cast<size_t>(y-y0)*to.Stride;
                for(int x=x0;x<x1;++x)
                {
                    const float sx=std::clamp((x+0.5f)*scale-0.5f,0.0f,static_cast<float>(lw-1));
                    const int rx=std::min(static_cast<int>(sx),lw-2);
                    const float fx=sx-rx;
                    const BYTE* a=row0+rx*4;
                    const BYTE* b=row1+rx*4;
                    if((a[3]|a[7]|b[3]|b[7])==0)continue;
                    BYTE* d=out+(x-x0)*4;
                    for(int c=0;c<3;++c)
                    {
                        const float v=(a[c]*(1-fx)+a[c+4]*fx)*(1-fy)+(b[c]*(1-fx)+b[c+4]*fx)*fy;
                        d[c]=static_cast<BYTE>(std::min(255.0f,d[c]+v));
                    }
                    const float alpha=(a[3]*(1-fx)+a[7]*fx)*(1-fy)+(b[3]*(1-fx)+b[7]*fx)*fy;
                    d[3]=static_cast<BYTE>(std::min(255.0f,d[3]+alpha*(255-d[3])/255.0f));
                }
            }
            layer.UnlockBits(&from);
            canvas->UnlockBits(&to);
        }

        // A glow's picture: 64 px, its colour at the middle fading softly to nothing at its edge;
        // one per colour and per sixteenth of strength.
        Bitmap* GlowDisc(Color colour)
        {
            static std::map<std::uint32_t,std::unique_ptr<Bitmap>> made;
            const int level=std::clamp((colour.GetA()+8)/16,1,16);
            const std::uint32_t key=static_cast<std::uint32_t>(level)<<24|colour.GetR()<<16|colour.GetG()<<8|colour.GetB();
            auto found=made.find(key);
            if(found==made.end())
            {
                constexpr int Size=64;
                auto disc=std::make_unique<Bitmap>(Size,Size,PixelFormat32bppPARGB);
                Graphics g(disc.get());
                g.SetSmoothingMode(SmoothingModeAntiAlias);
                GraphicsPath path;
                path.AddEllipse(0.0f,0.0f,static_cast<REAL>(Size),static_cast<REAL>(Size));
                PathGradientBrush brush(&path);
                brush.SetCenterColor(Color(static_cast<BYTE>(std::min(255,level*16)),colour.GetR(),colour.GetG(),colour.GetB()));
                Color edge(0,colour.GetR(),colour.GetG(),colour.GetB());
                int n=1;
                brush.SetSurroundColors(&edge,&n);
                static const REAL factors[5]={0,0.08f,0.3f,0.7f,1},positions[5]={0,0.3f,0.6f,0.85f,1};
                brush.SetBlend(factors,positions,5);
                g.FillPath(&brush,&path);
                found=made.emplace(key,std::move(disc)).first;
            }
            return found->second.get();
        }

        // Where member m of a side stands (front columns first; tanks, being larger, further apart).
        PointF MemberAt(const RectF& field,int side,int m,Kind kind=Kind::Infantry)
        {
            const float f=side==0?1.0f:-1.0f,mid=field.X+field.Width/2;
            const int col=m/4,row=m%4;
            if(Flies(kind))
            {
                // Aircraft: in loose rows across the sky above the field.
                const float sx=field.Width*0.1f,sy=field.Height*0.13f;
                return PointF(mid-f*(field.Width*0.13f+col*sx+(row%2)*0.5f*sx),field.Y+field.Height*0.08f+row*sy+col*0.1f*sy);
            }
            const float sx=field.Width*(kind==Kind::Heavy?0.1f:HexWar::Info(kind).armoured?0.092f:0.075f),sy=field.Height*0.2f;
            const float x=mid-f*(field.Width*0.12f+col*sx+(row%2)*0.45f*sx);
            const float y=field.Y+field.Height*0.18f+row*sy+col*0.08f*sy;
            return PointF(x,y);
        }

        void PaintFight(Graphics& g,const Layout& l,float s)
        {
            const auto& b=state.fight.b;
            const double a=Seconds()-state.fight.start;
            const float W=l.area,H=l.map.Height+l.panel;
            const Color white(255,240,240,236),gold(255,250,214,90),soft(255,190,196,220);
            SolidBrush night(Color(255,16,20,52));
            g.FillRectangle(&night,0.0f,0.0f,W,H);
            // The sides on the screen: yours at the left.
            const int left=b.who[0]==You?0:1,right=left^1;
            const int sideOf[2]={left==0?0:1,left==0?1:0};   // battle index -> screen side
            const float topH=0.15f*H,bottomH=0.2f*H;
            const RectF field(0,topH+0.05f*H,W,H-topH-bottomH-0.1f*H);
            // Members hit by the time a, and when the shots land.
            const double shotStart[2]={ShotStart(0),ShotStart(1)};
            constexpr double Flight=FightFlight;
            int lost[2]{};
            for(int side=0;side<2;++side)
                for(size_t i=0;i<b.shots[side].size();++i)
                    if(b.shots[side][i]&&a>=shotStart[side]+ShotGap*i+Flight)++lost[side^1];
            // The top: each side's name, experience and members left.
            for(int idx:{left,right})
            {
                const bool atLeft=idx==left;
                const RectF name(atLeft?16*s:W/2+0.17f*W,10*s,W/2-0.17f*W-16*s,topH-14*s);
                Text(g,KindCode(b.kinds[idx])+L" "+KindName(b.kinds[idx]),RectF(name.X,name.Y,name.Width,name.Height*0.5f),22*s,white,true,atLeft?StringAlignmentNear:StringAlignmentFar,StringAlignmentCenter);
                // (With what it gains, once the shooting is over.)
                const bool over=a>=FightLength(b)-FightHold&&b.gained[idx]>0;
                Text(g,SideName(b.who[idx])+(idx==0?T(L"（攻撃）",L" (attacking)"):L"")+L"   EXP "+std::to_wstring(b.exp[idx])+(over?L" +"+std::to_wstring(b.gained[idx]):L""),
                     RectF(name.X,name.Y+name.Height*0.5f,name.Width,name.Height*0.5f),15*s,soft,false,atLeft?StringAlignmentNear:StringAlignmentFar,StringAlignmentCenter);
                const float box=topH*0.72f;
                const RectF count(atLeft?W/2-0.14f*W:W/2+0.14f*W-box,(topH-box)/2+2*s,box,box);
                FillRound(g,count,4*s,Color(255,10,10,30),Color(255,210,176,60),3*s);
                Text(g,std::to_wstring(b.before[idx]-lost[idx]),count,box*0.62f,gold,true,StringAlignmentCenter,StringAlignmentCenter);
            }
            // The ground: each side on its own terrain's colour. It stays as it is through the
            // battle, so it is drawn once (for these grounds and this size) into a picture of its
            // own and copied after: drawn afresh, the views and grounds took most of a frame.
            static std::unique_ptr<Bitmap> groundDrawn;
            static std::tuple<Terrain,Terrain,float,float,float,float,float> groundFor;
            const auto groundNow=std::tuple{state.table.At(b.at[left]),state.table.At(b.at[right]),W,H,field.Y,field.Height,s};
            if(!groundDrawn||groundFor!=groundNow)
            {
                groundDrawn=std::make_unique<Bitmap>(static_cast<INT>(std::ceil(W)),static_cast<INT>(std::ceil(H)),PixelFormat32bppPARGB);
                groundFor=groundNow;
                Graphics g(groundDrawn.get());
                g.SetSmoothingMode(SmoothingModeAntiAlias);
                const float y0=field.Y+field.Height*0.08f;
                LinearGradientBrush sky(PointF(0,field.Y-0.05f*H),PointF(0,y0),Color(255,16,20,52),Color(255,40,44,78));
                g.FillRectangle(&sky,0.0f,field.Y-0.05f*H,W,y0-field.Y+0.05f*H);
                for(int idx:{left,right})
                {
                    const bool atLeft=idx==left;
                    const Terrain t=state.table.At(b.at[idx]);
                    if(Bitmap* view=Backdrop(t))
                    {
                        // The view of its ground at the top of its half (the near side at the
                        // screen's edge: hers turned round), the ground's colour below it.
                        // (Scaled to show much of it, not so much that it grows coarse.)
                        const float top=field.Y-0.05f*H,k=std::max((W/2)/(0.8f*view->GetWidth()),(field.Height*0.3f)/view->GetHeight());
                        const float srcW=std::min(static_cast<float>(view->GetWidth()),(W/2)/k);
                        const RectF band(0,top,W/2,view->GetHeight()*k);
                        // (The whole view made to this size once; its left part copied.)
                        Bitmap* sized=Scaled(view,static_cast<int>(std::lround(view->GetWidth()*k)),static_cast<int>(std::lround(band.Height)));
                        const GraphicsState saved=g.Save();
                        if(!atLeft)
                        {
                            g.TranslateTransform(W,0);
                            g.ScaleTransform(-1,1);
                        }
                        if(sized)g.DrawImage(sized,band,0,0,std::min(static_cast<REAL>(sized->GetWidth()),srcW*k),static_cast<REAL>(sized->GetHeight()),UnitPixel);
                        g.Restore(saved);
                        const Color soil=BackdropGround(view);
                        const float gy=band.GetBottom()-1,x0=atLeft?0.0f:W/2,h=field.GetBottom()+0.04f*H-gy;
                        if(Bitmap* floor=Floor(t))
                        {
                            // The ground's picture filling the half below the view (its top
                            // kept, cut at the bottom or sides as the half's shape asks), the
                            // seam blended into the view's colour, and darker toward the front.
                            const float k=std::max((W/2)/floor->GetWidth(),h/floor->GetHeight());
                            // (Made to this size once; the part that fits copied.)
                            if(Bitmap* sized=Scaled(floor,static_cast<int>(std::ceil(floor->GetWidth()*k)),static_cast<int>(std::ceil(floor->GetHeight()*k))))
                            {
                                const GraphicsState kept=g.Save();
                                g.SetClip(RectF(x0,gy,W/2,h));
                                g.DrawImage(sized,RectF(x0,gy,W/2,h),std::floor((sized->GetWidth()-W/2)/2),0,W/2,h,UnitPixel);
                                g.Restore(kept);
                            }
                            const float seam=0.06f*H;
                            LinearGradientBrush blend(PointF(0,gy-1),PointF(0,gy+seam),soil,Color(0,soil.GetR(),soil.GetG(),soil.GetB()));
                            g.FillRectangle(&blend,x0,gy,W/2,seam);
                            LinearGradientBrush shade(PointF(0,gy+h*0.4f),PointF(0,gy+h+1),Color(0,0,0,0),Color(110,0,0,0));
                            g.FillRectangle(&shade,x0,gy+h*0.4f,W/2,h*0.6f);
                            continue;
                        }
                        LinearGradientBrush ground(PointF(0,gy),PointF(0,field.GetBottom()+0.04f*H),soil,
                                                   Color(255,soil.GetR()*6/10,soil.GetG()*6/10,soil.GetB()*6/10));
                        g.FillRectangle(&ground,x0,gy,W/2,h);
                        continue;
                    }
                    const Color ground=t==Terrain::Mountain?Color(255,104,96,86):t==Terrain::Base?Color(255,92,94,98):Color(255,96,118,66);
                    SolidBrush fill(ground);
                    g.FillRectangle(&fill,atLeft?0.0f:W/2,y0,W/2,field.GetBottom()-y0+0.04f*H);
                    if(t==Terrain::Mountain)
                    {
                        SolidBrush distant(Color(255,70,66,76));
                        for(int i=0;i<3;++i)
                        {
                            const float px=(atLeft?0.0f:W/2)+W*(0.08f+0.15f*i),ph=field.Height*(0.12f+0.05f*(i%2));
                            const PointF p[3]={PointF(px-0.1f*W,y0),PointF(px,y0-ph),PointF(px+0.1f*W,y0)};
                            g.FillPolygon(&distant,p,3);
                        }
                    }
                }
                Pen seam(Color(120,0,0,0),2*s);
                g.DrawLine(&seam,W/2,y0,W/2,field.GetBottom()+0.04f*H);
            }
            g.DrawImage(groundDrawn.get(),0.0f,0.0f,static_cast<REAL>(groundDrawn->GetWidth()),static_cast<REAL>(groundDrawn->GetHeight()));
            // A member's size (those hit go from the front).
            auto sizeOf=[&](Kind k){return field.Height*(k==Kind::Heavy||Flies(k)?0.08f:Vehicle(k)?0.075f:0.068f);};
            auto memberAt=[&](int side,int m,Kind k){return MemberAt(field,side,m,k);};
            // Each shot: who fires it, from where, at what, and when (as the members are drawn).
            struct Round {int shooter;size_t i;bool hit,supporting;PointF muzzle,aim;double since;};
            std::vector<Round> rounds;
            for(int shooter=0;shooter<2;++shooter)
            {
                const int target=shooter^1,ss=sideOf[shooter],ts=sideOf[target];
                const float f=ss==0?1.0f:-1.0f,u=sizeOf(b.kinds[shooter]),ut=sizeOf(b.kinds[target]);
                int hits=0;
                for(size_t i=0;i<b.shots[shooter].size();++i)
                {
                    const bool hit=b.shots[shooter][i];
                    // Those hit fall from the front (they have shot by then).
                    const int victim=hits;
                    if(hit)++hits;
                    // Supporting fire comes in from beyond the screen's edge behind the attacker.
                    const bool supporting=shooter==0&&static_cast<int>(i)>=b.own[0];
                    const PointF from=supporting?PointF(ss==0?-0.02f*W:1.02f*W,field.Y+field.Height*(0.1f+0.08f*((i*7)%10))):memberAt(ss,static_cast<int>(i),b.kinds[shooter]);
                    // The muzzle: at the end of the gun (high on the self-propelled gun, at the
                    // launcher's mouth on the shoulder).
                    const Kind sk=b.kinds[shooter];
                    const bool tank=Vehicle(sk);
                    const float ahead=sk==Kind::Heavy?1.3f:sk==Kind::Artillery?1.1f:sk==Kind::Scout?0.95f:sk==Kind::AntiAir?0.25f:Flies(sk)?1.2f:tank?1.15f:sk==Kind::Bazooka?0.9f:0.7f;
                    const float up=sk==Kind::Artillery?0.75f:sk==Kind::AntiAir?1.05f:Flies(sk)?-0.05f:sk==Kind::Bazooka?0.45f:tank?0.42f:0.2f;
                    const PointF muzzle=supporting?from:PointF(from.X+f*ahead*u,from.Y-up*u);
                    const PointF mark=memberAt(ts,hit?victim:static_cast<int>(i%4),b.kinds[target]);
                    const PointF aim=hit?PointF(mark.X,mark.Y-0.1f*ut):PointF(mark.X+f*0.7f*ut,mark.Y+0.55f*ut);
                    rounds.push_back({shooter,i,hit,supporting,muzzle,aim,a-(shotStart[shooter]+ShotGap*i)});
                }
            }
            // Soft glows: discs fading from their colour at the middle to nothing at the edge. They
            // are gathered on a layer of a quarter the size and added to the scene once, at the
            // end, as light (by hand: drawn at full size, or the layer stretched by GDI+, the
            // glows took most of a frame). glowBox: the part of the layer drawn on.
            constexpr float GlowScale=0.25f;
            static std::unique_ptr<Bitmap> glowLayer;
            const int gw=static_cast<int>(std::ceil(W*GlowScale))+1,gh=static_cast<int>(std::ceil(H*GlowScale))+1;
            if(!glowLayer||static_cast<int>(glowLayer->GetWidth())!=gw||static_cast<int>(glowLayer->GetHeight())!=gh)
                glowLayer=std::make_unique<Bitmap>(gw,gh,PixelFormat32bppPARGB);
            Graphics lights(glowLayer.get());
            lights.Clear(Color(0,0,0,0));
            lights.SetInterpolationMode(InterpolationModeBilinear);
            int glowBox[4]={gw,gh,0,0};
            auto glow=[&](PointF c,float radius,Color colour){
                if(radius<1||colour.GetA()<8)return;
                const float r=radius*GlowScale;
                if(Bitmap* disc=GlowDisc(colour))lights.DrawImage(disc,RectF(c.X*GlowScale-r,c.Y*GlowScale-r,2*r,2*r));
                glowBox[0]=std::min(glowBox[0],static_cast<int>(c.X*GlowScale-r)-1);
                glowBox[1]=std::min(glowBox[1],static_cast<int>(c.Y*GlowScale-r)-1);
                glowBox[2]=std::max(glowBox[2],static_cast<int>(c.X*GlowScale+r)+2);
                glowBox[3]=std::max(glowBox[3],static_cast<int>(c.Y*GlowScale+r)+2);
            };
            auto alpha=[](double v){return static_cast<BYTE>(std::clamp(v,0.0,1.0)*255);};
            // A little hash, for sparks and flames that stay the same from frame to frame.
            auto noise=[](unsigned n){n=(n^61)^(n>>16);n*=9;n^=n>>4;n*=0x27d4eb2d;n^=n>>15;return (n&0xFFFF)/65535.0f;};
            const Color magic[2]={Color(255,120,220,255),Color(255,255,120,200)};   // the shots' glow, by screen side
            // The members hit burn where they stood until the battle's end: flames flickering over
            // a red glow, smoke rising.
            for(const Round& q:rounds)
            {
                const double burning=q.since-Flight;
                if(!q.hit||burning<0.25||Flies(b.kinds[q.shooter^1]))continue;
                const float ut=sizeOf(b.kinds[q.shooter^1]),fade=static_cast<float>(std::clamp((FightLength(b)-q.since-0.1)/0.6,0.0,1.0));
                const float tt=static_cast<float>(a);
                const unsigned seed=static_cast<unsigned>(q.shooter*97+q.i*13);
                const PointF base(q.aim.X,q.aim.Y+0.35f*ut);
                glow(base,1.9f*ut,Color(alpha(0.7*fade*(0.85+0.15*std::sin(a*13+q.i))),255,90,30));
                SolidBrush scorch(Color(alpha(0.6*fade),30,20,16));
                g.FillEllipse(&scorch,base.X-0.8f*ut,base.Y-0.12f*ut,1.6f*ut,0.4f*ut);
                for(int k=0;k<5;++k)
                {
                    const float dx=(noise(seed+k)-0.5f)*1.2f*ut,flick=0.75f+0.25f*std::sin(tt*(9+k*2.3f)+k);
                    const float h=(0.8f+0.7f*noise(seed+k+7))*ut*flick,w=0.36f*ut;
                    const PointF foot(base.X+dx,base.Y);
                    // A tongue of flame: wide at the foot, pointed at the top, leaning a little.
                    const float lean=0.15f*ut*std::sin(tt*5+k);
                    const PointF tongue[4]={PointF(foot.X-w,foot.Y),PointF(foot.X-w*0.3f+lean*0.5f,foot.Y-h*0.6f),PointF(foot.X+lean,foot.Y-h),PointF(foot.X+w,foot.Y)};
                    SolidBrush outer(Color(alpha(0.85*fade),255,110+static_cast<int>(40*noise(seed+k+3)),30));
                    g.FillClosedCurve(&outer,tongue,4,FillModeAlternate,0.5f);
                    const PointF inner[4]={PointF(foot.X-w*0.5f,foot.Y),PointF(foot.X-w*0.15f+lean*0.3f,foot.Y-h*0.35f),PointF(foot.X+lean*0.6f,foot.Y-h*0.6f),PointF(foot.X+w*0.5f,foot.Y)};
                    SolidBrush hot(Color(alpha(0.9*fade),255,230,140));
                    g.FillClosedCurve(&hot,inner,4,FillModeAlternate,0.5f);
                }
                for(int k=0;k<4;++k)
                {
                    // Smoke: puffs rising and spreading, each on its own cycle.
                    const float cycle=static_cast<float>(std::fmod(burning*0.6+k*0.25,1.0));
                    const float rise=cycle*2.2f*ut,size=(0.4f+cycle*1.1f)*ut;
                    SolidBrush smoke(Color(alpha(0.45*(1-cycle)*fade),50,46,52));
                    g.FillEllipse(&smoke,base.X+(noise(seed+k+20)-0.5f)*ut+cycle*0.4f*ut-size/2,base.Y-0.8f*ut-rise-size/2,size,size);
                }
            }
            // The members come in from the screen's edge one after another: vehicles shaking as
            // they roll, with dust behind, and rocking to a stop; men bobbing as they run.
            for(int idx:{left,right})
            {
                const int side=sideOf[idx];
                const bool vehicle=Vehicle(b.kinds[idx]);
                const float f=side==0?1.0f:-1.0f,u=sizeOf(b.kinds[idx]);
                for(int m=b.before[idx]-1;m>=lost[idx];--m)
                {
                    const double start=0.06*(m%4)+0.08*(m/4),length=MoveIn;
                    const float go=static_cast<float>(std::clamp((a-start)/length,0.0,1.0)),eased=1-(1-go)*(1-go);
                    const float slide=(1-eased)*W*0.55f*(-f);
                    const float moving=1-go,tt=static_cast<float>(a);
                    float dx=0,dy=0,tilt=0;
                    const bool flying=Flies(b.kinds[idx]);
                    if(flying)dy=std::sin(tt*2.4f+m*0.9f)*0.1f*u;   // aircraft: riding the air
                    else if(vehicle)
                    {
                        // Shaking the less, the slower it goes (its speed falls as 1 - go).
                        dy=(std::sin(tt*55+m*1.7f)*0.05f+std::sin(tt*91+m)*0.025f)*u*moving;
                        // Stopping: it pitches forward onto its nose and settles back.
                        const double since=a-start-length;
                        if(since>=0&&since<Pitch)
                        {
                            const double k=since/Pitch;
                            tilt=static_cast<float>(5*std::sin(k*3.14159)*(1-0.5*k));
                            dx=static_cast<float>(f*std::sin(k*3.14159)*0.06*u);
                        }
                    }
                    else dy=moving>0?-std::abs(std::sin(tt*16+m*1.3f))*0.14f*u:0;
                    const PointF p=memberAt(side,m,b.kinds[idx]);
                    const PointF at(p.X+slide+dx,p.Y+dy);
                    if(vehicle&&!flying&&moving>0)
                        for(int k=0;k<3;++k)
                        {
                            // Dust thrown up behind it.
                            const float cycle=static_cast<float>(std::fmod(a*3+k/3.0+m*0.17,1.0)),size=(0.35f+0.6f*cycle)*u;
                            SolidBrush dust(Color(alpha(0.5*(1-cycle)*std::min(1.0f,moving*3)),150,130,100));
                            g.FillEllipse(&dust,at.X-f*(1.1f+0.8f*cycle)*u-size/2,at.Y+0.35f*u-0.4f*cycle*u-size/2,size,size*0.8f);
                        }
                    const Color sc=SideColor(b.who[idx]);
                    SolidBrush shadow(Color(120,sc.GetR(),sc.GetG(),sc.GetB()));
                    if(!flying)g.FillEllipse(&shadow,p.X+slide+dx-1.1f*u,p.Y+(vehicle?0.45f:0.75f)*u,2.2f*u,0.5f*u);
                    DrawSprite(g,at,u,b.kinds[idx],b.who[idx],f,false,tilt);
                }
            }
            // Shots: fire from the muzzle in a ring of light, the round a glowing bolt with a
            // trail, and where it lands a burst (on a member hit: a fireball, a ring of light
            // going out, sparks; a miss: a puff of earth).
            for(const Round& q:rounds)
            {
                if(q.since<0||q.since>Flight+0.7)continue;
                const int ss=sideOf[q.shooter];
                const float f=ss==0?1.0f:-1.0f,u=sizeOf(b.kinds[q.shooter]);
                const bool tank=q.supporting||b.kinds[q.shooter]!=Kind::Infantry;
                const Color mc=magic[ss];
                const unsigned seed=static_cast<unsigned>(q.shooter*131+q.i*17);
                if(q.since<0.16&&!q.supporting)
                {
                    const float k=static_cast<float>(q.since/0.16),fade=1-k;
                    const float len=(tank?2.5f:1.2f)*u*(0.6f+0.8f*std::sqrt(k)),wid=(tank?0.48f:0.2f)*u*(1+k);
                    glow(q.muzzle,(tank?1.9f:0.8f)*u,Color(alpha(fade),255,190,80));
                    // The jet of fire: a long flame forward, a short one to each side.
                    for(const auto& [dir,scale]:{std::pair{0.0f,1.0f},std::pair{0.9f,0.35f},std::pair{-0.9f,0.35f}})
                    {
                        // Along d, n across it.
                        const float dx=f*std::cos(dir),dy=std::sin(dir),nx=-dy,ny=dx,l2=len*scale,w2=wid*(dir==0?1.0f:0.6f);
                        auto at=[&](float along,float across){return PointF(q.muzzle.X+dx*along+nx*across,q.muzzle.Y+dy*along+ny*across);};
                        const PointF flame[5]={at(0,w2),at(l2*0.45f,w2*0.8f),at(l2,0),at(l2*0.45f,-w2*0.8f),at(0,-w2)};
                        SolidBrush outer(Color(alpha(fade),255,140,40)),inner(Color(alpha(fade),255,245,190));
                        g.FillClosedCurve(&outer,flame,5,FillModeAlternate,0.5f);
                        const PointF core[3]={at(0,w2*0.45f),at(l2*0.55f,0),at(0,-w2*0.45f)};
                        g.FillPolygon(&inner,core,3);
                    }
                    // The ring of light the shot leaves through: a circle of runes facing out.
                    const float ring=(tank?0.75f:0.38f)*u*(0.7f+0.7f*k);
                    Pen rune(Color(alpha(0.9*fade),mc.GetR(),mc.GetG(),mc.GetB()),std::max(1.5f,0.06f*u));
                    g.DrawEllipse(&rune,q.muzzle.X+f*0.25f*u-ring*0.35f,q.muzzle.Y-ring,ring*0.7f,ring*2);
                    for(int k2=0;k2<6;++k2)
                    {
                        const float ang=k2*1.0472f+static_cast<float>(q.since)*6;
                        const PointF dot(q.muzzle.X+f*0.25f*u+std::cos(ang)*ring*0.35f,q.muzzle.Y+std::sin(ang)*ring);
                        SolidBrush spark(Color(alpha(fade),255,255,255));
                        g.FillEllipse(&spark,dot.X-0.05f*u,dot.Y-0.05f*u,0.1f*u,0.1f*u);
                    }
                }
                if(q.since<Flight)
                {
                    // The bolt on a low arc, its trail of light fading behind it.
                    const float lift=(tank?0.6f:0.25f)*u;
                    auto along=[&](float k){return PointF(q.muzzle.X+(q.aim.X-q.muzzle.X)*k,q.muzzle.Y+(q.aim.Y-q.muzzle.Y)*k-lift*4*k*(1-k));};
                    const float k=static_cast<float>(q.since/Flight);
                    for(int j=12;j>=1;--j)
                    {
                        const float back=std::max(0.0f,k-j*0.03f);
                        const PointF p=along(back);
                        const float r=(tank?0.32f:0.15f)*u*(1-j/13.0f);
                        SolidBrush trail(Color(alpha(0.6*(1-j/13.0)),mc.GetR(),mc.GetG(),mc.GetB()));
                        g.FillEllipse(&trail,p.X-r,p.Y-r,2*r,2*r);
                        if(j%3==0)
                        {
                            // Motes of light shed along the way.
                            const PointF mote(p.X+(noise(seed+j)-0.5f)*0.5f*u,p.Y+(noise(seed+j+50)-0.5f)*0.5f*u+j*0.02f*u);
                            SolidBrush white(Color(alpha(0.8*(1-j/13.0)),255,255,255));
                            g.FillEllipse(&white,mote.X-0.04f*u,mote.Y-0.04f*u,0.08f*u,0.08f*u);
                        }
                    }
                    const PointF at=along(k);
                    glow(at,(tank?1.3f:0.6f)*u,Color(230,mc.GetR(),mc.GetG(),mc.GetB()));
                    const float r=(tank?0.26f:0.13f)*u;
                    SolidBrush core(Color(255,255,255,250));
                    g.FillEllipse(&core,at.X-r,at.Y-r,2*r,2*r);
                }
                else
                {
                    const float k=static_cast<float>((q.since-Flight)/0.7);
                    const float ut=sizeOf(b.kinds[q.shooter^1]);
                    if(q.hit)
                    {
                        // No round shapes: a white flash, then ragged billows of fire (each a
                        // lumpy outline that churns, dark red at its rim, yellow within) rising
                        // as they cool into smoke, tongues of flame thrown out, streaks of
                        // sparks, bits flung up and falling.
                        const float big=(tank?3.0f:2.0f)*ut;
                        // A lumpy outline round c, r across: points at random distances, turning slowly.
                        auto ragged=[&](PointF c,float r,unsigned s,int n,float rough){
                            std::vector<PointF> points(static_cast<size_t>(n));
                            for(int j=0;j<n;++j)
                            {
                                const float ang=j*6.283f/n+0.6f*noise(s+j)+static_cast<float>(a)*0.8f;
                                const float d=r*(1-rough+rough*2*noise(s+j*7+static_cast<unsigned>(a*6)%5));
                                points[static_cast<size_t>(j)]=PointF(c.X+std::cos(ang)*d,c.Y+std::sin(ang)*d*0.8f);
                            }
                            return points;
                        };
                        if(k<0.18f)glow(q.aim,big*1.8f,Color(alpha(0.9*(1-k/0.18)),255,255,235));
                        // Smoke first (under the fire), then the fire.
                        for(int j=0;j<5;++j)
                        {
                            if(k<0.3f)break;
                            const float sk=(k-0.3f)/0.7f;
                            const float ang=noise(seed+j+60)*6.283f;
                            const PointF c(q.aim.X+std::cos(ang)*0.4f*big,q.aim.Y-big*(0.3f+0.9f*sk)+std::sin(ang)*0.2f*big);
                            const auto puff=ragged(c,big*(0.3f+0.35f*sk),seed+j*31,9,0.3f);
                            SolidBrush smoke(Color(alpha(0.55*(1-sk)),46,40,44));
                            g.FillClosedCurve(&smoke,puff.data(),static_cast<INT>(puff.size()),FillModeAlternate,0.6f);
                        }
                        for(int j=0;j<7;++j)
                        {
                            const float ang=noise(seed+j)*6.283f,d=(0.1f+0.5f*noise(seed+j+9))*big*(0.35f+k);
                            const float r=big*(0.26f+0.2f*noise(seed+j+4))*(0.6f+0.6f*k);
                            const PointF c(q.aim.X+std::cos(ang)*d,q.aim.Y+std::sin(ang)*d*0.6f-0.55f*big*k);
                            const auto outer=ragged(c,r,seed+j*13,10,0.35f);
                            const auto inner=ragged(PointF(c.X,c.Y-0.1f*r),r*0.62f,seed+j*17+3,8,0.4f);
                            SolidBrush rim(Color(alpha(0.85*(1-k)),static_cast<int>(210-90*k),static_cast<int>(60-40*k),20));
                            SolidBrush core(Color(alpha(0.95*(1-k)),255,static_cast<int>(220-140*k),static_cast<int>(100-80*k)));
                            g.FillClosedCurve(&rim,outer.data(),static_cast<INT>(outer.size()),FillModeAlternate,0.55f);
                            g.FillClosedCurve(&core,inner.data(),static_cast<INT>(inner.size()),FillModeAlternate,0.55f);
                            glow(c,r*2.2f,Color(alpha(0.55*(1-k)),255,130,40));
                        }
                        // Tongues of flame thrown out at first, mostly upward.
                        if(k<0.5f)
                            for(int j=0;j<6;++j)
                            {
                                const float ang=-1.5708f+(noise(seed+j+80)-0.5f)*2.6f;
                                const float len=big*(0.6f+0.6f*noise(seed+j+90))*(0.4f+1.2f*k),w=big*0.12f*(1-k);
                                const float dx=std::cos(ang),dy=std::sin(ang);
                                const PointF tongue[4]={PointF(q.aim.X-dy*w,q.aim.Y+dx*w),PointF(q.aim.X+dx*len*0.55f-dy*w*0.7f,q.aim.Y+dy*len*0.55f+dx*w*0.7f),
                                                        PointF(q.aim.X+dx*len,q.aim.Y+dy*len),PointF(q.aim.X+dy*w,q.aim.Y-dx*w)};
                                SolidBrush flame(Color(alpha(0.9*(1-k*2)),255,190,70));
                                g.FillClosedCurve(&flame,tongue,4,FillModeAlternate,0.4f);
                            }
                        // Streaks of sparks, and the shot's own light breaking into motes.
                        for(int j=0;j<12;++j)
                        {
                            const float ang=noise(seed+j+30)*6.283f,d=big*(0.2f+1.5f*k)*(0.5f+0.5f*noise(seed+j+40));
                            const float fall=0.8f*big*k*k;
                            const PointF head(q.aim.X+std::cos(ang)*d,q.aim.Y+std::sin(ang)*d*0.7f+fall);
                            const PointF tail(q.aim.X+std::cos(ang)*d*0.7f,q.aim.Y+std::sin(ang)*d*0.7f*0.7f+fall*0.7f);
                            const Color sc=j%4==0?Color(alpha(1-k),mc.GetR(),mc.GetG(),mc.GetB()):Color(alpha(1-k),255,j%3?225:255,j%3?110:230);
                            Pen streak(sc,std::max(1.0f,0.05f*ut*(1-k)));
                            streak.SetEndCap(LineCapRound);
                            g.DrawLine(&streak,tail,head);
                        }
                        // Bits flung up and falling.
                        for(int j=0;j<5;++j)
                        {
                            const float vx=(noise(seed+j+100)-0.5f)*2.2f*big,vy=-(0.8f+noise(seed+j+110))*1.6f*big;
                            const float tt=k*0.7f;
                            const PointF p(q.aim.X+vx*tt,q.aim.Y+vy*tt+3.2f*big*tt*tt);
                            const float r=0.07f*ut;
                            const PointF bit[3]={PointF(p.X-r,p.Y),PointF(p.X+r*0.6f,p.Y-r*1.1f),PointF(p.X+r,p.Y+r*0.5f)};
                            SolidBrush dark(Color(alpha(1-k*0.6),40,32,30));
                            g.FillPolygon(&dark,bit,3);
                        }
                    }
                    else
                    {
                        // A puff of earth, a few sparks of the bolt.
                        const float r=0.5f*ut*(0.5f+k);
                        SolidBrush dust(Color(alpha(0.55*(1-k)),120,100,80));
                        g.FillEllipse(&dust,q.aim.X-r,q.aim.Y-r*0.7f-0.3f*ut*k,2*r,1.4f*r);
                        glow(q.aim,0.6f*ut,Color(alpha(0.6*(1-k*2)),mc.GetR(),mc.GetG(),mc.GetB()));
                    }
                }
            }
            lights.Flush(FlushIntentionSync);
            AddLight(g,*glowLayer,GlowScale,glowBox,static_cast<int>(field.Y-0.05f*H),static_cast<int>(field.GetBottom()+0.05f*H));
            // The bottom: each side's attack and defence (as strengths), the terrain's cover,
            // the chance of each shot, and whether it is surrounded.
            {
                const float y=H-bottomH+8*s,bw=0.3f*W,bh=bottomH-16*s;
                for(int idx:{left,right})
                {
                    const bool atLeft=idx==left;
                    const RectF box(atLeft?12*s:W-12*s-bw,y,bw,bh);
                    FillRound(g,box,4*s,Color(255,14,16,40),Color(255,210,176,60),2.5f*s);
                    const float row=bh/3;
                    auto line=[&](int i,const std::wstring& label,const std::wstring& value,Color c){
                        Text(g,label,RectF(box.X+10*s,box.Y+row*i+4*s,box.Width-20*s,row),17*s,c,true,StringAlignmentNear,StringAlignmentCenter);
                        Text(g,value,RectF(box.X+10*s,box.Y+row*i+4*s,box.Width-20*s,row),17*s,c,true,StringAlignmentFar,StringAlignmentCenter);
                    };
                    line(0,T(L"攻撃力",L"Attack"),std::to_wstring(static_cast<int>(std::lround(b.attack[idx]*40))),white);
                    line(1,T(L"防御力",L"Defence"),std::to_wstring(static_cast<int>(std::lround(b.defence[idx]*40))),white);
                    line(2,b.surrounded[idx]?T(L"命中率（包囲されている）",L"Hit chance (surrounded)"):T(L"命中率",L"Hit chance"),
                         std::to_wstring(static_cast<int>(std::lround(b.chance[idx]*100)))+L"%",b.surrounded[idx]?Color(255,255,120,100):soft);
                }
                const RectF mid(12*s+bw,y,W-24*s-2*bw,bh);
                Text(g,T(L"地形効果",L"Terrain"),RectF(mid.X,mid.Y,mid.Width,bh*0.45f),20*s,white,true,StringAlignmentCenter,StringAlignmentCenter);
                if(b.support>0)
                    Text(g,Fmt(T(L"支援射撃 +{0}",L"Supporting fire +{0}"),{std::to_wstring(b.support)}),RectF(mid.X,mid.Y-bh*0.32f,mid.Width,bh*0.3f),17*s,gold,true,StringAlignmentCenter,StringAlignmentCenter);
                Text(g,std::to_wstring(static_cast<int>(std::lround(b.cover[left]*100)))+L"%",RectF(mid.X,mid.Y+bh*0.45f,mid.Width/2,bh*0.5f),20*s,white,true,StringAlignmentCenter,StringAlignmentCenter);
                Text(g,std::to_wstring(static_cast<int>(std::lround(b.cover[right]*100)))+L"%",RectF(mid.X+mid.Width/2,mid.Y+bh*0.45f,mid.Width/2,bh*0.5f),20*s,white,true,StringAlignmentCenter,StringAlignmentCenter);
            }
            Text(g,T(L"クリックで飛ばす",L"Click to skip"),RectF(0,field.Y-0.05f*H,W-10*s,20*s),12*s,Color(160,200,200,220),false,StringAlignmentFar);
        }

        // Where a map's first side (its 'B' base) stands against the other: left, right, below
        // or above, as the two buttons' names.
        std::pair<std::wstring,std::wstring> SideNames(int m)
        {
            const auto& info=HexWar::Map(m);
            int c[2]={0,0},r[2]={0,0};
            for(int row=0;row<static_cast<int>(info.terrain.size());++row)
                for(int col=0;col<info.columns;++col)
                {
                    const char ch=info.terrain[static_cast<size_t>(row)][col];
                    if(ch=='B'||ch=='b'){c[ch=='b']=col;r[ch=='b']=row;}
                }
            const std::wstring left=T(L"左側",L"Left"),right=T(L"右側",L"Right"),low=T(L"下側",L"Bottom"),high=T(L"上側",L"Top");
            if(1.5*std::abs(c[0]-c[1])>=1.7*std::abs(r[0]-r[1]))return c[0]<c[1]?std::pair{left,right}:std::pair{right,left};
            return r[0]>r[1]?std::pair{low,high}:std::pair{high,low};
        }

        // The map choice, over the map's side: a card for each map offered, its picture, name,
        // size and how you have done on it; one not open yet darker, with what opens it. Under
        // them, the side you take.
        void PaintChoice(Graphics& g,const Layout& l,float s)
        {
            const float W=l.area,H=l.map.Height+l.panel;
            const Color white(255,240,240,236),soft(255,190,196,214),gold(255,250,214,110);
            LinearGradientBrush back(PointF(0,0),PointF(0,H),Color(255,22,28,56),Color(255,10,12,28));
            g.FillRectangle(&back,0.0f,0.0f,W,H);
            Text(g,T(L"マップを選ぶ",L"Choose a map"),RectF(0,24*s,W,44*s),32*s,gold,true,StringAlignmentCenter,StringAlignmentCenter);
            Text(g,Fmt(T(L"マップを選び（クリックか←→、1〜{1}のキー）、陣地を選んで（Tab）、スタート（Enter）。マップは、前のマップで1回勝つか{0}回遊ぶと開きます。",
                         L"Choose a map (click, arrows or keys 1-{1}) and your side (Tab), then Start (Enter). A map opens once the one before it is won, or played {0} times."),
                       {std::to_wstring(PlaysToOpen),std::to_wstring(Offered())}),
                 RectF(20*s,72*s,W-40*s,24*s),15*s,soft,false,StringAlignmentCenter,StringAlignmentCenter);
            const int n=Offered();
            const float gap=18*s,cw=std::min(320*s,(W-gap*(n+1))/n),chh=std::min(H-230*s,cw*1.7f);
            const float x0=(W-(cw*n+gap*(n-1)))/2,y0=120*s;
            state.cards.assign(static_cast<size_t>(n),RectF());
            for(int m=0,k=0;m<HexWar::Maps();++m)
            {
                const auto& info=HexWar::Map(m);
                if(!info.offered)continue;
                const RectF card(x0+k*(cw+gap),y0,cw,chh);
                state.cards[static_cast<size_t>(k)]=card;
                const bool open=Open(m),on=state.choice==k;
                FillRound(g,card,12*s,on?Color(255,40,56,104):Color(255,28,34,64),on?gold:Color(255,90,96,130),on?3*s:1.5f*s);
                // Its picture, as large as fits.
                const RectF pic(card.X+12*s,card.Y+12*s,card.Width-24*s,card.Height-120*s);
                if(Bitmap* p=Art(info.picture))
                {
                    const float k2=std::min(pic.Width/p->GetWidth(),pic.Height/p->GetHeight());
                    const float pw=std::round(p->GetWidth()*k2),ph=std::round(p->GetHeight()*k2);
                    if(Bitmap* shrunk=Scaled(p,static_cast<int>(pw),static_cast<int>(ph)))
                        g.DrawImage(shrunk,std::round(pic.X+(pic.Width-pw)/2),std::round(pic.Y+(pic.Height-ph)/2),pw,ph);
                    if(!open)
                    {
                        SolidBrush dark(Color(170,10,12,24));
                        g.FillRectangle(&dark,pic.X+(pic.Width-pw)/2,pic.Y+(pic.Height-ph)/2,pw,ph);
                        Text(g,T(L"未開放",L"Locked"),pic,30*s,white,true,StringAlignmentCenter,StringAlignmentCenter);
                    }
                }
                const float ty=card.GetBottom()-102*s;
                Text(g,std::to_wstring(k+1)+L"  "+(English()?info.nameEn:info.nameJa),RectF(card.X+12*s,ty,card.Width-24*s,28*s),std::min(19*s,(card.Width-24*s)/10.5f),open?white:soft,true);
                Text(g,Fmt(T(L"{0}×{1}マス　{2}ターン",L"{0} x {1} hexes   {2} turns"),{std::to_wstring(info.columns),std::to_wstring(info.rows),std::to_wstring(info.turns)}),
                     RectF(card.X+12*s,ty+32*s,card.Width-24*s,22*s),14*s,soft);
                const std::wstring record=open?Fmt(T(L"遊んだ {0}回　勝ち {1}回",L"Played {0}   won {1}"),{std::to_wstring(state.played[static_cast<size_t>(m)]),std::to_wstring(state.won[static_cast<size_t>(m)])}):
                                              Fmt(T(L"前のマップで1勝か{0}回遊ぶと開く",L"Opens with a win (or {0} games) before"),{std::to_wstring(PlaysToOpen)});
                Text(g,record,RectF(card.X+12*s,ty+58*s,card.Width-24*s,22*s),14*s,open?gold:soft);
                ++k;
            }
            // The side: by chance, or one of the two (named for the map the keys are on).
            const auto [first,second]=SideNames(std::clamp(state.choice,0,HexWar::Maps()-1));
            const std::wstring names[3]={T(L"ランダム",L"Random"),first,second};
            const float bw=130*s,bh=36*s,by=y0+chh+24*s,lw=150*s;
            const float bx=(W-(lw+3*bw+2*12*s))/2;
            Text(g,T(L"あなたの陣地",L"Your side"),RectF(bx,by,lw-10*s,bh),17*s,soft,true,StringAlignmentFar,StringAlignmentCenter);
            for(int i=0;i<3;++i)
            {
                const RectF b(bx+lw+i*(bw+12*s),by,bw,bh);
                state.sideButtons[i]=b;
                const bool on=state.side==i-1;
                FillRound(g,b,8*s,on?Color(255,40,56,104):Color(255,28,34,64),on?gold:Color(255,90,96,130),on?2.5f*s:1.5f*s);
                Text(g,names[i],b,16*s,on?white:soft,on,StringAlignmentCenter,StringAlignmentCenter);
            }
            // Start, at the bottom: the map chosen, from the side chosen.
            const int m=std::clamp(state.choice,0,HexWar::Maps()-1);
            const bool open=Open(m);
            const float sw=260*s,sh=56*s;
            state.startButton=RectF((W-sw)/2,std::max(by+bh+30*s,H-sh-32*s),sw,sh);
            FillRound(g,state.startButton,12*s,open?Color(255,190,96,40):Color(255,60,62,80),open?gold:Color(255,90,96,130),2*s);
            Text(g,Fmt(T(L"スタート：{0}",L"Start: {0}"),{English()?HexWar::Map(m).nameEn:HexWar::Map(m).nameJa}),state.startButton,20*s,open?white:soft,true,StringAlignmentCenter,StringAlignmentCenter);
        }

        void PaintEnd(Graphics& g,float cw,float ch,float s)
        {
            auto& t=state.table;
            SolidBrush shade(Color(150,0,0,0));
            g.FillRectangle(&shade,0.0f,0.0f,cw,ch);
            const float w=std::min(cw-40*s,600*s),h=std::min(ch-40*s,300*s);
            const RectF box((cw-w)/2,(ch-h)/2,w,h);
            FillRound(g,box,14*s,Color(255,22,28,56),Color(255,200,170,90),2*s);
            float y=box.Y+22*s;
            const Color gold(255,240,206,120),white(255,240,240,236),soft(255,200,190,186);
            Text(g,T(L"作戦終了",L"Battle over"),RectF(box.X,y,w,44*s),34*s,gold,true,StringAlignmentCenter);
            y+=70*s;
            const int winner=t.Winner();
            if(winner==Her)Runs(g,{{HerName(),false},{T(L"の勝ち",L" wins"),false}},RectF(box.X,y,w,48*s),32*s,gold);
            else Runs(g,{{winner==You?T(L"あなたの勝ち！",L"You win!"):T(L"引き分け",L"A draw"),false}},RectF(box.X,y,w,48*s),32*s,gold);
            y+=60*s;
            std::wstring how=EndingText();
            if(t.HowEnded()==HexWar::Ending::Resigned)
                how=Fmt(T(L"{0}が降参しました",L"{0} gave up"),{winner==You?HerName():std::wstring(T(L"あなた",L"You"))});
            Text(g,how,RectF(box.X,y,w,30*s),19*s,white,false,StringAlignmentCenter);
            Text(g,T(L"Enter：もう一戦　M：マップを選ぶ　Esc：閉じる",L"Enter: play again   M: choose a map   Esc: close"),RectF(box.X,box.GetBottom()-44*s,w,30*s),17*s,soft,false,StringAlignmentCenter);
        }

        // The panel under the map: what is pointed at (a unit: its kind, members, experience,
        // strengths and ground; a battle it would give), and the turn with its button.
        void PaintPanel(Graphics& g,const Layout& l,float s)
        {
            auto& t=state.table;
            const Color white(255,240,240,236),soft(255,190,196,214),gold(255,250,214,110);
            const RectF box(0,l.map.Height,l.area,l.panel);
            SolidBrush fill(Color(255,20,26,50));
            g.FillRectangle(&fill,box);
            Pen top(Color(255,210,176,60),3*s);
            g.DrawLine(&top,box.X,box.Y+1.5f*s,box.GetRight(),box.Y+1.5f*s);
            // The turn and the button, at the right.
            const float rw=std::min(250*s,box.Width*0.32f),rx=box.GetRight()-rw-12*s;
            Text(g,Fmt(T(L"ターン {0} / {1}",L"Turn {0} / {1}"),{std::to_wstring(std::min(t.Turn(),t.TurnLimit())),std::to_wstring(t.TurnLimit())}),
                 RectF(rx,box.Y+10*s,rw,24*s),18*s,white,true);
            Text(g,Fmt(T(L"あなた {0}部隊　{1} {2}部隊",L"You {0} units   {1} {2}"),{std::to_wstring(t.Alive(You)),HerName(),std::to_wstring(t.Alive(Her))}),
                 RectF(rx,box.Y+36*s,rw,22*s),14*s,soft);
            state.endButton=RectF(rx,box.Y+64*s,rw,46*s);
            const bool can=YourTurnNow();
            FillRound(g,state.endButton,8*s,can?(state.overEnd?Color(255,214,120,48):Color(255,176,92,36)):Color(255,60,62,80),Color(255,240,206,120),1.5f*s);
            Text(g,t.Now()==Phase::HerTurn?Fmt(T(L"{0}の番",L"{0}'s turn"),{HerName()}):std::wstring(T(L"ターン終了（E）",L"End turn (E)")),
                 state.endButton,18*s,can?white:soft,true,StringAlignmentCenter,StringAlignmentCenter);
            // The orders: both gauges, and the button for the menu.
            const float ow=std::min(180*s,box.Width*0.22f),ox=rx-ow-14*s;
            {
                Text(g,T(L"作戦ゲージ",L"Command gauge"),RectF(ox,box.Y+8*s,ow,20*s),13*s,soft);
                for(int who:{You,Her})
                {
                    const float y=box.Y+(who==You?30:46)*s,h=(who==You?10:6)*s,k=static_cast<float>(t.Gauge(who))/HexWar::GaugeFull;
                    FillRound(g,RectF(ox,y,ow-36*s,h),h/2,Color(255,40,44,70));
                    if(k>0)FillRound(g,RectF(ox,y,(ow-36*s)*k,h),h/2,SideColor(who));
                    Text(g,std::to_wstring(t.Gauge(who)),RectF(ox+ow-34*s,y-5*s,34*s,h+10*s),12*s,who==You?white:soft,false,StringAlignmentFar,StringAlignmentCenter);
                }
                state.orderButton=RectF(ox,box.Y+64*s,ow,46*s);
                const bool can=YourTurnNow()&&!t.Commanded(You);
                FillRound(g,state.orderButton,8*s,can?(state.orderMenu||state.overOrder?Color(255,70,110,190):Color(255,46,82,150)):Color(255,60,62,80),Color(255,240,206,120),1.5f*s);
                Text(g,t.Commanded(You)&&t.Now()==Phase::YourTurn?std::wstring(T(L"作戦は済み",L"Ordered")):std::wstring(T(L"作戦（S）",L"Orders (S)")),
                     state.orderButton,17*s,can?white:soft,true,StringAlignmentCenter,StringAlignmentCenter);
            }
            // What is pointed at: the unit under the mouse, else the chosen one.
            const float lx=box.X+14*s,lw=ox-lx-14*s;
            const HexWar::Unit* acting=Acting();
            const HexWar::Unit* shown=state.hover>=0?t.UnitAt(state.hover):nullptr;
            if(!shown)shown=Chosen();
            if(state.buildMenu>=0)
            {
                Kind k=state.buildKinds.empty()?Kind::Tank:state.buildKinds[0];
                for(size_t i=0;i<state.buildRows.size();++i)
                    if(state.buildRows[i].Contains(state.hoverPoint.X,state.hoverPoint.Y))k=state.buildKinds[i];
                const auto& info=HexWar::Info(k);
                const float icon=std::min(l.panel-24*s,84*s);
                FillRound(g,RectF(lx,box.Y+12*s,icon,icon),8*s,Color(255,36,44,76));
                DrawSprite(g,PointF(lx+icon/2,box.Y+12*s+icon*0.55f),icon*0.36f,k,You,1,false);
                const float tx=lx+icon+14*s,tw=lw-icon-14*s;
                Text(g,KindCode(k)+L" "+KindName(k)+L"　"+Fmt(T(L"生産 {0}ターン",L"builds in {0} turns"),{std::to_wstring(HexWar::BuildTurns(k))}),RectF(tx,box.Y+10*s,tw,26*s),19*s,white,true);
                std::wstring more=info.captures?T(L"　占領できる",L"   captures"):L"";
                if(info.rangeMax>1)more+=Fmt(T(L"　射程{0}〜{1}",L"   range {0}-{1}"),{std::to_wstring(info.rangeMin),std::to_wstring(info.rangeMax)});
                Text(g,Fmt(T(L"攻撃 対人{0}・対装甲{1}・対空{5}　防御 {2}　移動 {3}{4}",L"Attack {0} on men, {1} on armour, {5} on aircraft   Defence {2}   Move {3}{4}"),
                           {std::to_wstring(info.attack),std::to_wstring(info.attackArmour),std::to_wstring(info.defence),std::to_wstring(info.move),more,std::to_wstring(info.attackAir)}),
                     RectF(tx,box.Y+40*s,tw,22*s),15*s,gold);
                Wrapped(g,KindAbout(k),RectF(tx,box.Y+64*s,tw,box.Height-68*s),14*s,soft);
                return;
            }
            if(state.aiming)
            {
                Text(g,T(L"砲撃支援：砲撃する敵の部隊をクリック（Escでやめる）",L"Artillery strike: click the enemy unit to shell (Esc: no)"),RectF(lx,box.Y+10*s,lw,26*s),18*s,gold,true);
                if(shown&&shown->who==Her)
                {
                    const double chance=t.StrikeChance(shown->id);
                    Text(g,Fmt(T(L"{0} {1}：命中率 {2}%　倒せるのは約{3}",L"{0} {1}: hit {2}%   about {3} destroyed"),
                               {KindCode(shown->kind),KindName(shown->kind),std::to_wstring(static_cast<int>(std::lround(chance*100))),
                                std::to_wstring(static_cast<int>(std::lround(std::min<double>(shown->count,10*chance))))}),
                         RectF(lx,box.Y+42*s,lw,24*s),16*s,white);
                }
                return;
            }
            if(acting&&!shown&&state.hover>=0)
            {
                const auto spots=t.BridgeSpots(acting->id);
                if(std::find(spots.begin(),spots.end(),state.hover)!=spots.end())
                {
                    Text(g,T(L"橋を架ける",L"Build a bridge"),RectF(lx,box.Y+10*s,lw,26*s),18*s,gold,true);
                    Wrapped(g,T(L"クリックで、この川のマスに橋を架けます（この部隊の行動は終わり）。橋は両軍とも渡れます。",
                                L"Click to bridge this river hex (the unit is then done). Both sides may cross it."),RectF(lx,box.Y+40*s,lw,box.Height-46*s),15*s,white);
                    return;
                }
            }
            if(acting&&shown&&shown->who==Her)
            {
                const auto foes=t.Targets(acting->id);
                if(std::find(foes.begin(),foes.end(),shown->id)!=foes.end())
                {
                    // The battle it would give.
                    const HexWar::Battle b=t.Preview(acting->id,shown->id);
                    Text(g,Fmt(T(L"攻撃の予測：{0} {1} → {2} {3}",L"Attack: {0} {1} -> {2} {3}"),{KindCode(acting->kind),KindName(acting->kind),KindCode(shown->kind),KindName(shown->kind)}),
                         RectF(lx,box.Y+10*s,lw,26*s),18*s,gold,true);
                    Text(g,Fmt(T(L"命中率 {0}%　与える損害 約{1}　受ける損害 約{2}",L"Hit {0}%   you destroy about {1}   you lose about {2}"),
                               {std::to_wstring(static_cast<int>(std::lround(b.chance[0]*100))),std::to_wstring(static_cast<int>(std::lround(b.expected[0]))),std::to_wstring(static_cast<int>(std::lround(b.expected[1])))}),
                         RectF(lx,box.Y+40*s,lw,24*s),16*s,white);
                    std::wstring notes=Fmt(T(L"攻撃力 {0}　相手の防御力 {1}（地形 {2}%）",L"Attack {0}   its defence {1} (terrain {2}%)"),
                                           {std::to_wstring(static_cast<int>(std::lround(b.attack[0]*40))),std::to_wstring(static_cast<int>(std::lround(b.defence[1]*40))),std::to_wstring(static_cast<int>(std::lround(b.cover[1]*100)))});
                    if(b.surrounded[1])notes+=T(L"　相手を包囲",L"   it is surrounded");
                    if(b.surrounded[0])notes+=T(L"　こちらが包囲されている",L"   you are surrounded");
                    if(b.support>0)notes+=Fmt(T(L"　支援射撃 +{0}",L"   supporting fire +{0}"),{std::to_wstring(b.support)});
                    Text(g,notes,RectF(lx,box.Y+68*s,lw,22*s),14*s,soft);
                    Text(g,T(L"クリックで攻撃",L"Click to attack"),RectF(lx,box.Y+92*s,lw,22*s),14*s,gold);
                    return;
                }
            }
            if(shown)
            {
                const auto& k=HexWar::Info(shown->kind);
                const float icon=std::min(l.panel-24*s,84*s);
                FillRound(g,RectF(lx,box.Y+12*s,icon,icon),8*s,Color(255,36,44,76));
                DrawSprite(g,PointF(lx+icon/2,box.Y+12*s+icon*0.55f),icon*0.36f,shown->kind,shown->who,static_cast<float>(t.Facing(shown->who)),false);
                const float tx=lx+icon+14*s,tw=lw-icon-14*s;
                Text(g,KindCode(shown->kind)+L" "+KindName(shown->kind)+L"　"+SideName(shown->who),RectF(tx,box.Y+10*s,tw,26*s),19*s,white,true);
                std::wstring stars;
                for(int i=0;i<HexWar::Level(shown->exp);++i)stars+=L"★";
                Text(g,Fmt(T(L"数 {0}/10　EXP {1} {2}",L"Members {0}/10   EXP {1} {2}"),{std::to_wstring(shown->count),std::to_wstring(shown->exp),stars}),RectF(tx,box.Y+40*s,tw,22*s),16*s,gold);
                std::wstring more=k.captures?T(L"　占領できる",L"   captures"):L"";
                if(k.rangeMax>1)more+=Fmt(T(L"　射程{0}〜{1}（動いたら撃てない・反撃されない）",L"   range {0}-{1} (not after moving; never fired back at)"),{std::to_wstring(k.rangeMin),std::to_wstring(k.rangeMax)});
                Text(g,Fmt(T(L"攻撃 対人{0}・対装甲{1}・対空{5}　防御 {2}　移動 {3}{4}",L"Attack {0} on men, {1} on armour, {5} on aircraft   Defence {2}   Move {3}{4}"),
                           {std::to_wstring(k.attack),std::to_wstring(k.attackArmour),std::to_wstring(k.defence),std::to_wstring(k.move),more,std::to_wstring(k.attackAir)}),
                     RectF(tx,box.Y+66*s,tw,22*s),15*s,soft);
                const Terrain ground=t.At(shown->at);
                Text(g,Fmt(T(L"地形 {0}（防御 +{1}%）{2}",L"On {0} (defence +{1}%){2}"),
                           {TerrainName(ground),std::to_wstring(static_cast<int>(std::lround(HexWar::Cover(ground,shown->kind)*100))),
                            shown->done&&shown->who==You&&t.Now()==Phase::YourTurn?T(L"　行動済み",L"   done"):L""}),
                     RectF(tx,box.Y+90*s,tw,22*s),15*s,soft);
                return;
            }
            if(state.hover>=0&&t.FactoryAt(state.hover)>=0)
            {
                const auto& fa=t.Factories()[static_cast<size_t>(t.FactoryAt(state.hover))];
                const std::wstring whose=fa.owner<0?std::wstring(T(L"中立",L"no one's")):fa.owner==You?std::wstring(T(L"あなたの",L"yours")):HerName()+T(L"の",L"'s");
                Text(g,std::wstring(T(L"工場",L"Factory"))+L"（"+whose+L"）",RectF(lx,box.Y+12*s,lw,28*s),20*s,white,true);
                std::wstring about=fa.building>=0?Fmt(T(L"生産中：{0} {1}（あと{2}ターン）",L"Building: {0} {1} ({2} turns left)"),{KindCode(static_cast<Kind>(fa.building)),KindName(static_cast<Kind>(fa.building)),std::to_wstring(fa.left)}):
                                   std::wstring(T(L"歩兵・対戦車兵が入ると占領。自分の工場では部隊を生産でき、上の部隊はターンの始めに兵が戻る。",L"Infantry takes it by moving onto it. Your own builds units, and mends the one on it each turn."));
                if(fa.owner==You&&fa.building<0)about+=T(L"　クリック（部隊がいるときはB）で生産。",L"  Click (B with a unit on it) to build.");
                Wrapped(g,about,RectF(lx,box.Y+44*s,lw,box.Height-50*s),15*s,soft);
                return;
            }
            if(state.hover>=0)
            {
                const Terrain ground=t.At(state.hover);
                Text(g,TerrainName(ground),RectF(lx,box.Y+12*s,lw,28*s),20*s,white,true);
                Text(g,Fmt(T(L"防御 +{0}%　戦車：{1}　歩兵：{2}",L"Defence +{0}%   tank: {1}   infantry: {2}"),
                           {std::to_wstring(static_cast<int>(std::lround(HexWar::Cover(ground)*100))),
                            HexWar::Info(Kind::Tank).cost[static_cast<size_t>(ground)]?Fmt(T(L"移動 {0}",L"costs {0}"),{std::to_wstring(HexWar::Info(Kind::Tank).cost[static_cast<size_t>(ground)])}):std::wstring(T(L"入れない",L"cannot enter")),
                            HexWar::Info(Kind::Infantry).cost[static_cast<size_t>(ground)]?Fmt(T(L"移動 {0}",L"costs {0}"),{std::to_wstring(HexWar::Info(Kind::Infantry).cost[static_cast<size_t>(ground)])}):std::wstring(T(L"入れない",L"cannot enter"))}),
                     RectF(lx,box.Y+44*s,lw,24*s),15*s,soft);
                return;
            }
            Wrapped(g,T(L"自分のユニットをクリックし、動かす先をクリックします。隣の敵をクリックすると攻撃します（その場からでも）。右クリックかEscで移動を取り消し。全部動かしたら「ターン終了」。ホイールで地図をスクロール。",
                        L"Click a unit of yours, then where it goes; click an enemy next to it to attack (from where it stands too). Right click or Esc takes a move back. \"End turn\" when done. The wheel scrolls the map."),
                    RectF(lx,box.Y+12*s,lw,box.Height-20*s),15*s,soft);
        }

        void PaintNow(HDC dc,const RECT& dirty);
        void Paint(HDC dc,const RECT& dirty)
        {
            const auto from=std::chrono::steady_clock::now();
            PaintNow(dc,dirty);
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-from).count();
            static ULONGLONG told=0;
            if(ms>=40&&GetTickCount64()-told>2000)
            {
                told=GetTickCount64();
                Log(L"hexwar: slow frame %.0f ms (map %d, %s, scroll %.0f,%.0f)",ms,state.table.MapIndex(),
                    state.choosing?L"choosing":state.fight.on?L"battle":L"map",state.scrollX,state.scrollY);
            }
        }
        void PaintNow(HDC dc,const RECT& dirty)
        {
            RECT client{};
            GetClientRect(state.window,&client);
            const int cw=std::max(1L,client.right),ch=std::max(1L,client.bottom);
            if(!state.canvas||static_cast<int>(state.canvas->GetWidth())!=cw||static_cast<int>(state.canvas->GetHeight())!=ch)
                state.canvas=std::make_unique<Bitmap>(cw,ch,PixelFormat32bppPARGB);
            Bitmap& canvas=*state.canvas;
            Graphics g(&canvas);
            const bool hud=state.renderer!=nullptr;
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(hud?TextRenderingHintAntiAlias:TextRenderingHintAntiAliasGridFit);
            const float s=Scale(),W=static_cast<float>(cw),H=static_cast<float>(ch);
            const Layout l=Place(W,H,s);
            state.mapArea=l.map;
            state.radius=l.r;
            const float mid=l.area/2;
            auto& t=state.table;
            const Color white(255,240,240,236),soft(255,206,196,186),gold(255,240,206,120),panel(185,22,26,52),edge(220,150,140,190);
            // The map's side is dark; her column shows the 3D room with her in it (in 2D, a
            // dark column with her face).
            g.Clear(hud?Color(0,0,0,0):Color(255,18,22,40));
            {
                SolidBrush fill(Color(255,30,36,44));
                g.FillRectangle(&fill,0.0f,0.0f,l.area,H);
                LinearGradientBrush shade(PointF(l.area-1,0),PointF(l.area+14*s,0),Color(150,0,0,0),Color(0,0,0,0));
                g.FillRectangle(&shade,l.area-1,0.0f,15*s,H);
            }

            // Her: her name and whose turn, at the top of her column.
            const float hx=l.area+10*s,hw=l.her-20*s;
            {
                FillRound(g,RectF(hx,12*s,hw,70*s),10*s,panel,edge,1.2f*s);
                Text(g,HerName(),RectF(hx+12*s,18*s,hw-20*s,28*s),20*s,white,true);
                Text(g,t.Now()==Phase::HerTurn?T(L"作戦中…",L"Making her moves..."):t.Now()==Phase::YourTurn?T(L"あなたの番",L"Your turn"):L"",
                     RectF(hx+12*s,48*s,hw-20*s,26*s),17*s,t.Now()==Phase::HerTurn?gold:soft);
            }
            if(!hud)
            {
                const float side=std::min(hw,240*s);
                const RectF faceBox(hx+(hw-side)/2,96*s,side,side);
                FillRound(g,faceBox,10*s,Color(255,24,30,60),Color(255,120,130,170),1.5f*s);
                if(state.face)g.DrawImage(state.face.get(),faceBox.X+4*s,faceBox.Y+4*s,faceBox.Width-8*s,faceBox.Height-8*s);
                else Text(g,HerName().substr(0,1),faceBox,side*0.45f,white,true,StringAlignmentCenter,StringAlignmentCenter);
            }
            // Her words: a bubble in her column, below her head (below her face in 2D).
            if(!state.words.text.empty())
            {
                const std::wstring& said=state.words.text;
                const WordsBubble bubble(g,said,s,std::max(120*s,hw-10*s));
                const float bw=bubble.Width(),bh=bubble.Height();
                const float top=hud&&state.headScreen.y>=0?static_cast<float>(state.headScreen.y)+0.16f*H:96*s+std::min(hw,240*s)+12*s;
                const float x=std::clamp(hx+(hw-bw)/2,l.area+4*s,W-6*s-bw),y=std::clamp(top,90*s,std::max(90*s,H-130*s-bh));
                const std::wstring shown=said.substr(0,std::min(said.size(),std::max<size_t>(1,static_cast<size_t>(state.words.shown))));
                // The tail toward her mouth: a little below her head in 3D, low on her face in 2D.
                const float side=std::min(hw,240*s);
                const PointF mouth=hud&&state.headScreen.y>=0?PointF(static_cast<float>(state.headScreen.x),static_cast<float>(state.headScreen.y)+0.035f*H):
                                                             PointF(hx+hw/2,96*s+side*0.74f);
                bubble.DrawPointing(g,x,y,mouth.X,mouth.Y,shown);
            }

            if(state.choosing)PaintChoice(g,l,s);
            else if(state.fight.on)PaintFight(g,l,s);
            else
            {
                // The map, clipped to its area. Only the part of its picture in sight is copied,
                // pixel for pixel (the whole of a large map's picture, clipped, cost more).
                const GraphicsState saved=g.Save();
                g.SetClip(l.map);
                {
                    Bitmap& ground=Ground(l);
                    const int x0=std::max(0,static_cast<int>(l.map.X-l.ox)),y0=std::max(0,static_cast<int>(l.map.Y-l.oy));
                    const int x1=std::min(static_cast<int>(ground.GetWidth()),static_cast<int>(std::ceil(l.map.GetRight()-l.ox)));
                    const int y1=std::min(static_cast<int>(ground.GetHeight()),static_cast<int>(std::ceil(l.map.GetBottom()-l.oy)));
                    if(x1>x0&&y1>y0)
                    {
                        const GraphicsState copy=g.Save();
                        g.SetInterpolationMode(InterpolationModeNearestNeighbor);
                        g.SetPixelOffsetMode(PixelOffsetModeHalf);
                        g.DrawImage(&ground,Rect(static_cast<int>(l.ox)+x0,static_cast<int>(l.oy)+y0,x1-x0,y1-y0),x0,y0,x1-x0,y1-y0,UnitPixel);
                        g.Restore(copy);
                    }
                }
                const float r=l.r;
                const HexWar::Unit* chosen=Chosen();
                const HexWar::Unit* acting=Acting();
                // The chosen unit's reach (yours bright, hers red) and whom it may attack.
                std::vector<int> foes;
                if(chosen&&t.Now()!=Phase::GameOver&&!Showing())
                {
                    std::vector<HexWar::Route> reach;
                    if(acting)reach=t.Reach(acting->id);
                    else if(chosen->who==Her)reach=t.Range(chosen->id);   // hers: where it could go on its turn
                    if(acting)foes=t.Targets(acting->id);
                    // Everything it can't go to or strike is dimmed, so the way it may take stands out
                    // even on a busy picture (the ruined city).
                    {
                        GraphicsPath open;
                        HexPath(open,HexAt(l,chosen->at),r);
                        for(const auto& q:reach)HexPath(open,HexAt(l,q.to),r);
                        for(int id:foes)if(const HexWar::Unit* e=t.UnitById(id))HexPath(open,HexAt(l,e->at),r);
                        if(acting)for(int h:t.BridgeSpots(acting->id))HexPath(open,HexAt(l,h),r);
                        open.SetFillMode(FillModeWinding);
                        const GraphicsState dim=g.Save();
                        g.SetClip(&open,CombineModeExclude);
                        SolidBrush shade(Color(120,8,10,20));
                        g.FillRectangle(&shade,l.map);
                        g.Restore(dim);
                    }
                    for(const auto& q:reach)
                        if(q.to!=chosen->at)FillHex(g,HexAt(l,q.to),r*0.94f,chosen->who==You?Color(70,255,255,230):Color(70,255,90,80));
                    if(acting)
                    {
                        for(int h:t.BridgeSpots(acting->id))
                        {
                            FillHex(g,HexAt(l,h),r*0.9f,Color(90,255,220,120));
                            EdgeHex(g,HexAt(l,h),r*0.9f,Color(255,255,210,90),2.5f*s);
                        }
                    }
                    EdgeHex(g,HexAt(l,chosen->at),r*0.9f,Color(255,255,240,140),3*s);
                }
                for(int id:foes)
                    if(const HexWar::Unit* e=t.UnitById(id))EdgeHex(g,HexAt(l,e->at),r*0.9f,Color(255,255,70,50),3.5f*s);
                if(state.hover>=0&&YourTurnNow())EdgeHex(g,HexAt(l,state.hover),r*0.96f,Color(200,255,255,200),2*s);
                if(state.cursor>=0&&state.hover<0&&YourTurnNow())EdgeHex(g,HexAt(l,state.cursor),r*0.96f,gold,2.5f*s);
                // The bridges built: a deck of planks across the hex, its rails at the sides.
                for(int h:t.Bridges())
                {
                    const PointF c=HexAt(l,h);
                    const RectF plank(c.X-0.85f*r,c.Y-0.36f*r,1.7f*r,0.72f*r);
                    FillRound(g,plank,0.08f*r,Color(255,150,108,66));
                    Pen seam(Color(255,96,66,40),std::max(1.0f,0.04f*r));
                    for(int i=1;i<7;++i){const float x=plank.X+plank.Width*i/7;g.DrawLine(&seam,x,plank.Y,x,plank.GetBottom());}
                    Pen rail(Color(255,80,54,34),std::max(1.5f,0.08f*r));
                    g.DrawLine(&rail,plank.X,plank.Y,plank.GetRight(),plank.Y);
                    g.DrawLine(&rail,plank.X,plank.GetBottom(),plank.GetRight(),plank.GetBottom());
                }
                // The factories: the building on a ring of its side's colour (grey: no one's), and
                // what it builds with the turns left.
                for(const auto& fa:t.Factories())
                {
                    const PointF c=HexAt(l,fa.hex);
                    const Color side=fa.owner<0?Color(255,150,150,150):SideColor(fa.owner);
                    SolidBrush ring(Color(170,side.GetR(),side.GetG(),side.GetB()));
                    g.FillEllipse(&ring,c.X-0.75f*r,c.Y+0.05f*r,1.5f*r,0.62f*r);
                    if(Bitmap* building=Art(L"FACTORY"))
                    {
                        const float h=1.2f*r,w=h*building->GetWidth()/building->GetHeight();
                        g.DrawImage(building,RectF(c.X-w/2,c.Y+0.45f*r-h,w,h));
                        // Smoke from its top, puffs rising, spreading and drifting as they fade.
                        const PointF stack(c.X+0.12f*w,c.Y+0.45f*r-h*0.92f);
                        for(int k=0;k<4;++k)
                        {
                            const float cycle=static_cast<float>(std::fmod(Seconds()/2.4+k/4.0+fa.hex*0.37,1.0));
                            const float size=(0.18f+0.4f*cycle)*r;
                            SolidBrush smoke(Color(static_cast<BYTE>(150*(1-cycle)*std::min(1.0f,cycle*6)),200,200,204));
                            g.FillEllipse(&smoke,stack.X+0.35f*r*cycle+0.06f*r*std::sin(cycle*9+k)-size/2,stack.Y-1.1f*r*cycle-size/2,size,size*0.85f);
                        }
                    }
                    if(fa.building>=0)
                    {
                        const std::wstring what=KindCode(static_cast<Kind>(fa.building))+L" "+Fmt(T(L"あと{0}",L"{0} left"),{std::to_wstring(fa.left)});
                        const float tw=TextWidth(g,what,0.26f*r,true)+0.3f*r;
                        const RectF tag(c.X-tw/2,c.Y-1.05f*r,tw,0.4f*r);
                        FillRound(g,tag,0.1f*r,Color(230,16,16,24),side,std::max(1.0f,0.03f*r));
                        Text(g,what,tag,0.26f*r,Color(255,255,255,255),true,StringAlignmentCenter,StringAlignmentCenter);
                    }
                }
                // The units: one gliding drawn on its way; those in a battle still being shown
                // with their members before it (one destroyed in it still there).
                const bool before=state.fightNext;
                const HexWar::Battle& b=state.pending;
                for(const auto& u:t.Units())
                {
                    PointF at=HexAt(l,u.at);
                    if(state.glide.unit==u.id&&state.glide.path.size()>1)
                    {
                        const double k=std::clamp((Seconds()-state.glide.start)/HexGlide,0.0,static_cast<double>(state.glide.path.size()-1));
                        const size_t i=std::min(static_cast<size_t>(k),state.glide.path.size()-2);
                        const float f=static_cast<float>(k-i);
                        const PointF p0=HexAt(l,state.glide.path[i]),p1=HexAt(l,state.glide.path[i+1]);
                        at=PointF(p0.X+(p1.X-p0.X)*f,p0.Y+(p1.Y-p0.Y)*f);
                    }
                    int count=u.count,exp=u.exp;
                    if(before)for(int k=0;k<2;++k)if((k==0?b.attacker:b.defender)==u.id){count=b.before[k];exp=b.exp[k];}
                    // Darker once done, in its own side's turn.
                    const bool done=u.done&&t.Now()==(u.who==You?Phase::YourTurn:Phase::HerTurn);
                    DrawUnit(g,at,r,u.kind,u.who,count,exp,done);
                }
                if(before)
                    for(int k=0;k<2;++k)
                        if(b.after[k]==0)DrawUnit(g,HexAt(l,b.at[k]),r,b.kinds[k],b.who[k],b.before[k],b.exp[k],false);
                // A strike: the unit (gone, or its members before) while the shells fall on it.
                if(state.order.on&&state.order.a.skill==static_cast<int>(HexWar::Skill::Strike))
                {
                    const auto& a=state.order.a;
                    const double since=Seconds()-state.order.start;
                    const PointF at=HexAt(l,a.strikeHex);
                    if(a.strikeAfter==0&&since<1.1)DrawUnit(g,at,r,a.strikeKind,a.who^1,a.strikeBefore,0,false);
                    for(int i=0;i<7;++i)
                    {
                        const double k=(since-0.3-0.11*i)/0.45;
                        if(k<0||k>1)continue;
                        const float dx=static_cast<float>(((i*37)%11)-5)/5*0.45f*r,dy=static_cast<float>(((i*53)%9)-4)/4*0.35f*r,size=r*(0.5f+0.9f*static_cast<float>(k));
                        SolidBrush burst(Color(static_cast<BYTE>(230*(1-k)),255,170,60)),core(Color(static_cast<BYTE>(240*(1-k)),255,250,220));
                        g.FillEllipse(&burst,at.X+dx-size/2,at.Y+dy-size/2,size,size);
                        g.FillEllipse(&core,at.X+dx-size/4,at.Y+dy-size/4,size/2,size/2);
                    }
                }
                g.Restore(saved);
                // Scroll bars, when the map is larger than its area.
                if(!l.fitsX||!l.fitsY)
                {
                    SolidBrush bar(Color(140,240,240,240));
                    if(!l.fitsX){const float span=l.map.Width/(l.mw+20*s);g.FillRectangle(&bar,l.map.Width*(state.scrollX/(l.mw+20*s)),l.map.GetBottom()-6*s,l.map.Width*span,4*s);}
                    if(!l.fitsY){const float span=l.map.Height/(l.mh+20*s);g.FillRectangle(&bar,l.map.GetRight()-6*s,l.map.Height*(state.scrollY/(l.mh+20*s)),4*s,l.map.Height*span);}
                }
                PaintPanel(g,l,s);
                // Whose turn it is, a band across the map's middle fading in and out.
                if(const double since=Seconds()-state.bannerAt;since>=0&&since<BannerShown&&!state.banner.empty())
                {
                    const float fade=static_cast<float>(std::min({1.0,since/0.2,(BannerShown-since)/0.35}));
                    const bool yours=state.bannerYours;
                    // (An order's near the top, clear of the unit it may be about.)
                    const float bh=86*s,by=state.order.on?l.map.Y+70*s:l.map.Y+l.map.Height/2-bh/2;
                    LinearGradientBrush band(PointF(0,by),PointF(0,by+bh),Color(static_cast<BYTE>(215*fade),yours?18:70,yours?40:16,yours?96:20),
                                             Color(static_cast<BYTE>(215*fade),yours?8:40,yours?18:8,yours?50:10));
                    g.FillRectangle(&band,l.map.X,by,l.map.Width,bh);
                    Pen rule(Color(static_cast<BYTE>(255*fade),210,176,60),2.5f*s);
                    g.DrawLine(&rule,l.map.X,by,l.map.GetRight(),by);
                    g.DrawLine(&rule,l.map.X,by+bh,l.map.GetRight(),by+bh);
                    const float slide=(1-std::min(1.0f,static_cast<float>(since/0.3)))*60*s;
                    Text(g,state.banner,RectF(l.map.X+slide,by,l.map.Width,bh*0.66f),40*s,Color(static_cast<BYTE>(255*fade),255,236,170),true,StringAlignmentCenter,StringAlignmentCenter);
                    Text(g,state.bannerSub,RectF(l.map.X+slide,by+bh*0.6f,l.map.Width,bh*0.36f),17*s,Color(static_cast<BYTE>(230*fade),220,220,230),false,StringAlignmentCenter,StringAlignmentCenter);
                }
                // The map's name at the top left.
                {
                    const auto& m=HexWar::Map(t.MapIndex());
                    const std::wstring name=Fmt(T(L"マップ{0}　{1}",L"Map {0}   {1}"),{std::to_wstring(t.MapIndex()+1),English()?m.nameEn:m.nameJa});
                    const float w=TextWidth(g,name,15*s,true)+20*s;
                    FillRound(g,RectF(8*s,8*s,w,28*s),6*s,Color(190,16,20,40),Color(200,210,176,60),1.2f*s);
                    Text(g,name,RectF(8*s,8*s,w,28*s),15*s,white,true,StringAlignmentCenter,StringAlignmentCenter);
                    // The orders in force, under it.
                    float x=8*s;
                    for(int who:{You,Her})
                        for(auto k:{HexWar::Skill::Barrage,HexWar::Skill::Smoke,HexWar::Skill::March})
                        {
                            if(!t.InForce(who,k))continue;
                            const std::wstring label=(who==You?L"":HerName()+L"：")+SkillName(k);
                            const float bw=TextWidth(g,label,13*s,true)+18*s;
                            FillRound(g,RectF(x,42*s,bw,24*s),12*s,Color(220,SideColor(who,1).GetR(),SideColor(who,1).GetG(),SideColor(who,1).GetB()),SideColor(who,2),1.2f*s);
                            Text(g,label,RectF(x,42*s,bw,24*s),13*s,white,true,StringAlignmentCenter,StringAlignmentCenter);
                            x+=bw+6*s;
                        }
                }
                // What to build, over the map above the orders' button.
                if(state.buildMenu>=0)
                {
                    const size_t n=state.buildKinds.size();
                    const float mw=std::min(420*s,l.area-24*s),rh=46*s,mh=rh*n+52*s;
                    const RectF menu(std::max(8*s,state.orderButton.GetRight()-mw),std::max(l.map.Y+56*s,l.map.GetBottom()-mh-60*s-0.25f*l.map.Height),mw,mh);
                    FillRound(g,menu,10*s,Color(245,18,24,52),Color(255,210,176,60),2*s);
                    Text(g,T(L"工場で生産する部隊",L"Build at the factory"),RectF(menu.X+16*s,menu.Y+10*s,menu.Width-32*s,26*s),18*s,gold,true);
                    for(size_t i=0;i<n;++i)
                    {
                        const Kind k=state.buildKinds[i];
                        const RectF row(menu.X+8*s,menu.Y+44*s+i*rh,menu.Width-16*s,rh-4*s);
                        state.buildRows[i]=row;
                        const bool can=t.CanBuild(You,state.buildMenu,k);
                        const bool over=state.hoverPoint.X>=row.X&&state.hoverPoint.X<=row.GetRight()&&state.hoverPoint.Y>=row.Y&&state.hoverPoint.Y<=row.GetBottom();
                        if(can&&over)FillRound(g,row,6*s,Color(255,46,82,150));
                        const Color c=can?white:Color(255,110,112,130);
                        DrawSprite(g,PointF(row.X+28*s,row.Y+rh*0.45f),12*s,k,You,1,false);
                        Text(g,std::to_wstring(i+1)+L"  "+KindCode(k)+L" "+KindName(k),RectF(row.X+56*s,row.Y,row.Width-64*s,row.Height),17*s,c,true,StringAlignmentNear,StringAlignmentCenter);
                        Text(g,Fmt(T(L"{0}ターン",L"{0} turns"),{std::to_wstring(HexWar::BuildTurns(k))}),RectF(row.X+8*s,row.Y,row.Width-16*s,row.Height),15*s,can?gold:c,true,StringAlignmentFar,StringAlignmentCenter);
                    }
                }
                // The orders' menu, over the map above its button.
                if(state.orderMenu)
                {
                    const float mw=std::min(420*s,l.area-24*s),rh=56*s,mh=rh*HexWar::Skills+16*s;
                    const RectF menu(std::max(8*s,state.orderButton.GetRight()-mw),state.orderButton.Y-mh-8*s,mw,mh);
                    FillRound(g,menu,10*s,Color(245,18,24,52),Color(255,210,176,60),2*s);
                    for(int i=0;i<HexWar::Skills;++i)
                    {
                        const auto k=static_cast<HexWar::Skill>(i);
                        const RectF row(menu.X+8*s,menu.Y+8*s+i*rh,menu.Width-16*s,rh-4*s);
                        state.orderRows[i]=row;
                        const bool can=t.CanCommand(You,k);
                        const bool over=state.hoverPoint.X>=row.X&&state.hoverPoint.X<=row.GetRight()&&state.hoverPoint.Y>=row.Y&&state.hoverPoint.Y<=row.GetBottom();
                        if(can&&over)FillRound(g,row,6*s,Color(255,46,82,150));
                        const Color c=can?white:Color(255,110,112,130);
                        Text(g,std::to_wstring(i+1)+L"  "+SkillName(k),RectF(row.X+8*s,row.Y+3*s,row.Width-16*s,24*s),17*s,c,true);
                        Text(g,std::to_wstring(HexWar::SkillCost(k)),RectF(row.X+8*s,row.Y+3*s,row.Width-16*s,24*s),15*s,can?gold:c,true,StringAlignmentFar);
                        Text(g,SkillAbout(k),RectF(row.X+26*s,row.Y+28*s,row.Width-34*s,20*s),13*s,can?Color(255,200,204,220):c);
                    }
                }
            }
            if(!state.said.empty())
            {
                const float tw=TextWidth(g,state.said,24*s,true)+40*s;
                const RectF bubble(mid-tw/2,H*0.40f,tw,50*s);
                FillRound(g,bubble,14*s,Color(245,255,252,240),Color(255,230,120,60),2*s);
                Text(g,state.said,bubble,24*s,Color(255,200,40,40),true,StringAlignmentCenter,StringAlignmentCenter);
            }

            if(EndShown())PaintEnd(g,l.area,H,s);
            if(state.confirmResign)
            {
                SolidBrush shade(Color(160,0,0,0));
                g.FillRectangle(&shade,0.0f,0.0f,W,H);
                const RectF box(mid-220*s,ch/2.0f-70*s,440*s,140*s);
                FillRound(g,box,14*s,Color(250,22,28,56),Color(255,200,170,90),2*s);
                Text(g,T(L"降参しますか？",L"Give up?"),RectF(box.X,box.Y+22*s,box.Width,36*s),24*s,white,true,StringAlignmentCenter);
                const float gap2=20*s,width=(box.Width-3*gap2)/2;
                const std::wstring labels[2]={T(L"降参する（Y）",L"Give up (Y)"),T(L"続ける（N）",L"Keep playing (N)")};
                for(int i=0;i<2;++i)
                {
                    const RectF button(box.X+gap2+i*(width+gap2),box.Y+74*s,width,44*s);
                    state.quitButtons[i]=button;
                    FillRound(g,button,10*s,i==0?Color(240,150,46,52):Color(240,46,92,150),Color(255,200,170,90),1.5f*s);
                    Text(g,labels[i],RectF(button.X,button.Y+11*s,button.Width,26*s),17*s,white,true,StringAlignmentCenter);
                }
            }

            if(hud)
            {
                ImageRgba8 picture;
                picture.levels.resize(1);
                auto& level=picture.levels[0];
                level.width=static_cast<unsigned>(cw);level.height=static_cast<unsigned>(ch);
                level.pixels.resize(static_cast<size_t>(cw)*ch*4);
                BitmapData bits{};
                Rect whole(0,0,cw,ch);
                if(canvas.LockBits(&whole,ImageLockModeRead,PixelFormat32bppPARGB,&bits)==Ok)
                {
                    for(int y=0;y<ch;++y)
                    {
                        const BYTE* from=static_cast<const BYTE*>(bits.Scan0)+static_cast<size_t>(y)*bits.Stride;
                        BYTE* to=level.pixels.data()+static_cast<size_t>(y)*cw*4;
                        for(int x=0;x<cw;++x,from+=4,to+=4){to[0]=from[2];to[1]=from[1];to[2]=from[0];to[3]=from[3];}
                    }
                    canvas.UnlockBits(&bits);
                }
                state.renderer->SetOverlay(picture);
                return;
            }
            Graphics screen(dc);
            screen.DrawImage(&canvas,static_cast<INT>(dirty.left),static_cast<INT>(dirty.top),static_cast<INT>(dirty.left),static_cast<INT>(dirty.top),
                             static_cast<INT>(dirty.right-dirty.left),static_cast<INT>(dirty.bottom-dirty.top),UnitPixel);
        }

        // The hex under (x, y) in the window, -1 when none.
        int TargetAt(float x,float y)
        {
            if(!state.mapArea.Contains(x,y)||state.radius<=0)return -1;
            RECT client{};
            GetClientRect(state.window,&client);
            const Layout l=Place(static_cast<float>(client.right),static_cast<float>(client.bottom),Scale());
            const float r=l.r;
            const int c0=static_cast<int>(std::floor((x-l.ox-r)/(1.5f*r)+0.5f));
            int best=-1;
            float nearest=r*r;
            for(int c=c0-1;c<=c0+1;++c)
            {
                if(c<0||c>=state.table.Columns())continue;
                const int r0=static_cast<int>(std::floor((y-l.oy)/(Root3*r)-0.5f*(c&1)));
                for(int row=r0-1;row<=r0+1;++row)
                {
                    if(row<0||row>=state.table.Rows())continue;
                    const PointF p=HexAt(l,state.table.Hex(c,row));
                    const float d=(p.X-x)*(p.X-x)+(p.Y-y)*(p.Y-y);
                    if(d<nearest){nearest=d;best=state.table.Hex(c,row);}
                }
            }
            return best;
        }

        // The view scrolled so that a hex is in it.
        void ShowHex(int hex)
        {
            if(!state.window||hex<0)return;
            RECT client{};
            GetClientRect(state.window,&client);
            const Layout l=Place(static_cast<float>(client.right),static_cast<float>(client.bottom),Scale());
            const PointF p=HexAt(l,hex);
            const float margin=2*l.r;
            if(!l.fitsX)
            {
                if(p.X<l.map.X+margin)state.scrollX-=l.map.X+margin-p.X;
                if(p.X>l.map.GetRight()-margin)state.scrollX+=p.X-(l.map.GetRight()-margin);
            }
            if(!l.fitsY)
            {
                if(p.Y<l.map.Y+margin)state.scrollY-=l.map.Y+margin-p.Y;
                if(p.Y>l.map.GetBottom()-margin)state.scrollY+=p.Y-(l.map.GetBottom()-margin);
            }
        }

        // The next unit of yours still to act (Tab).
        void NextUnit()
        {
            const auto& units=state.table.Units();
            const size_t n=units.size();
            size_t from=0;
            for(size_t i=0;i<n;++i)if(units[i].id==state.selected)from=i+1;
            for(size_t k=0;k<n;++k)
            {
                const auto& u=units[(from+k)%n];
                if(u.who==You&&!u.done){LetGo();Choose(u.id);state.cursor=u.at;ShowHex(u.at);return;}
            }
        }

        void OnKey(WPARAM key)
        {
            auto& t=state.table;
            if(key==VK_F1){state.camera={};return;}
            if(state.choosing)
            {
                const int n=Offered();
                if(key==VK_LEFT||key==VK_UP)state.choice=(state.choice+n-1)%n;
                else if(key==VK_RIGHT||key==VK_DOWN)state.choice=(state.choice+1)%n;
                else if(key>='1'&&key<'1'+n)state.choice=static_cast<int>(key-'1');
                else if(key==VK_TAB)state.side=state.side==1?-1:state.side+1;
                else if(key==VK_ESCAPE){Finish();return;}
                if(key==VK_RETURN||key==VK_SPACE)StartMap(state.choice);
                Redraw();
                return;
            }
            if(state.fight.on||state.fightNext){if(key==VK_RETURN||key==VK_SPACE||key==VK_ESCAPE)SkipFight();return;}
            if(state.confirmResign)
            {
                if(key=='Y'){state.confirmResign=false;t.Resign(You);Redraw();}
                else if(key=='N'||key==VK_ESCAPE){state.confirmResign=false;Redraw();}
                return;
            }
            if(t.Now()==Phase::GameOver)
            {
                if((key==VK_RETURN||key==VK_SPACE)&&EndShown())Continue();
                else if(key=='M'&&EndShown()){Continue();state.choosing=true;state.choice=state.map;Redraw();}
                else if(key==VK_ESCAPE)Finish();
                return;
            }
            if(key==VK_ESCAPE)
            {
                if(!Cancel()&&t.Now()==Phase::YourTurn){state.confirmResign=true;Redraw();}
                return;
            }
            if(!YourTurnNow())return;
            if(key=='E'){EndTurn();return;}
            if(state.buildMenu>=0&&key>='1'&&key<'1'+static_cast<int>(state.buildKinds.size())){ChooseBuild(static_cast<size_t>(key-'1'));return;}
            if(key=='B'){const int at=state.hover>=0?state.hover:state.cursor;if(at>=0){LetGo();OpenBuild(at);Redraw();}return;}
            if(key=='S'){if(!t.Commanded(You)){state.orderMenu=!state.orderMenu;state.aiming=false;state.buildMenu=-1;Redraw();}return;}
            if(state.orderMenu&&key>='1'&&key<'1'+HexWar::Skills){ChooseOrder(static_cast<int>(key-'1'));return;}
            if(key==VK_TAB){NextUnit();Redraw();return;}
            if(key==VK_LEFT||key==VK_RIGHT||key==VK_UP||key==VK_DOWN)
            {
                // A hex at a time, from the unit chosen (the middle of the map to start).
                if(state.cursor<0)state.cursor=Chosen()?Chosen()->at:t.Hex(t.Columns()/2,t.Rows()/2);
                else
                {
                    int r=state.cursor/t.Columns(),c=state.cursor%t.Columns();
                    r=std::clamp(r+(key==VK_UP?-1:key==VK_DOWN?1:0),0,t.Rows()-1);
                    c=std::clamp(c+(key==VK_LEFT?-1:key==VK_RIGHT?1:0),0,t.Columns()-1);
                    state.cursor=t.Hex(c,r);
                }
                state.hover=-1;
                ShowHex(state.cursor);
                Redraw();
            }
            else if((key==VK_RETURN||key==VK_SPACE)&&state.cursor>=0)Click(state.cursor);
        }

        LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            switch(message)
            {
            case WM_MOUSEWHEEL:
            {
                // Over the map: it scrolls (Shift: sideways); over her column: the camera.
                POINT at{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
                ScreenToClient(hwnd,&at);
                if(state.mapArea.Contains(static_cast<float>(at.x),static_cast<float>(at.y)))
                {
                    const float step=-GET_WHEEL_DELTA_WPARAM(wParam)/120.0f*state.radius*1.5f;
                    if(wParam&MK_SHIFT)state.scrollX+=step;else state.scrollY+=step;
                    Redraw();
                    return 0;
                }
                if(state.view)return ViewProc(state.view,message,wParam,lParam);
                break;
            }
            case WM_KEYDOWN:
                if(!(lParam&(1<<30)))OnKey(wParam);
                return 0;
            case WM_TIMER:
                Tick();
                return 0;
            case WM_MOUSEMOVE:
            {
                const float x=static_cast<float>(GET_X_LPARAM(lParam)),y=static_cast<float>(GET_Y_LPARAM(lParam));
                if(state.panning)
                {
                    state.scrollX-=x-state.panFrom.x;
                    state.scrollY-=y-state.panFrom.y;
                    state.panFrom={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
                    Redraw();
                    return 0;
                }
                const int at=TargetAt(x,y);
                const bool overEnd=state.endButton.Contains(x,y),overOrder=state.orderButton.Contains(x,y);
                const bool moved=(state.orderMenu||state.buildMenu>=0)&&(state.hoverPoint.X!=x||state.hoverPoint.Y!=y);
                state.hoverPoint=PointF(x,y);
                if(at!=state.hover||overEnd!=state.overEnd||overOrder!=state.overOrder||moved){state.hover=at;state.overEnd=overEnd;state.overOrder=overOrder;Redraw();}
                return 0;
            }
            case WM_MBUTTONDOWN:
                state.panning=true;
                state.panFrom={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
                SetCapture(hwnd);
                return 0;
            case WM_MBUTTONUP:
                if(state.panning){state.panning=false;ReleaseCapture();}
                return 0;
            case WM_CAPTURECHANGED:
                state.panning=false;
                return 0;
            case WM_RBUTTONDOWN:
                if(state.fight.on||state.fightNext){SkipFight();return 0;}
                Cancel();
                return 0;
            case WM_LBUTTONDOWN:
            {
                const float x=static_cast<float>(GET_X_LPARAM(lParam)),y=static_cast<float>(GET_Y_LPARAM(lParam));
                auto& t=state.table;
                if(state.confirmResign)
                {
                    if(state.quitButtons[0].Contains(x,y)){state.confirmResign=false;t.Resign(You);Redraw();}
                    else if(state.quitButtons[1].Contains(x,y)){state.confirmResign=false;Redraw();}
                    return 0;
                }
                if(state.choosing)
                {
                    for(size_t i=0;i<state.cards.size();++i)
                        if(state.cards[i].Contains(x,y)){state.choice=static_cast<int>(i);Redraw();}
                    if(state.startButton.Contains(x,y)){StartMap(state.choice);Redraw();}
                    for(int i=0;i<3;++i)
                        if(state.sideButtons[i].Contains(x,y)){state.side=i-1;Redraw();}
                    return 0;
                }
                if(state.fight.on||state.fightNext){SkipFight();return 0;}
                if(t.Now()==Phase::GameOver){if(EndShown())Continue();return 0;}
                if(state.buildMenu>=0)
                {
                    for(size_t i=0;i<state.buildRows.size();++i)if(state.buildRows[i].Contains(x,y)){ChooseBuild(i);return 0;}
                    state.buildMenu=-1;
                    Redraw();
                    return 0;
                }
                if(state.orderMenu)
                {
                    for(int i=0;i<HexWar::Skills;++i)if(state.orderRows[i].Contains(x,y)){ChooseOrder(i);return 0;}
                    state.orderMenu=false;
                    Redraw();
                    return 0;
                }
                if(state.orderButton.Contains(x,y))
                {
                    if(YourTurnNow()&&!t.Commanded(You)){state.orderMenu=true;state.aiming=false;Redraw();}
                    return 0;
                }
                if(state.aiming)
                {
                    const int hex=TargetAt(x,y);
                    const HexWar::Unit* e=hex>=0?t.UnitAt(hex):nullptr;
                    if(e&&e->who==Her&&YourTurnNow()){state.aiming=false;t.Command(You,HexWar::Skill::Strike,e->id);Redraw();}
                    return 0;
                }
                if(state.endButton.Contains(x,y)){EndTurn();return 0;}
                Click(TargetAt(x,y));
                return 0;
            }
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC dc=BeginPaint(hwnd,&ps);
                // Over the 3D view the 2D layer is drawn with the next frame, once: drawn here,
                // a stream of mouse moves (dragging the map) kept the program painting between
                // them and never getting to show a frame.
                if(state.renderer)state.overlayDue=true;
                else Paint(dc,ps.rcPaint);
                EndPaint(hwnd,&ps);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_SIZE:
                PlaceView();
                InvalidateRect(hwnd,nullptr,FALSE);
                return 0;
            case WM_CLOSE:
                Finish();
                return 0;
            case WM_DESTROY:
                StopMahjongSounds();
                StopHexWarSounds();
                state.window=nullptr;
                ReleaseView();
                state.face.reset();
                state.canvas.reset();
                groundImage.reset();
                return 0;
            }
            return DefWindowProcW(hwnd,message,wParam,lParam);
        }
    }

    bool HexWarWindowOpen(){return state.window!=nullptr;}
    HWND HexWarWindowHandle(){return state.window;}
    bool HexWarWindowRect(RECT& r)
    {
        return state.window&&IsWindowVisible(state.window)&&!IsIconic(state.window)&&GetWindowRect(state.window,&r);
    }

    namespace
    {
        // Diagnostics: turns played for both (yours as she would play them), nothing shown on
        // the way.
        void PlaySimply(int turns)
        {
            auto& t=state.table;
            for(int i=0;i<turns&&t.Now()!=Phase::GameOver;++i)
            {
                if(t.Now()==Phase::YourTurn)t.PlayTurnForYou();
                for(int guard=0;guard<100&&t.Now()==Phase::HerTurn;++guard)t.Step();
            }
            state.nextAt=0;
            state.actionsSeen=t.Actions();
            state.glide={};
            state.fight={};
            state.fightNext=false;
            state.hover=state.cursor=state.selected=-1;
        }
    }

    void HexWarLines(const std::wstring& answer)
    {
        state.prefetching=false;
        if(!state.window)return;
        state.lines.Answered(answer);
        Prefetch();
    }

    bool HexWarAfterword(std::wstring& situation,std::wstring& fallback)
    {
        if(state.afterword.empty())return false;
        situation=std::move(state.afterword);
        fallback=std::move(state.afterwordLine);
        state.afterword.clear();
        state.afterwordLine.clear();
        return true;
    }

    void HexWarHeard(const std::wstring& line,bool done)
    {
        auto& w=state.words;
        if(!w.waiting||!state.window)return;
        if(done&&line.empty()){SayWords(w.fallback,w.weight);return;}
        if(!line.empty())
        {
            w.text=line;
            w.until=GetTickCount64()+static_cast<ULONGLONG>(CharSeconds()*1000*line.size())+2600+90*static_cast<ULONGLONG>(line.size());
            Redraw();
        }
        if(done)
        {
            w.waiting=false;
            if(w.recent.size()>=4)w.recent.erase(w.recent.begin());
            w.recent.push_back(line);
            w.told+=L"  she says (LLM): "+line+L"\n";
        }
    }

    void PlayHexWarTurns(int turns,float yaw)
    {
        if(!state.window)return;
        state.choosing=false;
        PlaySimply(turns);
        // Then your turn ended: hers is played and shown as in a game.
        EndTurn();
        state.camera.yaw+=yaw;
        Redraw();
    }

    namespace
    {
        void SetTableSolids(MascotModel& model,DirectX::FXMMATRIX world)
        {
            using namespace DirectX;
            const auto t=Table3D();
            const float floor=FloorY();
            std::vector<std::array<float,6>> boxes{
                {t.x0,t.top-0.05f*state.upper,t.zFar,t.x1,t.top,t.zNear},
                {t.x0-t.rw,t.frameBottom,t.zFar-t.rw,t.x1+t.rw,t.frameTop,t.zFar},
                {t.x0-t.rw,t.frameBottom,t.zNear,t.x1+t.rw,t.frameTop,t.zNear+t.rw},
                {t.x0-t.rw,t.frameBottom,t.zFar,t.x0,t.frameTop,t.zNear},
                {t.x1,t.frameBottom,t.zFar,t.x1+t.rw,t.frameTop,t.zNear}};
            for(float lx:{t.x0-t.rw+0.02f*state.upper,t.x1+t.rw-0.02f*state.upper-t.leg})
                for(float lz:{t.zFar-t.rw+0.02f*state.upper,t.zNear+t.rw-0.02f*state.upper-t.leg})
                    boxes.push_back({lx,floor,lz,lx+t.leg,t.frameBottom,lz+t.leg});
            XMVECTOR scale,turn,move;
            const XMMATRIX toModel=XMMatrixInverse(nullptr,world);
            XMMatrixDecompose(&scale,&turn,&move,toModel);
            std::vector<MascotModel::Solid> solids;
            for(const auto& b:boxes)
            {
                MascotModel::Solid s;
                const XMVECTOR middle=XMVector3Transform(XMVectorSet((b[0]+b[3])/2,(b[1]+b[4])/2,(b[2]+b[5])/2,0),toModel);
                XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(s.middle.data()),middle);
                s.half={(b[3]-b[0])/2,(b[4]-b[1])/2,(b[5]-b[2])/2};
                XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(s.turn.data()),turn);
                solids.push_back(s);
            }
            model.SetSolids(solids);
        }

    }

    void TickHexWarWindow(MascotModel& model,double dt)
    {
        if(!state.window||!state.renderer)return;
        if(state.overlayDue)
        {
            state.overlayDue=false;
            RECT client{};
            GetClientRect(state.window,&client);
            Paint(nullptr,client);
        }
        auto& r=*state.renderer;
        using namespace DirectX;
        const float D=state.upper,top=Top();
        dt=std::min(dt,0.1);
        auto& t=state.table;
        // The entrance, as at the mahjong table: she walks in, turns to you, sits down and
        // moves in with her chair.
        const float seatZ=SeatZ(),seatHeight=-FloorY(),step=static_cast<float>(dt);
        const float standX=0,standZ=0.25f*D+SeatDepth(),pulled=standZ+0.25f*D+SeatFront(),startX=-1.8f*D,startZ=standZ;
        float lift=0,standing=1,lean=0;
        auto ease=[](float u){return u*u*(3-2*u);};
        if(state.entrance==0)
        {
            model.SetFacingWalk(true);
            model.SetActivity(Activity::Walk,1);
            state.herX=startX;
            state.herZ=startZ;
            state.herAngle=std::atan2(startX-standX,startZ-standZ);
            state.chairZ=pulled;
            state.entrance=1;
        }
        if(state.entrance==1)
        {
            const float dx=standX-state.herX,dz=standZ-state.herZ,left=std::sqrt(dx*dx+dz*dz);
            const float go=std::min(left,std::max(model.GroundSpeed(),0.25f*D)*step);
            if(left>0){state.herX+=dx/left*go;state.herZ+=dz/left*go;}
            lift=-seatHeight;
            if(left-go<=0.001f*D){state.herX=standX;state.herZ=standZ;model.SetActivity(Activity::Idle);state.entrance=2;state.entranceTime=0;}
        }
        else if(state.entrance==2)
        {
            state.herAngle*=std::exp(-step/0.1f);
            lift=-seatHeight;
            if((state.entranceTime+=step)>0.45f)
            {
                model.SetFacingWalk(false);
                if(model.CanSit())model.SetActivity(Activity::Sit,0);
                state.herAngle=0;
                state.entrance=3;
                state.entranceTime=0;
            }
        }
        else if(state.entrance==3)
        {
            const float u=ease(std::min(1.0f,(state.entranceTime+=step)/0.7f));
            lift=-seatHeight*(1-u);
            state.herZ=standZ+(pulled-standZ)*u;
            standing=1-u;
            if(u>=1){state.entrance=4;state.entranceTime=0;}
        }
        else if(state.entrance==4)
        {
            const float tt=std::clamp(((state.entranceTime+=step)-0.45f)/0.8f,0.0f,1.0f),u=ease(tt);
            const float bump=std::sin(tt*XM_PI);
            lift=0.05f*state.hips*bump;
            lean=0.22f*bump;
            standing=0;
            state.herZ=state.chairZ=pulled+(seatZ-pulled)*u;
            if(u>=1)state.entrance=5;
        }
        else if(state.entrance==5){standing=0;state.herX=0;state.herZ=state.chairZ=seatZ;}
        state.clock+=dt;
        if(auto scene=Scene();scene!=state.scene||state.scene.empty()){state.scene=std::move(scene);r.SetGeometry(BuildTable());}
        if(state.thinkFor>0&&state.entrance==5){model.EmoteSeated(L"ponder",state.thinkFor);state.thinkFor=0;}
        {
            const bool over=t.Now()==Phase::GameOver;
            if(over&&!state.seenOver&&t.Winner()>=0)
            {
                std::uniform_real_distribution<double> any(0,1);
                state.endGesture=t.Winner()==Her?(any(state.sparkRandom)<0.5?L"cheeks":L"clap"):L"oops";
                state.endGestureUntil=state.clock+1.5;
            }
            state.seenOver=over;
            if(!state.endGesture.empty()&&state.entrance==5&&(model.EmoteSeated(state.endGesture,3.2)||state.clock>state.endGestureUntil))
                state.endGesture.clear();
        }
        // What moves: her chair.
        r.SetMovingGeometry(BuildChair(state.chairZ));
        {
            std::vector<MascotRenderer::Particle> shown;
            const float fall=0.9f*D;
            for(auto& k:state.sparks)
            {
                k.age+=step;
                for(int a=0;a<3;++a)k.at[a]+=k.velocity[a]*step;
                k.velocity[1]-=fall*step;
                for(int a:{0,2})k.velocity[a]*=std::exp(-step*2.5f);
                k.angle+=k.spin*step;
            }
            state.sparks.erase(std::remove_if(state.sparks.begin(),state.sparks.end(),[](const State::Spark& k){return k.age>=k.life;}),state.sparks.end());
            for(const auto& k:state.sparks)
            {
                const float u=k.age/k.life,fade=(1-u)*std::min(1.0f,u*8);
                MascotRenderer::Particle p;
                p.position=k.at;
                p.size=k.size*(k.glow?1.0f+u:1.0f-0.4f*u);
                p.color={k.color[0],k.color[1],k.color[2],k.color[3]*fade*(k.glow?0.45f:1.0f)};
                p.angle=k.angle;
                p.glow=k.glow;
                shown.push_back(p);
            }
            shown.insert(shown.end(),state.roomGlows.begin(),state.roomGlows.end());
            r.SetParticles(shown);
        }
        const float hip=0.12f*state.hips;
        const XMMATRIX world=XMMatrixTranslation(-model.RootOffset(),-hip,0)*XMMatrixRotationX(-lean)*XMMatrixTranslation(0,hip,0)*XMMatrixRotationY(state.herAngle)*XMMatrixTranslation(state.herX,lift,state.entrance==0?seatZ:state.herZ);
        r.SetModelTransform(world);
        SetTableSolids(model,world);
        // Her upper body in the column at the right of the window (the board is drawn flat
        // over the rest): the camera follows her head from in front of her, set off so that
        // her head is in the middle of the column a third of the way down.
        const auto& c=state.camera;
        {
            std::array<float,3> at{state.herX,top+0.9f*D,state.herZ};
            if(std::array<float,3> head{};model.HeadPosition(head))
            {
                const XMVECTOR h=XMVector3Transform(XMVectorSet(head[0],head[1],head[2],1),world);
                at={XMVectorGetX(h),XMVectorGetY(h),XMVectorGetZ(h)};
            }
            const float k=state.following?1-std::exp(-step/0.25f):1;
            for(int i=0;i<3;++i)state.follow[i]+=(at[i]-state.follow[i])*k;
            state.following=true;
        }
        RECT client{};
        GetClientRect(state.window,&client);
        const float aspect=static_cast<float>(std::max(1L,client.right))/static_cast<float>(std::max(1L,client.bottom));
        const float fov=XMConvertToRadians(34.0f),distance=c.distance*0.9f*D,half=std::tan(fov/2)*distance;
        const XMVECTOR toEye=XMVectorSet(std::sin(c.yaw)*std::cos(c.pitch),std::sin(c.pitch),-std::cos(c.yaw)*std::cos(c.pitch),0);
        const XMVECTOR forward=XMVectorNegate(toEye);
        const XMVECTOR right=XMVector3Normalize(XMVector3Cross(XMVectorSet(0,1,0,0),forward));
        const XMVECTOR up=XMVector3Cross(forward,right);
        const XMVECTOR focus=XMVectorSet(state.follow[0]+c.x*D,state.follow[1]+c.y*D,state.follow[2]+c.z*D,0);
        const XMVECTOR target=XMVectorSubtract(focus,XMVectorAdd(XMVectorScale(right,(1-HerColumn)*half*aspect),XMVectorScale(up,0.3f*half)));
        const XMVECTOR eye=XMVectorAdd(target,XMVectorScale(toEye,distance));
        r.SetCamera(eye,target,fov);
        if(std::array<float,3> head{};model.HeadPosition(head))
        {
            const XMVECTOR at=XMVector3Transform(XMVectorSet(head[0],head[1],head[2],1),world);
            state.headScreen=r.Project({XMVectorGetX(at),XMVectorGetY(at),XMVectorGetZ(at)});
        }
        {
            XMVECTOR det;
            const XMVECTOR you=XMVector3Transform(eye,XMMatrixInverse(&det,world));
            std::array<float,3> head{};
            const bool talking=!state.words.text.empty()&&model.HeadPosition(head);
            model.LookToward({XMVectorGetX(you),head[1],XMVectorGetZ(you)},talking);
        }
        {
            MascotRenderer::PointLight lamp;
            lamp.position[0]=0;lamp.position[1]=top+2.3f*D;lamp.position[2]=FieldZ();
            lamp.radius=4.2f*D;
            lamp.color[0]=0.42f;lamp.color[1]=0.30f;lamp.color[2]=0.16f;
            lamp.onModel=false;
            r.SetLights({lamp});
        }
        {
            // Her mouth moves while her words are being shown (a vowel or two, opening and
            // closing about five times a second), and closes after; with a voice file, with it.
            const auto& w=state.words;
            const bool saying=!w.text.empty()&&w.text!=L"…"&&w.shown<static_cast<double>(w.text.size());
            state.voice.Tick(dt);
            if(state.voice.Playing())model.SetMouth(state.voice.Mouth(),true);
            else if(saying)
            {
                const float u=static_cast<float>(state.clock*10.5);
                const float open=0.25f+0.75f*std::abs(std::sin(u));
                const int vowel=static_cast<int>(state.clock*5.3)%3;
                model.SetMouth({vowel==0?open:0.0f,vowel==1?open*0.6f:0.0f,0,0,vowel==2?open*0.8f:0.0f,0},true);
            }
            else model.SetMouth({},false);
        }
        r.Render(model,state.her.edgeScale);
    }

    namespace
    {
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

    std::wstring DescribeHexWar()
    {
        if(!state.window)return L"";
        auto& t=state.table;
        std::wstring out=Fmt(L"hexwar: entrance {0}, phase {1}, turn {2}, actions {3}, winner {4}, ending {5}, units {6}/{7}, strength {8}/{9}\n",
                             {std::to_wstring(state.entrance),std::to_wstring(static_cast<int>(t.Now())),std::to_wstring(t.Turn()),std::to_wstring(t.Actions()),
                              std::to_wstring(t.Winner()),std::to_wstring(static_cast<int>(t.HowEnded())),std::to_wstring(t.Alive(You)),std::to_wstring(t.Alive(Her)),
                              std::to_wstring(t.Strength(You)),std::to_wstring(t.Strength(Her))});
        return out+(state.words.told.empty()?std::wstring():L"hexwar words:\n"+state.words.told);
    }

    bool SaveHexWarView(const std::wstring& path)
    {
        return state.renderer&&state.renderer->SaveLastFrame(path);
    }

    bool SaveHexWarShot(const std::wstring& path,int turns,std::uint32_t seed,const std::wstring& what,double seconds)
    {
        GdiplusStartupInput input;
        if(!state.gdiplus)GdiplusStartup(&state.gdiplus,&input,nullptr);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc=DefWindowProcW;
        wc.hInstance=GetModuleHandleW(nullptr);
        wc.lpszClassName=L"MikuMikuDesktopPerchHexWarShot";
        RegisterClassExW(&wc);
        const int dpi=static_cast<int>(GetDpiForSystem());
        state.window=CreateWindowExW(0,wc.lpszClassName,L"",WS_OVERLAPPEDWINDOW,0,0,MulDiv(static_cast<int>(BaseWidth)+16,dpi,96),MulDiv(static_cast<int>(BaseHeight)+39,dpi,96),
                                     nullptr,nullptr,wc.hInstance,nullptr);
        state.her=HexWarOpponent{L"ミク"};
        std::wstring show=what;
        state.map=0;
        bool swapped=false,scrolled=false,flying=false;
        if(const size_t colon=show.find(L':');colon!=std::wstring::npos)
        {
            state.map=std::clamp(_wtoi(show.c_str()+colon+1),0,HexWar::Maps()-1);
            swapped=show.find(L'r',colon)!=std::wstring::npos;
            scrolled=show.find(L's',colon)!=std::wstring::npos;   // the map scrolled 333.3 px each way
            flying=show.find(L'f',colon)!=std::wstring::npos;     // an aircraft of yours attacks, when one can
            show.resize(colon);
        }
        state.table.NewGame(state.map,seed,Care,swapped);
        LoadProgress();
        state.scrollX=state.scrollY=scrolled?333.3f:0;
        PlaySimply(turns);
        auto& t=state.table;
        // A unit of yours that could attack now (moved next to one of hers when it can be).
        auto attacker=[&]()->int{
            for(int pass=flying?0:1;pass<2;++pass)
                for(const auto& u:t.Units())
                    if(u.who==You&&(pass||Flies(u.kind)))
                        for(const auto& r:t.Reach(u.id))
                            if(pass==0&&!t.TargetsFrom(u,r.to,true).empty()){t.Move(u.id,r.to);return u.id;}
            for(const auto& u:t.Units())if(u.who==You&&!t.Targets(u.id).empty())return u.id;
            for(const auto& u:t.Units())
                if(u.who==You)
                    for(const auto& r:t.Reach(u.id))
                        if(t.HeldBy(r.to,You)){t.Move(u.id,r.to);return u.id;}
            return -1;
        };
        if(t.Now()==Phase::YourTurn&&show==L"select")
        {
            for(const auto& u:t.Units())if(u.who==You&&!t.Reach(u.id).empty()){state.selected=u.id;break;}
        }
        else if(t.Now()==Phase::YourTurn&&(show==L"aim"||show==L"fight"))
        {
            const int a=attacker();
            state.actionsSeen=t.Actions();
            if(a>=0&&!t.Targets(a).empty())
            {
                const int d=t.Targets(a)[0];
                if(show==L"aim"){state.selected=a;state.hover=t.UnitById(d)->at;}
                else
                {
                    t.Attack(a,d);
                    state.actionsSeen=t.Actions();
                    state.fight={true,t.LastAction().battle,Seconds()-seconds};
                }
            }
        }
        else if(t.Now()==Phase::YourTurn&&show==L"orders")state.orderMenu=true;
        else if(t.Now()==Phase::YourTurn&&show==L"build")
        {
            // (A factory of yours that is idle, when there is one by then.)
            for(const auto& f:t.Factories())if(f.owner==You&&f.building<0){OpenBuild(f.hex);state.hover=f.hex;break;}
        }
        else if(show==L"choose")
        {
            state.choosing=true;
            state.won[0]=1;   // (the second map open, the third not)
            state.choice=1;
        }
        else if(t.Now()==Phase::YourTurn&&show==L"strike")
        {
            // The enemy unit with most members shelled (the gauge filled for it).
            const HexWar::Unit* target=nullptr;
            for(const auto& u:t.Units())if(u.who==Her&&(!target||u.count>target->count))target=&u;
            while(target&&!t.CanCommand(You,HexWar::Skill::Strike)&&t.Gauge(You)<HexWar::GaugeFull)t.FillForTest(You);
            if(target&&t.Command(You,HexWar::Skill::Strike,target->id))
            {
                Watch();
                state.order.start-=seconds;
                state.bannerAt-=seconds;
            }
        }
        else if(show==L"turn")
        {
            state.bannerFor=t.Turn()*2;
            state.banner=T(L"あなたのターン",L"Your turn");
            state.bannerAt=Seconds()-seconds;
        }
        else if(show==L"end")t.Resign(Her);
        state.mapArea={};
        {
            RECT client{};
            GetClientRect(state.window,&client);
            const float s=Scale();
            state.mapArea=Place(static_cast<float>(client.right),static_cast<float>(client.bottom),s).map;
        }
        if(!SampleWords().empty()){state.words.text=SampleWords();state.words.shown=1e9;}
        RECT client{};
        GetClientRect(state.window,&client);
        Bitmap shot(client.right,client.bottom,PixelFormat32bppARGB);
        {
            Graphics g(&shot);
            const HDC dc=g.GetHDC();
            Paint(dc,client);
            // How long a frame takes to draw (ten more), written beside the picture.
            const auto from=std::chrono::steady_clock::now();
            for(int i=0;i<10;++i)Paint(dc,client);
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-from).count()/10;
            if(std::wofstream out(path+L".txt");out)out<<L"paint "<<ms<<L" ms\n";
            g.ReleaseHDC(dc);
        }
        DestroyWindow(state.window);
        state.window=nullptr;
        return SavePng(shot,path);
    }

    void ShowHexWarWindow(HWND owner,const HexWarOpponent& her,std::function<void()> onEnd)
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
        state.face.reset();
        if(her.faceSize>0&&her.face.size()>=static_cast<size_t>(her.faceSize)*her.faceSize*4)
        {
            Bitmap view(her.faceSize,her.faceSize,her.faceSize*4,PixelFormat32bppPARGB,const_cast<BYTE*>(state.her.face.data()));
            state.face.reset(view.Clone(0,0,her.faceSize,her.faceSize,PixelFormat32bppPARGB));
        }
        state.map=0;
        state.following=false;
        LoadProgress();
        state.choosing=true;
        state.choice=0;
        state.lines.Load(L"HEXWARLINES",L"hexwar",her.ownLinesFile,her.linesFile,her.styleKey,English(),
                         {L"ヘックス戦線（ユニットを動かして戦う戦術ゲーム）",L"Hex Front (a war game of moving units)",L"",L""});
        state.voice.Stop();
        state.tell=Expression::Neutral;
        state.prefetching=false;
        state.words={};
        state.words.told=state.lines.Report();
        if(!her.linesFile.empty())ShowLinesProblem(owner,state.lines);   // (none kept: a diagnostic run, which must not stop)
        NewGame();

        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&info);
        const RECT work=info.rcWork;
        const int dpi=static_cast<int>(GetDpiForSystem());
        const int w=std::min(static_cast<int>(work.right-work.left)*9/10,MulDiv(static_cast<int>(BaseWidth)+16,dpi,96));
        const int h=std::min(static_cast<int>(work.bottom-work.top)*9/10,MulDiv(static_cast<int>(BaseHeight)+39,dpi,96));
        const std::wstring title=std::wstring(T(L"ヘックス戦線",L"Hex Front"))+L" - "+HerName();
        state.window=CreateWindowExW(0,ClassName,title.c_str(),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                                     work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,
                                     nullptr,nullptr,instance,nullptr);
        if(CreateView())PlaceView();
        SetTimer(state.window,TimerId,40,nullptr);
        ShowWindow(state.window,SW_SHOWNORMAL);
        SetForegroundWindow(state.window);
    }
}
