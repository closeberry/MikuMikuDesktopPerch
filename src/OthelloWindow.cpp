#define NOMINMAX
#include "OthelloWindow.hpp"
#include "Othello.hpp"
#include "MahjongSound.hpp"
#include "Log.hpp"
#include "MascotModel.hpp"
#include "MascotRenderer.hpp"
#include "Text.hpp"
#include "BubbleText.hpp"
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
#pragma comment(lib,"gdiplus.lib")
#pragma comment(lib,"shlwapi.lib")

namespace MikuMikuDesktopPerch
{
    namespace
    {
        using namespace Gdiplus;
        using Othello::Phase;
        using Othello::Squares;
        constexpr wchar_t ClassName[]=L"MikuMikuDesktopPerchOthello";
        constexpr wchar_t ViewClassName[]=L"MikuMikuDesktopPerchOthelloView";
        constexpr UINT_PTR TimerId=1;
        // How carefully she plays (Othello::Table::NewGame): a little less as you grow closer,
        // and after games you lost in a row.
        constexpr double Care=0.85,LessPerBond=0.03,LessPerLoss=0.08,LeastCare=0.4;
        // Her pace: before her disc (in 2D), before the game's end is shown.
        constexpr ULONGLONG HerPlayWait=700,EndWait=900;
        constexpr ULONGLONG SaidShown=1800;
        constexpr ULONGLONG ReachLead=600;         // her hand starts out this long before her disc goes down
        constexpr ULONGLONG HerPlayWait3D=1100;    // at the 3D table: time for her hand to go out
        constexpr ULONGLONG ThinkExtra=2000;       // her disc later by this when she stops to think
        constexpr ULONGLONG EndWait3D=2200;        // at the 3D table: the end waits for her reaction
        // The discs on their way at the 3D table (seconds): one laid drops onto its square; those
        // it turns over turn each this much after the one before, outward from it.
        constexpr double Drop=0.24,Turn=0.30,Ripple=0.07,Opening=0.12;
        // Sizes in logical pixels (at 96 dpi, at the size the window opens).
        constexpr float BaseWidth=1180,BaseHeight=840;
        constexpr float Cell2D=62;   // a square of the 2D board

        // A disc at the 3D table (world units): its middle, and how far it is turned over (0: the
        // black face up, 1: the white face up).
        struct Disc {float x=0,y=0,z=0,turn=0;bool shown=false;};
        // A disc on its way: where it is at each time (seconds of the clock); arc: lifted on the way.
        struct Flight {int square=-1;std::vector<std::pair<double,Disc>> keys;bool arc=true;};

        struct State
        {
            HWND window=nullptr;
            ULONG_PTR gdiplus=0;
            OthelloOpponent her;
            std::unique_ptr<Bitmap> face;
            std::function<void()> onEnd;
            Othello::Table table;
            int lost=0;                // games you lost in a row
            int nextBlack=-1;          // who has black next game (-1: drawn by lot)
            bool gameOverTold=false;
            int hover=-1,cursor=-1;    // squares: under the mouse, and the keys' choice
            bool confirmQuit=false;
            RectF quitButtons[2];
            ULONGLONG nextAt=0;
            int passesSeen=0;
            std::wstring said;         // a pass, in the big bubble
            ULONGLONG saidAt=0;
            // Her words in the bubble (as at mahjong).
            struct Words
            {
                std::wstring text,fallback;
                ULONGLONG since=0,until=0,asked=0;
                bool waiting=false;
                int weight=0;
                std::vector<std::wstring> recent;
                std::wstring revealing;
                double shown=0;
                ULONGLONG shownAt=0;
                std::wstring told;
            } words;
            std::vector<std::wstring> goodLines,thinkLines;
            bool prefetching=false;
            POINT headScreen{-1,-1},besideHead{-1,-1};
            std::wstring afterword,afterwordLine;
            struct Seen
            {
                bool started=false,over=false;
                int moves=0,passes=0;
            } seen;
            std::array<RectF,Squares> cellBoxes{};                 // the 2D board's squares
            std::array<std::array<POINT,4>,Squares> cellQuads{};   // the 3D board's, on the screen
            bool quadsSet=false;
            std::unique_ptr<Bitmap> canvas;
            // Her at the table in 3D (none: 2D only).
            HWND view=nullptr;
            std::unique_ptr<MascotRenderer> renderer;
            int entrance=0;
            float herX=0,herZ=0,herAngle=0,entranceTime=0,chairZ=0;
            struct Camera {float yaw=0,pitch=0.46f,distance=3.0f,x=0,y=0,z=0;} camera;
            POINT dragFrom{};
            int dragging=0;
            float upper=10,hips=10;
            float floorY=-5;
            std::vector<int> scene;
            // Where each disc lies (world units), as the board is now.
            std::array<Disc,Squares> discs{};
            // The discs on their way, and until when (on the clock); sounds due on the way.
            std::vector<Flight> flights;
            double flightsUntil=0;
            std::vector<std::pair<double,MahjongSound>> sounds;
            int movesSeen=-1;
            double sparkAt=0;         // gold sparkles then, where her disc took a corner
            int sparkSquare=-1;
            double clock=0;
            int plannedFor=-1;        // the move count her turn's wait was last set at
            int reachedFor=-1;        // and her hand last went out at
            struct Spark {std::array<float,3> at,velocity;float age,life,size,angle,spin;std::array<float,4> color;bool glow;};
            std::vector<Spark> sparks;
            std::vector<MascotRenderer::Particle> roomGlows;
            std::mt19937 sparkRandom{12345};
            double thinkFor=0;
            std::wstring endGesture;
            double endGestureUntil=0;
            bool seenOver=false;
            int heardMoves=-1;        // in 2D, the move last heard (its sound)
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
        std::wstring Discs(int n){return std::to_wstring(n)+T(L"枚",L" discs");}
        // Who has which colour, in words.
        std::wstring ColourOf(int who){return state.table.Black()==who?T(L"黒",L"black"):T(L"白",L"white");}

        bool Waiting(){return state.nextAt>GetTickCount64();}
        // The discs still on their way at the 3D table (a move not yet shown counts).
        bool Flying()
        {
            return state.renderer&&(state.movesSeen!=state.table.Moves()||state.clock<state.flightsUntil);
        }
        bool YourMoveNow(){return state.table.Now()==Phase::YourMove&&!Waiting()&&!Flying();}
        bool EndShown(){return state.table.Now()==Phase::GameOver&&!Waiting()&&!Flying();}

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
            // puts it back. A left click is the window's (a square, a button).
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
                Log(L"othello: resize: %s",error.c_str());
            InvalidateRect(state.window,nullptr,FALSE);
        }

