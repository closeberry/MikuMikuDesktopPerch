#define NOMINMAX
#include "OldMaidWindow.hpp"
#include "OldMaid.hpp"
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
#include <sstream>
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
        using OldMaid::Phase;
        using OldMaid::Side;
        constexpr wchar_t ClassName[]=L"MikuMikuDesktopPerchOldMaid";
        constexpr wchar_t ViewClassName[]=L"MikuMikuDesktopPerchOldMaidView";
        constexpr UINT_PTR TimerId=1;
        // Her pace: before her draw (in 2D, and at the 3D table where her hand goes out first),
        // after a draw of yours.
        constexpr ULONGLONG HerPlayWait=900,HerPlayWait3D=3500,YourWait=250;
        constexpr ULONGLONG SaidShown=1600;
        constexpr ULONGLONG ReachLead=3200;        // her hand starts out (and wavers) this long before her draw
        constexpr double LeanShare=0.35;           // how often she leans in to take your card
        // The cards on their way at the 3D table (seconds): a drawn card to the drawer's hand; a
        // pair shown there this long, then to the pile; her hand shuffled.
        constexpr double Travel=0.40,Settle=0.55,Warp=0.30,Shuffle=0.45;
        // Her arms as she holds her cards before her chin (腕, 腕捩, ひじ, 手捩, 手首; the left,
        // then the right), the hands together, the wrists turned to her.
        constexpr std::array<std::array<float,4>,5> HoldLeft{{{0.0208f,0.3818f,-0.2853f,0.8788f},{0.0933f,-0.0750f,-0.0014f,0.9928f},
                                                           {0.5502f,0.7096f,0.0f,0.4402f},{0.5268f,-0.4084f,0.0189f,0.7452f},{0.0511f,0.0678f,0.0f,0.9964f}}};
        constexpr std::array<std::array<float,4>,5> HoldRight{{{0.0208f,-0.3818f,0.2853f,0.8788f},{0.0933f,0.0750f,0.0014f,0.9928f},
                                                            {0.5375f,-0.6933f,0.0f,0.4801f},{0.5268f,0.4084f,-0.0189f,0.7452f},{0.0511f,-0.0678f,0.0f,0.9964f}}};

        // How often she gives a false face as you point at a card (each card, once a turn).
        constexpr double BluffShare=0.35;
        // Her hands held up as fans: tipped back this far from upright, the cards this far apart
        // (no more than FanMost across in all); a word as you point at a card once it has been
        // pointed at this long, no sooner than HoverEvery after the last.
        constexpr float FanTip=0.34f,FanStep=0.17f,FanMost=1.25f;
        constexpr ULONGLONG HoverDwell=450,HoverEvery=1800;
        // Sizes in logical pixels (at 96 dpi, at the size the window opens).
        constexpr float BaseWidth=1180,BaseHeight=840;
        constexpr float CardW2D=81,CardH2D=149;   // a card on the 2D table
        constexpr int Slots=OldMaid::Joker+1;      // the cards' numbers (the back's unused)

        // Where a card is at the 3D table (world units): its middle, turned about the upright by
        // yaw, turned over about its length by flip (1: face down), w wide and l long; held up,
        // stood up by tilt (about its width) and turned in its own plane by spin (a fan).
        struct Pose {float x=0,y=0,z=0,yaw=0,flip=1,w=0,l=0;bool shown=false;float tilt=0,spin=0;};
        // A card on its way: where it is at each time (seconds of the clock); arc: lifted on the way.
        struct Flight {int card=-1;std::vector<std::pair<double,Pose>> keys;bool arc=true;};

        struct State
        {
            HWND window=nullptr;
            ULONG_PTR gdiplus=0;
            OldMaidOpponent her;
            std::unique_ptr<Bitmap> face;
            std::function<void()> onEnd;
            OldMaid::Table table;
            Side firstNext=OldMaid::You;   // who draws first next game (by turns)
            int wins=0,losses=0;            // this time the window is open
            bool gameOverTold=false;
            bool confirmQuit=false;
            RectF quitButtons[2];
            int hover=-1;              // the card of hers under the mouse (its place in her hand)
            std::vector<RectF> cardBoxes;   // her cards on the 2D table
            std::vector<std::array<POINT,4>> herQuads;   // and at the 3D table, on the screen
            int herPick=-1;            // the card of yours she will draw
            int honestFor=-1;          // the draw her faces below were chosen for
            std::map<int,bool> honest; // per card: her true face (or a false one) as you point at it
            ULONGLONG hoverSince=0,hoverSaid=0;
            int saidFor=-1;            // the card her last word as you pointed was for
            std::array<float,3> fanAt{};   // where her hand holds her cards (world units)
            bool fanSet=false;
            double handsBusyUntil=0;   // her right hand reaching: it lets go of her cards
            double gestureFreeAt=0;    // a gesture of hers playing: both let go
            int reachCard=-1;          // the card of yours her right hand goes out to
            bool leanIn=false;         // and whether she leans in to it
            float forward=0;           // her hands put forward (eased) as you choose
            std::vector<std::pair<double,int>> waver;   // her hand over these cards of yours from these times
            float offer=0;             // your hand held out toward her (eased) as she takes
            float handDown=0;          // her hand down to your card (eased) as she takes it
            std::array<float,2> bubbleAt{-1,-1};        // her words' bubble on the screen (eased)
            Expression tell=Expression::Neutral;
            int shuffledFor=-1;        // the draw count her hand was last shuffled at
            ULONGLONG nextAt=0;
            std::wstring said;         // the joker drawn, in the big bubble
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
            // Her lines by scene (res/oldmaid/lines.txt): the plain draft, and the same put in her
            // own way of talking by the LLM (kept per character); the scene being asked for.
            struct Scene {std::wstring name,about;std::vector<std::wstring> draft,styled;bool asked=false;};
            std::vector<Scene> scenes;
            int asking=-1;
            bool prefetching=false;
            POINT headScreen{-1,-1},besideHead{-1,-1};
            std::wstring afterword,afterwordLine;
            struct Seen
            {
                bool started=false,over=false,sorted=false,nearEnd=false;
                int draws=0,teased=-1,muttered=-1;
            } seen;
            std::unique_ptr<Bitmap> canvas;
            // Her at the table in 3D (none: 2D only).
            HWND view=nullptr;
            std::unique_ptr<MascotRenderer> renderer;
            int entrance=0;
            float herX=0,herZ=0,herAngle=0,entranceTime=0,chairZ=0;
            struct Camera {float yaw=0,pitch=0.18f,distance=2.5f,x=0,y=0,z=0;} camera;
            std::array<float,3> yourFan{0,0,0};   // where your hand is held (before the camera)
            float yourYaw=0;
            bool yourFanSet=false;
            std::array<float,3> headSeen{};   // her head, followed slowly (the camera's aim)
            bool faceSet=false;
            std::array<float,3> holdAt{}; // where her hands hold her cards (model space)
            bool holdSet=false;
            std::wstring holdNote;
            POINT dragFrom{};
            int dragging=0;
            float upper=10,hips=10;
            float floorY=-5;
            std::vector<int> scene;
            // Where each card is (world units), as the table is now.
            std::array<Pose,Slots> poses{};
            // The cards on their way, and until when (on the clock); sounds due on the way.
            std::vector<Flight> flights;
            double flightsUntil=0;
            std::vector<std::pair<double,MahjongSound>> sounds;
            int movesSeen=-1;
            double sparkAt=0;         // gold sparkles then, over your empty place when you win
            std::vector<std::pair<double,std::array<float,3>>> warps;   // a pair warped away then, there
            std::array<float,3> sparkSpot{};
            double clock=0;
            int plannedFor=-1;        // the move count her draw's wait was last set at
            int reachedFor=-1;        // and her hand last went out at
            struct Spark {std::array<float,3> at,velocity;float age,life,size,angle,spin;std::array<float,4> color;bool glow;};
            std::vector<Spark> sparks;
            std::vector<MascotRenderer::Particle> roomGlows;
            std::mt19937 sparkRandom{12345};
            double thinkFor=0;
            Expression reactFace=Expression::Neutral;   // her face on the joker drawn, until then
            double reactUntil=0;
            std::wstring endGesture;
            double endGestureUntil=0;
            int gestureFor=-1;        // the draw count her reaction was last given for
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
        std::wstring Cards(int n){return std::to_wstring(n)+T(L"枚",L" cards");}

        bool Waiting(){return state.nextAt>GetTickCount64();}
        // The cards still on their way at the 3D table (a move not yet shown counts).
        bool Flying()
        {
            return state.renderer&&(state.movesSeen!=state.table.Moves()||state.clock<state.flightsUntil);
        }
        bool Ready(){return !Waiting()&&!Flying();}
        bool YourDrawNow(){return state.table.Now()==Phase::YourDraw&&Ready();}
        bool EndShown(){return state.table.Now()==Phase::GameOver&&Ready();}
        bool HerJoker(){return state.table.JokerHolder()==OldMaid::Her;}

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
                Log(L"oldmaid: resize: %s",error.c_str());
            InvalidateRect(state.window,nullptr,FALSE);
        }

        void ReleaseView()
        {
            if(state.renderer){state.renderer->WaitIdle();state.renderer.reset();Log(L"oldmaid: 3D view released");}
            state.view=nullptr;
        }

        // The cards' faces (res/blackjack/cards.jpg): CardSide by CardTop each, eight across,
        // card n at column n%8, row n/8 (the back at OldMaid::Back).
        constexpr int CardSide=176,CardTop=324,AtlasColumns=8;
        constexpr int ScrollW=320,ScrollH=768;   // the hanging scroll's painting, under the faces in the 3D table's texture

        Bitmap* Faces()
        {
            static std::unique_ptr<Bitmap> atlas=[]{
                std::unique_ptr<Bitmap> loaded;
                const HRSRC info=FindResourceW(nullptr,L"CARDS",L"BLACKJACKCARDS");
                const HGLOBAL data=info?LoadResource(nullptr,info):nullptr;
                if(!data)return loaded;
                IStream* stream=SHCreateMemStream(static_cast<const BYTE*>(LockResource(data)),SizeofResource(nullptr,info));
                if(!stream)return loaded;
                std::unique_ptr<Bitmap> read(Bitmap::FromStream(stream));
                stream->Release();
                if(read&&read->GetLastStatus()==Ok)loaded.reset(read->Clone(0,0,read->GetWidth(),read->GetHeight(),PixelFormat32bppPARGB));
                return loaded;
            }();
            return atlas.get();
        }

        constexpr float CornerShare=0.09f;   // a card's corners rounded by this share of its width

        // A card's face at w by h pixels, its corners rounded, made once (scaling it each time
        // it is drawn is slow).
        Bitmap* Face(int card,int w,int h)
        {
            static std::map<std::tuple<int,int,int>,std::unique_ptr<Bitmap>> made;
            Bitmap* faces=Faces();
            if(!faces||w<1||h<1)return nullptr;
            if(made.size()>600)made.clear();
            auto& face=made[{card,w,h}];
            if(!face)
            {
                face=std::make_unique<Bitmap>(w,h,PixelFormat32bppPARGB);
                Graphics g(face.get());
                g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                g.SetPixelOffsetMode(PixelOffsetModeHalf);
                g.SetSmoothingMode(SmoothingModeAntiAlias);
                GraphicsPath path;
                RoundRect(path,0,0,static_cast<float>(w),static_cast<float>(h),w*CornerShare);
                g.SetClip(&path);
                g.DrawImage(faces,Rect(0,0,w,h),card%AtlasColumns*CardSide,card/AtlasColumns*CardTop,CardSide,CardTop,UnitPixel);
            }
            return face.get();
        }

        // A card at (x, y), w by h: its face (or its back); dim: greyed; glow: a gold rim.
        void DrawCard(Graphics& g,float x,float y,float w,float h,int card,bool faceUp=true,bool dim=false,bool glow=false)
        {
            const float r=w*CornerShare;
            FillRound(g,RectF(x+w*0.05f,y+h*0.04f,w,h),r,Color(70,0,0,0));
            if(glow)FillRound(g,RectF(x-w*0.07f,y-w*0.07f,w*1.14f,h+w*0.14f),r*1.6f,Color(235,255,214,90));
            if(!faceUp||card<0)card=OldMaid::Back;
            if(Bitmap* face=Face(card,static_cast<int>(std::lround(w)),static_cast<int>(std::lround(h))))
                g.DrawImage(face,RectF(x,y,static_cast<REAL>(face->GetWidth()),static_cast<REAL>(face->GetHeight())));
            else
            {
                FillRound(g,RectF(x,y,w,h),r,Color(255,242,233,208),Color(255,30,26,24),2);
                Text(g,std::to_wstring(OldMaid::RankOf(card)),RectF(x,y,w,h),h*0.2f,Color(255,30,30,36),true,StringAlignmentCenter,StringAlignmentCenter);
            }
            if(dim)FillRound(g,RectF(x,y,w,h),r,Color(130,30,30,40));
        }

        // ---- The room (as the mahjong table's) ----


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


        // The faces for the 3D table, with the scroll's painting under them, and their mipmaps.
        ImageRgba8 CardsImage()
        {
            ImageRgba8 image;
            Bitmap* faces=Faces();
            if(!faces)return image;
            const int w=static_cast<int>(faces->GetWidth()),facesH=static_cast<int>(faces->GetHeight()),h=facesH+ScrollH;
            Bitmap atlas(w,h,PixelFormat32bppARGB);
            {
                Graphics g(&atlas);
                g.Clear(Color(255,236,231,218));
                g.DrawImage(faces,0,0,w,facesH);
                Bitmap painting(ScrollW,ScrollH,PixelFormat32bppARGB);
                {
                    Graphics p(&painting);
                    float h0,h1,width;
                    ScrollSpan(h0,h1,width);
                    const int tall=std::max(ScrollW,static_cast<int>(std::lround(ScrollW*(h1-h0)/std::max(width,0.001f))));
                    p.ScaleTransform(1.0f,static_cast<float>(ScrollH)/tall);
                    PaintScroll(p,ScrollW,tall);
                }
                g.DrawImage(&painting,0,facesH,ScrollW,ScrollH);
            }
            BitmapData bits{};
            Rect whole(0,0,w,h);
            if(atlas.LockBits(&whole,ImageLockModeRead,PixelFormat32bppARGB,&bits)!=Ok)return image;
            image.levels.resize(1);
            auto& level=image.levels[0];
            level.width=static_cast<unsigned>(w);level.height=static_cast<unsigned>(h);
            level.pixels.resize(static_cast<size_t>(w)*h*4);
            for(int y=0;y<h;++y)
            {
                const BYTE* from=static_cast<const BYTE*>(bits.Scan0)+static_cast<size_t>(y)*bits.Stride;
                BYTE* to=level.pixels.data()+static_cast<size_t>(y)*w*4;
                for(int x=0;x<w;++x,from+=4,to+=4){to[0]=from[2];to[1]=from[1];to[2]=from[0];to[3]=255;}
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
                Log(L"oldmaid: no 3D view (%s)",error.c_str());
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
            Log(L"oldmaid: 3D view %ux%u on %s (hips %.1f, upper body %.1f, floor %.1f)",w,h,renderer->AdapterName().c_str(),state.hips,state.upper,state.floorY);
            if(!renderer->SetGeometryTexture(CardsImage()))Log(L"oldmaid: no card faces on the 3D table");
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
            state.gestureFor=-1;
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
            Bitmap* faces=Faces();
            const float aw=faces?static_cast<float>(faces->GetWidth()):1,facesH=faces?static_cast<float>(faces->GetHeight()):0;
            const float u0=1/aw,u1=(ScrollW-1)/aw,v0=(facesH+1)/(facesH+ScrollH),v1=(facesH+ScrollH-1)/(facesH+ScrollH);
            const float corners[8]={u0,v0,u1,v0,u1,v1,u0,v1};
            std::copy(std::begin(corners),std::end(corners),uv);
        }

        // A card's face on the atlas (u, v at its corners: top left, top right, bottom right,
        // bottom left).
        void CardUv(int card,float (&uv)[8])
        {
            Bitmap* faces=Faces();
            const float aw=faces?static_cast<float>(faces->GetWidth()):1,ah=faces?static_cast<float>(faces->GetHeight()+ScrollH):1;
            const float u0=(card%AtlasColumns*CardSide+2)/aw,u1=((card%AtlasColumns+1)*CardSide-2)/aw;
            const float v0=(card/AtlasColumns*CardTop+2)/ah,v1=((card/AtlasColumns+1)*CardTop-2)/ah;
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
            // (No table: the cards are held up, and the pairs go to the floor.)
            (void)frame;(void)skirt;(void)x0;(void)x1;(void)zNear;(void)zFar;

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

        // ---- The cards on the table ----
        // Her hand in a row before her, face down; yours before you, face up; the pairs thrown out
        // face up in a pile at her right (your left).

        float Top(){return 0.12f*state.hips+0.30f*state.upper;}
        float FieldZ(){return -1.10f*state.upper;}
        float Thick(){return 0.003f*state.upper;}
        float CardW(){return 0.149f*state.upper;}
        float CardL(){return 0.275f*state.upper;}
        float HerZ(){return -0.80f*state.upper;}
        float YourZ(){return -1.42f*state.upper;}
        std::array<float,2> PileAt(){return {-0.30f*state.upper,-1.05f*state.upper};}
        // Cards in a row: this far apart, the row no wider than the table allows.
        float Step(size_t n){return std::min(0.62f*CardW(),1.25f*state.upper/std::max<size_t>(n,1));}
        float RowX(size_t i,size_t n){return (i-(n-1)/2.0f)*Step(n);}

        Pose Lying(float x,float z,bool faceUp,float above=0)
        {
            Pose p;
            p.x=x;p.y=Top()+Thick()/2+above;p.z=z;
            p.flip=faceUp?0.0f:1.0f;
            p.w=CardW();p.l=CardL();
            p.shown=true;
            return p;
        }

        DirectX::XMMATRIX Turn(const Pose& p)
        {
            using namespace DirectX;
            return XMMatrixRotationY(p.spin)*XMMatrixRotationZ(p.flip*XM_PI)*XMMatrixRotationX(p.tilt)*XMMatrixRotationY(p.yaw);
        }

        // A point on the card: across (-w/2 to w/2), out of its face (thickness), along it
        // toward its top (-l/2 to l/2).
        std::array<float,3> At(const Pose& p,float u,float v,float w)
        {
            using namespace DirectX;
            const XMVECTOR q=XMVector3TransformNormal(XMVectorSet(u,v,w,0),Turn(p));
            return {p.x+XMVectorGetX(q),p.y+XMVectorGetY(q),p.z+XMVectorGetZ(q)};
        }

        // Where her hand holds her cards: where her hands are (as she sits, not reaching), or
        // before her place at the table until then.
        std::array<float,3> HerFan()
        {
            if(state.fanSet)return state.fanAt;
            return {0,Top()+0.14f*state.upper,HerZ()+0.18f*state.upper};
        }
        // Yours in the air before you.
        float Smooth01(float u){u=std::clamp(u,0.0f,1.0f);return u*u*(3-2*u);}
        std::array<float,3> YourFan(){return state.yourFan[1]!=0?state.yourFan:std::array<float,3>{0,Top()+0.06f*state.upper,YourZ()-0.22f*state.upper};}

        // Where a card at place i of n in a hand is held: up in a fan from the hand (the point
        // at its foot), tipped back, its face to the one who holds it (hers her back to you, from
        // your right to your left; yours from your left), each a little before the one before.
        // The card of hers you point at drawn up out of the fan toward you.
        Pose InHand(Side side,size_t i,size_t n)
        {
            using namespace DirectX;
            const bool hers=side==OldMaid::Her;
            const float step=std::min(FanStep,FanMost/std::max<size_t>(n,1));
            const float a=(hers?-1.0f:1.0f)*(i-(n-1)/2.0f)*step;
            const std::array<float,3> foot=hers?HerFan():YourFan();
            Pose p;
            p.w=CardW();p.l=CardL();p.shown=true;
            p.flip=hers?1.0f:0.0f;
            // Tipped back (the top away from you), so that both face up toward your eyes.
            p.tilt=-(XM_PIDIV2-FanTip);
            p.spin=hers?-a:a;
            // Up in the fan's plane: across (your right) and the card's way up.
            const float upY=std::cos(FanTip),upZ=std::sin(FanTip);
            float r=0.42f*CardL();
            if(hers&&state.table.Now()==Phase::YourDraw&&static_cast<int>(i)==state.hover)r+=0.07f*state.upper;
            const float toYou=(hers?-1.0f:-1.0f)*i*0.004f*state.upper;
            float dx=r*std::sin(a),dz=r*std::cos(a)*upZ+toYou;
            if(!hers)
            {
                // Turned about the upright as the camera is (DirectX's turn about y).
                p.yaw=state.yourYaw;
                const float c=std::cos(p.yaw),s=std::sin(p.yaw),x=dx*c+dz*s,z=-dx*s+dz*c;
                dx=x;dz=z;
            }
            p.x=foot[0]+dx;
            p.y=foot[1]+r*std::cos(a)*upY;
            p.z=foot[2]+dz;
            if(hers&&state.table.Now()==Phase::YourDraw&&static_cast<int>(i)==state.hover)p.z-=0.03f*state.upper;
            // The card of yours her hand is over drawn up a little toward her.
            if(!hers&&state.table.Hand(OldMaid::You)[i]==state.reachCard&&state.clock<state.handsBusyUntil)
            {
                p.y+=0.04f*state.upper;
                p.z+=0.03f*state.upper;
            }
            return p;
        }

        // Where every card is, as the table is now (state.poses).
        void PlaceCards()
        {
            auto& t=state.table;
            for(auto& p:state.poses)p=Pose{};
            for(Side side:{OldMaid::You,OldMaid::Her})
            {
                const auto& h=t.Hand(side);
                for(size_t i=0;i<h.size();++i)state.poses[static_cast<size_t>(h[i])]=InHand(side,i,h.size());
            }
            const auto& out=t.Discards();
            for(size_t i=0;i<out.size();++i)
            {
                // A pair a little apart, each pair turned a little its own way.
                const size_t pair=i/2;
                Pose p=Lying(PileAt()[0]+(i%2?0.03f:-0.03f)*state.upper,PileAt()[1],true,i*Thick());
                p.shown=false;   // warped away
                p.yaw=static_cast<float>(static_cast<int>(pair*37%13)-6)*0.05f+(i%2?0.08f:-0.08f);
                state.poses[static_cast<size_t>(out[i])]=p;
            }
        }

        // A card at pose, its corners rounded: its face and its back from the atlas, cream edges.
        void AddCard(Mesh& m,const Pose& p,int card)
        {
            using V=std::array<float,3>;
            using namespace DirectX;
            constexpr int Bend=5;   // steps round each corner
            const float hw=p.w/2,hl=p.l/2,ht=Thick()/2,r=p.w*CornerShare;
            const XMMATRIX turn=Turn(p);
            auto at=[&](float u,float v,float w)->V{return At(p,u,v,w);};
            auto dir=[&](float u,float v,float w)->V{
                const XMVECTOR q=XMVector3TransformNormal(XMVectorSet(u,v,w,0),turn);
                return {XMVectorGetX(q),XMVectorGetY(q),XMVectorGetZ(q)};
            };
            const float white[4]={1,1,1,1},edge[4]={0.86f,0.83f,0.76f,1};
            float face[8],back[8];
            CardUv(card,face);
            CardUv(OldMaid::Back,back);
            // The outline, clockwise seen from the face from its top right (across, along, and
            // the way out of it there).
            struct Point {float u,v,a;};
            std::vector<Point> rim;
            const float corners[4][3]={{hw-r,hl-r,XM_PIDIV2},{hw-r,-hl+r,0},{-hw+r,-hl+r,-XM_PIDIV2},{-hw+r,hl-r,XM_PI}};
            for(const auto& c:corners)
                for(int k=0;k<=Bend;++k)
                {
                    const float a=c[2]-XM_PIDIV2*k/Bend;
                    rim.push_back({c[0]+r*std::cos(a),c[1]+r*std::sin(a),a});
                }
            // A point's place on the atlas: on the face as it lies, on the back mirrored across.
            auto map=[&](const float (&q)[8],float u,float v,bool mirror,float* out){
                const float s=mirror?(hw-u)/(2*hw):(u+hw)/(2*hw),t=(hl-v)/(2*hl);
                out[0]=q[0]+(q[2]-q[0])*s;out[1]=q[1]+(q[7]-q[1])*t;
            };
            for(size_t i=0;i<rim.size();++i)
            {
                const Point& a=rim[i];
                const Point& b=rim[(i+1)%rim.size()];
                float f[8],k[8];
                map(face,0,0,false,f);map(face,a.u,a.v,false,f+2);map(face,b.u,b.v,false,f+4);
                f[6]=f[0];f[7]=f[1];
                m.Quad(at(0,ht,0),at(a.u,ht,a.v),at(b.u,ht,b.v),at(0,ht,0),dir(0,1,0),white,f);
                map(back,0,0,true,k);map(back,b.u,b.v,true,k+2);map(back,a.u,a.v,true,k+4);
                k[6]=k[0];k[7]=k[1];
                m.Quad(at(0,-ht,0),at(b.u,-ht,b.v),at(a.u,-ht,a.v),at(0,-ht,0),dir(0,-1,0),white,k);
                const float mid=(a.a+b.a)/2;
                m.Quad(at(a.u,-ht,a.v),at(b.u,-ht,b.v),at(b.u,ht,b.v),at(a.u,ht,a.v),dir(std::cos(mid),0,std::sin(mid)),edge);
            }
        }

        bool InFlight(int card)
        {
            return std::any_of(state.flights.begin(),state.flights.end(),[&](const Flight& f){return f.card==card;});
        }

        // What the table's geometry shows.
        std::vector<int> Scene()
        {
            auto& t=state.table;
            std::vector<int> out;
            out.push_back(t.Moves());
            out.push_back(state.movesSeen);
            out.push_back(static_cast<int>(t.Now()));
            out.push_back(state.hover);
            for(const auto& f:state.flights)out.push_back(1000+f.card);
            return out;
        }

        std::vector<MascotRenderer::GeometryVertex> BuildTable()
        {
            Mesh mesh;
            const float D=state.upper;
            BuildRoom(mesh,FieldZ());
            (void)D;
            PlaceCards();
            // The cards thrown out (those on their way, and the hands, are drawn with what moves).
            if(state.movesSeen==state.table.Moves())
                for(int c:state.table.Discards())
                {
                    const Pose& p=state.poses[static_cast<size_t>(c)];
                    if(p.shown&&!InFlight(c))AddCard(mesh,p,c);
                }
            return std::move(mesh.out);
        }

        // The pose between keys at time (eased, arcing up a little on the way).
        bool PoseAt(const Flight& f,double time,Pose& out)
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
                out={mix(a.x,b.x),mix(a.y,b.y),mix(a.z,b.z),mix(a.yaw,b.yaw),mix(a.flip,b.flip),mix(a.w,b.w),mix(a.l,b.l),true,mix(a.tilt,b.tilt),mix(a.spin,b.spin)};
                if(f.arc)out.y+=std::sin(u*DirectX::XM_PI)*std::max(0.06f*state.upper,0.15f*std::hypot(b.x-a.x,b.z-a.z));
                return true;
            }
            return false;   // arrived
        }

        void Finish()
        {
            if(!state.window)return;
            KillTimer(state.window,TimerId);
            if(state.her.model)
            {
                state.her.model->SetSolids({});
                state.her.model->Hold(0,{},false);
                state.her.model->Hold(1,{},false);
                state.her.model->HoldPose(0,nullptr);
                state.her.model->HoldPose(1,nullptr);
                state.her.model->SetMouth({},false);
            }
            if(state.her.model&&state.tell!=Expression::Neutral)state.her.model->SetExpression(Expression::Neutral,false);
            state.tell=Expression::Neutral;
            if(state.afterword.empty()&&state.seen.started&&state.table.Now()!=Phase::GameOver)
            {
                const auto& t=state.table;
                state.afterword=Fmt(T(L"いま、話し相手とババ抜きをしていたが、途中でやめてデスクトップに戻ってきたところ（ババは{0}が持っていた）。そのことを話し相手にひと言（1〜2文）",
                                      L"You were playing Old Maid with them, but they stopped midway and you have come back to the desktop ({0} had the joker). Say a word about it to them (one or two sentences)"),
                                    {t.JokerHolder()==OldMaid::Her?std::wstring(T(L"あなた",L"you")):std::wstring(T(L"話し相手",L"they"))});
                state.afterwordLine=T(L"また続き、やろうね。",L"Let's finish it another time.");
            }
            auto onEnd=std::move(state.onEnd);
            DestroyWindow(state.window);
            if(onEnd)onEnd();
        }

        void Prefetch();

        void SetTell(Expression e)
        {
            if(e==state.tell)return;
            state.tell=e;
            if(state.her.model&&state.renderer)state.her.model->SetExpression(e,false);
        }

        // Her face as you point at a card, when she holds the joker: a little smile at the
        // joker and a worried look at the others, or (each card once a turn, now and then) the
        // other way about, a smile put on to tempt you or a worried look to put you off.
        void UpdateTell()
        {
            auto& t=state.table;
            Expression e=Expression::Neutral;
            const auto& h=t.Hand(OldMaid::Her);
            if(t.Now()==Phase::YourDraw&&HerJoker()&&state.hover>=0&&state.hover<static_cast<int>(h.size()))
            {
                if(state.honestFor!=t.Draws()){state.honestFor=t.Draws();state.honest.clear();}
                const int card=h[static_cast<size_t>(state.hover)];
                if(!state.honest.count(card))
                {
                    std::uniform_real_distribution<double> any(0,1);
                    state.honest[card]=any(state.sparkRandom)>=BluffShare;
                }
                const bool joker=card==OldMaid::Joker;
                e=joker==state.honest[card]?Expression::Smile:Expression::Troubled;
            }
            else if(state.clock<state.reactUntil)e=state.reactFace;   // a moment's face on the joker drawn
            SetTell(e);
        }

        void NewGame()
        {
            state.gameOverTold=false;
            std::random_device device;
            state.table.NewGame(device(),state.firstNext);
            state.firstNext=state.firstNext==OldMaid::You?OldMaid::Her:OldMaid::You;
            state.hover=-1;
            state.herPick=-1;
            state.shuffledFor=0;
            state.saidFor=-1;
            state.honestFor=-1;
            state.confirmQuit=false;
            state.said.clear();
            state.nextAt=GetTickCount64()+300;
            state.seen={};
            state.words.waiting=false;
            state.words.text.clear();
            state.afterword.clear();
            state.afterwordLine.clear();
            state.plannedFor=state.reachedFor=-1;
            UpdateTell();
            Prefetch();
        }

        // After a draw of yours: hers waits a moment.
        void Moved(ULONGLONG wait=YourWait)
        {
            state.nextAt=GetTickCount64()+wait;
            state.hover=-1;
            UpdateTell();
            Redraw();
        }

        void Continue()
        {
            if(EndShown())NewGame();
            Redraw();
        }

        // Your draw: the card at place i of her hand.
        void DrawAt(int i)
        {
            auto& t=state.table;
            if(!YourDrawNow()||i<0||i>=static_cast<int>(t.Hand(OldMaid::Her).size()))return;
            t.Draw(i);
            Moved();
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
            state.words.told+=Fmt(L"  {0} s, weight {1}: {2}\n",{std::to_wstring(static_cast<int>(state.clock)),std::to_wstring(weight),seen});
            if(state.her.onEvent&&!theirs.empty())
                state.her.onEvent(seen+Fmt(T(L"（手札は{0}が{1}枚、持ち主が{2}枚）",L" ({0} has {1} cards, the owner {2})"),
                                           {HerName(),std::to_wstring(t.Hand(OldMaid::Her).size()),std::to_wstring(t.Hand(OldMaid::You).size())}),weight,Pick(theirs));
            if(lines.empty())return;
            auto& w=state.words;
            const ULONGLONG now=GetTickCount64();
            if((w.waiting||now-w.since<WordsHold)&&weight<w.weight)return;
            const std::wstring line=Pick(lines);
            if(!line.empty())SayWords(line,weight);
        }

        // The scenes' plain lines (res/oldmaid/lines.txt: "[name] about" then a line each).
        void LoadScenes()
        {
            state.scenes.clear();
            const HRSRC info=FindResourceW(nullptr,L"LINES",L"OLDMAIDLINES");
            const HGLOBAL data=info?LoadResource(nullptr,info):nullptr;
            const char* text=data?static_cast<const char*>(LockResource(data)):nullptr;
            if(!text)return;
            const int size=static_cast<int>(SizeofResource(nullptr,info));
            std::wstring all(static_cast<size_t>(MultiByteToWideChar(CP_UTF8,0,text,size,nullptr,0)),L'\0');
            MultiByteToWideChar(CP_UTF8,0,text,size,all.data(),static_cast<int>(all.size()));
            for(size_t start=0;start<all.size();)
            {
                size_t end=all.find(L'\n',start);
                if(end==std::wstring::npos)end=all.size();
                std::wstring line=all.substr(start,end-start);
                start=end+1;
                if(!line.empty()&&line.back()==L'\r')line.pop_back();
                if(!line.empty()&&line[0]==0xFEFF)line.erase(0,1);
                if(line.empty()||line[0]==L'#')continue;
                if(line[0]==L'[')
                {
                    const size_t close=line.find(L']');
                    if(close==std::wstring::npos)continue;
                    State::Scene scene;
                    scene.name=line.substr(1,close-1);
                    const size_t about=line.find_first_not_of(L" \t",close+1);
                    if(about!=std::wstring::npos)scene.about=line.substr(about);
                    state.scenes.push_back(std::move(scene));
                }
                else if(!state.scenes.empty())state.scenes.back().draft.push_back(line);
            }
        }

        State::Scene* SceneNamed(const wchar_t* name)
        {
            for(auto& scene:state.scenes)if(scene.name==name)return &scene;
            return nullptr;
        }

        // The key her styled lines are kept under: who she is and how she talks, the language,
        // and the draft (a change to any asks again).
        std::wstring StyleKey()
        {
            std::wstring all=state.her.styleKey+(English()?L"\nen":L"\nja");
            for(const auto& scene:state.scenes){all+=L"\n["+scene.name+L"]";for(const auto& l:scene.draft)all+=L"\n"+l;}
            unsigned long long hash=1469598103934665603ull;
            for(const wchar_t c:all){hash^=static_cast<unsigned long long>(c);hash*=1099511628211ull;}
            wchar_t hex[20]{};
            swprintf_s(hex,L"%016llx",hash);
            return hex;
        }

        // Her styled lines kept from before (the file: "# key", then "[name]" and a line each).
        void LoadStyled()
        {
            if(state.her.linesFile.empty())return;
            FILE* in=nullptr;
            if(_wfopen_s(&in,state.her.linesFile.c_str(),L"r, ccs=UTF-8")!=0||!in)return;
            std::wstring all;
            wchar_t chunk[1024];
            while(fgetws(chunk,1024,in))all+=chunk;
            fclose(in);
            std::wstringstream lines(all);
            std::wstring line,key=L"# "+StyleKey();
            if(!std::getline(lines,line)||line!=key)return;
            State::Scene* at=nullptr;
            while(std::getline(lines,line))
            {
                if(!line.empty()&&line.back()==L'\r')line.pop_back();
                if(line.empty())continue;
                if(line[0]==L'['){const size_t close=line.find(L']');at=close==std::wstring::npos?nullptr:SceneNamed(line.substr(1,close-1).c_str());continue;}
                if(at)at->styled.push_back(line);
            }
            for(auto& scene:state.scenes)scene.asked=!scene.styled.empty();
        }

        void SaveStyled()
        {
            if(state.her.linesFile.empty())return;
            FILE* out=nullptr;
            if(_wfopen_s(&out,state.her.linesFile.c_str(),L"w, ccs=UTF-8")!=0||!out)return;
            fputws((L"# "+StyleKey()+L"\n").c_str(),out);
            for(const auto& scene:state.scenes)
            {
                if(scene.styled.empty())continue;
                fputws((L"["+scene.name+L"]\n").c_str(),out);
                for(const auto& l:scene.styled)fputws((l+L"\n").c_str(),out);
            }
            fclose(out);
        }

        // The next scene not yet in her own words: its draft given to the LLM to say again her
        // way, a scene at a time (a small model keeps to the form so).
        void Prefetch()
        {
            if(state.prefetching||!state.her.prefetch)return;
            State::Scene* want=nullptr;
            for(auto& scene:state.scenes)if(!scene.asked&&!scene.draft.empty()){want=&scene;break;}
            if(!want)return;
            std::wstring numbered;
            for(size_t i=0;i<want->draft.size();++i)numbered+=std::to_wstring(i+1)+(English()?L": ":L"：")+want->draft[i]+L"\n";
            const std::wstring count=std::to_wstring(want->draft.size());
            const std::wstring ask=English()?
                Fmt(L"You are playing Old Maid with them. The moment: {0}.\n"
                    L"Say each of these {1} lines (Japanese) in English, the way you yourself talk to them (your way of speaking in every line). "
                    L"Keep what each says; add nothing, one sentence each.\n{2}"
                    L"Answer in {1} lines, numbered as these, in the form \"1: the line\". Do not say \"joker\" unless the line does. "
                    L"Write nothing else. No feeling tags, names, anything in ( ) or emoji.",{want->about,count,numbered}):
                Fmt(L"話し相手とババ抜きをしています。場面：{0}。台詞はどれも、あなたが話し相手に向かって言うものです。\n"
                    L"次の{1}個の台詞を、それぞれ、あなたがふだん話すときの口調に言い換えてください（一人称、語尾、口ぐせなど、あなたらしい話し方をどの行にも必ず入れる。元のままの行は作らない）。"
                    L"言っていることは変えず、言葉を足したり、2文にしたりしません。\n{2}"
                    L"答えは{1}行で、元と同じ番号を付けて「1：言い換えた台詞」の形で書きます。"
                    L"ババやジョーカーという言葉は、元になければ使いません。この形の行のほかは何も書かないでください。気持ちの印、名前、（ ）書き、絵文字は使わないでください。",
                    {want->about,count,numbered});
            state.prefetching=state.her.prefetch(ask);
            if(state.prefetching)
            {
                want->asked=true;
                state.asking=static_cast<int>(want-state.scenes.data());
            }
        }

        // A line of the scene: in her own words when there are, else the draft (none in English
        // until they come); not one of her last few.
        std::wstring Line(const wchar_t* name)
        {
            const State::Scene* scene=SceneNamed(name);
            if(!scene)return {};
            const auto& pool=!scene->styled.empty()?scene->styled:English()?scene->styled:scene->draft;
            if(pool.empty())return {};
            std::vector<const std::wstring*> fresh;
            for(const auto& l:pool)
                if(std::find(state.words.recent.begin(),state.words.recent.end(),l)==state.words.recent.end())fresh.push_back(&l);
            if(fresh.empty())for(const auto& l:pool)fresh.push_back(&l);
            std::uniform_int_distribution<size_t> any(0,fresh.size()-1);
            return *fresh[any(state.sparkRandom)];
        }

        void Say(const wchar_t* name,int weight=2)
        {
            const std::wstring line=Line(name);
            if(!line.empty())SayWords(line,weight);
        }

        // What is new in the game since the last tick, for words: its start, the joker drawn
        // (by either), the game's end.
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
            if(Flying())return;   // the cards are still on their way
            const std::wstring name=HerName();
            if(!seen.started)
            {
                seen.started=true;
                Happened(6,T(L"これから話し相手とババ抜きが始まる。",L"A game of Old Maid with them is about to begin. "),
                         T(L"わくわくする気持ち",L"how you look forward to it"),
                         {Line(L"始まり")},
                         Fmt(T(L"{0}とこのパソコンの持ち主のババ抜きが始まる。",L"{0} and the owner of this computer are starting a game of Old Maid."),{name}),
                         {T(L"がんばれー！",L"Good luck!"),T(L"ババを引くのはどっちかな？",L"Who'll end up with the joker?")});
            }
            // After her first word, a word on the pairs she threw out as the cards were dealt.
            if(!seen.sorted&&t.Draws()==0&&w.text.empty()&&now-w.since>1200)
            {
                seen.sorted=true;
                Say(L"揃えて捨てる");
            }
            // Each draw gets a word: the joker drawn, a pair made, or none.
            if(t.Draws()!=seen.draws&&t.Now()!=Phase::GameOver)
            {
                seen.draws=t.Draws();
                if(t.LastCard()==OldMaid::Joker&&t.LastDrawer()==OldMaid::You)
                    Happened(5,T(L"ババ抜きで、話し相手があなたの手札からババ（ジョーカー）を引いた。",L"At Old Maid, they drew the joker from your hand."),
                             T(L"ほっとして喜ぶ気持ち",L"how relieved and pleased you are"),
                             {Line(L"ジョーカーを引かせた")},
                             Fmt(T(L"ババ抜きで、持ち主が{0}からババを引いた。",L"At Old Maid, the owner drew the joker from {0}."),{name}),
                             {T(L"あっ、ババ引いた！",L"Oh, the joker!")});
                else if(t.LastCard()==OldMaid::Joker)
                    Happened(5,T(L"ババ抜きで、あなたが話し相手の手札からババ（ジョーカー）を引いてしまった。",L"At Old Maid, you drew the joker from their hand."),
                             T(L"がっかりする気持ち",L"how let down you are"),
                             {Line(L"ジョーカーを引いた")},
                             Fmt(T(L"ババ抜きで、{0}が持ち主からババを引いた。",L"At Old Maid, {0} drew the joker from the owner."),{name}),
                             {Fmt(T(L"{0}がババを引いた！",L"{0} drew the joker!"),{name})});
                else if(t.LastDrawer()==OldMaid::You)Say(t.LastPaired()?L"取られた":L"引かれた",3);
                else Say(t.LastPaired()?L"そろった":L"外れた",3);
            }
            const bool over=t.Now()==Phase::GameOver;
            if(over&&!seen.over)
            {
                const bool herLoss=t.Loser()==OldMaid::Her;
                if(herLoss)++state.wins;else ++state.losses;
                const std::wstring tally=Fmt(T(L"今回は話し相手の{0}勝{1}敗",L"so far they have won {0} and lost {1}"),{std::to_wstring(state.wins),std::to_wstring(state.losses)});
                state.afterword=Fmt(T(L"いま、話し相手とババ抜きをして、デスクトップに戻ってきたところ（{0}）。その感想を、話し相手にひと言（1〜2文）",
                                      L"You have just played Old Maid with them and come back to the desktop ({0}). Tell them what you thought of it (one or two sentences)"),{tally});
                state.afterwordLine=herLoss?T(L"ババ抜き、次は負けないよ！",L"I'll win at Old Maid next time!"):T(L"ババ抜き、楽しかったね！またやろう。",L"Old Maid was fun! Let's play again.");
                if(herLoss)Happened(9,Fmt(T(L"ババ抜きが終わって、あなたがババ（ジョーカー）を最後まで持っていて負けた（{0}）。",L"The game of Old Maid is over: you were left with the joker and lost ({0})."),{tally}),
                                    T(L"悔しい気持ち",L"how annoyed you are"),
                                    {Line(L"負けた")},
                                    Fmt(T(L"{0}と持ち主のババ抜きが終わり、{0}がババを持って負けた。",L"The Old Maid between {0} and the owner is over: {0} was left with the joker."),{name}),
                                    {T(L"持ち主の勝ちだ！",L"The owner wins!"),T(L"おつかれさま！",L"Well played!")});
                else Happened(9,Fmt(T(L"ババ抜きが終わって、話し相手がババ（ジョーカー）を最後まで持っていて、あなたの勝ち（{0}）。",L"The game of Old Maid is over: they were left with the joker, and you won ({0})."),{tally}),
                              T(L"うれしい気持ち",L"how pleased you are"),
                              {Line(L"勝った")},
                              Fmt(T(L"{0}と持ち主のババ抜きが終わり、{0}が勝った。",L"The Old Maid between {0} and the owner is over: {0} won."),{name}),
                              {Fmt(T(L"{0}の勝ちだ！",L"{0} wins!"),{name}),T(L"おつかれさま！",L"Well played!")});
            }
            seen.over=over;
        }

        void Tick()
        {
            auto& t=state.table;
            const ULONGLONG now=GetTickCount64();
            const bool seated=!state.renderer||state.entrance==5;
            if(Flying())state.nextAt=std::max(state.nextAt,now+120);   // play waits for the cards
            if(!state.said.empty()&&now-state.saidAt>SaidShown){state.said.clear();Redraw();}
            // Before your draw she shuffles her hand (once her own draw has landed).
            if(t.Now()==Phase::YourDraw&&state.shuffledFor!=t.Draws()&&t.Draws()>0&&!Flying()&&seated)
            {
                state.shuffledFor=t.Draws();
                t.ShuffleHer();
                state.hover=-1;
                state.nextAt=now+(state.renderer?static_cast<ULONGLONG>(Shuffle*1000):0);
            }
            // As you point at a card of hers (a moment on it): with the joker in her hand, a word
            // to go with her face (tempting with a smile, fretting with a worried look); without
            // it, once a turn, a word at ease.
            if(YourDrawNow()&&state.hover>=0&&state.hover<static_cast<int>(t.Hand(OldMaid::Her).size())&&now-state.hoverSince>=HoverDwell&&
               now-state.hoverSaid>=HoverEvery&&!state.words.waiting)
            {
                const int card=t.Hand(OldMaid::Her)[static_cast<size_t>(state.hover)];
                if(HerJoker()&&state.saidFor!=card)
                {
                    state.saidFor=card;
                    state.hoverSaid=now;
                    Say(state.tell==Expression::Smile?L"誘う":L"焦る");
                }
                else if(!HerJoker()&&state.seen.teased!=-t.Draws()-2)
                {
                    state.seen.teased=-t.Draws()-2;
                    state.hoverSaid=now;
                    Say(L"余裕");
                }
            }
            // Your turn starting: a word (the end coming near; her hand just shuffled; to unsettle
            // you when she holds the joker; else to let you choose).
            const bool ending=std::min(t.Hand(OldMaid::You).size(),t.Hand(OldMaid::Her).size())<=3;
            if(YourDrawNow()&&state.seen.teased!=t.Draws()&&state.hover<0)
            {
                state.seen.teased=t.Draws();
                std::uniform_real_distribution<double> any(0,1);
                if(ending&&!state.seen.nearEnd){state.seen.nearEnd=true;Say(L"残りわずか");}
                else if(any(state.sparkRandom)<0.75&&now-state.words.since>WordsHold/2)
                    Say(t.Draws()>0&&any(state.sparkRandom)<0.3?L"混ぜる":HerJoker()?L"揺さぶる":L"あなたの番");
            }
            // Her draw coming: which card of yours, time for her hand to go out to it, now and
            // then a word.
            if(t.Now()==Phase::HerDraw&&state.plannedFor!=t.Moves()&&!Flying()&&seated)
            {
                state.plannedFor=t.Moves();
                state.herPick=t.HerPick();
                state.nextAt=std::max(state.nextAt,now+(state.renderer?HerPlayWait3D:HerPlayWait));
                std::uniform_real_distribution<double> any(0,1);
                if(ending&&!state.seen.nearEnd){state.seen.nearEnd=true;Say(L"残りわずか");}
                else if(any(state.sparkRandom)<0.85&&now-state.words.since>WordsHold/2)Say(L"迷う");
            }
            if(now>=state.nextAt&&t.Now()==Phase::HerDraw&&state.plannedFor==t.Moves()&&seated)
            {
                t.Draw(state.herPick);
                state.nextAt=now+150;
            }
            UpdateTell();
            Redraw();
            NoticeWords();
            if(t.Now()==Phase::GameOver&&!state.gameOverTold&&!Flying())
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

        void PaintEnd(Graphics& g,float cw,float ch,float s)
        {
            auto& t=state.table;
            SolidBrush shade(Color(150,0,0,0));
            g.FillRectangle(&shade,0.0f,0.0f,cw,ch);
            const float w=std::min(cw-40*s,600*s),h=std::min(ch-40*s,300*s);
            const RectF box((cw-w)/2,(ch-h)/2,w,h);
            FillRound(g,box,14*s,Color(255,40,22,22),Color(255,200,170,90),2*s);
            float y=box.Y+24*s;
            const Color gold(255,240,206,120),white(255,240,240,236),soft(255,200,190,186);
            const bool youWon=t.Loser()==OldMaid::Her;
            if(youWon)Runs(g,{{T(L"あなたの勝ち！",L"You win!")}},RectF(box.X,y,w,52*s),34*s,gold);
            else Runs(g,{{HerName()},{T(L"の勝ち",L" wins")}},RectF(box.X,y,w,52*s),34*s,gold);
            y+=72*s;
            Text(g,youWon?Fmt(T(L"ババは{0}の手に残りました",L"{0} was left with the joker"),{HerName()}):std::wstring(T(L"ババはあなたの手に残りました",L"You were left with the joker")),
                 RectF(box.X,y,w,34*s),22*s,white,false,StringAlignmentCenter);
            y+=46*s;
            Text(g,Fmt(T(L"今回の成績 {0}勝{1}敗",L"This time: won {0}, lost {1}"),{std::to_wstring(state.wins),std::to_wstring(state.losses)}),RectF(box.X,y,w,28*s),18*s,soft,false,StringAlignmentCenter);
            Text(g,T(L"Enter：もう一度　Esc：閉じる",L"Enter: play again   Esc: close"),RectF(box.X,box.GetBottom()-44*s,w,30*s),17*s,soft,false,StringAlignmentCenter);
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
            const auto& hers=t.Hand(OldMaid::Her);
            const auto& yours=t.Hand(OldMaid::You);

            // Her: name and how many cards she holds.
            const float nameX=hud?28*s:112*s,nameY=hud?20*s:14*s;
            if(hud)FillRound(g,RectF(14*s,12*s,260*s,92*s),10*s,panel,edge,1.2f*s);
            else
            {
                const RectF faceBox(16*s,14*s,84*s,84*s);
                FillRound(g,faceBox,10*s,Color(255,14,50,36),Color(255,120,160,130),1.5f*s);
                if(state.face)g.DrawImage(state.face.get(),faceBox.X+3*s,faceBox.Y+3*s,faceBox.Width-6*s,faceBox.Height-6*s);
                else Text(g,HerName().substr(0,1),faceBox,40*s,white,true,StringAlignmentCenter,StringAlignmentCenter);
            }
            Text(g,HerName(),RectF(nameX,nameY,240*s,28*s),20*s,white,true);
            Text(g,T(L"手札 ",L"Cards ")+std::to_wstring(hers.size()),RectF(nameX,nameY+32*s,230*s,26*s),18*s,soft);
            // Her words: a bubble just right of her head (as at mahjong).
            if(!state.words.text.empty())
            {
                const std::wstring& said=state.words.text;
                const float most=std::max(160*s,W*0.4f);
                const WordsBubble bubble(g,said,s,most);
                const float bw=bubble.Width(),bh=bubble.Height();
                const bool beside=hud&&state.bubbleAt[0]>=0;
                const float x=beside?std::clamp(state.bubbleAt[0],14*s,W-14*s-bw):W-14*s-bw;
                const float y=beside?std::clamp(state.bubbleAt[1]-bh*0.6f,96*s,std::max(96*s,H*0.6f-bh)):96*s;
                const float ty=beside?std::clamp(state.bubbleAt[1],y+12*s,y+bh-12*s):y+std::min(bh*0.5f,30*s);
                const std::wstring shown=said.substr(0,std::min(said.size(),std::max<size_t>(1,static_cast<size_t>(state.words.shown))));
                bubble.Draw(g,x,y,ty,shown);
            }

            // The game and the tally (at the top right).
            {
                const RectF info(W-254*s,12*s,240*s,64*s);
                FillRound(g,info,10*s,Color(hud?215:200,40,18,16),edge,1.2f*s);
                Text(g,T(L"ババ抜き",L"Old Maid"),RectF(info.X+12*s,info.Y+6*s,228*s,26*s),18*s,white,true);
                Text(g,Fmt(T(L"{0}勝{1}敗　捨てた組 {2}",L"Won {0}, lost {1}   pairs out {2}"),{std::to_wstring(state.wins),std::to_wstring(state.losses),std::to_wstring(t.Discards().size()/2)}),
                     RectF(info.X+12*s,info.Y+34*s,224*s,24*s),15*s,soft);
            }

            // The cards (on the 3D table when there is one): hers in a row at the top, face down,
            // the one you point at lifted; yours below; the last pair thrown out at the left.
            if(!hud)
            {
                const float cw2=CardW2D*s,ch2=CardH2D*s;
                auto row=[&](size_t n){return std::min(cw2*0.62f,(W-360*s)/std::max<size_t>(n,1));};
                const float hs=row(hers.size());
                const float hx=mid-((hers.size()?hers.size()-1:0)*hs+cw2)/2;
                // From your left (the end of her hand) to your right, each on the one before; the
                // one you point at last, whole.
                state.cardBoxes.assign(hers.size(),RectF());
                int lifted=-1;
                for(size_t k=hers.size();k-->0;)
                {
                    const float x=hx+(hers.size()-1-k)*hs;
                    state.cardBoxes[k]=RectF(x,130*s,k==0?cw2:hs,ch2);
                    if(t.Now()==Phase::YourDraw&&static_cast<int>(k)==state.hover){lifted=static_cast<int>(k);continue;}
                    DrawCard(g,x,130*s,cw2,ch2,hers[k],false);
                }
                if(lifted>=0)
                {
                    const float x=hx+(hers.size()-1-lifted)*hs;
                    DrawCard(g,x,150*s,cw2,ch2,hers[static_cast<size_t>(lifted)],false,false,true);
                    state.cardBoxes[static_cast<size_t>(lifted)]=RectF(x,150*s,cw2,ch2);
                }
                const float ys=row(yours.size());
                const float yx=mid-((yours.size()?yours.size()-1:0)*ys+cw2)/2;
                for(size_t i=0;i<yours.size();++i)DrawCard(g,yx+i*ys,470*s,cw2,ch2,yours[i],true);
            }

            // You: how many cards you hold.
            {
                const float boxW=hud?250*s:240*s,boxH=72*s,boxY=H-boxH-12*s;
                FillRound(g,RectF(8*s,boxY,boxW,boxH),10*s,panel,edge,1.2f*s);
                Text(g,T(L"あなた",L"You"),RectF(22*s,boxY+8*s,boxW,26*s),18*s,white,true);
                Text(g,T(L"手札 ",L"Cards ")+std::to_wstring(yours.size()),RectF(22*s,boxY+38*s,boxW-24*s,24*s),16*s,soft);
            }

            // What to do now.
            if(t.Now()!=Phase::GameOver)
            {
                const std::wstring prompt=t.Now()==Phase::YourDraw?
                    std::wstring(T(L"相手の手札から1枚選んで引いてください（クリック、または←→とEnter）",L"Choose a card from her hand and draw it (click, or the arrows and Enter)")):
                    Fmt(T(L"{0}があなたの手札から引いています",L"{0} is drawing from your hand"),{HerName()});
                const float tw=TextWidth(g,prompt,17*s)+28*s,y=H-58*s;
                FillRound(g,RectF(mid-tw/2,y,tw,34*s),10*s,panel,edge,1.2f*s);
                Text(g,prompt,RectF(mid-tw/2,y,tw,34*s),17*s,t.Now()==Phase::YourDraw?gold:soft,false,StringAlignmentCenter,StringAlignmentCenter);
            }
            if(!state.said.empty())
            {
                const float tw=TextWidth(g,state.said,26*s,true)+40*s;
                const RectF bubble(mid-tw/2,H*0.30f,tw,52*s);
                FillRound(g,bubble,14*s,Color(245,255,252,240),Color(255,230,120,60),2*s);
                Text(g,state.said,bubble,26*s,Color(255,200,40,40),true,StringAlignmentCenter,StringAlignmentCenter);
            }

            if(EndShown())PaintEnd(g,W,H,s);
            if(state.confirmQuit)
            {
                SolidBrush shade(Color(160,0,0,0));
                g.FillRectangle(&shade,0.0f,0.0f,W,H);
                const RectF box(mid-220*s,ch/2.0f-70*s,440*s,140*s);
                FillRound(g,box,14*s,Color(250,40,22,22),Color(255,200,170,90),2*s);
                Text(g,T(L"ババ抜きをやめますか？",L"Stop playing?"),RectF(box.X,box.Y+22*s,box.Width,36*s),24*s,white,true,StringAlignmentCenter);
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

        // The card of hers under (x, y): at the 3D table as it is seen, or on the 2D one (the
        // ones on top first).
        int CardAt(float x,float y)
        {
            if(state.renderer)
            {
                for(int i=static_cast<int>(state.herQuads.size())-1;i>=0;--i)
                {
                    const auto& q=state.herQuads[static_cast<size_t>(i)];
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
                    if(inside&&sign!=0)return i;
                }
                return -1;
            }
            for(int i=static_cast<int>(state.cardBoxes.size())-1;i>=0;--i)
                if(state.cardBoxes[static_cast<size_t>(i)].Contains(x,y))return i;
            return -1;
        }

        bool pinned=false;   // diagnostics: the card pointed at kept (the mouse aside)

        void Point(int at)
        {
            if(pinned)return;
            if(state.table.Now()!=Phase::YourDraw)at=-1;
            if(at==state.hover)return;
            state.hover=at;
            state.hoverSince=GetTickCount64();
            UpdateTell();
            Redraw();
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
            if(t.Now()!=Phase::YourDraw)return;
            // Her cards from your left to your right are her hand from its end to its start.
            const int n=static_cast<int>(t.Hand(OldMaid::Her).size());
            if(key==VK_LEFT)Point(state.hover<0?n-1:std::min(n-1,state.hover+1));
            else if(key==VK_RIGHT)Point(state.hover<0?0:std::max(0,state.hover-1));
            else if((key==VK_RETURN||key==VK_SPACE)&&state.hover>=0)DrawAt(state.hover);
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
                Point(CardAt(x,y));
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
                if(const int at=CardAt(x,y);at>=0)DrawAt(at);
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

    bool OldMaidWindowOpen(){return state.window!=nullptr;}
    HWND OldMaidWindowHandle(){return state.window;}
    bool OldMaidWindowRect(RECT& r)
    {
        return state.window&&IsWindowVisible(state.window)&&!IsIconic(state.window)&&GetWindowRect(state.window,&r);
    }

    namespace
    {
        // Diagnostics: draws made for both (yours at random), her hand shuffled before yours.
        void PlaySimply(int moves)
        {
            auto& t=state.table;
            for(int i=0;i<moves&&t.Now()!=Phase::GameOver;++i)
            {
                t.PlayForYou();
                if(t.Now()==Phase::YourDraw){t.ShuffleHer();state.shuffledFor=t.Draws();}
            }
            state.nextAt=0;
            state.hover=-1;
        }
    }

    void OldMaidLines(const std::wstring& answer)
    {
        state.prefetching=false;
        if(!state.window)return;
        const int at=state.asking;
        state.asking=-1;
        if(at>=0&&at<static_cast<int>(state.scenes.size()))
        {
            auto& scene=state.scenes[static_cast<size_t>(at)];
            std::vector<std::wstring> got;
            for(size_t from=0;from<answer.size();)
            {
                size_t end=answer.find(L'\n',from);
                if(end==std::wstring::npos)end=answer.size();
                std::wstring line=answer.substr(from,end-from);
                from=end+1;
                // "1：line" (a model may put 番号 or a dot before it): what follows the number.
                size_t i=0;
                auto skip=[&](const wchar_t* chars){while(i<line.size()&&wcschr(chars,line[i]))++i;};
                skip(L" \t\r　");
                if(line.compare(i,2,L"番号")==0)i+=2;
                skip(L"：: ");
                const size_t digits=i;
                skip(L"0123456789０１２３４５６７８９");
                if(i==digits)continue;   // no number: not one of the lines
                skip(L"：:.．)） \t　");
                std::wstring said=line.substr(i);
                for(const wchar_t* drop:{L"「",L"」",L"\"",L"*"})
                    for(size_t p;(p=said.find(drop))!=std::wstring::npos;)said.erase(p,wcslen(drop));
                const size_t a=said.find_first_not_of(L" \t\r　"),b=said.find_last_not_of(L" \t\r　");
                if(a==std::wstring::npos)continue;
                said=said.substr(a,b-a+1);
                if(said.size()>60||std::find(got.begin(),got.end(),said)!=got.end())continue;
                got.push_back(said);
            }
            // Kept when most came back (a few short is fine; a stray answer is not).
            if(got.size()*2>=scene.draft.size())
            {
                scene.styled=std::move(got);
                SaveStyled();
            }
            state.words.told+=Fmt(L"  lines in her words: {0} ({1} of {2})\n",{scene.name,std::to_wstring(scene.styled.size()),std::to_wstring(scene.draft.size())});
        }
        Prefetch();
    }

    bool OldMaidAfterword(std::wstring& situation,std::wstring& fallback)
    {
        if(state.afterword.empty())return false;
        situation=std::move(state.afterword);
        fallback=std::move(state.afterwordLine);
        state.afterword.clear();
        state.afterwordLine.clear();
        return true;
    }

    void OldMaidHeard(const std::wstring& line,bool done)
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

    void PlayOldMaidMoves(int moves,float yaw)
    {
        if(!state.window)return;
        PlaySimply(moves);
        // Your draw next, when it is: the joker pointed at if she has it (her face shows it).
        if(state.table.Now()==Phase::YourDraw)
        {
            const auto& h=state.table.Hand(OldMaid::Her);
            const auto j=std::find(h.begin(),h.end(),OldMaid::Joker);
            state.hover=j!=h.end()?static_cast<int>(j-h.begin()):static_cast<int>(h.size())/2;
            pinned=true;
            UpdateTell();
        }
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
            return model.Reach(1,from,tap?1.65:3.0,tap);   // the right hand (the left holds the fan)
        }


        // The cards on their way since the table was last seen: the card drawn to the drawer's
        // hand (and, when it pairs, shown there face up with its twin a moment before both go to
        // the pile); the rest of a hand sliding to their new places (her hand shuffled).
        void StartFlights(const std::array<Pose,Slots>& before,bool fresh)
        {
            const double now=state.clock;
            auto& t=state.table;
            const auto& after=state.poses;
            if(fresh){state.flights.clear();return;}
            auto add=[&](int card,std::vector<std::pair<double,Pose>> keys,bool arc){
                state.flightsUntil=std::max(state.flightsUntil,keys.back().first);
                state.flights.erase(std::remove_if(state.flights.begin(),state.flights.end(),[&](const Flight& f){return f.card==card;}),state.flights.end());
                state.flights.push_back({card,std::move(keys),arc});
            };
            const int drawn=t.LastCard();
            bool handled[Slots]{};
            if(drawn>=0&&before[static_cast<size_t>(drawn)].shown&&std::abs(before[static_cast<size_t>(drawn)].z-after[static_cast<size_t>(drawn)].z)+std::abs(before[static_cast<size_t>(drawn)].x-after[static_cast<size_t>(drawn)].x)>0.001f*state.upper)
            {
                handled[drawn]=true;
                if(t.LastPaired())
                {
                    // Beside its twin in the drawer's hand, both face up a moment, then warped away
                    // (shrunk into sparkles where they are).
                    const auto& out=t.Discards();
                    const int twin=out[out.size()-2];
                    handled[twin]=true;
                    const Pose& at=before[static_cast<size_t>(twin)];
                    Pose shown=at;
                    shown.flip=0;
                    shown.x+=(t.LastDrawer()==OldMaid::You?1:-1)*0.5f*CardW();
                    shown.z+=(t.LastDrawer()==OldMaid::You?1:-1)*0.04f*state.upper;
                    shown.y+=2*Thick();
                    Pose up=at;
                    up.flip=0;
                    const double held=now+Travel+Settle,gone=held+Warp;
                    auto shrunk=[&](Pose p){p.w*=0.04f;p.l*=0.04f;p.spin+=1.2f;return p;};
                    add(drawn,{{now,before[static_cast<size_t>(drawn)]},{now+Travel,shown},{held,shown},{gone,shrunk(shown)}},true);
                    add(twin,{{now,at},{now+Travel*0.6,up},{held,up},{gone,shrunk(up)}},false);
                    for(const Pose* q:{&shown,&up})state.warps.push_back({held+Warp*0.6,{q->x,q->y,q->z}});
                }
                else
                {
                    add(drawn,{{now,before[static_cast<size_t>(drawn)]},{now+Travel,after[static_cast<size_t>(drawn)]}},true);
                }
            }
            // The rest that moved: closing up the rows, or her hand shuffled.
            bool slid=false;
            for(int c=0;c<Slots;++c)
            {
                if(handled[c])continue;
                const Pose& a=before[static_cast<size_t>(c)];
                const Pose& b=after[static_cast<size_t>(c)];
                if(!a.shown||!b.shown)continue;
                const float moved=std::abs(a.x-b.x)+std::abs(a.z-b.z)+std::abs(a.y-b.y);
                if(moved>0.002f*state.upper)
                {
                    add(c,{{now,a},{now+Shuffle,b}},false);
                    slid=true;
                }
            }
            // You out of cards (you win): gold sparkles where your hand was.
            if(t.Now()==Phase::GameOver&&t.Loser()==OldMaid::Her)
            {
                state.sparkAt=now+Travel+Settle+Warp;
                state.sparkSpot=YourFan();
            }
        }
    }

    void TickOldMaidWindow(MascotModel& model,double dt)
    {
        if(!state.window||!state.renderer)return;
        auto& r=*state.renderer;
        using namespace DirectX;
        const float D=state.upper,top=Top();
        dt=std::min(dt,0.1);
        auto& t=state.table;
        // What changed on the table since the last frame: its cards set on their way.
        if(t.Moves()!=state.movesSeen)
        {
            const bool fresh=state.movesSeen<0||t.Moves()<state.movesSeen;
            const std::array<Pose,Slots> before=state.poses;
            PlaceCards();
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
        for(auto w=state.warps.begin();w!=state.warps.end();)
            if(w->first<=state.clock){Sparkle(w->second,0.5f);w=state.warps.erase(w);}else ++w;
        if(state.sparkAt>0&&state.clock>=state.sparkAt)
        {
            state.sparkAt=0;
            Sparkle(state.sparkSpot,1.2f);
        }
        // Her hand goes out to the card of yours she will draw (tried again next frame while a
        // gesture of hers ends).
        // She wavers first: her hand goes over one or two other cards of yours before hers,
        // her eyes on the card and up at you by turns. Now and then she leans in to it, looking
        // up at you as she takes it.
        if(t.Now()==Phase::HerDraw&&state.entrance==5&&!Flying()&&state.plannedFor==t.Moves()&&state.reachedFor!=t.Moves()&&
           GetTickCount64()+ReachLead>=state.nextAt)
        {
            state.reachedFor=t.Moves();
            const auto& yours=t.Hand(OldMaid::You);
            state.waver.clear();
            if(state.herPick>=0&&state.herPick<static_cast<int>(yours.size()))
            {
                std::uniform_real_distribution<double> any(0,1);
                const double start=state.clock+0.35,end=state.clock+ReachLead/1000.0;
                const int others=yours.size()<2?0:(any(state.sparkRandom)<0.5?1:2);
                std::uniform_int_distribution<int> pick(0,static_cast<int>(yours.size())-1);
                const double each=(end-start)/(others+1);
                for(int k=0;k<others;++k)
                {
                    int c=pick(state.sparkRandom);
                    if(c==state.herPick)c=(c+1)%static_cast<int>(yours.size());
                    state.waver.push_back({start+k*each,yours[static_cast<size_t>(c)]});
                }
                state.waver.push_back({start+others*each,yours[static_cast<size_t>(state.herPick)]});
                state.reachCard=state.waver.front().second;
                state.leanIn=any(state.sparkRandom)<LeanShare;
                state.handsBusyUntil=end+0.55;
            }
        }
        // The card her hand is over now.
        for(const auto& [from,card]:state.waver)if(state.clock>=from)state.reachCard=card;
        // Your hand held out toward her while she takes from it.
        state.offer+=(((t.Now()==Phase::HerDraw&&state.clock<state.handsBusyUntil)?1.0f:0.0f)-state.offer)*static_cast<float>(1.0-std::exp(-dt/0.35));
        {
            // The joker drawn, or the game over: glad when it goes her way, "oops" when it does
            // not. With cards in her hands, her face only (a gesture of the arms would let go of
            // them); a clap (or her cheeks) or "oops" once her hands are empty.
            if(!Flying()&&state.gestureFor!=t.Draws()&&t.Draws()>0)
            {
                state.gestureFor=t.Draws();
                int glad=-1;
                if(t.Now()==Phase::GameOver)glad=t.Loser()==OldMaid::Her?0:1;
                else if(t.LastCard()==OldMaid::Joker)glad=t.LastDrawer()==OldMaid::Her?0:1;
                if(glad>=0&&!t.Hand(OldMaid::Her).empty())
                {
                    state.reactFace=glad?Expression::Smile:Expression::Troubled;
                    state.reactUntil=state.clock+2.5;
                }
                else if(glad>=0)
                {
                    std::uniform_real_distribution<double> any(0,1);
                    state.endGesture=!glad?L"oops":any(state.sparkRandom)<0.5?L"cheeks":L"clap";
                    state.endGestureUntil=state.clock+1.5;
                    state.gestureFreeAt=state.clock+3.6;
                }
            }
            if(!state.endGesture.empty()&&state.entrance==5&&(model.EmoteSeated(state.endGesture,3.2)||state.clock>state.endGestureUntil))
                state.endGesture.clear();
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
        {
            // Her cards held up in her hands before her: where, found once as she sits (below her
            // head, toward you; kept with her body after); both hands there (upright: she does not
            // bend over them), the right letting go to reach for yours or in a gesture. The fan
            // is drawn from between her palms, as they are.
            std::array<float,3> head{};
            if(!state.holdSet&&state.entrance==5&&model.HeadPosition(head))
            {
                const XMVECTOR h=XMVector3Transform(XMVectorSet(head[0],head[1],head[2],1),world);
                // (Her hands here put the cards' tops at her chin.)
                const XMVECTOR there=XMVector3Transform(XMVectorSet(XMVectorGetX(h),XMVectorGetY(h)-0.36f*D,XMVectorGetZ(h)-0.34f*D,1),XMMatrixInverse(nullptr,world));
                state.holdAt={XMVectorGetX(there),XMVectorGetY(there),XMVectorGetZ(there)};
                state.holdSet=true;
            }
            const bool free=state.clock>state.gestureFreeAt&&state.endGesture.empty();
            const bool holding=state.holdSet&&free&&t.Now()!=Phase::GameOver;
            // Held out toward you while you choose from them.
            state.forward+=((t.Now()==Phase::YourDraw?0.13f:0.0f)-state.forward)*static_cast<float>(1.0-std::exp(-dt/0.3));
            const XMVECTOR hold=XMVectorAdd(XMVector3Transform(XMVectorSet(state.holdAt[0],state.holdAt[1],state.holdAt[2],1),world),XMVectorSet(0,0.02f*state.forward/0.13f*D,-state.forward*D,0));
            auto wrist=[&](float across)->std::array<float,3>{
                const XMVECTOR q=XMVector3Transform(XMVectorAdd(hold,XMVectorSet(across*D,-0.04f*D,0.02f*D,0)),XMMatrixInverse(nullptr,world));
                return {XMVectorGetX(q),XMVectorGetY(q),XMVectorGetZ(q)};
            };
            {
                std::array<float,3> pl{},pr{};
                model.PalmPosition(0,pl);model.PalmPosition(1,pr);
                const auto wl=wrist(0.07f);
                state.holdNote=Fmt(L"oldmaid hold: set {0}, holding {1}, busy {2}, wrist L {3} {4} {5}, palm L {6} {7} {8}, palm R {9} {10} {11}\n",
                                   {std::to_wstring(state.holdSet),std::to_wstring(holding),std::to_wstring(state.clock<state.handsBusyUntil),std::to_wstring(wl[0]),std::to_wstring(wl[1]),std::to_wstring(wl[2]),
                                    std::to_wstring(pl[0]),std::to_wstring(pl[1]),std::to_wstring(pl[2]),std::to_wstring(pr[0]),std::to_wstring(pr[1]),std::to_wstring(pr[2])});
            }
            model.HoldPose(0,&HoldLeft);
            model.HoldPose(1,&HoldRight);
            model.Hold(0,wrist(0.045f),holding,2);   // her left (your right) holds the fan
            // Her right beside it, or out to the card of yours she takes: right at it (leaning
            // in, as the hold leans over what is far), or as far as her arm goes sitting up.
            const auto& yours=t.Hand(OldMaid::You);
            const bool reaching=state.clock<state.handsBusyUntil&&state.reachCard>=0&&
                                std::find(yours.begin(),yours.end(),state.reachCard)!=yours.end();
            if(reaching)
            {
                const Pose& p=state.poses[static_cast<size_t>(state.reachCard)];
                const auto top=At(p,0,0,p.l*0.42f);
                // Well above the cards while she wavers, down to the card only as she takes it.
                const ULONGLONG nowTick=GetTickCount64();
                const float taking=state.nextAt>nowTick&&state.nextAt-nowTick>450?0.0f:1.0f;
                state.handDown+=(taking-state.handDown)*static_cast<float>(1.0-std::exp(-dt/0.12));
                const XMVECTOR at=XMVector3Transform(XMVectorSet(top[0],top[1]+(0.24f-0.13f*state.handDown)*D,top[2]+0.07f*D,1),XMMatrixInverse(nullptr,world));
                model.Hold(1,{XMVectorGetX(at),XMVectorGetY(at),XMVectorGetZ(at)},holding,state.leanIn?0:1);   // palm down
            }
            else model.Hold(1,wrist(-0.045f),holding,2);
            // The fan where her left hand is now (drawn this frame as she is): its foot just
            // behind her fingers, toward the middle.
            std::array<float,3> want{XMVectorGetX(hold),XMVectorGetY(hold),XMVectorGetZ(hold)};
            if(std::array<float,3> left{};holding&&model.PalmPosition(0,left))
            {
                const XMVECTOR palm=XMVector3Transform(XMVectorSet(left[0],left[1],left[2],1),world);
                want={XMVectorGetX(palm)-0.045f*D,XMVectorGetY(palm)+0.06f*D,XMVectorGetZ(palm)+0.03f*D};
            }
            if(state.holdSet||!state.fanSet)
            {
                state.fanAt=want;
                state.fanSet=true;
            }
        }
        // Close before her, a little above her hands, looking at her and her cards.
        const auto& c=state.camera;
        // (Her head followed slowly: its little moves would shake the view, and your hand in it.)
        if(std::array<float,3> h{};model.HeadPosition(h))
        {
            const XMVECTOR q=XMVector3Transform(XMVectorSet(h[0],h[1],h[2],1),world);
            const std::array<float,3> now{XMVectorGetX(q),XMVectorGetY(q),XMVectorGetZ(q)};
            if(!state.faceSet){state.headSeen=now;state.faceSet=true;}
            const float k=static_cast<float>(1.0-std::exp(-dt/(state.entrance==5?0.8:0.3)));
            for(int a=0;a<3;++a)state.headSeen[a]+=(now[a]-state.headSeen[a])*k;
        }
        const std::array<float,3> face=state.faceSet?state.headSeen:std::array<float,3>{0,top+0.9f*D,state.herZ-0.6f*D};
        const XMVECTOR target=XMVectorSet(face[0]+c.x*D,face[1]+(c.y-0.44f)*D,face[2]+c.z*D,0);
        const XMVECTOR toEye=XMVectorSet(std::sin(c.yaw)*std::cos(c.pitch),std::sin(c.pitch),-std::cos(c.yaw)*std::cos(c.pitch),0);
        const XMVECTOR eye=XMVectorAdd(target,XMVectorScale(toEye,c.distance*(1+0.22f*standing)*D));
        r.SetCamera(eye,target,XMConvertToRadians(42.0f));
        {
            // Your hand in the air before you, low at the right of the view, turned to face you.
            const XMVECTOR ahead=XMVector3Normalize(XMVectorSubtract(target,eye));
            const XMVECTOR side=XMVector3Normalize(XMVector3Cross(XMVectorSet(0,1,0,0),ahead));   // your right
            const XMVECTOR at=XMVectorAdd(XMVectorAdd(XMVectorAdd(eye,XMVectorScale(ahead,0.80f*D)),XMVectorScale(side,0.34f*D)),XMVectorSet(0,-0.30f*D,0,0));
            // Held out toward her, before her hands, as she takes from it.
            const XMVECTOR toHer=XMVectorSet(state.fanAt[0]*0.5f+0.12f*D,state.fanAt[1]-0.30f*D,state.fanAt[2]-0.45f*D,0);
            const XMVECTOR held=XMVectorLerp(at,toHer,0.6f*Smooth01(state.offer));
            state.yourFan={XMVectorGetX(held),XMVectorGetY(held),XMVectorGetZ(held)};
            state.yourYaw=-c.yaw;
        }
        // What moves: her chair, the hands held up (hers where her hands are now), and the
        // cards on their way.
        {
            Mesh moving;
            moving.out=BuildChair(state.chairZ);
            if(state.movesSeen==t.Moves())
            {
                PlaceCards();
                for(Side side:{OldMaid::You,OldMaid::Her})
                    for(int c:t.Hand(side))
                        if(!InFlight(c))AddCard(moving,state.poses[static_cast<size_t>(c)],c);
            }
            for(const auto& f:state.flights)
            {
                Pose p;
                if(PoseAt(f,state.clock,p)&&state.clock>=f.keys.front().first)AddCard(moving,p,f.card);
            }
            r.SetMovingGeometry(std::move(moving.out));
        }
        {
            const auto& hers=t.Hand(OldMaid::Her);
            state.herQuads.resize(hers.size());
            for(size_t i=0;i<hers.size();++i)
            {
                const Pose& p=state.poses[static_cast<size_t>(hers[i])];
                const float hw=p.w/2,hl=p.l/2;
                const std::array<float,3> corners[4]={At(p,-hw,0,hl),At(p,hw,0,hl),At(p,hw,0,-hl),At(p,-hw,0,-hl)};
                for(int k=0;k<4;++k)state.herQuads[i][static_cast<size_t>(k)]=r.Project(corners[k]);
            }
        }
        if(std::array<float,3> head{};model.HeadPosition(head))
        {
            const XMVECTOR at=XMVector3Transform(XMVectorSet(head[0],head[1],head[2],1),world);
            const XMVECTOR right=XMVector3Normalize(XMVector3Cross(XMVectorSet(0,1,0,0),XMVectorSubtract(target,eye)));
            const XMVECTOR beside=XMVectorAdd(at,XMVectorScale(right,0.26f*D));
            state.headScreen=r.Project({XMVectorGetX(at),XMVectorGetY(at),XMVectorGetZ(at)});
            state.besideHead=r.Project({XMVectorGetX(beside),XMVectorGetY(beside),XMVectorGetZ(beside)});
            RECT client{};
            GetClientRect(state.window,&client);
            const float want[2]={std::max(static_cast<float>(state.besideHead.x),static_cast<float>(state.headScreen.x)+0.11f*client.right),static_cast<float>(state.headScreen.y)};
            const float k=state.bubbleAt[0]<0?1.0f:static_cast<float>(1.0-std::exp(-dt/0.4));
            for(int a=0;a<2;++a)state.bubbleAt[static_cast<size_t>(a)]+=(want[a]-state.bubbleAt[static_cast<size_t>(a)])*k;
        }
        {
            const XMMATRIX toModel=XMMatrixInverse(nullptr,world);
            std::array<float,3> head{};
            model.HeadPosition(head);
            XMVECTOR look=eye;
            bool level=true;
            const auto& hers=t.Hand(OldMaid::Her);
            const auto& yours=t.Hand(OldMaid::You);
            if(t.Now()==Phase::YourDraw&&state.hover>=0&&state.hover<static_cast<int>(hers.size()))
            {
                const Pose& p=state.poses[static_cast<size_t>(hers[static_cast<size_t>(state.hover)])];
                look=XMVectorSet(p.x,p.y,p.z,1);
                level=false;
            }
            else if(state.clock<state.handsBusyUntil&&state.reachCard>=0&&std::find(yours.begin(),yours.end(),state.reachCard)!=yours.end())
            {
                const Pose& p=state.poses[static_cast<size_t>(state.reachCard)];
                const bool atYou=state.leanIn||std::fmod(state.clock,1.1)>0.7;
                if(!atYou)look=XMVectorSet(p.x,p.y,p.z,1);
                level=atYou;   // (at you at her own eyes' height: looking up rolled her eyes back)
            }
            const XMVECTOR at=XMVector3Transform(look,toModel);
            model.LookToward({XMVectorGetX(at),level?head[1]:XMVectorGetY(at),XMVectorGetZ(at)},true);
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
            // closing about five times a second), and closes after.
            const auto& w=state.words;
            const bool saying=!w.text.empty()&&w.text!=L"…"&&w.shown<static_cast<double>(w.text.size());
            if(saying)
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

    std::wstring DescribeOldMaid()
    {
        if(!state.window)return L"";
        auto& t=state.table;
        std::wstring out=Fmt(L"oldmaid: entrance {0}, phase {1}, draws {2}, yours {3}, hers {4}, joker {5}, pairs out {6}, flights {7}, pointed {8}, face {9}\n",
                             {std::to_wstring(state.entrance),std::to_wstring(static_cast<int>(t.Now())),std::to_wstring(t.Draws()),std::to_wstring(t.Hand(OldMaid::You).size()),
                              std::to_wstring(t.Hand(OldMaid::Her).size()),HerJoker()?L"hers":L"yours",std::to_wstring(t.Discards().size()/2),std::to_wstring(state.flights.size()),
                              std::to_wstring(state.hover),std::to_wstring(static_cast<int>(state.tell))});
        out+=state.holdNote;
        out+=Fmt(L"oldmaid fans: hers {0} {1} {2}, yours {3} {4} {5} (upper {6}, top {7}, her z {8})\n",{std::to_wstring(state.fanAt[0]),std::to_wstring(state.fanAt[1]),std::to_wstring(state.fanAt[2]),
                 std::to_wstring(state.yourFan[0]),std::to_wstring(state.yourFan[1]),std::to_wstring(state.yourFan[2]),std::to_wstring(state.upper),std::to_wstring(Top()),std::to_wstring(state.herZ)});
        for(size_t i=0;i<state.herQuads.size();++i)
        {
            const auto& q=state.herQuads[i];
            const auto& h=t.Hand(OldMaid::Her);
            const Pose& p=state.poses[static_cast<size_t>(h[i])];
            out+=Fmt(L"oldmaid her card {0}: screen {1},{2} {3},{4}; pose {5} {6} {7} tilt {8} spin {9} shown {10}\n",{std::to_wstring(i),std::to_wstring(q[0].x),std::to_wstring(q[0].y),std::to_wstring(q[2].x),std::to_wstring(q[2].y),
                     std::to_wstring(p.x),std::to_wstring(p.y),std::to_wstring(p.z),std::to_wstring(p.tilt),std::to_wstring(p.spin),std::to_wstring(p.shown)});
        }
        return out+(state.words.told.empty()?std::wstring():L"oldmaid words:\n"+state.words.told);
    }

    bool SaveOldMaidView(const std::wstring& path)
    {
        return state.renderer&&state.renderer->SaveLastFrame(path);
    }

    bool SaveOldMaidShot(const std::wstring& path,int moves,std::uint32_t seed)
    {
        GdiplusStartupInput input;
        if(!state.gdiplus)GdiplusStartup(&state.gdiplus,&input,nullptr);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc=DefWindowProcW;
        wc.hInstance=GetModuleHandleW(nullptr);
        wc.lpszClassName=L"MikuMikuDesktopPerchOldMaidShot";
        RegisterClassExW(&wc);
        const int dpi=static_cast<int>(GetDpiForSystem());
        state.window=CreateWindowExW(0,wc.lpszClassName,L"",WS_OVERLAPPEDWINDOW,0,0,MulDiv(static_cast<int>(BaseWidth)+16,dpi,96),MulDiv(static_cast<int>(BaseHeight)+39,dpi,96),
                                     nullptr,nullptr,wc.hInstance,nullptr);
        state.her=OldMaidOpponent{L"ミク"};
        state.table.NewGame(seed,OldMaid::You);
        PlaySimply(moves);
        // Your draw next, when it is: one of her cards pointed at.
        if(state.table.Now()==Phase::YourDraw)state.hover=static_cast<int>(state.table.Hand(OldMaid::Her).size())/2;
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

    void ShowOldMaidWindow(HWND owner,const OldMaidOpponent& her,std::function<void()> onEnd)
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
        state.poses={};
        state.flights.clear();
        state.gestureFor=-1;
        LoadScenes();
        LoadStyled();
        state.asking=-1;
        state.fanSet=state.faceSet=state.holdSet=false;
        state.warps.clear();
        state.handsBusyUntil=state.gestureFreeAt=0;
        state.reachCard=-1;
        state.forward=state.offer=0;
        state.waver.clear();
        state.bubbleAt={-1,-1};
        state.wins=state.losses=0;
        state.tell=Expression::Neutral;
        state.prefetching=false;
        state.words={};
        NewGame();

        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&info);
        const RECT work=info.rcWork;
        const int dpi=static_cast<int>(GetDpiForSystem());
        const int w=std::min(static_cast<int>(work.right-work.left)*9/10,MulDiv(static_cast<int>(BaseWidth)+16,dpi,96));
        const int h=std::min(static_cast<int>(work.bottom-work.top)*9/10,MulDiv(static_cast<int>(BaseHeight)+39,dpi,96));
        const std::wstring title=std::wstring(T(L"ババ抜き",L"Old Maid"))+L" - "+HerName();
        state.window=CreateWindowExW(0,ClassName,title.c_str(),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                                     work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,
                                     nullptr,nullptr,instance,nullptr);
        if(CreateView())PlaceView();
        SetTimer(state.window,TimerId,40,nullptr);
        ShowWindow(state.window,SW_SHOWNORMAL);
        SetForegroundWindow(state.window);
    }
}
