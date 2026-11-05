#define NOMINMAX
#include "SolitaireWindow.hpp"
#include "ShownKey.hpp"
#include "Solitaire.hpp"
#include "MahjongSound.hpp"
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
#include <cstdio>
#include <map>
#include <memory>
#include <tuple>
#include <random>
#pragma comment(lib,"gdiplus.lib")
#pragma comment(lib,"shlwapi.lib")

namespace MikuMikuDesktopPerch
{
    namespace
    {
        using namespace Gdiplus;
        using Solitaire::Phase;
        using Solitaire::Mode;
        using Solitaire::Pair;
        constexpr int Tiles=Solitaire::TileCount;
        constexpr wchar_t ClassName[]=L"MikuMikuDesktopPerchSolitaire";
        constexpr wchar_t ViewClassName[]=L"MikuMikuDesktopPerchSolitaireView";
        constexpr UINT_PTR TimerId=1;
        // How carefully she plays (Solitaire::Table::NewGame): a little less as you grow closer,
        // and after games you lost in a row.
        constexpr double Care=0.85,LessPerBond=0.03,LessPerLoss=0.08,LeastCare=0.4;
        // Her pace: before her pair is taken, how long it is shown first, before the end is shown.
        constexpr ULONGLONG HerPlayWait=900,PickShown=500,EndWait=900;
        constexpr ULONGLONG SaidShown=1800;
        constexpr ULONGLONG ReachLead=600;         // her hand starts out this long before her pair goes
        constexpr ULONGLONG ThinkExtra=1800;       // her pair later by this when she stops to think
        constexpr ULONGLONG EndWait3D=2200;        // at the 3D table: the end waits for her reaction
        constexpr ULONGLONG FadeTime=280;          // a pair taken fades in this long
        constexpr ULONGLONG HintShown=5000;
        // Sizes in logical pixels (at 96 dpi, at the size the window opens).
        constexpr float BaseWidth=1180,BaseHeight=840;
        constexpr float HerColumn=0.24f;           // her column at the right, a share of the window's width
        constexpr float StripHeight=54;            // the row of buttons above the tiles
        enum Action {Hint,Undo,Shuffle,Fresh,ActionCount};

        // How a pair goes (one chosen at random each time).
        enum TakeStyle {Rise,Spin,Meet,Shatter,Petals,Burst,TakeStyles};
        // A tile on its way out, a bit of sparkle, a ring or a glow spreading.
        struct Fade {int face=0,style=Rise;float x=0,y=0,toX=0,toY=0,turn=1;ULONGLONG at=0;};
        enum SparkShape {Dot,Petal,Star,Paper};
        struct Spark2D {float x,y,vx,vy,age,life,size;Color color;int shape;float angle,spin,gravity,drag;};
        struct Ring {float x,y,from,to,width;ULONGLONG at;float life;Color color;bool glow;};
        // The show at the end of a game: all cleared (the tiles of the seasons and flowers come
        // down and all), or a contest won.
        struct Festival
        {
            int kind=0;        // 0 none, 1 a contest won, 2 all cleared
            ULONGLONG at=0;
            std::vector<float> bursts;
            size_t burstsDone=0;
            int landed=0;
            float paperOwed=0;
        };
        // Where the tiles go in the window.
        struct Board {float her=0,area=0,x0=0,y0=0;int tw=1,th=1,dz=1,minX=0,minY=0;};

        struct State
        {
            HWND window=nullptr;
            ULONG_PTR gdiplus=0;
            SolitaireOpponent her;
            std::unique_ptr<Bitmap> face;
            std::function<void()> onEnd;
            Solitaire::Table table;
            int lost=0;                // games you lost to her in a row
            int nextFirst=-1;          // who takes first next game together (-1: drawn by lot)
            bool gameOverTold=false;
            bool choosing=true;        // the screen choosing how to play
            bool playedAny=false;      // a game has been started since the window opened
            Mode modeChosen=Mode::Alone;
            int layoutChosen=0;
            RectF modeButtons[2];
            std::array<RectF,Solitaire::LayoutCountMax+1> layoutButtons{};   // the last: any at random
            std::array<RectF,ActionCount> actionButtons{};
            int hover=-1,cursor=-1,selected=-1;   // tiles: under the mouse, the keys', chosen
            Pair hint,herPick;
            ULONGLONG hintUntil=0;
            bool confirmQuit=false;
            RectF quitButtons[2];
            ULONGLONG nextAt=0;
            ULONGLONG startedAt=0,clearedAfter=0,lastTakeAt=0;
            std::wstring said;         // why a tile cannot be taken, in the big bubble
            ULONGLONG saidAt=0;
            // Her words in the bubble (as at mahjong).
            struct Words
            {
                std::wstring text,fallback;
                ULONGLONG since=0,until=0,asked=0;
                bool waiting=false;
                bool voiced=false;
                int weight=0;
                std::vector<std::wstring> recent;
                std::wstring revealing;
                double shown=0;
                ULONGLONG shownAt=0;
                std::wstring told;
            } words;
            GameLines lines;
            GameVoice voice;
            bool prefetching=false;
            Expression tell=Expression::Neutral;
            POINT headScreen{-1,-1};
            std::wstring afterword,afterwordLine;
            struct Seen
            {
                bool started=false,over=false,half=false,nearly=false,stuck=false,certainWin=false,certainLoss=false;
                int taken=0;
                ULONGLONG watchedAt=0;
            } seen;
            Board board;
            std::vector<int> order;                  // the tiles back to front
            int orderLayout=-1;
            std::unique_ptr<Bitmap> layer;           // the felt and tiles, kept until they change
            std::vector<long long> layerKey;
            std::map<std::tuple<int,int,int,int>,std::unique_ptr<Bitmap>> sprites;
            std::map<std::tuple<int,int,int>,std::unique_ptr<Bitmap>> thumbs;
            std::vector<Fade> fades;
            std::vector<Spark2D> sparks2D;
            std::vector<Ring> rings;
            Festival festival;
            ULONGLONG sparksAt=0,fxShift=0;   // (fxShift: the diagnostic shots' time moved on)
            int forceStyle=-1;
            std::mt19937 fxRandom{std::random_device{}()};
            std::unique_ptr<Bitmap> canvas;
            bool overlayDue=true;      // (3D) the 2D layer to be drawn again with the next frame
            std::uint64_t shown=0;     // (3D) what it showed when last drawn (Shown)
            // Her at the table in 3D (none: 2D only).
            HWND view=nullptr;
            std::unique_ptr<MascotRenderer> renderer;
            int entrance=0;
            float herX=0,herZ=0,herAngle=0,entranceTime=0,chairZ=0;
            struct Camera {float yaw=0,pitch=0.12f,distance=3.0f,x=0,y=0,z=0;} camera;
            std::array<float,3> follow{};
            bool following=false;
            POINT dragFrom{};
            int dragging=0;
            float upper=10,hips=10;
            float floorY=-5;
            std::vector<int> scene;
            double clock=0;
            int plannedFor=-1,reachedFor=-1;
            std::vector<MascotRenderer::Particle> roomGlows;
            std::mt19937 sparkRandom{12345};
            double thinkFor=0;
            std::wstring endGesture;
            double endGestureUntil=0;
            bool seenOver=false;
            int heardVersion=-1;
        } state;

        float Scale()
        {
            if(!state.window)return 1;
            const float dpi=static_cast<float>(GetDpiForWindow(state.window))/96.0f;
            RECT client{};
            GetClientRect(state.window,&client);
            const float grow=std::min(static_cast<float>(client.right)/(BaseWidth*dpi),static_cast<float>(client.bottom)/(BaseHeight*dpi));
            return dpi*std::clamp(grow,0.6f,2.2f);
        }

        void Redraw()
        {
            if(state.renderer)state.overlayDue=true;
            else if(state.window)InvalidateRect(state.window,nullptr,FALSE);
        }

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
        bool YourMoveNow(){return !state.choosing&&state.table.Now()==Phase::YourMove&&!Waiting();}
        bool EndShown(){return !state.choosing&&state.table.Now()==Phase::GameOver&&!Waiting();}

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
            // The camera as at the mahjong table: the right button turns it, the middle one (or
            // Shift and the right) moves it, the wheel comes nearer; a right double-click or F1
            // puts it back. A left click is the window's (a tile, a button).
            auto& c=state.camera;
            if(message==WM_LBUTTONDOWN||message==WM_LBUTTONDBLCLK)
            {
                SetFocus(state.window);
                return Proc(state.window,WM_LBUTTONDOWN,wParam,lParam);
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
                Log(L"solitaire: resize: %s",error.c_str());
            InvalidateRect(state.window,nullptr,FALSE);
        }