        void ReleaseView()
        {
            if(state.renderer){state.renderer->WaitIdle();state.renderer.reset();Log(L"othello: 3D view released");}
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


        // The scroll's painting for the 3D table's texture, and its mipmaps.
        ImageRgba8 ScrollImage()
        {
            ImageRgba8 image;
            Bitmap painting(ScrollW,ScrollH,PixelFormat32bppARGB);
            {
                Graphics p(&painting);
                float h0,h1,width;
                ScrollSpan(h0,h1,width);
                const int tall=std::max(ScrollW,static_cast<int>(std::lround(ScrollW*(h1-h0)/std::max(width,0.001f))));
                p.ScaleTransform(1.0f,static_cast<float>(ScrollH)/tall);
                PaintScroll(p,ScrollW,tall);
            }
            BitmapData bits{};
            Rect whole(0,0,ScrollW,ScrollH);
            if(painting.LockBits(&whole,ImageLockModeRead,PixelFormat32bppARGB,&bits)!=Ok)return image;
            image.levels.resize(1);
            auto& level=image.levels[0];
            level.width=ScrollW;level.height=ScrollH;
            level.pixels.resize(static_cast<size_t>(ScrollW)*ScrollH*4);
            for(int y=0;y<ScrollH;++y)
            {
                const BYTE* from=static_cast<const BYTE*>(bits.Scan0)+static_cast<size_t>(y)*bits.Stride;
                BYTE* to=level.pixels.data()+static_cast<size_t>(y)*ScrollW*4;
                for(int x=0;x<ScrollW;++x,from+=4,to+=4){to[0]=from[2];to[1]=from[1];to[2]=from[0];to[3]=255;}
            }
            painting.UnlockBits(&bits);
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
                Log(L"othello: no 3D view (%s)",error.c_str());
                renderer->WaitIdle();
                DestroyWindow(state.view);
                state.view=nullptr;
                return false;
            }
            renderer->SetLight(20,45);
            std::array<float,3> lo{},hi{},legLo{},legHi{},head{};
            if(state.her.model->BoneBounds(lo,hi)&&state.her.model->LegBounds(legLo,legHi)&&state.her.model->HeadPosition(head)&&head[1]>legHi[1])
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
            Log(L"othello: 3D view %ux%u on %s (hips %.1f, upper body %.1f, floor %.1f)",w,h,renderer->AdapterName().c_str(),state.hips,state.upper,state.floorY);
            if(!renderer->SetGeometryTexture(ScrollImage()))Log(L"othello: no texture for the 3D table");
            state.renderer=std::move(renderer);
            state.entrance=0;
            state.scene.clear();
            state.camera={};
            state.dragging=0;
            state.sparks.clear();
            state.flights.clear();
            state.flightsUntil=0;
            state.sounds.clear();
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
            const float u0=1.0f/ScrollW,u1=1-1.0f/ScrollW,v0=1.0f/ScrollH,v1=1-1.0f/ScrollH;
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

        // ---- The board and the discs ----
        // The board lies in the middle of the felt, row 0 at her side and row 7 at yours, column
        // 0 at your left; each side's discs in a pile at its right.

        float Top(){return 0.12f*state.hips+0.30f*state.upper;}
        float BoardZ(){return -1.10f*state.upper;}
        float FieldZ(){return BoardZ();}
        float Cell(){return 0.128f*state.upper;}
        float BoardTop(){return Top()+0.022f*state.upper;}
        float DiscR(){return 0.42f*Cell();}
        float DiscH(){return 0.11f*Cell();}
        float SquareX(int s){return (s%8-3.5f)*Cell();}
        float SquareZ(int s){return BoardZ()+(3.5f-s/8)*Cell();}
        std::array<float,3> SquareSpot(int s){return {SquareX(s),BoardTop(),SquareZ(s)};}
        // Where each side's pile of discs stands.
        std::array<float,2> PileAt(int who)
        {
            const float D=state.upper;
            return who==Othello::You?std::array<float,2>{0.86f*D,BoardZ()-0.30f*D}:std::array<float,2>{-0.86f*D,BoardZ()+0.30f*D};
        }

        Disc Resting(int square,int owner)
        {
            Disc d;
            d.x=SquareX(square);
            d.y=BoardTop()+DiscH()/2;
            d.z=SquareZ(square);
            d.turn=owner==state.table.Black()?0.0f:1.0f;
            d.shown=true;
            return d;
        }

        // Where every disc is, as the board is now (state.discs).
        void PlaceDiscs()
        {
            for(int s=0;s<Squares;++s)
            {
                const int owner=state.table.At(s);
                state.discs[static_cast<size_t>(s)]=owner<0?Disc{}:Resting(s,owner);
            }
        }

        enum class Mark {None,Flips,Last};

        // A disc at d: its black face and white face (black up when turn is 0), and its edge,
        // thick (0: a disc's own thickness). mark: a little spot on its top face, gold where it
        // would be turned over, red on the disc laid last.
        void AddDisc(Mesh& m,const Disc& d,Mark mark=Mark::None,float thick=0)
        {
            using V=std::array<float,3>;
            using namespace DirectX;
            constexpr int N=24;
            const float r=DiscR(),h=(thick>0?thick:DiscH())/2;
            const XMMATRIX turn=XMMatrixRotationX(d.turn*XM_PI);
            auto at=[&](float x,float y,float z)->V{
                const XMVECTOR q=XMVector3TransformNormal(XMVectorSet(x,y,z,0),turn);
                return {d.x+XMVectorGetX(q),d.y+XMVectorGetY(q),d.z+XMVectorGetZ(q)};
            };
            auto dir=[&](float x,float y,float z)->V{
                const XMVECTOR q=XMVector3TransformNormal(XMVectorSet(x,y,z,0),turn);
                return {XMVectorGetX(q),XMVectorGetY(q),XMVectorGetZ(q)};
            };
            const float black[4]={0.06f,0.06f,0.065f,1},white[4]={0.90f,0.89f,0.85f,1},rim[4]={0.20f,0.20f,0.21f,1},gold[4]={1.0f,0.80f,0.32f,1};
            for(int i=0;i<N;++i)
            {
                const float a0=i*XM_2PI/N,a1=(i+1)*XM_2PI/N;
                const float c0=std::cos(a0)*r,s0=std::sin(a0)*r,c1=std::cos(a1)*r,s1=std::sin(a1)*r;
                m.Quad(at(0,h,0),at(c0,h,s0),at(c1,h,s1),at(0,h,0),dir(0,1,0),black);
                m.Quad(at(0,-h,0),at(c1,-h,s1),at(c0,-h,s0),at(0,-h,0),dir(0,-1,0),white);
                const V n=dir(std::cos((a0+a1)/2),0,std::sin((a0+a1)/2));
                m.Quad(at(c0,0,s0),at(c1,0,s1),at(c1,h,s1),at(c0,h,s0),n,rim);
                m.Quad(at(c0,-h,s0),at(c1,-h,s1),at(c1,0,s1),at(c0,0,s0),n,white);
            }
            if(mark!=Mark::None)
            {
                // On whichever face is up now, round.
                const float red[4]={0.86f,0.22f,0.18f,1};
                const float up=d.turn<0.5f?h:-h,y=up+(up>0?0.0006f:-0.0006f)*state.upper,k=(mark==Mark::Last?0.16f:0.22f)*r;
                for(int i=0;i<12;++i)
                {
                    const float a0=i*XM_2PI/12,a1=(i+1)*XM_2PI/12;
                    m.Quad(at(0,y,0),at(std::cos(a0)*k,y,std::sin(a0)*k),at(std::cos(a1)*k,y,std::sin(a1)*k),at(0,y,0),dir(0,up>0?1.0f:-1.0f,0),mark==Mark::Last?red:gold);
                }
            }
        }

        // A flat round spot on the board (the squares you can lay on).
        void AddSpot(Mesh& m,float x,float z,float r,const float (&colour)[4])
        {
            constexpr int N=16;
            const float y=BoardTop()+0.0008f*state.upper;
            for(int i=0;i<N;++i)
            {
                const float a0=i*DirectX::XM_2PI/N,a1=(i+1)*DirectX::XM_2PI/N;
                m.Quad({x,y,z},{x+std::cos(a0)*r,y,z+std::sin(a0)*r},{x+std::cos(a1)*r,y,z+std::sin(a1)*r},{x,y,z},{0,1,0},colour);
            }
        }

        bool InFlight(int square)
        {
            return std::any_of(state.flights.begin(),state.flights.end(),[&](const Flight& f){return f.square==square;});
        }

        // The square pointed at (the mouse, or the keys' choice), when you can lay there now.
        int Aimed()
        {
            if(!YourMoveNow())return -1;
            const int at=state.hover>=0?state.hover:state.cursor;
            return state.table.CanPlay(at)?at:-1;
        }

        // What the table's geometry shows.
        std::vector<int> Scene()
        {
            auto& t=state.table;
            std::vector<int> out;
            for(int s=0;s<Squares;++s)out.push_back(t.At(s));
            out.push_back(t.Moves());
            out.push_back(state.movesSeen);
            out.push_back(YourMoveNow()?1:0);
            out.push_back(Aimed());
            out.push_back(YourMoveNow()?state.cursor:-9);
            for(const auto& f:state.flights)out.push_back(1000+f.square);
            return out;
        }

        std::vector<MascotRenderer::GeometryVertex> BuildTable()
        {
            Mesh mesh;
            const float D=state.upper;
            const float felt[4]={0.09f,0.33f,0.22f,1};
            const TableShape t=Table3D();
            BuildRoom(mesh,FieldZ());
            mesh.Box(t.x0,t.top-0.05f*D,t.zFar,t.x1,t.top,t.zNear,felt);
            // The board: a wooden frame, the green field, its lines and four dots.
            const float half=4*Cell(),rim=0.035f*D,bz=BoardZ(),bt=BoardTop();
            const float frame[4]={0.30f,0.17f,0.08f,Wood},green[4]={0.05f,0.40f,0.20f,1},line[4]={0.02f,0.10f,0.05f,1};
            mesh.grain=0;mesh.grainScale=0.35f;
            mesh.Box(-half-rim,t.top,bz-half-rim,half+rim,bt-0.0004f*D,bz+half+rim,frame);
            mesh.Quad({-half,bt,bz-half},{half,bt,bz-half},{half,bt,bz+half},{-half,bt,bz+half},{0,1,0},green);
            const float lw=0.0022f*D,ly=bt+0.0004f*D;
            for(int i=0;i<=8;++i)
            {
                const float p=-half+i*Cell();
                mesh.Quad({p-lw,ly,bz-half},{p+lw,ly,bz-half},{p+lw,ly,bz+half},{p-lw,ly,bz+half},{0,1,0},line);
                mesh.Quad({-half,ly,bz+p-lw},{half,ly,bz+p-lw},{half,ly,bz+p+lw},{-half,ly,bz+p+lw},{0,1,0},line);
            }
            for(float px:{-2.0f,2.0f})
                for(float pz:{-2.0f,2.0f})
                    AddSpot(mesh,px*Cell(),bz+pz*Cell(),0.07f*Cell(),line);
            // Each side's pile of discs, as many as it has left to lay.
            for(int who:{Othello::You,Othello::Her})
            {
                const auto p=PileAt(who);
                const int left=std::max(0,Othello::Squares-4-state.table.Moves())/2+(who==state.table.Black()?(Othello::Squares-4-state.table.Moves())%2:0);
                Disc top;
                top.x=p[0];top.z=p[1];
                top.turn=who==state.table.Black()?0.0f:1.0f;
                for(int k=0;k<left;++k)
                {
                    Disc d=top;
                    d.y=t.top+(k+0.5f)*0.006f*D;
                    AddDisc(mesh,d,Mark::None,0.006f*D);
                }
            }
            // The discs on the board, the squares you can lay on, and what the one pointed at
            // would turn over.
            PlaceDiscs();
            const int aimed=Aimed();
            const auto flips=aimed>=0?state.table.Flips(Othello::You,aimed):std::vector<int>{};
            for(int s=0;s<Squares&&state.movesSeen==state.table.Moves();++s)
            {
                const Disc& d=state.discs[static_cast<size_t>(s)];
                if(!d.shown||InFlight(s))continue;
                AddDisc(mesh,d,std::find(flips.begin(),flips.end(),s)!=flips.end()?Mark::Flips:s==state.table.LastMove().square?Mark::Last:Mark::None);
            }
            if(YourMoveNow())
            {
                const float gold[4]={1.0f,0.80f,0.32f,1},pale[4]={0.95f,0.86f,0.50f,1};
                for(int s:state.table.Legal())AddSpot(mesh,SquareX(s),SquareZ(s),(s==aimed?0.30f:0.13f)*Cell(),s==aimed?gold:pale);
                if(state.cursor>=0&&state.hover<0)
                {
                    // The keys' choice: a gold frame round its square.
                    const float x=SquareX(state.cursor),z=SquareZ(state.cursor),c=Cell()/2,w=0.006f*D,y=BoardTop()+0.0012f*D;
                    mesh.Quad({x-c,y,z-c},{x+c,y,z-c},{x+c,y,z-c+w},{x-c,y,z-c+w},{0,1,0},gold);
                    mesh.Quad({x-c,y,z+c-w},{x+c,y,z+c-w},{x+c,y,z+c},{x-c,y,z+c},{0,1,0},gold);
                    mesh.Quad({x-c,y,z-c},{x-c+w,y,z-c},{x-c+w,y,z+c},{x-c,y,z+c},{0,1,0},gold);
                    mesh.Quad({x+c-w,y,z-c},{x+c,y,z-c},{x+c,y,z+c},{x+c-w,y,z+c},{0,1,0},gold);
                }
            }
            return std::move(mesh.out);
        }

        // The disc between keys at time (eased, lifted on the way when it arcs).
        bool DiscAt(const Flight& f,double time,Disc& out)
        {
            if(f.keys.empty())return false;
            if(time<=f.keys.front().first){out=f.keys.front().second;return true;}
            for(size_t i=1;i<f.keys.size();++i)
            {
                const auto& [t1,b]=f.keys[i];
                if(time>t1)continue;
                const auto& [t0,a]=f.keys[i-1];
                const float u=t1>t0?static_cast<float>((time-t0)/(t1-t0)):1,e=u*u*(3-2*u);
                auto mix=[&](float p,float q){return p+(q-p)*e;};
                out={mix(a.x,b.x),mix(a.y,b.y),mix(a.z,b.z),mix(a.turn,b.turn),true};
                if(f.arc)out.y+=std::sin(u*DirectX::XM_PI)*0.45f*Cell();
                return true;
            }
            return false;   // arrived
        }

        void Finish()
        {
            if(!state.window)return;
            KillTimer(state.window,TimerId);
            if(state.her.model)state.her.model->SetSolids({});
            if(state.afterword.empty()&&state.seen.started&&state.table.Now()!=Phase::GameOver)
            {
                const auto& t=state.table;
                state.afterword=Fmt(T(L"いま、話し相手とオセロをしていたが、途中でやめてデスクトップに戻ってきたところ（石はあなた{0}枚、話し相手{1}枚だった）。そのことを話し相手にひと言（1〜2文）",
                                      L"You were playing Othello with them, but they stopped midway and you have come back to the desktop (you had {0} discs, they had {1}). Say a word about it to them (one or two sentences)"),
                                    {std::to_wstring(t.Count(Othello::Her)),std::to_wstring(t.Count(Othello::You))});
                state.afterwordLine=T(L"また続き、やろうね。",L"Let's finish it another time.");
            }
            auto onEnd=std::move(state.onEnd);
            DestroyWindow(state.window);
            if(onEnd)onEnd();
        }

        void Prefetch();

        void NewGame()
        {
            state.gameOverTold=false;
            std::random_device device;
            // The first game's black (who lays first) by lot; then it goes back and forth.
            const int black=state.nextBlack>=0?state.nextBlack:static_cast<int>(device()%2);
            state.nextBlack=1-black;
            state.table.NewGame(device(),std::max(LeastCare,Care-LessPerBond*state.her.bond-LessPerLoss*state.lost),black);
            state.passesSeen=state.table.Passes();
            state.hover=state.cursor=-1;
            state.confirmQuit=false;
            state.said.clear();
            state.nextAt=GetTickCount64()+HerPlayWait;
            state.seen={};
            state.words.waiting=false;
            state.words.text.clear();
            state.afterword.clear();
            state.afterwordLine.clear();
            state.plannedFor=state.reachedFor=-1;
            Prefetch();
        }

        // After a move of yours: the next waits a moment.
        void Moved()
        {
            state.hover=-1;
            const ULONGLONG now=GetTickCount64();
            state.nextAt=now+(state.table.Now()==Phase::GameOver?(state.renderer?EndWait3D:EndWait):HerPlayWait);
            Redraw();
        }

        void PlayAt(int square)
        {
            if(!YourMoveNow()||!state.table.CanPlay(square))return;
            state.table.Play(square);
            Moved();
        }

        void Continue()
        {
            auto& t=state.table;
            if(t.Now()==Phase::GameOver)
            {
                state.lost=t.Winner()==Othello::Her?state.lost+1:0;
                NewGame();
            }
            Redraw();
        }

        // In 2D, a disc's sound as soon as it is laid (at the 3D table, as it lands).
        void Listen()
        {
            if(state.renderer)return;
            auto& t=state.table;
            if(t.Moves()>state.heardMoves&&state.heardMoves>=0)
            {
                PlayMahjongSound(MahjongSound::CardPlace);
                if(!t.LastMove().flipped.empty())PlayMahjongSound(MahjongSound::CardFlip);
            }
            state.heardMoves=t.Moves();
        }

        // ---- Her words and the watchers' (as at mahjong) ----

        constexpr ULONGLONG WordsHold=3500;
        constexpr ULONGLONG AskEvery=12000;
        constexpr ULONGLONG AnswerWait=9000;

        void SayWords(const std::wstring& line,int weight)
        {
            auto& w=state.words;
            const ULONGLONG now=GetTickCount64();
            w.text=line;
            w.waiting=false;
            w.weight=weight;
            w.since=now;
            w.told+=L"  she says: "+line+L"\n";
            w.until=now+static_cast<ULONGLONG>(CharSeconds()*1000*line.size())+2600+90*static_cast<ULONGLONG>(line.size());
            if(!line.empty()){w.recent.push_back(line);if(w.recent.size()>4)w.recent.erase(w.recent.begin());}
            Redraw();
        }

        const std::wstring& Pick(const std::vector<std::wstring>& lines)
        {
            std::uniform_int_distribution<size_t> any(0,lines.size()-1);
            return lines[any(state.sparkRandom)];
        }

        // Something happened: she speaks of it, and those watching hear of it. facts: for her (she
        // is "you"); feel: how she is to feel about it; seen: the same for one watching; lines:
        // hers without the LLM (none: she says nothing; one of them is given the LLM as the
        // feeling to put in her own words); theirs: the watchers'.
        void Happened(int weight,const std::wstring& facts,const std::wstring& feel,const std::vector<std::wstring>& lines,
                      const std::wstring& seen,const std::vector<std::wstring>& theirs)
        {
            auto& t=state.table;
            const int you=t.Count(Othello::You),her=t.Count(Othello::Her);
            state.words.told+=Fmt(L"  {0} s, weight {1}: {2}\n",{std::to_wstring(static_cast<int>(state.clock)),std::to_wstring(weight),seen});
            if(state.her.onEvent&&!theirs.empty())
                state.her.onEvent(seen+Fmt(T(L"（石は{0}が{1}枚、持ち主が{2}枚）",L" ({0} has {1} discs, the owner {2})"),
                                           {HerName(),std::to_wstring(her),std::to_wstring(you)}),weight,Pick(theirs));
            if(lines.empty())return;
            auto& w=state.words;
            const ULONGLONG now=GetTickCount64();
            if((w.waiting||now-w.since<WordsHold)&&weight<w.weight)return;
            const std::wstring line=Pick(lines);
            if(state.her.talk&&(weight>=6||now-w.asked>AskEvery))
            {
                // The score and her last lines first, then what happened and the feeling, with
                // her own line as an example last (a small model keeps to the point so).
                std::wstring situation=Fmt(T(L"いまの石の数は、あなた（{0}）{1}枚、話し相手{2}枚、盤のあきは{3}マス。",
                                             L"Now you ({0}) have {1} discs, they have {2}, and {3} squares are empty. "),
                                           {ColourOf(Othello::Her),std::to_wstring(her),std::to_wstring(you),std::to_wstring(t.Empties())});
                if(!w.recent.empty())
                {
                    situation+=T(L"さっきあなたが言ったこと：",L"What you said just before: ");
                    for(const auto& r:w.recent)situation+=English()?L"\""+r+L"\" ":L"「"+r+L"」";
                    situation+=T(L"。同じ言い回しは避けて。",L"- do not say the same again. ");
                }
                situation+=facts+Fmt(T(L"{0}を、あなたの言葉で話し相手にひと言（たとえば「{1}」のような気持ち。そのままは使わない）",
                                       L" Tell them, in a word of your own, {0} (like \"{1}\", but not those words)"),{feel,line});
                if(state.her.talk(situation))
                {
                    w.waiting=true;
                    w.fallback=line;
                    w.text=L"…";
                    w.weight=weight;
                    w.since=w.asked=now;
                    w.until=now+AnswerWait+4000;
                    Redraw();
                    return;
                }
            }
            SayWords(line,weight);
        }

        void Prefetch()
        {
            if(state.prefetching||!state.her.prefetch||(state.goodLines.size()>=2&&state.thinkLines.size()>=2))return;
            const std::wstring ask=T(
                L"これから話し相手とオセロをします。対局中に言う短いひと言を用意してください。"
                L"次の形で、1行に1つずつ、それぞれ6つずつ書いてください（どれも違う言い方で、1文の短い話し言葉）。\n"
                L"いい手：（盤のすみ（角）を取ったときなど、いい手を打ったときのひと言）\n"
                L"考える：（自分の番に、どこに置くか迷いながら自分につぶやくひと言。相手への問いかけにはしない）\n"
                L"この形の行のほかは何も書かないでください。気持ちの印、名前、（ ）書き、絵文字は使わないでください。",
                L"You are about to play Othello with them. Get ready some little lines to say during the game. "
                L"Write six of each, one to a line, in this form (each different, one short spoken sentence):\n"
                L"Good: (a word as you make a good move, such as taking a corner)\n"
                L"Think: (a word muttered to yourself on your turn as you wonder where to lay your disc; not a question to them)\n"
                L"Write nothing but these lines. No feeling tags, names, anything in ( ) or emoji.");
            state.prefetching=state.her.prefetch(ask);
        }

        std::wstring TakeLine(std::vector<std::wstring>& ready,const std::vector<std::wstring>& own)
        {
            std::wstring line;
            if(!ready.empty())
            {
                std::uniform_int_distribution<size_t> any(0,ready.size()-1);
                const size_t i=any(state.sparkRandom);
                line=ready[i];
                ready.erase(ready.begin()+static_cast<std::ptrdiff_t>(i));
            }
            else line=Pick(own);
            Prefetch();
            return line;
        }

        bool Corner(int s){return s==0||s==7||s==56||s==63;}

        // What is new in the game since the last tick, for words: its start, a corner taken, a
        // pass, the end.
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
            w.shown=std::min(static_cast<double>(w.text.size()),w.shown+(now-std::min(now,w.shownAt))/1000.0/CharSeconds());
            w.shownAt=now;
            if(static_cast<size_t>(w.shown)!=before)Redraw();
            if(Flying())return;   // the discs are still on their way
            const std::wstring name=HerName();
            if(!seen.started)
            {
                seen.started=true;
                const bool first=t.Black()==Othello::Her;
                Happened(6,Fmt(T(L"これから話し相手とオセロの対局が始まる。あなたは{0}で、{1}。",L"A game of Othello with them is about to begin. You have {0} and {1}. "),
                               {ColourOf(Othello::Her),first?T(L"先に打つ",L"move first"):T(L"後から打つ",L"move second")}),
                         T(L"わくわくする気持ち",L"how you look forward to it"),
                         {T(L"よろしくね！負けないよ。",L"Let's play! I won't lose."),T(L"よーし、オセロ勝負だ！",L"Okay, Othello time!"),T(L"お手やわらかにね。",L"Go easy on me.")},
                         Fmt(T(L"{0}と、このパソコンの持ち主のオセロの対局が始まる。",L"A game of Othello between {0} and the owner of this computer is about to begin."),{name}),
                         {T(L"がんばれー！",L"Good luck, you two!"),T(L"どっちが勝つかな？",L"Who's going to win?")});
            }
            if(t.Moves()!=seen.moves)
            {
                seen.moves=t.Moves();
                const auto& m=t.LastMove();
                if(m.who==Othello::You&&Corner(m.square))
                    Happened(4,T(L"オセロで、話し相手に盤のすみ（角）を取られた。",L"At Othello, they took a corner from you."),
                             T(L"しまったという気持ち",L"that you wish you had seen it coming"),
                             {T(L"あっ、角取られた！",L"Oh no, a corner!"),T(L"うう、そこかあ…",L"Ugh, right there..."),T(L"やるなあ…",L"Not bad...")},
                             Fmt(T(L"オセロで、持ち主が角を取った（{0}との対局）。",L"At Othello, the owner took a corner (against {0})."),{name}),
                             {T(L"おっ、角だ！",L"Ooh, a corner!"),T(L"いいところ取ったね！",L"Nice spot!")});
                else if(m.who==Othello::Her&&Corner(m.square))
                    Happened(3,L"",L"",{},
                             Fmt(T(L"オセロで、{0}が角を取った。",L"At Othello, {0} took a corner."),{name}),
                             {Fmt(T(L"{0}、角を取った！",L"{0} got a corner!"),{name}),T(L"これは大きいね。",L"That's a big one.")});
                else if(m.who==Othello::You&&m.flipped.size()>=6)
                    Happened(3,Fmt(T(L"オセロで、話し相手の一手で、あなたの石が{0}枚まとめてひっくり返された。",L"At Othello, their move turned over {0} of your discs at once."),{std::to_wstring(m.flipped.size())}),
                             T(L"驚く気持ち",L"your surprise"),
                             {T(L"わっ、そんなに返されるの!?",L"Whoa, that many?!"),T(L"ひっくり返されたー！",L"So many flipped!")},
                             Fmt(T(L"オセロで、持ち主が{0}の石を{1}枚まとめてひっくり返した。",L"At Othello, the owner turned over {1} of {0}'s discs at once."),{name,std::to_wstring(m.flipped.size())}),
                             {T(L"おお、いっぱい返した！",L"Wow, a lot of them!")});
            }
            if(t.Passes()!=seen.passes)
            {
                seen.passes=t.Passes();
                if(t.LastPass()==Othello::You)
                    Happened(4,T(L"オセロで、話し相手は石を置ける場所がなくてパスになり、あなたがもう一度打つ。",L"At Othello, they have nowhere to lay a disc and must pass: you move again."),
                             T(L"うれしい気持ち",L"how pleased you are"),
                             {T(L"やった、もう一回わたしの番！",L"Yay, my turn again!"),T(L"ふふ、置くところないでしょ。",L"Hehe, nowhere to go, right?")},
                             Fmt(T(L"オセロで、持ち主が置ける場所がなくてパスした（{0}との対局）。",L"At Othello, the owner had to pass (against {0})."),{name}),
                             {T(L"パスだ！",L"A pass!")});
                else if(t.LastPass()==Othello::Her)
                    Happened(4,T(L"オセロで、あなたは石を置ける場所がなくてパスになり、話し相手がもう一度打つ。",L"At Othello, you have nowhere to lay a disc and must pass: they move again."),
                             T(L"困った気持ち",L"how stuck you feel"),
                             {T(L"うう、置ける所がない…",L"Ugh, nowhere to go..."),T(L"パスかあ…くやしい。",L"A pass... how annoying.")},
                             Fmt(T(L"オセロで、{0}が置ける場所がなくてパスした。",L"At Othello, {0} had to pass."),{name}),
                             {Fmt(T(L"{0}、パスだって！",L"{0} has to pass!"),{name})});
            }
            const bool over=t.Now()==Phase::GameOver;
            if(over&&!seen.over)
            {
                const int you=t.Count(Othello::You),her=t.Count(Othello::Her);
                const bool won=her>you,tie=her==you;
                const std::wstring count=Fmt(T(L"あなた{0}枚、話し相手{1}枚",L"you have {0} discs, they have {1}"),{std::to_wstring(her),std::to_wstring(you)});
                state.afterword=Fmt(T(L"いま、話し相手とオセロを最後まで打って、デスクトップに戻ってきたところ。結果は{0}（{1}）。その対局の感想を、話し相手にひと言（1〜2文）",
                                      L"You have just played a game of Othello to its end with them and come back to the desktop. {0} ({1}). Tell them what you thought of the game (one or two sentences)"),
                                    {won?T(L"あなたの勝ち",L"You won"):tie?T(L"引き分け",L"A tie"):T(L"あなたの負け",L"You lost"),count});
                state.afterwordLine=won?T(L"オセロ、楽しかったね！またやろう。",L"That was fun! Let's play again sometime."):T(L"くやしいなあ…次は勝つからね！",L"So close... I'll win next time!");
                if(won)Happened(9,T(L"オセロの対局が終わって、あなたの勝ち（",L"The game of Othello is over, and you won (")+count+T(L"）。",L")."),
                                T(L"勝ってうれしい気持ち",L"how glad you are to have won"),
                                {T(L"勝った！楽しかったね！",L"I won! That was fun!"),T(L"やったー、わたしの勝ち！",L"Yay, I win!")},
                                Fmt(T(L"{0}と持ち主のオセロの対局が終わり、{0}が勝った。",L"The Othello game between {0} and the owner is over: {0} won."),{name}),
                                {T(L"おつかれさま！いい勝負だったね。",L"Well played, both of you!"),Fmt(T(L"{0}の勝ちだ！",L"{0} wins!"),{name})});
                else Happened(9,(tie?T(L"オセロの対局が終わって、引き分け（",L"The game of Othello is over: a tie ("):T(L"オセロの対局が終わって、あなたの負け（",L"The game of Othello is over, and you lost ("))+count+T(L"）。",L")."),
                              tie?T(L"引き分けの気持ち",L"how you feel about a tie"):T(L"負けて悔しい気持ち",L"how sorry you are to have lost"),
                              {T(L"負けちゃった…次は負けないよ！",L"I lost... I'll get you next time!"),T(L"うう、くやしい！もう一回！",L"Ugh, so close! One more!")},
                              Fmt(T(L"{0}と持ち主のオセロの対局が終わった。",L"The Othello game between {0} and the owner is over."),{name}),
                              {T(L"おつかれさま！いい勝負だったね。",L"Well played, both of you!"),T(L"おめでとう！",L"Congratulations!")});
            }
            seen.over=over;
        }