        void ReleaseView()
        {
            if(state.renderer){state.renderer->WaitIdle();state.renderer.reset();Log(L"solitaire: 3D view released");}
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

        // ---- The tiles' faces ----

        Bitmap* LoadAtlas(const wchar_t* name,const wchar_t* type)
        {
            std::unique_ptr<Bitmap> loaded;
            const HRSRC info=FindResourceW(nullptr,name,type);
            const HGLOBAL data=info?LoadResource(nullptr,info):nullptr;
            if(!data)return nullptr;
            IStream* stream=SHCreateMemStream(static_cast<const BYTE*>(LockResource(data)),SizeofResource(nullptr,info));
            if(!stream)return nullptr;
            std::unique_ptr<Bitmap> read(Bitmap::FromStream(stream));
            stream->Release();
            if(read&&read->GetLastStatus()==Ok)return read->Clone(0,0,read->GetWidth(),read->GetHeight(),PixelFormat32bppPARGB);
            return nullptr;
        }

        // A face at w by h: the mahjong tiles' atlas (150x200 each, ten across), and the seasons'
        // and flowers' (four across).
        void DrawFace(Graphics& g,int face,const Rect& to)
        {
            static std::unique_ptr<Bitmap> tiles(LoadAtlas(L"TILES",L"MAHJONGTILES")),flowers(LoadAtlas(L"FLOWERS",L"SOLITAIRETILES"));
            constexpr int Side=150,Top=200;
            if(face<Solitaire::Seasons)
            {
                if(tiles)g.DrawImage(tiles.get(),to,face%10*Side,face/10*Top,Side,Top,UnitPixel);
            }
            else if(flowers)
            {
                const int at=face-Solitaire::Seasons;
                g.DrawImage(flowers.get(),to,at%4*Side,at/4*Top,Side,Top,UnitPixel);
            }
        }

        // A tile as a block seen from the upper left: its face at the top left of the picture, its
        // thickness going back to the lower right, ivory and then the brown back, lit from the
        // upper left (so the bottom side lighter than the right), and its shadow on what is below.
        Bitmap* Sprite(int face,int w,int h,int d)
        {
            if(state.sprites.size()>260)state.sprites.clear();   // sizes the window had before
            auto& made=state.sprites[{face,w,h,d}];
            if(made)return made.get();
            const int shadow=std::max(2,d*2/3);
            made=std::make_unique<Bitmap>(w+d+shadow+2,h+d+shadow+2,PixelFormat32bppPARGB);
            Graphics g(made.get());
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            const float r=w*0.12f,W=static_cast<float>(w),H=static_cast<float>(h);
            for(int i=shadow;i>=1;--i)
                FillRound(g,RectF(static_cast<float>(d+i),static_cast<float>(d+i),W,H),r,Color(static_cast<BYTE>(70/shadow+6),0,0,0));
            const int seam=std::max(1,d*9/20);   // how much of the thickness is ivory
            for(int i=d;i>=1;--i)
            {
                const bool back=i>seam;
                // Darker toward the far edge of each part.
                const float deep=back?static_cast<float>(i-seam)/std::max(1,d-seam):static_cast<float>(i)/seam;
                const auto shade=[&](int c,float k){return static_cast<BYTE>(std::clamp(c*k*(1-0.16f*deep),0.0f,255.0f));};
                const int cr=back?142:232,cg=back?88:218,cb=back?44:186;
                const float x=static_cast<float>(i);
                LinearGradientBrush side(PointF(x,x+H),PointF(x+W,x),Color(255,shade(cr,1.0f),shade(cg,1.0f),shade(cb,1.0f)),Color(255,shade(cr,0.66f),shade(cg,0.66f),shade(cb,0.66f)));
                GraphicsPath path;
                RoundRect(path,x,x,W,H,r);
                g.FillPath(&side,&path);
            }
            // The edges: where the ivory meets the brown, and round the whole block.
            {
                GraphicsPath joint;
                RoundRect(joint,static_cast<float>(seam)+0.5f,static_cast<float>(seam)+0.5f,W,H,r);
                Pen line(Color(120,60,34,14),1);
                g.DrawPath(&line,&joint);
                GraphicsPath outer;
                RoundRect(outer,static_cast<float>(d),static_cast<float>(d),W,H,r);
                Pen dark(Color(190,44,24,10),1);
                g.DrawPath(&dark,&outer);
            }
            DrawFace(g,face,Rect(0,0,w,h));
            // The face's rim, its edge rounding off: bright at the upper left, shaded at the lower right.
            {
                GraphicsPath rim;
                RoundRect(rim,0.6f,0.6f,W-1.2f,H-1.2f,r);
                LinearGradientBrush light(PointF(0,0),PointF(W,H),Color(235,255,252,240),Color(210,120,98,62));
                Pen pen(&light,std::max(1.0f,w*0.018f));
                g.DrawPath(&pen,&rim);
            }
            return made.get();
        }

        Board PlaceBoard(float W,float H,float s)
        {
            Board b;
            b.her=std::round(W*HerColumn);
            b.area=W-b.her;
            const auto& spots=state.table.Spots();
            int maxX=0,maxY=0,maxZ=0;
            b.minX=b.minY=1000;
            for(const auto& p:spots)
            {
                b.minX=std::min(b.minX,p.x);b.minY=std::min(b.minY,p.y);
                maxX=std::max(maxX,p.x);maxY=std::max(maxY,p.y);maxZ=std::max(maxZ,p.z);
            }
            const float cols=(maxX-b.minX)/2.0f+1,rows=(maxY-b.minY)/2.0f+1;
            const float strip=StripHeight*s,space=16*s;
            constexpr float Thick=0.14f,Aspect=0.75f;
            const float byW=(b.area-2*space)/(cols+Thick*(maxZ*0.5f+1));
            const float byH=(H-strip-2*space)/(rows+Thick*Aspect*(maxZ*0.5f+1));
            const float tw=std::max(10.0f,std::min(byW,byH*Aspect));
            b.tw=static_cast<int>(tw);
            b.th=static_cast<int>(std::round(tw/Aspect));
            b.dz=std::max(2,static_cast<int>(std::round(tw*Thick)));
            b.x0=std::round((b.area-cols*b.tw)/2+b.dz*maxZ*0.25f-b.dz*0.5f);
            b.y0=std::round(strip+(H-strip-rows*b.th)/2+b.dz*maxZ*0.25f-b.dz*0.5f);
            return b;
        }

        // A tile's face on screen.
        RectF TileBox(int tile)
        {
            const auto& p=state.table.Spots()[static_cast<size_t>(tile)];
            const auto& b=state.board;
            return RectF(std::floor(b.x0+(p.x-b.minX)*b.tw*0.5f-p.z*b.dz),std::floor(b.y0+(p.y-b.minY)*b.th*0.5f-p.z*b.dz),
                         static_cast<float>(b.tw),static_cast<float>(b.th));
        }

        // The tiles back to front: the layers from the bottom, each column from the left and down
        // it, so a tile's face covers the thickness of those at its left and above it (a tile set
        // half a row off, among columns of whole rows, must not lie over the next column).
        const std::vector<int>& Order()
        {
            if(state.orderLayout!=state.table.Layout()||state.order.empty())
            {
                state.orderLayout=state.table.Layout();
                state.order.resize(Tiles);
                for(int i=0;i<Tiles;++i)state.order[static_cast<size_t>(i)]=i;
                const auto& s=state.table.Spots();
                std::sort(state.order.begin(),state.order.end(),[&](int a,int b){
                    const auto& p=s[static_cast<size_t>(a)];
                    const auto& q=s[static_cast<size_t>(b)];
                    return std::tie(p.z,p.x,p.y)<std::tie(q.z,q.x,q.y);
                });
            }
            return state.order;
        }

        int TileAt(float x,float y)
        {
            const auto& order=Order();
            for(auto i=order.rbegin();i!=order.rend();++i)
                if(state.table.FaceAt(*i)>=0&&TileBox(*i).Contains(x,y))return *i;
            return -1;
        }

        bool HintOn(){return state.hint.Valid()&&GetTickCount64()<state.hintUntil;}

        // The felt and the tiles, drawn again only when something on them changes.
        Bitmap* BoardLayer(float s,int W,int H)
        {
            auto& t=state.table;
            const Board& b=state.board;
            const int aw=std::max(1,static_cast<int>(b.area));
            const bool canPoint=YourMoveNow();
            std::vector<long long> key{aw,H,b.tw,t.Version(),t.Layout(),state.selected,canPoint?state.hover:-1,canPoint?state.cursor:-1,
                                       HintOn()?state.hint.a:-1,HintOn()?state.hint.b:-1,state.herPick.a,state.herPick.b};
            if(state.layer&&key==state.layerKey)return state.layer.get();
            state.layerKey=std::move(key);
            if(!state.layer||static_cast<int>(state.layer->GetWidth())!=aw||static_cast<int>(state.layer->GetHeight())!=H)
                state.layer=std::make_unique<Bitmap>(aw,H,PixelFormat32bppPARGB);
            Graphics g(state.layer.get());
            {
                const float cx=aw*0.5f,cy=H*0.55f;
                GraphicsPath disc;
                const float rx=aw*0.78f,ry=H*0.85f;
                disc.AddEllipse(cx-rx,cy-ry,2*rx,2*ry);
                PathGradientBrush felt(&disc);
                felt.SetCenterColor(Color(255,32,96,70));
                const Color rim(255,14,52,38);
                INT one=1;
                felt.SetSurroundColors(&rim,&one);
                g.Clear(rim);
                g.FillPath(&felt,&disc);
                LinearGradientBrush shade(PointF(aw-16*s,0),PointF(static_cast<float>(aw),0),Color(0,0,0,0),Color(150,0,0,0));
                g.FillRectangle(&shade,aw-16*s,0.0f,16*s,static_cast<float>(H));
            }
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            const float r=b.tw*0.12f;
            for(int tile:Order())
            {
                const int face=t.FaceAt(tile);
                if(face<0)continue;
                const RectF box=TileBox(tile);
                const bool free=t.Free(tile);
                if(Bitmap* sprite=Sprite(face,b.tw,b.th,b.dz))
                    g.DrawImage(sprite,static_cast<INT>(box.X),static_cast<INT>(box.Y),0,0,sprite->GetWidth(),sprite->GetHeight(),UnitPixel);
                const RectF inner(box.X+0.5f,box.Y+0.5f,box.Width-1,box.Height-1);
                if(tile==state.selected)FillRound(g,inner,r,Color(96,250,190,60),Color(255,240,150,30),2.6f*s);
                else if(tile==state.herPick.a||tile==state.herPick.b)FillRound(g,inner,r,Color(96,240,90,130),Color(255,230,60,110),2.6f*s);
                else if(HintOn()&&(tile==state.hint.a||tile==state.hint.b))FillRound(g,inner,r,Color(90,90,220,250),Color(255,40,170,230),2.6f*s);
                else if(canPoint&&tile==state.hover&&free)FillRound(g,inner,r,Color(40,255,240,170),Color(220,250,226,140),1.6f*s);
                if(canPoint&&tile==state.cursor)FillRound(g,inner,r,Color(0,0,0,0),Color(255,240,206,120),2.4f*s);
            }
            return state.layer.get();
        }

        // ---- The room's texture ----

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
                Log(L"solitaire: no 3D view (%s)",error.c_str());
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
                model.SettlePose();   // nothing begun meanwhile comes along
                model.Update(1/30.0);
                model.ResetPhysics();
            }
            Log(L"solitaire: 3D view %ux%u on %s (hips %.1f, upper body %.1f, floor %.1f)",w,h,renderer->AdapterName().c_str(),state.hips,state.upper,state.floorY);
            if(!renderer->SetGeometryTexture(ScrollImage()))Log(L"solitaire: no texture for the 3D table");
            state.renderer=std::move(renderer);
            state.overlayDue=true;
            state.entrance=0;
            state.scene.clear();
            state.camera={};
            state.dragging=0;
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

        float Top(){return 0.12f*state.hips+0.30f*state.upper;}
        float FieldZ(){return -1.10f*state.upper;}

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

        void Prefetch();

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
                state.afterword=Fmt(T(L"いま、話し相手と麻雀ソリティア（積んだ牌から同じ牌を2枚ずつ取るゲーム）をしていたが、途中でやめてデスクトップに戻ってきたところ（{0}組取ったところ）。そのことを話し相手にひと言（1〜2文）",
                                      L"You were at mahjong solitaire (taking pairs of tiles from a stack) with them, but they stopped midway and you have come back to the desktop ({0} pairs taken). Say a word about it to them (one or two sentences)"),
                                    {std::to_wstring(state.table.Taken())});
                state.afterwordLine=T(L"また続き、やろうね。",L"Let's try it again another time.");
            }
            auto onEnd=std::move(state.onEnd);
            DestroyWindow(state.window);
            if(onEnd)onEnd();
        }

        // A deal to show behind the choosing screen, or a game begun.
        void Deal(Mode mode,int layout)
        {
            std::random_device device;
            int first=Solitaire::You;
            if(mode==Mode::Together)
            {
                // The first game's first take by lot; then it goes back and forth.
                first=state.nextFirst>=0?state.nextFirst:static_cast<int>(device()%2);
                state.nextFirst=1-first;
            }
            state.table.NewGame(device(),mode,layout,first,std::max(LeastCare,Care-LessPerBond*state.her.bond-LessPerLoss*state.lost));
            state.hover=state.cursor=state.selected=-1;
            state.hint=state.herPick={};
            state.hintUntil=0;
            state.fades.clear();
            state.sparks2D.clear();
            state.rings.clear();
            state.festival={};
            state.plannedFor=state.reachedFor=-1;
            state.layerKey.clear();
        }

        void StartGame()
        {
            state.choosing=false;
            state.playedAny=true;
            state.gameOverTold=false;
            Deal(state.modeChosen,state.layoutChosen<Solitaire::LayoutCount()?state.layoutChosen:static_cast<int>(state.fxRandom()%Solitaire::LayoutCount()));
            state.confirmQuit=false;
            state.said.clear();
            state.nextAt=GetTickCount64()+HerPlayWait;
            state.startedAt=state.lastTakeAt=GetTickCount64();
            state.clearedAfter=0;
            state.seen={};
            state.seen.watchedAt=state.startedAt;
            state.words.waiting=false;
            state.words.text.clear();
            state.afterword.clear();
            state.afterwordLine.clear();
            state.heardVersion=state.table.Version();
            PlayMahjongSound(MahjongSound::Deal);
            Prefetch();
            Redraw();
        }

        void Tell(const std::wstring& why)
        {
            state.said=why;
            state.saidAt=GetTickCount64();
            Redraw();
        }

        ULONGLONG FxNow(){return GetTickCount64()+state.fxShift;}
        float Any(float a=1){return std::uniform_real_distribution<float>(0,a)(state.fxRandom);}

        void Emit(float x,float y,float vx,float vy,float delay,float life,float size,Color color,int shape=Dot,float gravity=220,float drag=0)
        {
            if(state.sparks2D.size()>900)return;
            state.sparks2D.push_back({x,y,vx,vy,-delay,life,size,color,shape,Any(6.2832f),(Any()-0.5f)*9,gravity,drag});
        }

        void AddRing(float x,float y,float from,float to,float width,float delay,float life,Color color,bool glow=false)
        {
            state.rings.push_back({x,y,from,to,width,FxNow()+static_cast<ULONGLONG>(delay*1000),life,color,glow});
        }

        // Sparks all round from a point.
        void Spray(float x,float y,int count,float speed,float delay,Color color,int shape=Dot,float size=1,float gravity=220,float drag=0)
        {
            const float s=Scale();
            for(int i=0;i<count;++i)
            {
                const float a=Any(6.2832f),v=speed*(0.35f+0.65f*Any())*s;
                Emit(x,y,std::cos(a)*v,std::sin(a)*v,delay,0.35f+0.35f*Any(),(1.4f+1.8f*Any())*size*s,i%3==0?Color(255,255,255,255):color,shape,gravity,drag);
            }
        }

        constexpr static ULONGLONG TakeTime=460;
        constexpr static float MeetShare=0.5f;   // how much of it the two tiles take to come together

        // The pair going, in one of its ways. accent: the sparks' colour.
        void SeeOff(const Pair& pair,int faceA,int faceB,Color accent)
        {
            const ULONGLONG now=FxNow();
            const float s=Scale();
            int style=state.forceStyle>=0?state.forceStyle:static_cast<int>(state.fxRandom()%TakeStyles);
            const RectF a=TileBox(pair.a),b=TileBox(pair.b);
            const float mx=(a.X+b.X)/2,my=(a.Y+b.Y)/2;
            const float cw=a.Width/2,ch=a.Height/2;
            int side=0;
            for(int tile:{pair.a,pair.b})
            {
                const RectF box=tile==pair.a?a:b;
                const float cx=box.X+cw,cy=box.Y+ch;
                Fade f;
                f.face=tile==pair.a?faceA:faceB;
                f.style=style;
                f.x=box.X;f.y=box.Y;f.toX=mx;f.toY=my;f.at=now;
                f.turn=side?-1.0f:1.0f;
                state.fades.push_back(f);
                switch(style)
                {
                case Rise:
                    Spray(cx,cy,12,150,0,accent,Dot,1,-40);
                    break;
                case Spin:
                    Spray(cx,cy,10,120,0.16f,accent,Star,1.8f,60);
                    break;
                case Shatter:
                    Spray(cx,cy,10,200,0,accent);
                    break;
                case Petals:
                    for(int i=0;i<16;++i)
                    {
                        const float angle=Any(6.2832f),v=(30+90*Any())*s;
                        const Color pink=i%4==0?Color(255,255,246,250):i%2?Color(255,250,170,196):Color(255,246,132,168);
                        Emit(cx+(Any()-0.5f)*box.Width,cy+(Any()-0.5f)*box.Height,std::cos(angle)*v,std::sin(angle)*v-60*s,Any(0.12f),0.8f+0.5f*Any(),(3.2f+2.4f*Any())*s,pink,Petal,90);
                    }
                    break;
                case Burst:
                    AddRing(cx,cy,0.2f*box.Width,1.25f*box.Width,5*s,0,0.42f,Color(255,255,226,130));
                    AddRing(cx,cy,0.1f*box.Width,0.85f*box.Width,3*s,0.08f,0.38f,Color(255,255,255,255));
                    AddRing(cx,cy,0,1.1f*box.Width,0,0,0.3f,Color(210,255,240,190),true);
                    Spray(cx,cy,8,240,0.02f,accent,Star,1.5f,40,2);
                    break;
                default:
                    break;
                }
                ++side;
            }
            if(style==Meet)
            {
                // Where they come together: a flash, a ring, sparks.
                const float cx=mx+cw,cy=my+ch,delay=MeetShare*TakeTime/1000.0f;
                AddRing(cx,cy,0,1.7f*a.Width,0,delay,0.34f,Color(235,255,240,190),true);
                AddRing(cx,cy,0.3f*a.Width,1.9f*a.Width,4*s,delay,0.42f,Color(255,255,226,130));
                Spray(cx,cy,26,300,delay,accent,Star,1.6f,60,2);
            }
        }

        // ---- The end's show ----

        RectF EndBox(float s,float H)
        {
            const float area=state.board.area,w=std::min(area-40*s,600*s),h=std::min(H-40*s,290*s);
            return RectF((area-w)/2,(H-h)/2,w,h);
        }

        constexpr static float TilesStart=0.5f,TileEvery=0.17f,TileFall=0.55f;
        constexpr static ULONGLONG FestivalMoves=10000;   // ms: its rays and tiles have settled by then
        constexpr static ULONGLONG ClearedShowFirst=3000;

        void StartFestival(int kind)
        {
            auto& f=state.festival;
            f={};
            f.kind=kind;
            f.at=FxNow();
            const int bursts=kind==2?20:7;
            for(int i=0;i<bursts;++i)f.bursts.push_back((kind==2?0.15f:0.1f)+i*(kind==2?0.34f:0.3f)+Any(0.12f));
            const float cx=state.board.area/2,cy=state.board.y0+state.board.th*4.0f;
            const float s=Scale();
            AddRing(cx,cy,0,state.board.area*0.75f,0,0,0.6f,Color(kind==2?245:190,255,244,200),true);
            AddRing(cx,cy,40*s,state.board.area*0.7f,10*s,0,0.8f,Color(255,255,226,130));
            if(kind==2)AddRing(cx,cy,20*s,state.board.area*0.55f,6*s,0.18f,0.8f,Color(255,255,255,255));
            PlayMahjongSound(MahjongSound::Deal);
        }

        Color FireColor(int i)
        {
            static const Color colors[6]={Color(255,255,206,90),Color(255,255,110,120),Color(255,110,220,255),Color(255,190,255,130),Color(255,255,150,230),Color(255,255,255,235)};
            return colors[i%6];
        }

        // Where the n-th of the eight tiles comes to rest, and its size.
        void TileSpot(int n,float s,float H,float& x,float& y,int& w,int& h)
        {
            const RectF box=EndBox(s,H);
            const float room=std::max(30*s,(H-box.Height-2*StripHeight*s)/2-26*s);
            h=static_cast<int>(std::min(room,168*s));
            w=static_cast<int>(h*0.75f);
            const float gap=14*s,row=4*w+3*gap;
            x=std::floor(state.board.area/2-row/2+(n%4)*(w+gap));
            y=std::floor(n<4?box.Y-h-16*s:box.GetBottom()+16*s);
        }

        void AdvanceFestival(float dt,float s,float H)
        {
            auto& f=state.festival;
            if(!f.kind)return;
            const float t=(FxNow()-f.at)/1000.0f;
            const float area=state.board.area;
            while(f.burstsDone<f.bursts.size()&&f.bursts[f.burstsDone]<=t)
            {
                const int i=static_cast<int>(f.burstsDone++);
                const float x=area*(0.1f+0.8f*Any()),y=H*(0.1f+0.62f*Any());
                const Color c=FireColor(static_cast<int>(state.fxRandom()%6));
                const float big=(f.kind==2?1.0f:0.8f)*(0.8f+0.5f*Any());
                // A round of sparks that slow and droop, a finer one inside it, a flash.
                for(int k=0;k<60;++k)
                {
                    const float a=k*6.2832f/60+Any(0.1f),v=(300+70*Any())*big*s;
                    Emit(x,y,std::cos(a)*v,std::sin(a)*v,0,0.9f+0.4f*Any(),(1.7f+1.2f*Any())*s,c,k%5==0?Star:Dot,90,2.6f);
                }
                Spray(x,y,28,170*big,0.05f,FireColor(i+3),Dot,1.1f,80,2.2f);
                AddRing(x,y,0,120*big*s,0,0,0.28f,Color(220,255,250,230),true);
                AddRing(x,y,10*s,230*big*s,2.4f*s,0,0.5f,c);
            }
            // Coloured paper falling for a while.
            if(t<(f.kind==2?9.0f:4.0f))
            {
                f.paperOwed+=dt*(f.kind==2?46.0f:20.0f);
                static const Color papers[7]={Color(255,255,216,110),Color(255,250,120,140),Color(255,120,210,250),Color(255,160,240,150),Color(255,250,170,220),Color(255,255,255,250),Color(255,190,150,255)};
                while(f.paperOwed>=1)
                {
                    f.paperOwed-=1;
                    Emit(area*Any(),-10*s-Any(40*s),(Any()-0.5f)*40*s,(70+90*Any())*s,0,H/((80+80*Any())*s),(3.2f+2.6f*Any())*s,papers[state.fxRandom()%7],Paper,14);
                }
            }
            // The eight tiles landing.
            if(f.kind==2)
                while(f.landed<8&&t>=TilesStart+f.landed*TileEvery+TileFall)
                {
                    float x,y;
                    int w,h;
                    TileSpot(f.landed,s,H,x,y,w,h);
                    PlayMahjongSound(MahjongSound::PlaceHard);
                    Spray(x+w/2.0f,y+h,18,240,0,FireColor(f.landed),Star,1.5f,160,1.5f);
                    AddRing(x+w/2.0f,y+h/2.0f,0.3f*w,1.3f*w,3*s,0,0.4f,Color(255,255,226,130));
                    ++f.landed;
                }
        }

        void AdvanceEffects(float s,float H)
        {
            const ULONGLONG now=FxNow();
            const float dt=state.sparksAt?std::min(0.1f,(now-state.sparksAt)/1000.0f):0;
            state.sparksAt=now;
            AdvanceFestival(dt,s,H);
            state.fades.erase(std::remove_if(state.fades.begin(),state.fades.end(),[&](const Fade& f){return now-f.at>=TakeTime;}),state.fades.end());
            state.rings.erase(std::remove_if(state.rings.begin(),state.rings.end(),[&](const Ring& r){return now>=r.at&&(now-r.at)/1000.0f>=r.life;}),state.rings.end());
            for(auto& k:state.sparks2D)
            {
                k.age+=dt;
                if(k.age<0)continue;
                if(k.drag>0){const float slow=std::exp(-k.drag*dt);k.vx*=slow;k.vy*=slow;}
                k.vy+=k.gravity*s*dt;
                k.angle+=k.spin*dt;
                k.x+=k.vx*dt;k.y+=k.vy*dt;
                if(k.shape==Petal||k.shape==Paper)k.x+=std::sin(k.angle*0.7f)*34*s*dt;
            }
            state.sparks2D.erase(std::remove_if(state.sparks2D.begin(),state.sparks2D.end(),[](const Spark2D& k){return k.age>=k.life;}),state.sparks2D.end());
        }

        // A tile's block drawn at (x, y), its face's middle kept, turned and scaled, faded.
        void DrawBlock(Graphics& g,Bitmap* sprite,int w,int h,float x,float y,float scale,float degrees,float alpha,const Rect* part=nullptr,float dx=0,float dy=0)
        {
            ColorMatrix m={1,0,0,0,0, 0,1,0,0,0, 0,0,1,0,0, 0,0,0,std::clamp(alpha,0.0f,1.0f),0, 0,0,0,0,1};
            ImageAttributes faded;
            faded.SetColorMatrix(&m);
            const GraphicsState saved=g.Save();
            g.TranslateTransform(x+w/2.0f+dx,y+h/2.0f+dy);
            g.RotateTransform(degrees);
            g.ScaleTransform(scale,scale);
            if(part)
            {
                const Rect to(part->X-w/2,part->Y-h/2,part->Width,part->Height);
                g.DrawImage(sprite,to,part->X,part->Y,part->Width,part->Height,UnitPixel,&faded);
            }
            else g.DrawImage(sprite,Rect(-w/2,-h/2,static_cast<INT>(sprite->GetWidth()),static_cast<INT>(sprite->GetHeight())),0,0,sprite->GetWidth(),sprite->GetHeight(),UnitPixel,&faded);
            g.Restore(saved);
        }

        void DrawFades(Graphics& g,float s)
        {
            const ULONGLONG now=FxNow();
            const Board& l=state.board;
            for(const Fade& f:state.fades)
            {
                const float u=static_cast<float>(now-f.at)/TakeTime;
                Bitmap* sprite=Sprite(f.face,l.tw,l.th,l.dz);
                if(!sprite)continue;
                const float e=u*u*(3-2*u);
                switch(f.style)
                {
                case Spin:
                    DrawBlock(g,sprite,l.tw,l.th,f.x,f.y,std::max(0.01f,1-e),f.turn*540*u*u,1-u*u*u);
                    break;
                case Meet:
                {
                    const float k=std::min(1.0f,u/MeetShare);
                    const float go=k*k;   // faster and faster, until they strike
                    if(k<1)DrawBlock(g,sprite,l.tw,l.th,f.x+(f.toX-f.x)*go,f.y+(f.toY-f.y)*go,1+0.12f*std::sin(k*3.1416f),f.turn*10*k,1);
                    break;
                }
                case Shatter:
                {
                    // Cut in three by four, the pieces flying apart and turning.
                    constexpr int Across=3,Down=4;
                    std::mt19937 pieces(static_cast<unsigned>(f.x*7+f.y*13));
                    std::uniform_real_distribution<float> any(-1,1);
                    for(int py=0;py<Down;++py)
                        for(int px=0;px<Across;++px)
                        {
                            const int w=l.tw/Across+1,h=l.th/Down+1;
                            const Rect part(px*l.tw/Across,py*l.th/Down,w,h);
                            const float ox=(px+0.5f)/Across-0.5f,oy=(py+0.5f)/Down-0.5f;
                            const float sx=(ox*2+0.5f*any(pieces))*150*s,sy=(oy*2+0.5f*any(pieces)-0.4f)*150*s;
                            const float t=u*TakeTime/1000.0f;
                            DrawBlock(g,sprite,l.tw,l.th,f.x,f.y,1,0,1-u*u,&part,sx*t+0,sy*t+500*s*t*t);
                        }
                    break;
                }
                case Petals:
                    DrawBlock(g,sprite,l.tw,l.th,f.x,f.y,1+0.08f*u,0,1-std::min(1.0f,u*2.2f));
                    break;
                case Burst:
                    DrawBlock(g,sprite,l.tw,l.th,f.x,f.y,1+0.35f*std::min(1.0f,u*4),0,1-std::min(1.0f,u*3.2f));
                    break;
                default:
                    DrawBlock(g,sprite,l.tw,l.th,f.x,f.y-34*s*e,1+0.1f*u,0,1-u);
                    break;
                }
            }
        }

        void DrawRings(Graphics& g)
        {
            const ULONGLONG now=FxNow();
            for(const Ring& r:state.rings)
            {
                if(now<r.at)continue;
                const float u=std::min(1.0f,(now-r.at)/1000.0f/r.life),e=1-(1-u)*(1-u);
                const float radius=std::max(1.0f,r.from+(r.to-r.from)*e);
                const BYTE alpha=static_cast<BYTE>(r.color.GetA()*(1-u));
                if(r.glow)
                {
                    GraphicsPath disc;
                    disc.AddEllipse(r.x-radius,r.y-radius,2*radius,2*radius);
                    PathGradientBrush glow(&disc);
                    glow.SetCenterColor(Color(alpha,r.color.GetR(),r.color.GetG(),r.color.GetB()));
                    const Color rim(0,r.color.GetR(),r.color.GetG(),r.color.GetB());
                    INT one=1;
                    glow.SetSurroundColors(&rim,&one);
                    g.FillPath(&glow,&disc);
                }
                else
                {
                    Pen pen(Color(alpha,r.color.GetR(),r.color.GetG(),r.color.GetB()),std::max(0.5f,r.width*(1-u)));
                    g.DrawEllipse(&pen,r.x-radius,r.y-radius,2*radius,2*radius);
                }
            }
        }

        void DrawSparks(Graphics& g,bool paper)
        {
            for(const auto& k:state.sparks2D)
            {
                if(k.age<0||(k.shape==Paper)!=paper)continue;
                const float u=k.age/k.life;
                const float fade=k.shape==Paper?std::min(1.0f,(1-u)*5):1-u*u;
                SolidBrush paint(Color(static_cast<BYTE>(k.color.GetA()*fade),k.color.GetR(),k.color.GetG(),k.color.GetB()));
                if(k.shape==Dot){g.FillEllipse(&paint,k.x-k.size,k.y-k.size,2*k.size,2*k.size);continue;}
                const GraphicsState saved=g.Save();
                g.TranslateTransform(k.x,k.y);
                g.RotateTransform(k.angle*57.3f);
                if(k.shape==Petal)g.FillEllipse(&paint,-k.size,-k.size*0.5f,2*k.size,k.size);
                else if(k.shape==Paper)
                {
                    g.ScaleTransform(1,std::max(0.12f,std::abs(std::cos(k.angle*1.7f))));
                    g.FillRectangle(&paint,-k.size,-k.size*0.6f,2*k.size,1.2f*k.size);
                }
                else
                {
                    const float a=k.size*1.5f,b=k.size*0.36f;
                    const PointF star[8]={{0,-a},{b,-b},{a,0},{b,b},{0,a},{-b,b},{-a,0},{-b,-b}};
                    g.FillPolygon(&paint,star,8);
                }
                g.Restore(saved);
            }
        }

        // Light turning behind everything, at all cleared.
        void DrawRays(Graphics& g,float H)
        {
            const auto& f=state.festival;
            if(f.kind!=2)return;
            const float t=(FxNow()-f.at)/1000.0f;
            const float strength=std::min(1.0f,t/0.5f)*std::clamp(1.2f-(t-6)/3,0.25f,1.0f);
            const float cx=state.board.area/2,cy=H/2,far2=std::hypot(state.board.area,H);
            SolidBrush ray(Color(static_cast<BYTE>(36*strength),255,226,140));
            for(int i=0;i<14;++i)
            {
                const float a=t*0.28f+i*6.2832f/14,w=0.11f;
                const PointF wedge[3]={{cx,cy},{cx+std::cos(a-w)*far2,cy+std::sin(a-w)*far2},{cx+std::cos(a+w)*far2,cy+std::sin(a+w)*far2}};
                g.FillPolygon(&ray,wedge,3);
            }
        }

        // The big word first, and the seasons' and flowers' tiles coming down round the panel.
        void DrawFestivalTiles(Graphics& g,float s,float H)
        {
            const auto& f=state.festival;
            if(f.kind!=2)return;
            const float t=(FxNow()-f.at)/1000.0f;
            for(int n=0;n<8;++n)
            {
                const float u=(t-TilesStart-n*TileEvery)/TileFall;
                if(u<=0)continue;
                float x,y;
                int w,h;
                TileSpot(n,s,H,x,y,w,h);
                const int d=std::max(3,static_cast<int>(w*0.14f));
                // Falling, a bounce as it lands, then swaying a little.
                float drop=0;
                if(u<1)drop=(1-u*u)*(n<4?y+h+40*s:y-H-40*s);   // the upper row from above, the lower from below
                else if(u<1.6f){const float b=(u-1)/0.6f;drop=b*(1-b)*4*0.12f*h*(n<4?1:-1);}
                const float sway=u>=1?std::sin(t*1.5f+n*0.9f):0;
                if(Bitmap* sprite=Sprite(Solitaire::Seasons+n,w,h,d))
                {
                    if(u>=1)
                    {
                        GraphicsPath disc;
                        const float gx=x+w/2.0f,gy=y+h/2.0f,gr=w*(0.95f+0.08f*sway);
                        disc.AddEllipse(gx-gr,gy-gr,2*gr,2*gr);
                        PathGradientBrush glow(&disc);
                        glow.SetCenterColor(Color(150,255,226,130));
                        const Color rim(0,255,226,130);
                        INT one=1;
                        glow.SetSurroundColors(&rim,&one);
                        g.FillPath(&glow,&disc);
                    }
                    DrawBlock(g,sprite,w,h,x,y-drop-3*s*sway,1,u>=1?1.6f*sway:(n%2?-1:1)*(1-u)*40,1);
                }
            }
            // The word, until the panel comes.
            if(t>0.05f&&t<ClearedShowFirst/1000.0f+0.1f)
            {
                const float grow=std::min(1.0f,t/0.35f),over=grow*(1+0.25f*std::sin(std::min(1.0f,t/0.5f)*3.1416f));
                const std::wstring word=T(L"全部取り切った！",L"All cleared!");
                const float size=62*s*over,cx=state.board.area/2,cy=H/2;
                const RectF box(cx-state.board.area/2,cy-size,state.board.area,2*size);
                for(const auto& [ox,oy]:{std::pair{-3.0f,0.0f},std::pair{3.0f,0.0f},std::pair{0.0f,-3.0f},std::pair{0.0f,3.0f}})
                    Text(g,word,RectF(box.X+ox*s,box.Y+oy*s,box.Width,box.Height),size,Color(255,120,50,10),true,StringAlignmentCenter,StringAlignmentCenter,true);
                Text(g,word,box,size,Color(255,255,226,120),true,StringAlignmentCenter,StringAlignmentCenter,true);
            }
        }

        void TakePair(const Pair& pair)
        {
            auto& t=state.table;
            const int faceA=t.FaceAt(pair.a),faceB=t.FaceAt(pair.b);
            if(!t.Take(pair))return;
            SeeOff(pair,faceA,faceB,Color(255,255,216,110));
            state.selected=-1;
            state.hint=state.herPick={};
            state.lastTakeAt=GetTickCount64();
            PlayMahjongSound(MahjongSound::Place);
            if(t.Now()==Phase::GameOver)
            {
                const bool cleared=t.GameMode()==Mode::Alone;
                state.nextAt=GetTickCount64()+(cleared?ClearedShowFirst:state.renderer?EndWait3D:EndWait);
                if(cleared)StartFestival(2);
                else if(t.Winner()==Solitaire::You)StartFestival(1);
            }
            Redraw();
        }

        // A click (or Enter) on a tile.
        void Click(int tile)
        {
            if(!YourMoveNow())return;
            auto& t=state.table;
            if(tile<0){if(state.selected>=0){state.selected=-1;Redraw();}return;}
            if(!t.Free(tile))
            {
                Tell(T(L"その牌はまだ取れません",L"That tile isn't free yet"));
                return;
            }
            if(tile==state.selected){state.selected=-1;Redraw();return;}
            if(state.selected>=0&&Solitaire::GroupOf(t.FaceAt(state.selected))==Solitaire::GroupOf(t.FaceAt(tile)))
            {
                TakePair({state.selected,tile});
                return;
            }
            state.selected=tile;
            PlayMahjongSound(MahjongSound::Draw);
            Redraw();
        }

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
        void Say(const wchar_t* name,int weight=2,double chance=1)
        {
            if(!state.lines.Roll(name,chance,state.sparkRandom))return;
            GameLine line;
            if(state.lines.Pick(name,state.words.recent,state.sparkRandom,line))SayLine(line,weight);
        }

        void SetTell(Expression e)
        {
            if(e==state.tell)return;
            state.tell=e;
            if(state.her.model&&state.renderer)state.her.model->SetExpression(e,false);
        }

        void Watchers(int weight,const std::wstring& seen,const std::vector<std::wstring>& theirs)
        {
            state.words.told+=Fmt(L"  {0} s, weight {1}: {2}\n",{std::to_wstring(static_cast<int>(state.clock)),std::to_wstring(weight),seen});
            if(state.her.onEvent&&!theirs.empty())state.her.onEvent(seen,weight,Pick(theirs));
        }

        // A scene's line, unless she is saying something weightier just now.
        void SayIfFree(const wchar_t* scene,int weight,double chance=1)
        {
            auto& w=state.words;
            const ULONGLONG now=GetTickCount64();
            if((w.waiting||now-w.since<WordsHold)&&weight<w.weight)return;
            Say(scene,weight,chance);
        }

        // The next scene not yet in her own words, asked of the LLM.
        void Prefetch()
        {
            if(state.prefetching||!state.her.prefetch)return;
            const std::wstring ask=state.lines.NextAsk();
            if(!ask.empty())state.prefetching=state.her.prefetch(ask);
        }

        std::wstring Clock(ULONGLONG ms)
        {
            const int sec=static_cast<int>(ms/1000);
            const std::wstring second=std::to_wstring(sec%60);
            return std::to_wstring(sec/60)+L":"+(second.size()<2?L"0":L"")+second;
        }

        // What is new in the game since the last tick, for words.
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
            if(state.choosing||!state.playedAny)return;
            const std::wstring name=HerName();
            const bool together=t.GameMode()==Mode::Together;
            if(!seen.started)
            {
                seen.started=true;
                SayIfFree(together?L"対局の始まり":L"始まり",6);
                Watchers(6,together?Fmt(T(L"{0}と、このパソコンの持ち主の麻雀ソリティアの勝負が始まる。",L"A game of mahjong solitaire between {0} and the owner of this computer is about to begin."),{name}):
                                    Fmt(T(L"このパソコンの持ち主が麻雀ソリティアを始めた。{0}が横で見ている。",L"The owner of this computer has started mahjong solitaire, with {0} looking on."),{name}),
                         {T(L"がんばれー！",L"Good luck!"),T(L"全部取れるかな？",L"Think it'll all come off?")});
            }
            if(t.Taken()!=seen.taken)
            {
                const bool more=t.Taken()>seen.taken;
                seen.taken=t.Taken();
                if(more&&!together&&t.Now()!=Phase::GameOver)
                {
                    if(t.Left()<=Tiles/2&&!seen.half){seen.half=true;SayIfFree(L"半分",3);}
                    else if(t.Left()<=16&&!seen.nearly)
                    {
                        seen.nearly=true;
                        SayIfFree(L"あと少し",4);
                        Watchers(3,Fmt(T(L"麻雀ソリティアで、持ち主があと{0}枚まで来た。",L"At mahjong solitaire, the owner is down to {0} tiles."),{std::to_wstring(t.Left())}),
                                 {T(L"もう少しだ！",L"Nearly there!")});
                    }
                    else if(seen.taken%12==0)SayIfFree(L"いい流れ",2,0.6);
                    seen.stuck=false;
                }
                if(more&&together&&t.Now()!=Phase::GameOver&&t.Pairs().size()<=2)
                {
                    if(t.LastWho()==Solitaire::You)SayIfFree(L"残りわずか",3,0.5);
                    else SayIfFree(L"塞いだ",3,0.6);
                }
            }
            // Alone: nothing to take, or a long look at the tiles.
            if(!together&&t.Now()!=Phase::GameOver)
            {
                if(t.Stuck()&&!seen.stuck)
                {
                    seen.stuck=true;
                    SayIfFree(L"行き止まり",5);
                    Watchers(3,T(L"麻雀ソリティアで、持ち主の取れる組がなくなった。",L"At mahjong solitaire, the owner has run out of pairs to take."),{T(L"あらら…",L"Oh dear...")});
                }
                else if(!t.Stuck())seen.stuck=false;
                if(now-std::max(state.lastTakeAt,seen.watchedAt)>35000)
                {
                    seen.watchedAt=now;
                    SayIfFree(L"見ている",1,0.7);
                }
            }
            // Together: what she has read to the end shows in her face and is said once.
            if(together&&t.Now()!=Phase::GameOver)
            {
                const int certain=t.Certain();
                if(certain>0&&!seen.certainWin){seen.certainWin=true;SayIfFree(L"勝ち確定",5);}
                if(certain<0&&!seen.certainLoss){seen.certainLoss=true;SayIfFree(L"負け確定",5);}
                SetTell(certain>0?Expression::Smile:certain<0?Expression::Sad:Expression::Neutral);
            }
            else SetTell(Expression::Neutral);
            const bool over=t.Now()==Phase::GameOver;
            if(over&&!seen.over)
            {
                const std::wstring time=Clock(now-state.startedAt);
                if(!state.clearedAfter)state.clearedAfter=now-state.startedAt;
                if(!together)
                {
                    state.afterword=Fmt(T(L"いま、話し相手が麻雀ソリティアの牌を全部取り切るのを横で見ていて、デスクトップに戻ってきたところ（タイム{0}）。その感想を、話し相手にひと言（1〜2文）",
                                          L"You have just watched them clear all the tiles at mahjong solitaire and come back to the desktop (in {0}). Tell them what you thought (one or two sentences)"),{time});
                    state.afterwordLine=T(L"全部取れたね、すごかった！",L"You cleared it all. That was great!");
                    SayIfFree(L"クリア",9);
                    Watchers(9,Fmt(T(L"持ち主が麻雀ソリティアの牌を全部取り切った（{0}）。",L"The owner has cleared all the tiles at mahjong solitaire ({0})."),{time}),
                             {T(L"全部取った！すごい！",L"All cleared! Wow!"),T(L"おめでとう！",L"Congratulations!")});
                }
                else
                {
                    const bool won=t.Winner()==Solitaire::Her,drawn=t.Winner()<0;
                    state.afterword=Fmt(T(L"いま、話し相手と麻雀ソリティアで勝負して、デスクトップに戻ってきたところ。交互に牌を2枚ずつ取って、取れなくなったほうが負け、全部取れたら引き分け。結果は{0}。その感想を、話し相手にひと言（1〜2文）",
                                          L"You have just played mahjong solitaire against them and come back to the desktop: taking pairs in turn, whoever cannot take loses, and all taken is a draw. {0}. Tell them what you thought of it (one or two sentences)"),
                                        {drawn?T(L"全部取れて引き分け",L"All were taken, a draw"):won?T(L"あなたの勝ち",L"You won"):T(L"あなたの負け",L"You lost")});
                    state.afterwordLine=drawn?T(L"引き分けかあ。次は詰まらせてみせるよ。",L"A draw, huh. Next time I'll stop you."):won?T(L"楽しかったね！また勝負しよう。",L"That was fun! Let's play again sometime."):T(L"くやしいなあ…次は勝つからね！",L"So close... I'll win next time!");
                    SayIfFree(drawn?L"引き分け":won?L"勝った":L"負けた",9);
                    Watchers(9,drawn?Fmt(T(L"{0}と持ち主の麻雀ソリティアの勝負は、全部取れて引き分けになった。",L"The mahjong solitaire game between {0} and the owner ended in a draw: all were taken."),{name}):
                               won?Fmt(T(L"{0}と持ち主の麻雀ソリティアの勝負が終わり、{0}が勝った。",L"The mahjong solitaire game between {0} and the owner is over: {0} won."),{name}):
                                   Fmt(T(L"{0}と持ち主の麻雀ソリティアの勝負が終わり、持ち主が勝った。",L"The mahjong solitaire game between {0} and the owner is over: the owner won."),{name}),
                             {T(L"おつかれさま！いい勝負だったね。",L"Well played, both of you!")});
                }
            }
            seen.over=over;
        }

        void Tick()
        {
            auto& t=state.table;
            const ULONGLONG now=GetTickCount64();
            if(!state.said.empty()&&now-state.saidAt>SaidShown){state.said.clear();Redraw();}
            if(state.hint.Valid()&&now>=state.hintUntil){state.hint={};Redraw();}
            // Her turn: she starts to choose at once (once she is seated), and now and then stops
            // to think first.
            if(!state.choosing&&t.Now()==Phase::HerTurn)
            {
                if(state.plannedFor!=t.Version()&&(!state.renderer||state.entrance==5))
                {
                    state.plannedFor=t.Version();
                    t.Think();
                    state.nextAt=std::max(state.nextAt,now+HerPlayWait);
                    std::uniform_real_distribution<double> any(0,1);
                    if(any(state.sparkRandom)<0.15&&t.Taken()>=4)
                    {
                        state.nextAt+=ThinkExtra;
                        state.thinkFor=(HerPlayWait+ThinkExtra-ReachLead)/1000.0-0.1;
                        SayIfFree(L"考える",2);
                    }
                }
                else if(state.plannedFor==t.Version()&&t.Thought())
                {
                    if(!state.herPick.Valid()&&now+PickShown>=state.nextAt){state.herPick=t.HerNext();Redraw();}
                    if(now>=state.nextAt)
                    {
                        const Pair pair=t.HerNext();
                        t.Step();
                        // (Step joined her thread; the pair went if it could.)
                        if(pair.Valid())SeeOff(pair,t.LastFaceA(),t.LastFaceB(),Color(255,255,160,200));
                        if(t.Now()==Phase::GameOver&&t.Winner()==Solitaire::You)StartFestival(1);
                        state.herPick={};
                        state.selected=-1;
                        state.lastTakeAt=GetTickCount64();
                        PlayMahjongSound(MahjongSound::Place);
                        state.nextAt=GetTickCount64()+(t.Now()==Phase::GameOver?(state.renderer?EndWait3D:EndWait):150);
                    }
                }
            }
            if(!state.renderer)Redraw();
            if(!state.renderer)state.voice.Tick(0.04);   // (at the 3D table the model's frame ticks it)
            NoticeWords();
            if(t.Now()==Phase::GameOver&&!state.choosing&&!state.gameOverTold)
            {
                state.gameOverTold=true;
                if(state.her.onGameOver)state.her.onGameOver();
            }
        }

        // ---- Drawing ----

        void Wrapped(Graphics& g,const std::wstring& text,const RectF& box,float size,Color color)
        {
            Font font(Family(),size,FontStyleRegular,UnitPixel);
            SolidBrush paint(color);
            StringFormat format;
            g.DrawString(text.c_str(),static_cast<INT>(text.size()),&font,box,&format,&paint);
        }

        void Button(Graphics& g,const RectF& box,const std::wstring& label,float s,bool on,bool hot=false)
        {
            FillRound(g,box,8*s,on?(hot?Color(245,120,84,44):Color(235,92,62,34)):Color(150,50,44,36),on?Color(255,200,170,90):Color(120,120,110,90),1.2f*s);
            Text(g,label,box,15*s,on?Color(255,250,240,220):Color(170,200,196,180),true,StringAlignmentCenter,StringAlignmentCenter);
        }

        bool ActionOn(int action)
        {
            auto& t=state.table;
            if(!YourMoveNow()&&action!=Fresh)return false;
            switch(action)
            {
            case Hint:return t.GameMode()==Mode::Alone&&!t.Stuck();
            case Undo:return t.CanUndo();
            case Shuffle:return t.GameMode()==Mode::Alone&&t.Left()>=2;
            default:return true;
            }
        }

        void DoAction(int action)
        {
            auto& t=state.table;
            if(!ActionOn(action))return;
            switch(action)
            {
            case Hint:
            {
                state.hint=t.Hint();
                state.hintUntil=GetTickCount64()+HintShown;
                state.selected=-1;
                PlayMahjongSound(MahjongSound::Draw);
                SayIfFree(L"ヒント",3);
                break;
            }
            case Undo:
                if(t.Undo())
                {
                    state.selected=-1;
                    state.hint={};
                    PlayMahjongSound(MahjongSound::Draw);
                }
                break;
            case Shuffle:
                if(t.Shuffle())
                {
                    state.selected=-1;
                    state.hint={};
                    state.seen.stuck=false;
                    PlayMahjongSound(MahjongSound::Deal);
                    SayIfFree(L"混ぜ直し",3);
                }
                // (Tiles left that lie only on each other can't be laid out to clear.)
                else Tell(T(L"この形は混ぜ直せません。戻してください",L"These can't be shuffled. Undo instead"));
                break;
            case Fresh:
                state.modeChosen=t.GameMode();
                if(state.layoutChosen<Solitaire::LayoutCount())state.layoutChosen=t.Layout();
                state.choosing=true;
                state.selected=-1;
                break;
            }
            Redraw();
        }

        // Tiles left, pairs to take, the time or whose turn: at the right of the strip.
        void PaintStrip(Graphics& g,float s)
        {
            auto& t=state.table;
            const Board& b=state.board;
            const Color white(255,240,240,236),soft(255,206,196,186),gold(255,240,206,120);
            const float h=StripHeight*s,bh=36*s,by=(h-bh)/2;
            const bool alone=t.GameMode()==Mode::Alone;
            float x=12*s;
            static const std::pair<int,std::pair<const wchar_t*,const wchar_t*>> labels[]={
                {Hint,{L"ヒント (H)",L"Hint (H)"}},{Undo,{L"戻す (Z)",L"Undo (Z)"}},{Shuffle,{L"混ぜ直す (S)",L"Shuffle (S)"}},{Fresh,{L"新規 (N)",L"New (N)"}}};
            for(auto& box:state.actionButtons)box=RectF();
            for(const auto& [action,words]:labels)
            {
                if(!alone&&action!=Fresh)continue;
                const std::wstring label=T(words.first,words.second);
                const float w=TextWidth(g,label,15*s,true)+24*s;
                const RectF box(x,by,w,bh);
                state.actionButtons[static_cast<size_t>(action)]=box;
                Button(g,box,label,s,ActionOn(action));
                x+=w+8*s;
            }
            const int pairs=static_cast<int>(t.Pairs().size());
            const std::wstring left=Fmt(T(L"残り{0}枚　取れる組{1}",L"{0} tiles, {1} pairs open"),{std::to_wstring(t.Left()),std::to_wstring(pairs)});
            std::wstring right;
            if(alone)
            {
                const ULONGLONG shown=state.clearedAfter?state.clearedAfter:(state.startedAt?GetTickCount64()-state.startedAt:0);
                right=Clock(shown);
            }
            else if(t.Now()==Phase::YourMove)right=T(L"あなたの番",L"Your turn");
            else if(t.Now()==Phase::HerTurn)right=Fmt(T(L"{0}の番",L"{0}'s turn"),{HerName()});
            Text(g,left+L"　"+right,RectF(x+6*s,0,std::max(10.0f,b.area-x-18*s),h),16*s,t.Now()==Phase::YourMove&&!alone?gold:white,true,StringAlignmentFar,StringAlignmentCenter);
            (void)soft;
        }

        // A stack seen from above, higher layers paler. Made once per size.
        Bitmap* Thumb(int layout,int w,int h)
        {
            auto& made=state.thumbs[{layout,w,h}];
            if(made)return made.get();
            made=std::make_unique<Bitmap>(w,h,PixelFormat32bppPARGB);
            Graphics g(made.get());
            const auto& spots=Solitaire::LayoutSpots(layout);
            int x0=1000,y0=1000,x1=0,y1=0,zMax=0;
            for(const auto& p:spots){x0=std::min(x0,p.x);y0=std::min(y0,p.y);x1=std::max(x1,p.x+2);y1=std::max(y1,p.y+2);zMax=std::max(zMax,p.z);}
            const float unit=std::min(static_cast<float>(w)/(x1-x0),static_cast<float>(h)/(y1-y0)*1.33f);
            const float cellW=unit*2,cellH=unit*2/1.33f;
            const float ox=(w-(x1-x0)*unit)/2,oy=(h-(y1-y0)*unit/1.33f)/2;
            std::vector<Solitaire::Spot> order(spots);
            std::sort(order.begin(),order.end(),[](const auto& a,const auto& b){return a.z<b.z;});
            for(const auto& p:order)
            {
                const float k=0.42f+0.58f*p.z/std::max(1,zMax);
                SolidBrush fill(Color(255,static_cast<BYTE>(150+105*k),static_cast<BYTE>(120+115*k),static_cast<BYTE>(80+110*k)));
                const RectF cell(ox+(p.x-x0)*unit,oy+(p.y-y0)*unit/1.33f,cellW,cellH);
                g.FillRectangle(&fill,cell);
                Pen edge(Color(150,40,24,10),1);
                g.DrawRectangle(&edge,cell);
            }
            return made.get();
        }

        void PaintChoose(Graphics& g,float s,float W,float H)
        {
            const Board& b=state.board;
            SolidBrush shade(Color(150,0,0,0));
            g.FillRectangle(&shade,0.0f,0.0f,b.area,H);
            const float w=std::min(b.area-24*s,720*s),h=std::min(H-16*s,640*s);
            const RectF box((b.area-w)/2,std::max(8*s,(H-h)/2),w,h);
            FillRound(g,box,14*s,Color(250,40,22,22),Color(255,200,170,90),2*s);
            const Color gold(255,240,206,120),white(255,240,240,236),soft(255,206,196,186);
            Text(g,T(L"麻雀ソリティア",L"Mahjong solitaire"),RectF(box.X,box.Y+12*s,w,40*s),28*s,gold,true,StringAlignmentCenter,StringAlignmentNear,true);
            const float bw=(w-3*16*s)/2,by=box.Y+58*s,bh=142*s;
            const std::wstring titles[2]={T(L"ひとりで (1)",L"Alone (1)"),Fmt(T(L"{0}と勝負 (2)",L"Against {0} (2)"),{HerName()})};
            const std::wstring about[2]={
                T(L"上に何もなく、左右の片側が空いている同じ牌を2枚ずつ取ります。全部取り切ればクリア。戻す・混ぜ直す・ヒントがあります。",
                  L"Take pairs of matching tiles that have nothing on them and one side open. Clear them all to win. Undo, shuffle and hints are there."),
                T(L"交互に1組ずつ取ります。自分の番に取れる組がなければ負け、全部取れたら引き分け。相手は詰まらせに来ます。",
                  L"One pair each in turn. Whoever has no pair to take loses, and all taken is a draw. She will try to stop you.")};
            for(int i=0;i<2;++i)
            {
                const RectF button(box.X+16*s+i*(bw+16*s),by,bw,bh);
                state.modeButtons[i]=button;
                const bool chosen=(i==1)==(state.modeChosen==Mode::Together);
                FillRound(g,button,10*s,i==0?Color(240,46,92,110):Color(240,150,46,52),chosen?Color(255,250,220,130):Color(255,200,170,90),(chosen?3:1.5f)*s);
                Text(g,titles[i],RectF(button.X,button.Y+8*s,bw,30*s),19*s,white,true,StringAlignmentCenter);
                Wrapped(g,about[i],RectF(button.X+12*s,button.Y+42*s,bw-24*s,bh-46*s),14*s,white);
            }
            float y=by+bh+8*s;
            Text(g,T(L"牌の積み方（矢印キー）",L"How the tiles are stacked (arrow keys)"),RectF(box.X,y,w,22*s),14*s,soft,false,StringAlignmentCenter);
            y+=24*s;
            // Four across, three down: the stacks, and one left to chance.
            const int count=Solitaire::LayoutCount()+1;
            const float gap=8*s,lw=(w-5*gap)/4,lh=(box.GetBottom()-y-36*s-2*gap)/3;
            for(int i=0;i<count;++i)
            {
                const RectF button(box.X+gap+(i%4)*(lw+gap),y+(i/4)*(lh+gap),lw,lh);
                state.layoutButtons[static_cast<size_t>(i)]=button;
                const bool chosen=i==state.layoutChosen,random=i==Solitaire::LayoutCount();
                FillRound(g,button,8*s,chosen?Color(245,120,84,44):Color(200,60,48,40),chosen?Color(255,250,220,130):Color(255,160,140,90),(chosen?2.4f:1.2f)*s);
                const float label=20*s;
                const int tw=static_cast<int>(button.Width-12*s),th=static_cast<int>(button.Height-label-8*s);
                if(random)Text(g,L"?",RectF(button.X,button.Y+2*s,button.Width,button.Height-label),std::max(12.0f,th*0.7f),gold,true,StringAlignmentCenter,StringAlignmentCenter);
                else if(tw>0&&th>0)
                    if(Bitmap* thumb=Thumb(i,tw,th))
                        g.DrawImage(thumb,static_cast<INT>(button.X+6*s),static_cast<INT>(button.Y+4*s),0,0,tw,th,UnitPixel);
                Text(g,random?std::wstring(T(L"おまかせ",L"Any")):std::wstring(Solitaire::LayoutName(i)),RectF(button.X,button.GetBottom()-label-2*s,button.Width,label),14*s,white,chosen,StringAlignmentCenter,StringAlignmentCenter);
            }
            Text(g,T(L"Enter：始める　Esc：閉じる",L"Enter: start   Esc: close"),RectF(box.X,box.GetBottom()-32*s,w,26*s),14*s,soft,false,StringAlignmentCenter);
            (void)W;
        }

        void PaintEnd(Graphics& g,float s,float H)
        {
            auto& t=state.table;
            const Board& b=state.board;
            SolidBrush shade(Color(state.festival.kind==2?50:120,0,0,0));
            g.FillRectangle(&shade,0.0f,0.0f,b.area,H);
            const RectF box=EndBox(s,H);
            const float w=box.Width;
            FillRound(g,box,14*s,Color(255,40,22,22),Color(255,200,170,90),2*s);
            const Color gold(255,240,206,120),white(255,240,240,236),soft(255,200,190,186);
            float y=box.Y+22*s;
            if(t.GameMode()==Mode::Alone)
            {
                Text(g,T(L"全部取り切りました！",L"All cleared!"),RectF(box.X,y,w,50*s),34*s,gold,true,StringAlignmentCenter,StringAlignmentNear,true);
                y+=76*s;
                Text(g,Fmt(T(L"タイム {0}",L"Time {0}"),{Clock(state.clearedAfter)}),RectF(box.X,y,w,36*s),24*s,white,true,StringAlignmentCenter);
                y+=44*s;
                Text(g,Fmt(T(L"ヒント{0}回　混ぜ直し{1}回",L"Hints {0}, shuffles {1}"),{std::to_wstring(t.Hints()),std::to_wstring(t.Shuffles())}),RectF(box.X,y,w,30*s),18*s,soft,false,StringAlignmentCenter);
            }
            else
            {
                Text(g,T(L"勝負あり",L"Game over"),RectF(box.X,y,w,44*s),34*s,gold,true,StringAlignmentCenter,StringAlignmentNear,true);
                y+=70*s;
                const bool drawn=t.Winner()<0;
                const std::wstring winner=drawn?std::wstring(T(L"引き分け",L"A draw")):t.Winner()==Solitaire::You?std::wstring(T(L"あなたの勝ち！",L"You win!")):Fmt(T(L"{0}の勝ち",L"{0} wins"),{HerName()});
                Text(g,winner,RectF(box.X,y,w,48*s),32*s,gold,true,StringAlignmentCenter);
                y+=58*s;
                const std::wstring loser=t.Winner()==Solitaire::You?HerName():std::wstring(T(L"あなた",L"you"));
                Text(g,drawn?std::wstring(T(L"牌を全部取り切りました",L"All the tiles were taken")):Fmt(T(L"{0}の番に取れる組がありませんでした（残り{1}枚）",L"No pair left for {0} to take ({1} tiles remain)"),{loser,std::to_wstring(t.Left())}),
                     RectF(box.X,y,w,30*s),18*s,white,false,StringAlignmentCenter);
            }
            Text(g,T(L"Enter：もう一度　N：遊び方を変える　Esc：閉じる",L"Enter: again   N: change the game   Esc: close"),RectF(box.X,box.GetBottom()-44*s,w,30*s),16*s,soft,false,StringAlignmentCenter);
        }

        void Paint(HDC dc,const RECT& dirty)
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
            state.board=PlaceBoard(W,H,s);
            const Board& l=state.board;
            auto& t=state.table;
            const Color white(255,240,240,236),soft(255,206,196,186),gold(255,240,206,120),panel(185,40,18,16),edge(220,170,120,100);
            g.Clear(hud?Color(0,0,0,0):Color(255,16,56,40));
            if(Bitmap* layer=BoardLayer(s,cw,ch))
            {
                const CompositingMode before=g.GetCompositingMode();
                g.SetCompositingMode(CompositingModeSourceCopy);
                g.DrawImage(layer,0,0,0,0,layer->GetWidth(),layer->GetHeight(),UnitPixel);
                g.SetCompositingMode(before);
            }

            AdvanceEffects(s,H);
            // The show keeps to the tiles' side: her column stays clear.
            const RectF showArea(0,0,l.area,H);
            if(!state.choosing)
            {
                g.SetClip(showArea);
                DrawRays(g,H);
                g.ResetClip();
            }

            PaintStrip(g,s);

            // Her: name, and what she is doing, at the top of her column.
            const float hx=l.area+10*s,hw=l.her-20*s;
            {
                FillRound(g,RectF(hx,12*s,hw,70*s),10*s,panel,edge,1.2f*s);
                Text(g,HerName(),RectF(hx+12*s,18*s,hw-20*s,28*s),20*s,white,true);
                const bool together=t.GameMode()==Mode::Together&&!state.choosing;
                const std::wstring doing=!together?T(L"見ています",L"looking on"):t.Now()==Phase::HerTurn?T(L"番です",L"to take"):t.Now()==Phase::GameOver?L"":T(L"あなたの番を待っています",L"waiting for you");
                Text(g,doing,RectF(hx+12*s,48*s,hw-20*s,26*s),16*s,together&&t.Now()==Phase::HerTurn?gold:soft);
            }
            if(!hud)
            {
                const float side=std::min(hw,240*s);
                const RectF faceBox(hx+(hw-side)/2,96*s,side,side);
                FillRound(g,faceBox,10*s,Color(255,14,50,36),Color(255,120,160,130),1.5f*s);
                if(state.face)g.DrawImage(state.face.get(),faceBox.X+4*s,faceBox.Y+4*s,faceBox.Width-8*s,faceBox.Height-8*s);
                else Text(g,HerName().substr(0,1),faceBox,side*0.45f,white,true,StringAlignmentCenter,StringAlignmentCenter);
            }
            if(!state.words.text.empty())
            {
                const std::wstring& said=state.words.text;
                const WordsBubble bubble(g,said,s,std::max(120*s,hw-10*s));
                const float bw=bubble.Width(),bh=bubble.Height();
                const float top=hud&&state.headScreen.y>=0?static_cast<float>(state.headScreen.y)+0.16f*H:96*s+std::min(hw,240*s)+12*s;
                const float x=std::clamp(hx+(hw-bw)/2,l.area+4*s,W-6*s-bw),y=std::clamp(top,90*s,std::max(90*s,H-130*s-bh));
                const std::wstring shown=said.substr(0,std::min(said.size(),std::max<size_t>(1,static_cast<size_t>(state.words.shown))));
                bubble.Draw(g,x,y,y+std::min(bh*0.5f,26*s),shown);
            }
            // What to do now, early on: at the foot of her column.
            if(YourMoveNow()&&t.Taken()<3)
            {
                const RectF box(hx,H-136*s,hw,124*s);
                FillRound(g,box,10*s,panel,edge,1.2f*s);
                Wrapped(g,T(L"同じ牌を2枚続けてクリックすると取れます。取れるのは、上に何もなく、左か右が空いている牌です。四季の牌どうし、花の牌どうしは絵が違っても取れます。",
                            L"Click two matching tiles to take them. A tile can be taken with nothing on it and its left or right side open. Any season goes with any season, any flower with any flower."),
                        RectF(box.X+10*s,box.Y+8*s,box.Width-20*s,box.Height-12*s),13*s,gold);
            }
            // Nothing left to take.
            if(!state.choosing&&t.GameMode()==Mode::Alone&&t.Stuck())
            {
                const std::wstring text=T(L"取れる組がありません。戻す（Z）か混ぜ直す（S）で続けられます",L"No pairs left. Undo (Z) or shuffle (S) to go on");
                const float tw=TextWidth(g,text,20*s,true)+44*s;
                const RectF box((l.area-tw)/2,H-70*s,tw,50*s);
                FillRound(g,box,12*s,Color(245,40,22,22),Color(255,240,150,60),2*s);
                Text(g,text,box,20*s,gold,true,StringAlignmentCenter,StringAlignmentCenter);
            }
            if(!state.said.empty())
            {
                const float tw=TextWidth(g,state.said,22*s,true)+40*s;
                const RectF bubble((l.area-tw)/2,H*0.42f,tw,48*s);
                FillRound(g,bubble,14*s,Color(245,255,252,240),Color(255,230,120,60),2*s);
                Text(g,state.said,bubble,22*s,Color(255,200,40,40),true,StringAlignmentCenter,StringAlignmentCenter);
            }
            // A pair going and the fireworks under the end's panel, so it can be read; the falling
            // paper and the tiles over it.
            if(!state.choosing)
            {
                g.SetClip(showArea);
                DrawFades(g,s);
                DrawRings(g);
                DrawSparks(g,false);
                g.ResetClip();
            }
            if(EndShown())PaintEnd(g,s,H);
            if(!state.choosing)
            {
                g.SetClip(showArea);
                DrawFestivalTiles(g,s,H);
                DrawSparks(g,true);
                g.ResetClip();
            }
            if(state.choosing)PaintChoose(g,s,W,H);
            if(state.confirmQuit)
            {
                SolidBrush shade(Color(160,0,0,0));
                g.FillRectangle(&shade,0.0f,0.0f,W,H);
                const RectF box(l.area/2-220*s,ch/2.0f-70*s,440*s,140*s);
                FillRound(g,box,14*s,Color(250,40,22,22),Color(255,200,170,90),2*s);
                Text(g,T(L"やめますか？",L"Stop playing?"),RectF(box.X,box.Y+22*s,box.Width,36*s),24*s,white,true,StringAlignmentCenter);
                const float gap2=20*s,width=(box.Width-3*gap2)/2;
                const std::wstring labels[2]={T(L"やめる（Y）",L"Stop (Y)"),T(L"続ける（N）",L"Keep playing (N)")};
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

        // All the 2D layer over the 3D view shows.
        std::uint64_t Shown()
        {
            auto& t=state.table;
            ShownKey k;
            k(t.Now(),t.Version(),t.Taken(),t.Left(),t.GameMode(),t.Layout(),t.Shuffles(),t.Hints(),t.CanUndo(),t.Stuck(),t.Winner());
            k(state.choosing,state.modeChosen,state.layoutChosen,state.hover,state.cursor,state.selected,HintOn(),state.hint.a,state.hint.b,state.herPick.a,state.herPick.b);
            k(Waiting(),state.seen.stuck,state.said,state.confirmQuit,state.clearedAfter);
            if(t.GameMode()==Mode::Alone&&state.startedAt)k((GetTickCount64()-state.startedAt)/1000);   // the clock
            // What moves by itself: while any of it is about, every frame.
            const auto& f=state.festival;
            if(!state.fades.empty()||!state.rings.empty()||!state.sparks2D.empty()||(f.kind&&(f.burstsDone<f.bursts.size()||FxNow()-f.at<FestivalMoves)))k(FxNow());
            k.Words(state.words.text,state.words.shown,0,static_cast<float>(state.headScreen.y),2*Scale());
            return k.Value();
        }

        // Over the 3D view the 2D layer is drawn from the frame (TickSolitaireWindow), and only when what it shows changed:
        // from WM_PAINT on every timer tick, a paint slower than the tick (a maximized window) kept
        // the main loop draining messages, and no frame came at all, for any character.
        void DrawOverlay()
        {
            const std::uint64_t shown=Shown();
            if((!state.overlayDue&&shown==state.shown)||IsIconic(state.window))return;
            state.shown=shown;
            state.overlayDue=false;
            RECT client{};
            GetClientRect(state.window,&client);
            Paint(nullptr,client);
        }

        void Back()
        {
            state.selected=-1;
            state.hint={};
        }

        void Quit()
        {
            state.confirmQuit=false;
            Finish();
        }

        void ChooseLayout(int layout)
        {
            const int choices=Solitaire::LayoutCount()+1;
            layout=(layout+choices)%choices;
            state.layoutChosen=layout;
            // Before any game, the deal behind this screen shows the stack chosen.
            if(!state.playedAny&&layout<Solitaire::LayoutCount())Deal(Mode::Alone,layout);
            Redraw();
        }

        void Continue()
        {
            auto& t=state.table;
            if(t.GameMode()==Mode::Together)state.lost=t.Winner()==Solitaire::Her?state.lost+1:0;
            state.modeChosen=t.GameMode();
            if(state.layoutChosen<Solitaire::LayoutCount())state.layoutChosen=t.Layout();   // (at random: again)
            StartGame();
        }

        // The arrow keys: the nearest free tile that way.
        void MoveCursor(WPARAM key)
        {
            auto& t=state.table;
            std::vector<int> free;
            for(int i=0;i<Tiles;++i)if(t.Free(i))free.push_back(i);
            if(free.empty())return;
            state.hover=-1;
            if(state.cursor<0||!t.Free(state.cursor)){state.cursor=state.selected>=0?state.selected:free[free.size()/2];Redraw();return;}
            const RectF at=TileBox(state.cursor);
            const float dx=key==VK_LEFT?-1.0f:key==VK_RIGHT?1.0f:0.0f,dy=key==VK_UP?-1.0f:key==VK_DOWN?1.0f:0.0f;
            int best=-1;
            float bestScore=1e9f;
            for(int i:free)
            {
                if(i==state.cursor)continue;
                const RectF to=TileBox(i);
                const float ox=to.X-at.X,oy=to.Y-at.Y,along=ox*dx+oy*dy;
                if(along<=1)continue;
                const float across=std::abs(ox*dy)+std::abs(oy*dx),score=along+2.5f*across;
                if(score<bestScore){bestScore=score;best=i;}
            }
            if(best>=0)state.cursor=best;
            Redraw();
        }

        void OnKey(WPARAM key)
        {
            auto& t=state.table;
            if(key==VK_F1){state.camera={};return;}
            if(state.confirmQuit)
            {
                if(key=='Y')Quit();
                else if(key=='N'||key==VK_ESCAPE){state.confirmQuit=false;Redraw();}
                return;
            }
            if(state.choosing)
            {
                if(key=='1'){state.modeChosen=Mode::Alone;StartGame();}
                else if(key=='2'){state.modeChosen=Mode::Together;StartGame();}
                else if(key==VK_RETURN||key==VK_SPACE)StartGame();
                else if(key==VK_LEFT)ChooseLayout(state.layoutChosen-1);
                else if(key==VK_RIGHT)ChooseLayout(state.layoutChosen+1);
                else if(key==VK_UP)ChooseLayout(state.layoutChosen-4);
                else if(key==VK_DOWN)ChooseLayout(state.layoutChosen+4);
                else if(key==VK_TAB){state.modeChosen=state.modeChosen==Mode::Alone?Mode::Together:Mode::Alone;Redraw();}
                else if(key==VK_ESCAPE)
                {
                    if(state.playedAny&&t.Now()!=Phase::GameOver){state.choosing=false;Redraw();}
                    else Finish();
                }
                return;
            }
            if(t.Now()==Phase::GameOver)
            {
                if((key==VK_RETURN||key==VK_SPACE)&&EndShown())Continue();
                else if(key=='N'&&EndShown())DoAction(Fresh);
                else if(key==VK_ESCAPE)Finish();
                return;
            }
            if(key==VK_ESCAPE)
            {
                if(state.selected>=0)state.selected=-1;
                else state.confirmQuit=true;
                Redraw();
                return;
            }
            if(key=='N')DoAction(Fresh);
            if(!YourMoveNow())return;
            if(key=='H')DoAction(Hint);
            else if(key=='Z'||key==VK_BACK)DoAction(Undo);
            else if(key=='S')DoAction(Shuffle);
            else if(key==VK_LEFT||key==VK_RIGHT||key==VK_UP||key==VK_DOWN)MoveCursor(key);
            else if((key==VK_RETURN||key==VK_SPACE)&&state.cursor>=0)Click(state.cursor);
        }

        LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            switch(message)
            {
            case WM_MOUSEWHEEL:
                if(state.view)
                {
                    POINT at{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
                    RECT r{};
                    GetWindowRect(state.view,&r);
                    if(PtInRect(&r,at))return ViewProc(state.view,message,wParam,lParam);
                }
                break;
            case WM_KEYDOWN:
                if(!(lParam&(1<<30)))OnKey(wParam);
                return 0;
            case WM_TIMER:
                Tick();
                return 0;
            case WM_MOUSEMOVE:
            {
                const float x=static_cast<float>(GET_X_LPARAM(lParam)),y=static_cast<float>(GET_Y_LPARAM(lParam));
                const int at=state.choosing||x>=state.board.area?-1:TileAt(x,y);
                if(at!=state.hover){state.hover=at;state.cursor=-1;Redraw();}
                return 0;
            }
            case WM_LBUTTONDOWN:
            {
                const float x=static_cast<float>(GET_X_LPARAM(lParam)),y=static_cast<float>(GET_Y_LPARAM(lParam));
                auto& t=state.table;
                if(state.confirmQuit)
                {
                    if(state.quitButtons[0].Contains(x,y))Quit();
                    else if(state.quitButtons[1].Contains(x,y)){state.confirmQuit=false;Redraw();}
                    return 0;
                }
                if(state.choosing)
                {
                    for(int i=0;i<2;++i)
                        if(state.modeButtons[i].Contains(x,y)){state.modeChosen=i?Mode::Together:Mode::Alone;StartGame();return 0;}
                    for(int i=0;i<=Solitaire::LayoutCount();++i)
                        if(state.layoutButtons[static_cast<size_t>(i)].Contains(x,y)){ChooseLayout(i);return 0;}
                    return 0;
                }
                if(t.Now()==Phase::GameOver){if(EndShown())Continue();return 0;}
                for(int i=0;i<ActionCount;++i)
                    if(state.actionButtons[static_cast<size_t>(i)].Width>0&&state.actionButtons[static_cast<size_t>(i)].Contains(x,y)){DoAction(i);return 0;}
                if(x<state.board.area)Click(TileAt(x,y));
                return 0;
            }
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC dc=BeginPaint(hwnd,&ps);
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
                state.window=nullptr;
                ReleaseView();
                state.face.reset();
                state.canvas.reset();
                state.layer.reset();
                state.layerKey.clear();
                state.sprites.clear();
                state.thumbs.clear();
                return 0;
            }
            return DefWindowProcW(hwnd,message,wParam,lParam);
        }
    }

    bool SolitaireWindowOpen(){return state.window!=nullptr;}
    HWND SolitaireWindowHandle(){return state.window;}
    bool SolitaireWindowRect(RECT& r)
    {
        return state.window&&IsWindowVisible(state.window)&&!IsIconic(state.window)&&GetWindowRect(state.window,&r);
    }

    namespace
    {
        // Diagnostics: pairs taken for both (or for you).
        void PlaySimply(int pairs)
        {
            auto& t=state.table;
            for(int i=0;i<pairs&&t.Now()!=Phase::GameOver;++i)
            {
                if(t.Now()==Phase::HerTurn)t.Step();
                else if(t.GameMode()==Mode::Together)t.PlayForYou(false);
                else
                {
                    const auto all=t.Pairs();
                    if(all.empty())break;
                    t.Take(all[0]);
                }
            }
            state.nextAt=0;
            state.hover=state.cursor=state.selected=-1;
        }
    }

    void SolitaireLines(const std::wstring& answer)
    {
        state.prefetching=false;
        if(!state.window)return;
        state.lines.Answered(answer);
        Prefetch();
    }

    bool SolitaireAfterword(std::wstring& situation,std::wstring& fallback)
    {
        if(state.afterword.empty())return false;
        situation=std::move(state.afterword);
        fallback=std::move(state.afterwordLine);
        state.afterword.clear();
        state.afterwordLine.clear();
        return true;
    }

    void SolitaireHeard(const std::wstring& line,bool done)
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

    void PlaySolitaireMoves(int pairs,float yaw,bool together,int layout)
    {
        if(!state.window)return;
        state.modeChosen=together?Mode::Together:Mode::Alone;
        state.layoutChosen=layout;
        StartGame();
        PlaySimply(pairs);
        state.camera.yaw+=yaw;
        Redraw();
    }

    namespace
    {
        // Her light pat from above toward a spot on the table (as at mahjong).
        bool ReachFor(MascotModel& model,const std::array<float,3>& spot,bool tap=true)
        {
            std::array<float,3> head{};
            if(!model.HeadPosition(head))return false;
            const float hx=head[0]-model.RootOffset()+state.herX,hz=head[2]+state.herZ;
            const std::array<float,3> from{spot[0]-hx,spot[1]+0.07f*state.hips-head[1],spot[2]-hz};
            return model.Reach(spot[0]>0.25f*state.upper?0:1,from,tap?1.65:3.0,tap);
        }

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

    void TickSolitaireWindow(MascotModel& model,double dt)
    {
        if(!state.window||!state.renderer)return;
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
            // A built-in motion begun while she stood up again must not come along (one that travels would carry her aside).
            model.SettlePose();
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
            if(left-go<=0.001f*D){state.herX=standX;state.herZ=standZ;model.SetActivity(Activity::Idle);model.EndGestures();state.entrance=2;state.entranceTime=0;}
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
                // As she sits she reaches down for the seat's sides, for moving in with it.
                model.SetChairGrip(true,0.34f*state.upper,-0.055f*state.upper,0.12f*state.upper-SeatFront());
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
            if(u>=1){state.entrance=5;model.SetChairGrip(false);}
        }
        else if(state.entrance==5){standing=0;state.herX=0;state.herZ=state.chairZ=seatZ;}
        state.clock+=dt;
        if(auto scene=std::vector<int>{1};scene!=state.scene){state.scene=std::move(scene);r.SetGeometry(BuildTable());}
        // Her hand goes out toward where her pair lies, once she has chosen.
        if(t.Now()==Phase::HerTurn&&!state.choosing&&state.entrance==5&&state.plannedFor==t.Version()&&state.reachedFor!=t.Version()&&
           t.Thought()&&GetTickCount64()+ReachLead>=state.nextAt)
        {
            const Pair m=t.HerNext();
            bool done=true;
            if(m.Valid())
            {
                const auto& p=t.Spots()[static_cast<size_t>(m.a)];
                const float across=std::clamp((p.x-14)/14.0f,-1.0f,1.0f);
                done=ReachFor(model,{-across*0.55f*D,Top()+0.03f*D,FieldZ()+0.4f*D});
            }
            if(done)state.reachedFor=t.Version();
        }
        if(state.thinkFor>0&&state.entrance==5){model.EmoteSeated(L"ponder",state.thinkFor);state.thinkFor=0;}
        {
            const bool over=t.Now()==Phase::GameOver&&!state.choosing;
            if(over&&!state.seenOver)
            {
                std::uniform_real_distribution<double> any(0,1);
                const bool glad=t.GameMode()==Mode::Alone||t.Winner()==Solitaire::Her;
                state.endGesture=glad?(any(state.sparkRandom)<0.5?L"cheeks":L"clap"):t.Winner()<0?L"ponder":L"oops";
                state.endGestureUntil=state.clock+1.5;
            }
            state.seenOver=over;
            if(!state.endGesture.empty()&&state.entrance==5&&(model.EmoteSeated(state.endGesture,3.2)||state.clock>state.endGestureUntil))
                state.endGesture.clear();
        }
        // What moves: her chair.
        {
            Mesh moving;
            moving.out=BuildChair(state.chairZ);
            r.SetMovingGeometry(std::move(moving.out));
        }
        r.SetParticles(state.roomGlows);
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
        DrawOverlay();
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

    std::wstring DescribeSolitaire()
    {
        if(!state.window)return L"";
        auto& t=state.table;
        std::wstring out=Fmt(L"solitaire: entrance {0}, phase {1}, mode {2}, layout {3}, left {4}, pairs open {5}, taken {6}, winner {7}, her read {8}, choosing {9}\n",
                             {std::to_wstring(state.entrance),std::to_wstring(static_cast<int>(t.Now())),t.GameMode()==Mode::Alone?L"alone":L"together",std::to_wstring(t.Layout()),
                              std::to_wstring(t.Left()),std::to_wstring(t.Pairs().size()),std::to_wstring(t.Taken()),std::to_wstring(t.Winner()),std::to_wstring(t.Certain()),state.choosing?L"yes":L"no"});
        return out+(state.words.told.empty()?std::wstring():L"solitaire words:\n"+state.words.told);
    }

    bool SaveSolitaireView(const std::wstring& path)
    {
        return state.renderer&&state.renderer->SaveLastFrame(path);
    }

    bool SaveSolitaireShot(const std::wstring& path,int pairs,std::uint32_t seed,const std::wstring& what,int layout)
    {
        GdiplusStartupInput input;
        if(!state.gdiplus)GdiplusStartup(&state.gdiplus,&input,nullptr);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc=DefWindowProcW;
        wc.hInstance=GetModuleHandleW(nullptr);
        wc.lpszClassName=L"MikuMikuDesktopPerchSolitaireShot";
        RegisterClassExW(&wc);
        const int dpi=static_cast<int>(GetDpiForSystem());
        state.window=CreateWindowExW(0,wc.lpszClassName,L"",WS_OVERLAPPEDWINDOW,0,0,MulDiv(static_cast<int>(BaseWidth)+16,dpi,96),MulDiv(static_cast<int>(BaseHeight)+39,dpi,96),
                                     nullptr,nullptr,wc.hInstance,nullptr);
        state.her=SolitaireOpponent{L"ミク"};
        // fx<style>:<ms> a pair going that long ago; clear:<ms>, win:<ms> the show that far on.
        std::wstring kind=what;
        int moment=0;
        if(const auto colon=kind.find(L':');colon!=std::wstring::npos){moment=_wtoi(kind.c_str()+colon+1);kind.resize(colon);}
        const bool together=what==L"together"||kind==L"win";
        state.layoutChosen=layout;
        state.modeChosen=together?Mode::Together:Mode::Alone;
        state.playedAny=what!=L"choose";
        state.choosing=what==L"choose";
        state.table.NewGame(seed,state.modeChosen,layout,seed%2?Solitaire::Her:Solitaire::You,Care);
        state.startedAt=GetTickCount64()-83000;
        if(what==L"end"||what==L"stuck")
        {
            // Taken by the solution, to the end; or at random until no pair is left.
            if(what==L"end"){for(const Pair& p:std::vector<Pair>(state.table.Solution()))state.table.Take(p);state.clearedAfter=83000;}
            else {std::mt19937 any(seed);while(!state.table.Stuck()&&state.table.Now()!=Phase::GameOver){const auto all=state.table.Pairs();state.table.Take(all[any()%all.size()]);}}
        }
        else if(what==L"hint")
        {
            PlaySimply(pairs);
            state.hint=state.table.Hint();
            state.hintUntil=GetTickCount64()+60000;
        }
        else PlaySimply(pairs);
        if(what==L"alone"&&state.table.Now()==Phase::YourMove)
        {
            for(int i=0;i<Tiles&&state.selected<0;++i)if(state.table.Free(i))state.selected=i;
        }
        state.nextAt=0;
        if(!SampleWords().empty()){state.words.text=SampleWords();state.words.shown=1e9;}
        if(kind.rfind(L"fx",0)==0||kind==L"clear"||kind==L"win")
        {
            RECT area{};
            GetClientRect(state.window,&area);
            const float s=Scale(),H=static_cast<float>(area.bottom);
            state.board=PlaceBoard(static_cast<float>(area.right),H,s);
            state.fades.clear();state.sparks2D.clear();state.rings.clear();state.festival={};
            state.fxRandom.seed(seed);
            state.selected=-1;
            state.words.text.clear();
            if(kind.rfind(L"fx",0)==0)
            {
                state.forceStyle=_wtoi(kind.c_str()+2);
                const auto all=state.table.Pairs();
                if(!all.empty())
                {
                    const int a=state.table.FaceAt(all[0].a),b=state.table.FaceAt(all[0].b);
                    state.table.Take(all[0]);
                    SeeOff(all[0],a,b,Color(255,255,216,110));
                }
                state.forceStyle=-1;
            }
            else
            {
                if(kind==L"clear")for(const Pair& p:std::vector<Pair>(state.table.Solution()))state.table.Take(p);
                state.clearedAfter=83000;
                StartFestival(kind==L"clear"?2:1);
                if(static_cast<ULONGLONG>(moment)<ClearedShowFirst)state.nextAt=GetTickCount64()+60000;
                if(kind==L"win")state.nextAt=GetTickCount64()+60000;
            }
            state.sparksAt=FxNow();
            for(int done=0;done<moment;done+=40){state.fxShift+=40;AdvanceEffects(s,H);}
        }
        // click: pairs taken by clicks where they are drawn (<out.png>.txt says how it went); the
        // tile under the middle of a free tile's face must be that tile.
        if(what==L"click")
        {
            RECT area{};
            GetClientRect(state.window,&area);
            state.board=PlaceBoard(static_cast<float>(area.right),static_cast<float>(area.bottom),Scale());
            int picked=0,taken=0;
            const auto clickAt=[](const RectF& box){
                const LPARAM at=MAKELPARAM(static_cast<int>(box.X+box.Width/2),static_cast<int>(box.Y+box.Height/2));
                Proc(state.window,WM_LBUTTONDOWN,MK_LBUTTON,at);
            };
            for(int i=0;i<std::max(pairs,1);++i)
            {
                const auto all=state.table.Pairs();
                if(all.empty())break;
                const Pair p=all[0];
                const int before=state.table.Taken();
                clickAt(TileBox(p.a));
                if(state.selected==p.a)++picked;
                clickAt(TileBox(p.b));
                if(state.table.Taken()==before+1)++taken;
                state.nextAt=0;
            }
            if(FILE* f=nullptr;_wfopen_s(&f,(path+L".txt").c_str(),L"w")==0&&f)
            {
                fwprintf(f,L"clicked %d, chosen %d, taken %d, left %d\n",std::max(pairs,1),picked,taken,state.table.Left());
                fclose(f);
            }
        }
        RECT client{};
        GetClientRect(state.window,&client);
        Bitmap shot(client.right,client.bottom,PixelFormat32bppARGB);
        {
            Graphics g(&shot);
            const HDC dc=g.GetHDC();
            Paint(dc,client);
            g.ReleaseHDC(dc);
        }
        // How long a change to the tiles takes to draw (they are drawn over only when one changes).
        {
            state.layerKey.clear();
            const auto from=GetTickCount64();
            for(int i=0;i<10;++i){state.layerKey.clear();BoardLayer(Scale(),client.right,client.bottom);}
            Log(L"solitaire: tiles drawn in %.1f ms",(GetTickCount64()-from)/10.0);
        }
        state.fxShift=0;
        DestroyWindow(state.window);
        state.window=nullptr;
        return SavePng(shot,path);
    }

    void ShowSolitaireWindow(HWND owner,const SolitaireOpponent& her,std::function<void()> onEnd)
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
        state.heardVersion=-1;
        state.nextFirst=-1;
        state.following=false;
        state.lines.Load(L"SOLITAIRELINES",L"solitaire",her.ownLinesFile,her.linesFile,her.styleKey,English(),
                         {L"麻雀ソリティア",L"Mahjong solitaire",L"",L""});
        state.voice.Stop();
        state.tell=Expression::Neutral;
        state.prefetching=false;
        state.words={};
        state.words.told=state.lines.Report();
        if(!her.linesFile.empty())ShowLinesProblem(owner,state.lines);   // (none kept: a diagnostic run, which must not stop)
        state.choosing=true;
        state.playedAny=false;
        state.confirmQuit=false;
        state.seen={};
        state.afterword.clear();
        state.afterwordLine.clear();
        state.modeChosen=Mode::Alone;
        Deal(Mode::Alone,state.layoutChosen);
        Prefetch();

        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&info);
        const RECT work=info.rcWork;
        const int dpi=static_cast<int>(GetDpiForSystem());
        const int w=std::min(static_cast<int>(work.right-work.left)*9/10,MulDiv(static_cast<int>(BaseWidth)+16,dpi,96));
        const int h=std::min(static_cast<int>(work.bottom-work.top)*9/10,MulDiv(static_cast<int>(BaseHeight)+39,dpi,96));
        const std::wstring title=std::wstring(T(L"麻雀ソリティア",L"Mahjong solitaire"))+L" - "+HerName();
        state.window=CreateWindowExW(0,ClassName,title.c_str(),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                                     work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,
                                     nullptr,nullptr,instance,nullptr);
        if(CreateView())PlaceView();
        SetTimer(state.window,TimerId,40,nullptr);
        ShowWindow(state.window,SW_SHOWNORMAL);
        SetForegroundWindow(state.window);
    }
}