        void Tick()
        {
            auto& t=state.table;
            const ULONGLONG now=GetTickCount64();
            if(Flying())state.nextAt=std::max(state.nextAt,now+120);   // play waits for the discs
            if(t.Passes()!=state.passesSeen&&!Flying())
            {
                state.passesSeen=t.Passes();
                state.said=t.LastPass()==Othello::Her?Fmt(T(L"{0}はパス",L"{0} passes"),{HerName()}):std::wstring(T(L"置ける所がないのでパス",L"Nowhere to go: you pass"));
                state.saidAt=now;
                state.nextAt=std::max(state.nextAt,now+SaidShown);
                Redraw();
            }
            if(!state.said.empty()&&now-state.saidAt>SaidShown){state.said.clear();Redraw();}
            // Her turn coming, at the 3D table once she is seated: time for her hand to go out
            // to her square, and now and then to stop and think first.
            if(t.Now()==Phase::HerTurn&&state.plannedFor!=t.Moves()&&state.renderer&&state.entrance==5&&!Flying())
            {
                state.plannedFor=t.Moves();
                state.nextAt=std::max(state.nextAt,now+HerPlayWait3D);
                std::uniform_real_distribution<double> any(0,1);
                if(any(state.sparkRandom)<0.15)
                {
                    state.nextAt+=ThinkExtra;
                    state.thinkFor=(HerPlayWait3D+ThinkExtra-ReachLead)/1000.0-0.1;
                    if(!state.words.waiting&&GetTickCount64()-state.words.since>WordsHold)
                        SayWords(TakeLine(state.thinkLines,{T(L"うーん…どこにしようかな。",L"Hmm... where to go?"),T(L"ちょっと考えさせて…",L"Let me think..."),T(L"むむ…",L"Hmm...")}),2);
                }
            }
            if(now>=state.nextAt&&t.Now()==Phase::HerTurn&&(!state.renderer||state.entrance==5))
            {
                t.Step();
                // A corner taken now and then gets a little word.
                const auto& m=t.LastMove();
                std::uniform_real_distribution<double> any(0,1);
                if(m.who==Othello::Her&&Corner(m.square)&&any(state.sparkRandom)<0.5&&!state.words.waiting&&GetTickCount64()-state.words.since>WordsHold)
                    SayWords(TakeLine(state.goodLines,{T(L"角、もらうね！",L"I'll take this corner!"),T(L"ふふ、いいところ。",L"Hehe, a good spot."),T(L"ここ、ずっと狙ってたんだ。",L"I've been waiting for this one.")}),2);
                if(t.Now()==Phase::HerTurn)state.nextAt=now+HerPlayWait;
                else if(t.Now()==Phase::GameOver)state.nextAt=now+(state.renderer?EndWait3D:EndWait);
                else state.nextAt=now+150;
            }
            Redraw();
            Listen();
            NoticeWords();
            if(t.Now()==Phase::GameOver&&!state.gameOverTold)
            {
                state.gameOverTold=true;
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

        // A disc drawn flat: its colour, black or white.
        void DrawDisc(Graphics& g,float cx,float cy,float r,bool black)
        {
            SolidBrush shadow(Color(80,0,0,0));
            g.FillEllipse(&shadow,cx-r+r*0.08f,cy-r+r*0.12f,2*r,2*r);
            SolidBrush fill(black?Color(255,22,22,24):Color(255,236,234,226));
            g.FillEllipse(&fill,cx-r,cy-r,2*r,2*r);
            Pen edge(black?Color(255,70,70,74):Color(255,150,148,140),std::max(1.0f,r*0.06f));
            g.DrawEllipse(&edge,cx-r,cy-r,2*r,2*r);
        }

        void PaintEnd(Graphics& g,float cw,float ch,float s)
        {
            auto& t=state.table;
            SolidBrush shade(Color(150,0,0,0));
            g.FillRectangle(&shade,0.0f,0.0f,cw,ch);
            const float w=std::min(cw-40*s,640*s),h=std::min(ch-40*s,360*s);
            const RectF box((cw-w)/2,(ch-h)/2,w,h);
            FillRound(g,box,14*s,Color(255,40,22,22),Color(255,200,170,90),2*s);
            float y=box.Y+22*s;
            const Color gold(255,240,206,120),white(255,240,240,236),soft(255,200,190,186);
            const int you=t.Count(Othello::You),her=t.Count(Othello::Her);
            Text(g,T(L"勝負あり",L"Game over"),RectF(box.X,y,w,44*s),34*s,gold,true,StringAlignmentCenter,StringAlignmentNear,true);
            y+=72*s;
            const float r=13*s;
            for(int i=0;i<2;++i)
            {
                const int who=i==0?Othello::You:Othello::Her;
                const std::wstring label=i==0?std::wstring(T(L"あなた",L"You")):HerName();
                const float rowY=y+i*44*s,cx=box.X+w/2-120*s;
                DrawDisc(g,cx,rowY+18*s,r,t.Black()==who);
                Text(g,label,RectF(cx+24*s,rowY,170*s,36*s),24*s,white,false,StringAlignmentNear,StringAlignmentCenter,true);
                Text(g,Discs(t.Count(who)),RectF(cx+24*s,rowY,220*s,36*s),24*s,white,false,StringAlignmentFar,StringAlignmentCenter,true);
            }
            y+=108*s;
            if(you<her)Runs(g,{{HerName()},{T(L"の勝ち",L" wins")}},RectF(box.X,y,w,48*s),32*s,gold);
            else Runs(g,{{you>her?T(L"あなたの勝ち！",L"You win!"):T(L"引き分け",L"A tie")}},RectF(box.X,y,w,48*s),32*s,gold);
            Text(g,T(L"Enter：もう一局　Esc：閉じる",L"Enter: play again   Esc: close"),RectF(box.X,box.GetBottom()-44*s,w,30*s),17*s,soft,false,StringAlignmentCenter);
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
            g.Clear(hud?Color(0,0,0,0):Color(255,22,74,52));
            const float s=Scale(),W=static_cast<float>(cw),H=static_cast<float>(ch),mid=W/2;
            auto& t=state.table;
            const Color white(255,240,240,236),soft(255,206,196,186),gold(255,240,206,120),panel(185,40,18,16),edge(220,170,120,100);

            // Her: name, colour and discs (at the top left over the 3D table; with her face when
            // there is none).
            const float nameX=hud?28*s:112*s,nameY=hud?20*s:14*s;
            if(hud)FillRound(g,RectF(14*s,12*s,260*s,92*s),10*s,panel,edge,1.2f*s);
            else
            {
                const RectF faceBox(16*s,14*s,84*s,84*s);
                FillRound(g,faceBox,10*s,Color(255,14,50,36),Color(255,120,160,130),1.5f*s);
                if(state.face)g.DrawImage(state.face.get(),faceBox.X+3*s,faceBox.Y+3*s,faceBox.Width-6*s,faceBox.Height-6*s);
                else Text(g,HerName().substr(0,1),faceBox,40*s,white,true,StringAlignmentCenter,StringAlignmentCenter);
            }
            Text(g,HerName(),RectF(nameX,nameY,230*s,28*s),20*s,white,true);
            DrawDisc(g,nameX+10*s,nameY+44*s,10*s,t.Black()==Othello::Her);
            Text(g,Discs(t.Count(Othello::Her))+(t.Now()==Phase::HerTurn?T(L"　（番）",L"  (to move)"):L""),RectF(nameX+28*s,nameY+30*s,220*s,28*s),18*s,t.Now()==Phase::HerTurn?gold:soft);
            // Her words: a bubble just right of her head (as at mahjong).
            if(!state.words.text.empty())
            {
                const std::wstring& said=state.words.text;
                const float most=std::max(160*s,W*0.4f);
                const WordsBubble bubble(g,said,s,most);
                const float bw=bubble.Width(),bh=bubble.Height();
                const bool beside=hud&&state.besideHead.x>=0;
                const float x=beside?std::clamp(static_cast<float>(state.besideHead.x),14*s,W-14*s-bw):W-14*s-bw;
                const float y=beside?std::clamp(static_cast<float>(state.headScreen.y)-bh*0.6f,96*s,std::max(96*s,H*0.6f-bh)):96*s;
                const float ty=beside?std::clamp(static_cast<float>(state.headScreen.y),y+12*s,y+bh-12*s):y+std::min(bh*0.5f,30*s);
                const std::wstring shown=said.substr(0,std::min(said.size(),std::max<size_t>(1,static_cast<size_t>(state.words.shown))));
                bubble.Draw(g,x,y,ty,shown);
            }

            // The squares left (at the top right).
            {
                const RectF info(W-254*s,12*s,240*s,64*s);
                FillRound(g,info,10*s,Color(hud?215:200,40,18,16),edge,1.2f*s);
                Text(g,T(L"オセロ",L"Othello"),RectF(info.X+12*s,info.Y+6*s,220*s,26*s),18*s,white,true);
                Text(g,Fmt(T(L"あき {0}マス",L"{0} squares empty"),{std::to_wstring(t.Empties())}),RectF(info.X+12*s,info.Y+34*s,220*s,24*s),15*s,soft);
            }

            // The board (on the 3D table when there is one).
            if(!hud)
            {
                const float c=Cell2D*s,x0=mid-4*c,y0=std::max(120*s,(H-8*c)/2-10*s);
                FillRound(g,RectF(x0-14*s,y0-14*s,8*c+28*s,8*c+28*s),8*s,Color(255,92,54,26),Color(255,40,24,12),2*s);
                SolidBrush green(Color(255,16,104,54));
                g.FillRectangle(&green,x0,y0,8*c,8*c);
                Pen line(Color(255,8,40,20),1.5f*s);
                for(int i=0;i<=8;++i)
                {
                    g.DrawLine(&line,x0+i*c,y0,x0+i*c,y0+8*c);
                    g.DrawLine(&line,x0,y0+i*c,x0+8*c,y0+i*c);
                }
                SolidBrush dot(Color(255,8,40,20));
                for(int px:{2,6})for(int py:{2,6})g.FillEllipse(&dot,x0+px*c-3*s,y0+py*c-3*s,6*s,6*s);
                const int aimed=Aimed();
                const auto flips=aimed>=0?t.Flips(Othello::You,aimed):std::vector<int>{};
                const auto legal=YourMoveNow()?t.Legal():std::vector<int>{};
                for(int sq=0;sq<Squares;++sq)
                {
                    const float cx=x0+(sq%8+0.5f)*c,cy=y0+(sq/8+0.5f)*c;
                    state.cellBoxes[static_cast<size_t>(sq)]=RectF(x0+(sq%8)*c,y0+(sq/8)*c,c,c);
                    const int owner=t.At(sq);
                    if(owner>=0)
                    {
                        DrawDisc(g,cx,cy,c*0.42f,owner==t.Black());
                        if(std::find(flips.begin(),flips.end(),sq)!=flips.end())
                        {
                            SolidBrush mark(gold);
                            g.FillEllipse(&mark,cx-c*0.09f,cy-c*0.09f,c*0.18f,c*0.18f);
                        }
                    }
                    else if(std::find(legal.begin(),legal.end(),sq)!=legal.end())
                    {
                        const float r=sq==aimed?c*0.30f:c*0.12f;
                        SolidBrush mark(sq==aimed?gold:Color(200,240,220,140));
                        g.FillEllipse(&mark,cx-r,cy-r,2*r,2*r);
                    }
                    if(sq==state.cursor&&state.hover<0&&YourMoveNow())
                    {
                        Pen frame(gold,2.5f*s);
                        g.DrawRectangle(&frame,x0+(sq%8)*c+2*s,y0+(sq/8)*c+2*s,c-4*s,c-4*s);
                    }
                }
                // The last disc laid: a small red dot on it.
                if(t.LastMove().square>=0)
                {
                    const int sq=t.LastMove().square;
                    SolidBrush red(Color(255,220,60,50));
                    g.FillEllipse(&red,x0+(sq%8+0.5f)*c-4*s,y0+(sq/8+0.5f)*c-4*s,8*s,8*s);
                }
            }

            // You: colour and discs.
            {
                const float boxY=hud?H-104*s:H-104*s,boxW=190*s;
                FillRound(g,RectF(8*s,boxY,boxW,92*s),10*s,panel,edge,1.2f*s);
                Text(g,T(L"あなた",L"You"),RectF(22*s,boxY+8*s,boxW,26*s),18*s,white,true);
                DrawDisc(g,32*s,boxY+56*s,10*s,t.Black()==Othello::You);
                Text(g,Discs(t.Count(Othello::You))+(t.Now()==Phase::YourMove?T(L"　（番）",L"  (to move)"):L""),RectF(50*s,boxY+42*s,boxW-44*s,28*s),18*s,t.Now()==Phase::YourMove?gold:soft);
            }

            // What to do now, early in the game.
            if(YourMoveNow()&&t.Moves()<6)
            {
                const std::wstring prompt=T(L"金色の印のマスに石を置けます（クリック、または矢印キーとEnter）",L"Lay your disc on a square marked in gold (click, or the arrow keys and Enter)");
                const float tw=TextWidth(g,prompt,17*s)+28*s,y=H-150*s;
                FillRound(g,RectF(mid-tw/2,y,tw,34*s),10*s,panel,edge,1.2f*s);
                Text(g,prompt,RectF(mid-tw/2,y,tw,34*s),17*s,gold,false,StringAlignmentCenter,StringAlignmentCenter);
            }
            if(!state.said.empty())
            {
                const float tw=TextWidth(g,state.said,28*s,true)+40*s;
                const RectF bubble(mid-tw/2,H*0.36f,tw,54*s);
                FillRound(g,bubble,14*s,Color(245,255,252,240),Color(255,230,120,60),2*s);
                Text(g,state.said,bubble,28*s,Color(255,200,40,40),true,StringAlignmentCenter,StringAlignmentCenter);
            }

            if(EndShown())PaintEnd(g,W,H,s);
            if(state.confirmQuit)
            {
                SolidBrush shade(Color(160,0,0,0));
                g.FillRectangle(&shade,0.0f,0.0f,W,H);
                const RectF box(mid-220*s,ch/2.0f-70*s,440*s,140*s);
                FillRound(g,box,14*s,Color(250,40,22,22),Color(255,200,170,90),2*s);
                Text(g,T(L"オセロをやめますか？",L"Stop playing?"),RectF(box.X,box.Y+22*s,box.Width,36*s),24*s,white,true,StringAlignmentCenter);
                const float gap=20*s,width=(box.Width-3*gap)/2;
                const std::wstring labels[2]={T(L"やめる（Y）",L"Stop (Y)"),T(L"続ける（N）",L"Keep playing (N)")};
                for(int i=0;i<2;++i)
                {
                    const RectF button(box.X+gap+i*(width+gap),box.Y+74*s,width,44*s);
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

        // The square under (x, y): on the 3D board as it is seen, or on the 2D one.
        int SquareAt(float x,float y)
        {
            if(state.renderer)
            {
                if(!state.quadsSet)return -1;
                for(int s=0;s<Squares;++s)
                {
                    const auto& q=state.cellQuads[static_cast<size_t>(s)];
                    bool inside=true;
                    float sign=0;
                    for(int k=0;k<4&&inside;++k)
                    {
                        const POINT a=q[static_cast<size_t>(k)],b=q[static_cast<size_t>((k+1)%4)];
                        const float cross=static_cast<float>(b.x-a.x)*(y-a.y)-static_cast<float>(b.y-a.y)*(x-a.x);
                        if(cross==0)continue;
                        if(sign==0)sign=cross;
                        else if((cross>0)!=(sign>0))inside=false;
                    }
                    if(inside&&sign!=0)return s;
                }
                return -1;
            }
            for(int s=0;s<Squares;++s)
                if(state.cellBoxes[static_cast<size_t>(s)].Width>0&&state.cellBoxes[static_cast<size_t>(s)].Contains(x,y))return s;
            return -1;
        }

        void OnKey(WPARAM key)
        {
            auto& t=state.table;
            if(key==VK_F1){state.camera={};return;}
            if(state.confirmQuit)
            {
                if(key=='Y')Finish();
                else if(key=='N'||key==VK_ESCAPE){state.confirmQuit=false;Redraw();}
                return;
            }
            if(t.Now()==Phase::GameOver)
            {
                if((key==VK_RETURN||key==VK_SPACE)&&EndShown())Continue();
                else if(key==VK_ESCAPE)Finish();
                return;
            }
            if(key==VK_ESCAPE){state.confirmQuit=true;Redraw();return;}
            if(!YourMoveNow())return;
            if(key==VK_LEFT||key==VK_RIGHT||key==VK_UP||key==VK_DOWN)
            {
                // Through the squares you can lay on, nearest that way (the first one to start).
                const auto legal=t.Legal();
                if(legal.empty())return;
                if(state.cursor<0||!t.CanPlay(state.cursor)){state.cursor=legal[0];state.hover=-1;Redraw();return;}
                const int cx=state.cursor%8,cy=state.cursor/8;
                const int dx=key==VK_LEFT?-1:key==VK_RIGHT?1:0,dy=key==VK_UP?-1:key==VK_DOWN?1:0;
                int best=-1;
                float bestScore=1e9f;
                for(int s:legal)
                {
                    const int ex=s%8-cx,ey=s/8-cy;
                    const int along=ex*dx+ey*dy,across=std::abs(ex*dy)+std::abs(ey*dx);
                    if(along<=0)continue;
                    const float score=along+2.0f*across;
                    if(score<bestScore){bestScore=score;best=s;}
                }
                if(best>=0)state.cursor=best;
                state.hover=-1;
                Redraw();
            }
            else if(key==VK_RETURN&&state.cursor>=0)PlayAt(state.cursor);
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
                const int at=SquareAt(x,y);
                if(at!=state.hover){state.hover=at;Redraw();}
                return 0;
            }
            case WM_LBUTTONDOWN:
            {
                const float x=static_cast<float>(GET_X_LPARAM(lParam)),y=static_cast<float>(GET_Y_LPARAM(lParam));
                auto& t=state.table;
                if(state.confirmQuit)
                {
                    if(state.quitButtons[0].Contains(x,y))Finish();
                    else if(state.quitButtons[1].Contains(x,y)){state.confirmQuit=false;Redraw();}
                    return 0;
                }
                if(t.Now()==Phase::GameOver){if(EndShown())Continue();return 0;}
                PlayAt(SquareAt(x,y));
                return 0;
            }
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC dc=BeginPaint(hwnd,&ps);
                Paint(dc,ps.rcPaint);
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
                return 0;
            }
            return DefWindowProcW(hwnd,message,wParam,lParam);
        }
    }

    bool OthelloWindowOpen(){return state.window!=nullptr;}
    HWND OthelloWindowHandle(){return state.window;}
    bool OthelloWindowRect(RECT& r)
    {
        return state.window&&IsWindowVisible(state.window)&&!IsIconic(state.window)&&GetWindowRect(state.window,&r);
    }

    namespace
    {
        // Diagnostics: moves played for both (yours as she would play them).
        void PlaySimply(int moves)
        {
            auto& t=state.table;
            for(int i=0;i<moves&&t.Now()!=Phase::GameOver;++i)
            {
                if(t.Now()==Phase::HerTurn)t.Step();
                else t.PlayForYou();
            }
            state.passesSeen=t.Passes();
            state.nextAt=0;
            state.hover=state.cursor=-1;
        }
    }

    void OthelloLines(const std::wstring& answer)
    {
        state.prefetching=false;
        if(!state.window)return;
        size_t at=0;
        while(at<answer.size())
        {
            size_t end=answer.find(L'\n',at);
            if(end==std::wstring::npos)end=answer.size();
            std::wstring line=answer.substr(at,end-at);
            at=end+1;
            const size_t colon=line.find_first_of(L":：");
            if(colon==std::wstring::npos)continue;
            const std::wstring kind=line.substr(0,colon);
            std::wstring said=line.substr(colon+1);
            for(const wchar_t* cut:{L"「",L"」",L"\"",L"*"})
                for(size_t p;(p=said.find(cut))!=std::wstring::npos;)said.erase(p,wcslen(cut));
            const size_t a=said.find_first_not_of(L" \t\r　"),b=said.find_last_not_of(L" \t\r　");
            if(a==std::wstring::npos)continue;
            said=said.substr(a,b-a+1);
            if(said.size()>60)continue;
            if(kind.find(L"いい")!=std::wstring::npos||kind.find(L"ood")!=std::wstring::npos)state.goodLines.push_back(said);
            else if(kind.find(L"考")!=std::wstring::npos||kind.find(L"hink")!=std::wstring::npos)state.thinkLines.push_back(said);
        }
        state.words.told+=L"  lines ready: "+std::to_wstring(state.goodLines.size())+L" for good moves, "+std::to_wstring(state.thinkLines.size())+L" for thinking\n";
    }

    bool OthelloAfterword(std::wstring& situation,std::wstring& fallback)
    {
        if(state.afterword.empty())return false;
        situation=std::move(state.afterword);
        fallback=std::move(state.afterwordLine);
        state.afterword.clear();
        state.afterwordLine.clear();
        return true;
    }

    void OthelloHeard(const std::wstring& line,bool done)
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

    void PlayOthelloMoves(int moves,float yaw)
    {
        if(!state.window)return;
        PlaySimply(moves);
        state.camera.yaw+=yaw;
        Redraw();
    }

    namespace
    {
        void Sparkle(const std::array<float,3>& at,float scale=1)
        {
            const float D=state.upper;
            std::uniform_real_distribution<float> any(0,1);
            auto& r=state.sparkRandom;
            for(int i=0;i<18;++i)
            {
                const float a=any(r)*DirectX::XM_2PI,up=0.35f+0.65f*any(r),speed=(0.25f+0.45f*any(r))*D*scale;
                const float around=std::sqrt(1-up*up);
                State::Spark k;
                k.at={at[0]+(any(r)-0.5f)*0.06f*D*scale,at[1]+0.01f*D,at[2]+(any(r)-0.5f)*0.06f*D*scale};
                k.velocity={std::cos(a)*around*speed,up*speed,std::sin(a)*around*speed};
                k.age=0;
                k.life=0.45f+0.45f*any(r);
                k.glow=i%6==0;
                k.size=(k.glow?0.05f:0.018f+0.016f*any(r))*D*scale;
                k.angle=any(r)*DirectX::XM_2PI;
                k.spin=(any(r)-0.5f)*8;
                k.color=i%3==0?std::array<float,4>{1,1,1,1}:std::array<float,4>{1,0.84f,0.42f,1};
                state.sparks.push_back(k);
            }
        }

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

        void Due(double at,MahjongSound sound){state.sounds.push_back({at,sound});}

        // The discs on their way since the board was last seen: the one laid dropping onto its
        // square, then those it turned over turning, nearest first; or, at a game's start, its
        // four discs laid one after another.
        void StartFlights(const std::array<Disc,Squares>& before,bool fresh)
        {
            const double now=state.clock;
            auto& t=state.table;
            const auto& after=state.discs;
            auto add=[&](int square,std::vector<std::pair<double,Disc>> keys,bool arc){
                state.flightsUntil=std::max(state.flightsUntil,keys.back().first);
                state.flights.erase(std::remove_if(state.flights.begin(),state.flights.end(),[&](const Flight& f){return f.square==square;}),state.flights.end());
                state.flights.push_back({square,std::move(keys),arc});
            };
            auto above=[&](int square){Disc d=after[static_cast<size_t>(square)];d.y+=0.9f*Cell();return d;};
            if(fresh)
            {
                state.flights.clear();
                int i=0;
                for(int s:{27,36,28,35})
                {
                    if(!after[static_cast<size_t>(s)].shown)continue;
                    const double start=now+0.3+i++*Opening;
                    add(s,{{start,above(s)},{start+Drop,after[static_cast<size_t>(s)]}},false);
                    Due(start+Drop,MahjongSound::CardPlace);
                }
                return;
            }
            const auto& m=t.LastMove();
            if(m.square<0)return;
            add(m.square,{{now,above(m.square)},{now+Drop,after[static_cast<size_t>(m.square)]}},false);
            Due(now+Drop,MahjongSound::CardPlace);
            // Those turned over: outward from the one laid, each a little after the one before.
            std::vector<int> order=m.flipped;
            auto apart=[&](int s){return std::max(std::abs(s%8-m.square%8),std::abs(s/8-m.square/8));};
            std::sort(order.begin(),order.end(),[&](int a,int b){return apart(a)<apart(b);});
            for(int s:order)
            {
                const double start=now+Drop+0.04+(apart(s)-1)*Ripple;
                Disc from=before[static_cast<size_t>(s)];
                if(!from.shown)from=after[static_cast<size_t>(s)];
                add(s,{{start,from},{start+Turn,after[static_cast<size_t>(s)]}},true);
            }
            if(!order.empty())
            {
                Due(now+Drop+0.04,MahjongSound::CardFlip);
                if(order.size()>=5)Due(now+Drop+0.04+2*Ripple,MahjongSound::CardFlip);
            }
            // Hers: gold sparkles as she takes a corner.
            if(m.who==Othello::Her&&Corner(m.square)){state.sparkAt=now+Drop;state.sparkSquare=m.square;}
        }
    }

    void TickOthelloWindow(MascotModel& model,double dt)
    {
        if(!state.window||!state.renderer)return;
        auto& r=*state.renderer;
        using namespace DirectX;
        const float D=state.upper,top=Top();
        dt=std::min(dt,0.1);
        auto& t=state.table;
        // What changed on the board since the last frame: its discs set on their way (a new
        // game's four as she sits down).
        if(t.Moves()!=state.movesSeen&&(t.Moves()>0||state.entrance==5))
        {
            const bool fresh=t.Moves()==0||state.movesSeen<0||t.Moves()<state.movesSeen;
            const std::array<Disc,Squares> before=state.discs;
            PlaceDiscs();
            if(fresh||t.Moves()==state.movesSeen+1)StartFlights(before,fresh);
            else state.flights.clear();
            state.movesSeen=t.Moves();
        }
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
        // The flights done: off the list, the table built again with them in place.
        state.flights.erase(std::remove_if(state.flights.begin(),state.flights.end(),[](const Flight& f){return f.keys.back().first<=state.clock;}),state.flights.end());
        if(auto scene=Scene();scene!=state.scene||state.scene.empty()){state.scene=std::move(scene);r.SetGeometry(BuildTable());}
        for(auto k=state.sounds.begin();k!=state.sounds.end();)
            if(k->first<=state.clock){PlayMahjongSound(k->second);k=state.sounds.erase(k);}else ++k;
        if(state.sparkAt>0&&state.clock>=state.sparkAt)
        {
            state.sparkAt=0;
            if(state.sparkSquare>=0)Sparkle(SquareSpot(state.sparkSquare));
        }
        // Her hand goes out to the square she will lay on (tried again next frame while a
        // gesture of hers ends).
        if(t.Now()==Phase::HerTurn&&state.entrance==5&&!Flying()&&state.plannedFor==t.Moves()&&state.reachedFor!=t.Moves()&&
           GetTickCount64()+ReachLead>=state.nextAt)
        {
            const int k=t.HerNext();
            if(k<0||ReachFor(model,SquareSpot(k)))state.reachedFor=t.Moves();
        }
        if(state.thinkFor>0&&state.entrance==5){model.EmoteSeated(L"ponder",state.thinkFor);state.thinkFor=0;}
        {
            const bool over=t.Now()==Phase::GameOver;
            if(over&&!state.seenOver&&t.Winner()>=0)
            {
                std::uniform_real_distribution<double> any(0,1);
                state.endGesture=t.Winner()==Othello::Her?(any(state.sparkRandom)<0.5?L"cheeks":L"clap"):L"oops";
                state.endGestureUntil=state.clock+1.5;
            }
            state.seenOver=over;
            if(!state.endGesture.empty()&&state.entrance==5&&(model.EmoteSeated(state.endGesture,3.2)||state.clock>state.endGestureUntil))
                state.endGesture.clear();
        }
        // What moves: her chair, and the discs on their way.
        {
            Mesh moving;
            moving.out=BuildChair(state.chairZ);
            for(const auto& f:state.flights)
            {
                Disc d;
                if(DiscAt(f,state.clock,d)&&state.clock>=f.keys.front().first)AddDisc(moving,d);
            }
            r.SetMovingGeometry(std::move(moving.out));
        }
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
        // From your side of the table, above her, looking down at the table between you.
        const auto& c=state.camera;
        const XMVECTOR target=XMVectorSet(c.x*D,top+(0.40f+c.y+0.7f*standing)*D,(c.z-1.22f)*D,0);
        const XMVECTOR toEye=XMVectorSet(std::sin(c.yaw)*std::cos(c.pitch),std::sin(c.pitch),-std::cos(c.yaw)*std::cos(c.pitch),0);
        const XMVECTOR eye=XMVectorAdd(target,XMVectorScale(toEye,c.distance*(1+0.22f*standing)*D));
        r.SetCamera(eye,target,XMConvertToRadians(42.0f));
        if(std::array<float,3> head{};model.HeadPosition(head))
        {
            const XMVECTOR at=XMVector3Transform(XMVectorSet(head[0],head[1],head[2],1),world);
            const XMVECTOR right=XMVector3Normalize(XMVector3Cross(XMVectorSet(0,1,0,0),XMVectorSubtract(target,eye)));
            const XMVECTOR beside=XMVectorAdd(at,XMVectorScale(right,0.26f*D));
            state.headScreen=r.Project({XMVectorGetX(at),XMVectorGetY(at),XMVectorGetZ(at)});
            state.besideHead=r.Project({XMVectorGetX(beside),XMVectorGetY(beside),XMVectorGetZ(beside)});
        }
        {
            XMVECTOR det;
            const XMVECTOR you=XMVector3Transform(eye,XMMatrixInverse(&det,world));
            std::array<float,3> head{};
            const bool talking=!state.words.text.empty()&&model.HeadPosition(head);
            model.LookToward({XMVectorGetX(you),head[1],XMVectorGetZ(you)},talking);
        }
        // The board's squares as they are seen, for pointing and clicking.
        {
            const float c=Cell()/2,y=BoardTop();
            for(int s=0;s<Squares;++s)
            {
                const float x=SquareX(s),z=SquareZ(s);
                const std::array<float,3> corners[4]={{x-c,y,z-c},{x+c,y,z-c},{x+c,y,z+c},{x-c,y,z+c}};
                for(int k=0;k<4;++k)state.cellQuads[static_cast<size_t>(s)][static_cast<size_t>(k)]=r.Project(corners[k]);
            }
            state.quadsSet=true;
        }
        {
            MascotRenderer::PointLight lamp;
            lamp.position[0]=0;lamp.position[1]=top+2.3f*D;lamp.position[2]=FieldZ();
            lamp.radius=4.2f*D;
            lamp.color[0]=0.42f;lamp.color[1]=0.30f;lamp.color[2]=0.16f;
            lamp.onModel=false;
            r.SetLights({lamp});
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

    std::wstring DescribeOthello()
    {
        if(!state.window)return L"";
        auto& t=state.table;
        std::wstring out=Fmt(L"othello: entrance {0}, phase {1}, moves {2}, passes {3}, black {4}, discs you {5} her {6}, flights {7}\n",
                             {std::to_wstring(state.entrance),std::to_wstring(static_cast<int>(t.Now())),std::to_wstring(t.Moves()),std::to_wstring(t.Passes()),
                              t.Black()==Othello::You?L"you":L"her",std::to_wstring(t.Count(Othello::You)),std::to_wstring(t.Count(Othello::Her)),std::to_wstring(state.flights.size())});
        return out+(state.words.told.empty()?std::wstring():L"othello words:\n"+state.words.told);
    }

    bool SaveOthelloView(const std::wstring& path)
    {
        return state.renderer&&state.renderer->SaveLastFrame(path);
    }

    bool SaveOthelloShot(const std::wstring& path,int moves,std::uint32_t seed)
    {
        GdiplusStartupInput input;
        if(!state.gdiplus)GdiplusStartup(&state.gdiplus,&input,nullptr);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc=DefWindowProcW;
        wc.hInstance=GetModuleHandleW(nullptr);
        wc.lpszClassName=L"MikuMikuDesktopPerchOthelloShot";
        RegisterClassExW(&wc);
        const int dpi=static_cast<int>(GetDpiForSystem());
        state.window=CreateWindowExW(0,wc.lpszClassName,L"",WS_OVERLAPPEDWINDOW,0,0,MulDiv(static_cast<int>(BaseWidth)+16,dpi,96),MulDiv(static_cast<int>(BaseHeight)+39,dpi,96),
                                     nullptr,nullptr,wc.hInstance,nullptr);
        state.her=OthelloOpponent{L"ミク"};
        state.table.NewGame(seed,Care,seed%2?Othello::Her:Othello::You);
        PlaySimply(moves);
        // Your move next, when it is: the squares you can lay on shown, one of them pointed at.
        if(state.table.Now()==Phase::YourMove)
        {
            const auto legal=state.table.Legal();
            if(!legal.empty())state.hover=legal[legal.size()/2];
        }
        if(!SampleWords().empty()){state.words.text=SampleWords();state.words.shown=1e9;}
        RECT client{};
        GetClientRect(state.window,&client);
        Bitmap shot(client.right,client.bottom,PixelFormat32bppARGB);
        {
            Graphics g(&shot);
            const HDC dc=g.GetHDC();
            Paint(dc,client);
            g.ReleaseHDC(dc);
        }
        DestroyWindow(state.window);
        state.window=nullptr;
        return SavePng(shot,path);
    }

    void ShowOthelloWindow(HWND owner,const OthelloOpponent& her,std::function<void()> onEnd)
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
        state.heardMoves=-1;
        state.movesSeen=-1;
        state.discs={};
        state.nextBlack=-1;
        state.quadsSet=false;
        state.goodLines.clear();
        state.thinkLines.clear();
        state.prefetching=false;
        state.words={};
        NewGame();

        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&info);
        const RECT work=info.rcWork;
        const int dpi=static_cast<int>(GetDpiForSystem());
        const int w=std::min(static_cast<int>(work.right-work.left)*9/10,MulDiv(static_cast<int>(BaseWidth)+16,dpi,96));
        const int h=std::min(static_cast<int>(work.bottom-work.top)*9/10,MulDiv(static_cast<int>(BaseHeight)+39,dpi,96));
        const std::wstring title=std::wstring(T(L"オセロ",L"Othello"))+L" - "+HerName();
        state.window=CreateWindowExW(0,ClassName,title.c_str(),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                                     work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,
                                     nullptr,nullptr,instance,nullptr);
        if(CreateView())PlaceView();
        SetTimer(state.window,TimerId,40,nullptr);
        ShowWindow(state.window,SW_SHOWNORMAL);
        SetForegroundWindow(state.window);
    }
}
