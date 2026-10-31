#define NOMINMAX
#include "HanafudaWindow.hpp"
#include "Hanafuda.hpp"
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
        using Hanafuda::Phase;
        using Hanafuda::Cards;
        constexpr wchar_t ClassName[]=L"MikuMikuDesktopPerchHanafuda";
        constexpr wchar_t ViewClassName[]=L"MikuMikuDesktopPerchHanafudaView";
        constexpr UINT_PTR TimerId=1;
        // How carefully she plays (Hanafuda::Table::NewGame): a little less as you grow closer,
        // and after games you lost in a row.
        constexpr double Care=0.85,LessPerBond=0.03,LessPerLoss=0.08,LeastCare=0.4;
        // Her pace: before her card, before she turns up the pile, before your pile card is
        // turned up for you, before a round's end is shown.
        constexpr ULONGLONG HerPlayWait=650,HerPileWait=450,YourPileWait=380,EndWait=900;
        constexpr ULONGLONG SaidShown=1600;
        constexpr ULONGLONG ReachLead=600;         // her hand starts out this long before her card goes
        constexpr ULONGLONG HerPlayWait3D=1000;    // at the 3D table: time for her hand to go out
        constexpr ULONGLONG ThinkExtra=2000;       // her card later by this when she stops to think
        constexpr ULONGLONG EndWait3D=2600;        // at the 3D table: a round won waits for her reaction
        // The cards on their way at the 3D table (seconds): to where they go, held there when
        // they took cards, and gathered into the pile taken; a deal's card each this far apart.
        constexpr double Travel=0.34,Hold=0.30,Gather=0.34,Slide=0.22,DealEach=0.062;
        // Sizes in logical pixels (at 96 dpi, at the size the window opens).
        constexpr float BaseWidth=1180,BaseHeight=840;
        constexpr float HandW=80,HandH=131,FieldW=64,FieldH=105,SmallW=40,SmallH=66;

        enum class Act {Koi,Stop};
        struct Button {Act act;std::wstring label;RectF box;};

        // Where a card is at the 3D table (world units): its middle, turned about the upright by
        // yaw, its top raised toward the upright by pitch (0: lying face up, its top away from
        // you), turned over about its length by flip (1: face down), w wide and l long.
        struct Pose {float x=0,y=0,z=0,yaw=0,pitch=0,flip=0,w=0,l=0;bool shown=false;};
        // A card on its way: where it is at each time (seconds of the clock).
        struct Flight {int card=-1;std::vector<std::pair<double,Pose>> keys;bool arc=true;};

        struct State
        {
            HWND window=nullptr;
            ULONG_PTR gdiplus=0;
            HanafudaOpponent her;
            std::unique_ptr<Bitmap> face;
            std::function<void()> onEnd;
            Hanafuda::Table table;
            int lost=0;                // games you lost in a row
            bool gameOverTold=false;
            int selected=-1,hover=-1;  // in your hand; while picking, in the matches
            bool confirmQuit=false;
            RectF quitButtons[2];
            ULONGLONG nextAt=0;
            int callsSeen=0;
            std::wstring said;
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
                bool voiced=false;   // the words are said by a voice file of their own
                std::wstring told;
            } words;
            // Her lines by scene (res/hanafuda/lines.txt, said again her way by the LLM, and the
            // user's own), and the voice of a line that has one.
            GameLines lines;
            GameVoice voice;
            bool prefetching=false;
            POINT headScreen{-1,-1},besideHead{-1,-1};
            std::wstring afterword,afterwordLine;
            struct Seen
            {
                bool started=false,over=false;
                int round=0;
                std::array<int,2> koi{};
            } seen;
            std::vector<RectF> handBoxes,fieldBoxes;   // your hand by index, the field by place
            std::vector<Button> buttons;
            std::unique_ptr<Bitmap> canvas;
            // Her at the table in 3D (none: 2D only).
            HWND view=nullptr;
            std::unique_ptr<MascotRenderer> renderer;
            int entrance=0;
            float herX=0,herZ=0,herAngle=0,entranceTime=0,chairZ=0;
            struct Camera {float yaw=0,pitch=0.46f,distance=3.0f,x=0,y=0,z=0;} camera;
            POINT dragFrom{};
            int dragging=0;
            bool leftDown=false;
            float upper=10,hips=10;
            float floorY=-5;
            std::vector<int> scene;
            // Where each card lies (world units), as the table is now, and the corners of your
            // cards' faces and the field's, to find them on the screen.
            std::array<Pose,Cards> poses{};
            std::vector<std::array<std::array<float,3>,4>> handCorners,fieldCorners;
            std::vector<std::array<float,3>> herHandSpots;
            std::array<float,3> pileSpot{};
            // The cards on their way, and until when (on the clock); sounds due on the way.
            std::vector<Flight> flights;
            double flightsUntil=0;
            std::vector<std::pair<double,MahjongSound>> sounds;
            int movesSeen=-1,pileSeen=-1,pendingSeen=-1;
            double sparkAt=0;         // gold sparkles then, where her card took a bright
            double clock=0;
            int frames=0;
            bool dealPending=false;   // a deal to show once the table is drawn smoothly
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
            // What the table was when last heard in 2D (sounds for what changed).
            int heardMoves=-1,heardPile=-1;
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

        // The cards' faces (res/hanafuda/cards.jpg): CardSide by CardTop each, eight across,
        // card n at column n%8, row n/8.
        constexpr int CardSide=192,CardTop=320,AtlasColumns=8;
        constexpr int ScrollW=320,ScrollH=768;   // the hanging scroll's painting, under the faces in the 3D table's texture

        Bitmap* Faces()
        {
            static std::unique_ptr<Bitmap> atlas=[]{
                std::unique_ptr<Bitmap> loaded;
                const HRSRC info=FindResourceW(nullptr,L"CARDS",L"HANAFUDACARDS");
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
                RoundRect(path,0,0,static_cast<float>(w),static_cast<float>(h),w*0.07f);
                g.SetClip(&path);
                g.DrawImage(faces,Rect(0,0,w,h),card%AtlasColumns*CardSide,card/AtlasColumns*CardTop,CardSide,CardTop,UnitPixel);
            }
            return face.get();
        }

        // A card at (x, y), w by h: its face (or its black back); dim: greyed; glow: a gold rim.
        void DrawCard(Graphics& g,float x,float y,float w,float h,int card,bool faceUp=true,bool dim=false,bool glow=false)
        {
            const float r=w*0.07f;
            FillRound(g,RectF(x+w*0.05f,y+h*0.04f,w,h),r,Color(70,0,0,0));
            if(glow)FillRound(g,RectF(x-w*0.07f,y-w*0.07f,w*1.14f,h+w*0.14f),r*1.6f,Color(235,255,214,90));
            if(!faceUp||card<0)
            {
                FillRound(g,RectF(x,y,w,h),r,Color(255,26,22,22),Color(255,70,40,34),1.2f);
                FillRound(g,RectF(x+w*0.12f,y+w*0.12f,w*0.76f,h-w*0.24f),r*0.6f,Color(255,52,30,28));
                return;
            }
            if(Bitmap* face=Face(card,static_cast<int>(std::lround(w)),static_cast<int>(std::lround(h))))
                g.DrawImage(face,RectF(x,y,static_cast<REAL>(face->GetWidth()),static_cast<REAL>(face->GetHeight())));
            else
            {
                FillRound(g,RectF(x,y,w,h),r,Color(255,242,233,208),Color(255,30,26,24),2);
                Text(g,Hanafuda::CardName(card),RectF(x,y,w,h),h*0.09f,Color(255,30,30,36),true,StringAlignmentCenter,StringAlignmentCenter);
            }
            if(dim)FillRound(g,RectF(x,y,w,h),r,Color(130,30,30,40));
        }

        std::wstring HerName(){return state.her.name.empty()?std::wstring(T(L"相手",L"Her")):state.her.name;}
        double CharSeconds(){return English()?0.045:0.12;}
        std::wstring Mon(int n){return std::to_wstring(n)+T(L"文",L" pts");}
        std::wstring MonthLabel(int round)
        {
            return English()?Fmt(L"Month {0} ({1})",{std::to_wstring(round),Hanafuda::MonthName(round-1)})
                            :Fmt(L"{0}月（{1}）",{std::to_wstring(round),Hanafuda::MonthName(round-1)});
        }

        bool Waiting(){return state.nextAt>GetTickCount64();}
        // The cards still on their way at the 3D table (a move not yet shown counts).
        bool Flying()
        {
            return state.renderer&&(state.dealPending||state.movesSeen!=state.table.Moves()||state.clock<state.flightsUntil);
        }
        bool Picking(){return state.table.Now()==Phase::YourPick||state.table.Now()==Phase::YourDrawPick;}
        bool YourPlayNow(){return state.table.Now()==Phase::YourPlay&&!Waiting()&&!Flying();}
        bool PickNow(){return Picking()&&!Waiting()&&!Flying();}

        void UpdateButtons()
        {
            std::vector<Button> b;
            if(state.table.Now()==Phase::YourKoi&&!Waiting()&&!Flying())
            {
                b.push_back({Act::Koi,T(L"こいこい",L"Koi-koi")});
                b.push_back({Act::Stop,T(L"勝負",L"Stop")});
            }
            state.buttons=std::move(b);
        }

        LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam);
        int HandAt(float x,float y);
        int FieldAt(float x,float y);

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
            // puts it back. A left click is the window's (a card, a button).
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
                Log(L"hanafuda: resize: %s",error.c_str());
            InvalidateRect(state.window,nullptr,FALSE);
        }

        void ReleaseView()
        {
            if(state.renderer){state.renderer->WaitIdle();state.renderer.reset();Log(L"hanafuda: 3D view released");}
            state.view=nullptr;
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
        ImageRgba8 FacesImage()
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
                Log(L"hanafuda: no 3D view (%s)",error.c_str());
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
            Log(L"hanafuda: 3D view %ux%u on %s (hips %.1f, upper body %.1f, floor %.1f)",w,h,renderer->AdapterName().c_str(),state.hips,state.upper,state.floorY);
            if(!renderer->SetGeometryTexture(FacesImage()))Log(L"hanafuda: no card faces on the 3D table");
            state.renderer=std::move(renderer);
            state.entrance=0;
            state.frames=0;
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

        // ---- The cards on the table ----
        // The felt from your edge (zFar) to hers (zNear): your hand standing at your edge,
        // leaning back, your piles taken at your right in four rows (brights, animals, ribbons,
        // chaff); the field in the middle, two rows from the middle outward, the pile at its
        // right; her hand standing at her edge with its backs to you, her piles taken at her
        // right (your left), turned to her.

        float Top(){return 0.12f*state.hips+0.30f*state.upper;}
        float Thick(){return 0.003f*state.upper;}
        float LyingW(){return 0.105f*state.upper;}
        float LyingL(){return 0.172f*state.upper;}
        float FieldZ(){return -1.10f*state.upper;}
        float PileX(){return 0.86f*state.upper;}
        float PileZ(){return -1.16f*state.upper;}

        Pose Lying(float x,float z,float yaw,float above=0,float scale=1)
        {
            Pose p;
            p.x=x;p.y=Top()+Thick()/2+above;p.z=z;p.yaw=yaw;
            p.w=LyingW()*scale;p.l=LyingL()*scale;
            p.shown=true;
            return p;
        }

        // Standing (or leaning) on its bottom edge, whose middle is at (x, z).
        Pose Standing(float x,float z,float yaw,float pitch,float w,float l,float above=0)
        {
            Pose p;
            p.w=w;p.l=l;p.yaw=yaw;p.pitch=pitch;p.shown=true;
            const float up=std::sin(pitch)*l/2,along=std::cos(pitch)*l/2;   // the middle from the bottom edge, before the yaw
            p.x=x+std::sin(yaw)*along;
            p.z=z+std::cos(yaw)*along;
            p.y=Top()+up+above;
            return p;
        }

        // A point on the card: across (-w/2 to w/2), out of its face (thickness), along it
        // toward its top (-l/2 to l/2).
        std::array<float,3> At(const Pose& p,float u,float v,float w)
        {
            using namespace DirectX;
            const XMMATRIX turn=XMMatrixRotationZ(p.flip*XM_PI)*XMMatrixRotationX(-p.pitch)*XMMatrixRotationY(p.yaw);
            const XMVECTOR q=XMVector3TransformNormal(XMVectorSet(u,v,w,0),turn);
            return {p.x+XMVectorGetX(q),p.y+XMVectorGetY(q),p.z+XMVectorGetZ(q)};
        }

        constexpr float FieldScale=1.2f;   // the field's cards, against those taken

        int FieldColumn(int place)
        {
            const int k=place/2;
            return k%2?(k+1)/2:-k/2;
        }

        // The place of a field card (its index in the field): two rows from the middle outward.
        Pose FieldPose(int place)
        {
            const float D=state.upper,pitchX=LyingW()*FieldScale+0.03f*D,pitchZ=LyingL()*FieldScale/2+0.016f*D;
            const float x=(FieldColumn(place)-0.5f)*pitchX,z=FieldZ()+(place%2==0?pitchZ:-pitchZ);
            return Lying(x,z,0,0,FieldScale);
        }

        // Where every card is, as the table is now (state.poses), and what of it is found on the
        // screen (your hand, the field, her hand's spots, the pile).
        void PlaceCards()
        {
            const float D=state.upper;
            auto& t=state.table;
            for(auto& p:state.poses)p.shown=false;
            const TableShape shape=Table3D();
            // The pile: the cards in it all at its top, face down (the pile itself is drawn as one).
            const int left=t.PileLeft();
            state.pileSpot={PileX(),Top()+left*Thick(),PileZ()};
            {
                Pose top=Lying(PileX(),PileZ(),0,std::max(0,left-1)*Thick());
                top.flip=1;
                top.shown=false;
                for(int c=0;c<Cards;++c)state.poses[static_cast<size_t>(c)]=top;
            }
            // The field, and its places on the screen.
            const auto& field=t.Field();
            const bool picking=Picking();
            const auto matches=picking?t.Matches(t.Pending()):std::vector<int>{};
            state.fieldCorners.assign(field.size(),{});
            for(size_t i=0;i<field.size();++i)
            {
                Pose p=FieldPose(static_cast<int>(i));
                const bool candidate=std::find(matches.begin(),matches.end(),static_cast<int>(i))!=matches.end();
                if(candidate)p.y+=(static_cast<int>(i)==state.hover?0.035f:0.018f)*D;
                for(int k=0;k<4;++k)state.fieldCorners[i][k]=At(p,(k==1||k==2?0.5f:-0.5f)*p.w,Thick()/2,(k<2?0.5f:-0.5f)*p.l);
                if(field[i]>=0)state.poses[static_cast<size_t>(field[i])]=p;
            }
            // Your hand, leaning back at your edge (the one pointed at lifted), and the card
            // waiting for a pick, held over the table before you.
            {
                const auto& hand=t.Of(Hanafuda::You).hand;
                const float w=0.14f*D,l=0.23f*D,gap=0.014f*D,front=shape.zFar+0.08f*D;
                const float start=-0.14f*D-(hand.size()*(w+gap)-gap)/2+w/2;
                state.handCorners.assign(hand.size(),{});
                for(size_t i=0;i<hand.size();++i)
                {
                    const bool lifted=YourPlayNow()&&(static_cast<int>(i)==state.hover||static_cast<int>(i)==state.selected);
                    const Pose p=Standing(start+i*(w+gap),front,0,1.0f,w,l,lifted?0.035f*D:0);
                    for(int k=0;k<4;++k)state.handCorners[i][k]=At(p,(k==1||k==2?0.5f:-0.5f)*w,Thick()/2,(k<2?0.5f:-0.5f)*l);
                    state.poses[static_cast<size_t>(hand[i])]=p;
                }
                if(t.Pending()>=0)
                {
                    Pose p=Lying(0,-1.62f*D,0,0.11f*D,1.25f);
                    p.pitch=0.55f;
                    state.poses[static_cast<size_t>(t.Pending())]=p;
                }
            }
            // Her hand, standing at her edge with the backs to you.
            {
                const auto& hand=t.Of(Hanafuda::Her).hand;
                const float w=LyingW(),l=LyingL(),gap=0.01f*D,edge=shape.zNear-0.07f*D;
                const float start=0.12f*D-(hand.size()*(w+gap)-gap)/2+w/2;
                state.herHandSpots.clear();
                for(size_t i=0;i<hand.size();++i)
                {
                    const Pose p=Standing(start+i*(w+gap),edge,DirectX::XM_PI,1.75f,w,l);
                    state.herHandSpots.push_back(At(p,0,0,l/2));
                    state.poses[static_cast<size_t>(hand[i])]=p;
                }
            }
            // The piles taken: four rows by kind, each from its edge in.
            for(int who:{Hanafuda::You,Hanafuda::Her})
            {
                const auto& taken=t.Of(who).taken;
                const float x0=0.56f*D,x1=1.24f*D,w=LyingW(),l=LyingL();
                for(int k=0;k<4;++k)
                {
                    std::vector<int> row;
                    for(int c:taken)if(static_cast<int>(Hanafuda::KindOf(c))==k)row.push_back(c);
                    const float step=row.size()>1?std::min(0.45f*w,(x1-x0-w)/(row.size()-1)):0;
                    const float z=who==Hanafuda::You?-1.38f*D-l/2-k*(l+0.012f*D):shape.zNear-0.07f*D-l/2-k*(l+0.012f*D);
                    for(size_t i=0;i<row.size();++i)
                    {
                        const float x=x0+w/2+i*step;
                        state.poses[static_cast<size_t>(row[i])]=who==Hanafuda::You?Lying(x,z,0,i*0.0024f*D):Lying(-x,z,DirectX::XM_PI,i*0.0024f*D);
                    }
                }
            }
        }

        // A card at pose: its face (dim: greyed; hidden: a back there too, for her hand, seen
        // from any side), its black back and edges; glow: a gold rim under it.
        void AddCard(Mesh& m,const Pose& p,int card,bool dim=false,bool glow=false,bool hidden=false)
        {
            using V=std::array<float,3>;
            using namespace DirectX;
            const float hw=p.w/2,hl=p.l/2,ht=Thick()/2;
            const XMMATRIX turn=XMMatrixRotationZ(p.flip*XM_PI)*XMMatrixRotationX(-p.pitch)*XMMatrixRotationY(p.yaw);
            auto at=[&](float u,float v,float w)->V{return At(p,u,v,w);};
            auto dir=[&](float u,float v,float w)->V{
                const XMVECTOR q=XMVector3TransformNormal(XMVectorSet(u,v,w,0),turn);
                return {XMVectorGetX(q),XMVectorGetY(q),XMVectorGetZ(q)};
            };
            const float white[4]={1,1,1,1},faded[4]={0.6f,0.6f,0.62f,1},core[4]={0.07f,0.055f,0.05f,1},gold[4]={1.0f,0.80f,0.32f,1};
            if(glow)
            {
                const float g=0.014f*state.upper;
                m.Quad(at(-hw-g,-ht-0.0006f*state.upper,hl+g),at(hw+g,-ht-0.0006f*state.upper,hl+g),at(hw+g,-ht-0.0006f*state.upper,-hl-g),at(-hw-g,-ht-0.0006f*state.upper,-hl-g),dir(0,1,0),gold);
            }
            float uv[8];
            CardUv(card,uv);
            if(hidden)m.Quad(at(-hw,ht,hl),at(hw,ht,hl),at(hw,ht,-hl),at(-hw,ht,-hl),dir(0,1,0),core);
            else m.Quad(at(-hw,ht,hl),at(hw,ht,hl),at(hw,ht,-hl),at(-hw,ht,-hl),dir(0,1,0),dim?faded:white,uv);
            m.Quad(at(-hw,-ht,-hl),at(hw,-ht,-hl),at(hw,-ht,hl),at(-hw,-ht,hl),dir(0,-1,0),core);
            m.Quad(at(-hw,-ht,hl),at(hw,-ht,hl),at(hw,ht,hl),at(-hw,ht,hl),dir(0,0,1),core);
            m.Quad(at(-hw,-ht,-hl),at(hw,-ht,-hl),at(hw,ht,-hl),at(-hw,ht,-hl),dir(0,0,-1),core);
            m.Quad(at(-hw,-ht,-hl),at(-hw,-ht,hl),at(-hw,ht,hl),at(-hw,ht,-hl),dir(-1,0,0),core);
            m.Quad(at(hw,-ht,-hl),at(hw,-ht,hl),at(hw,ht,hl),at(hw,ht,-hl),dir(1,0,0),core);
        }

        bool InFlight(int card)
        {
            return std::any_of(state.flights.begin(),state.flights.end(),[&](const Flight& f){return f.card==card;});
        }

        // Her hand's cards (on their way there too): never shown, however the camera turns.
        bool Hidden(int card)
        {
            const auto& hand=state.table.Of(Hanafuda::Her).hand;
            return std::find(hand.begin(),hand.end(),card)!=hand.end();
        }

        // The cards in your hand that would take from the field, while you are to play.
        bool CanTake(int card)
        {
            return YourPlayNow()&&!state.table.Matches(card).empty();
        }

        // The field cards of the month of the card pointed at in your hand (or of the card
        // waiting for a pick): they glow.
        std::vector<int> Glowing()
        {
            auto& t=state.table;
            if(Picking())return t.Matches(t.Pending());
            const auto& hand=t.Of(Hanafuda::You).hand;
            const int at=state.hover>=0?state.hover:state.selected;
            if(YourPlayNow()&&at>=0&&at<static_cast<int>(hand.size()))return t.Matches(hand[static_cast<size_t>(at)]);
            return {};
        }

        // What the table's geometry shows.
        std::vector<int> Scene()
        {
            auto& t=state.table;
            std::vector<int> out;
            for(int who:{Hanafuda::You,Hanafuda::Her})
            {
                for(int c:t.Of(who).hand)out.push_back(c);
                out.push_back(-1);
                for(int c:t.Of(who).taken)out.push_back(c);
                out.push_back(-2);
            }
            for(int c:t.Field())out.push_back(c);
            out.push_back(-3);
            out.push_back(t.PileLeft());
            out.push_back(t.Pending());
            out.push_back(static_cast<int>(t.Now()));
            out.push_back(YourPlayNow()?state.hover*100+state.selected:-9);
            out.push_back(PickNow()?state.hover:-9);
            for(const auto& f:state.flights)out.push_back(1000+f.card);
            out.push_back(state.dealPending?1:0);
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
            PlaceCards();
            // The pile: a stack of backs.
            const int left=state.table.PileLeft();
            if(left>0&&!state.dealPending)
            {
                const float core[4]={0.07f,0.055f,0.05f,1},back[4]={0.20f,0.07f,0.06f,1};
                const float hw=LyingW()/2,hl=LyingL()/2,h=left*Thick();
                mesh.Box(PileX()-hw,t.top,PileZ()-hl,PileX()+hw,t.top+h,PileZ()+hl,core,false);
                mesh.Quad({PileX()-hw,t.top+h,PileZ()+hl},{PileX()+hw,t.top+h,PileZ()+hl},{PileX()+hw,t.top+h,PileZ()-hl},{PileX()-hw,t.top+h,PileZ()-hl},{0,1,0},back);
            }
            const auto glowing=Glowing();
            const auto& field=state.table.Field();
            for(int c=0;c<Cards;++c)
            {
                const Pose& p=state.poses[static_cast<size_t>(c)];
                if(!p.shown||InFlight(c)||state.dealPending)continue;
                bool glow=false;
                for(int i:glowing)if(field[static_cast<size_t>(i)]==c)glow=true;
                const auto& hand=state.table.Of(Hanafuda::You).hand;
                if(std::find(hand.begin(),hand.end(),c)!=hand.end())glow=CanTake(c);
                AddCard(mesh,p,c,false,glow,Hidden(c));
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
                out={mix(a.x,b.x),mix(a.y,b.y),mix(a.z,b.z),mix(a.yaw,b.yaw),mix(a.pitch,b.pitch),mix(a.flip,b.flip),mix(a.w,b.w),mix(a.l,b.l),true};
                const float apart=std::hypot(b.x-a.x,b.z-a.z);
                if(f.arc&&apart>0.05f*state.upper)out.y+=std::sin(u*DirectX::XM_PI)*std::min(0.12f*state.upper,0.25f*apart);
                return true;
            }
            return false;   // arrived
        }

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
            if(state.afterword.empty()&&state.seen.started&&state.table.Now()!=Phase::GameOver)
            {
                const auto& t=state.table;
                state.afterword=Fmt(T(L"いま、話し相手と花札（こいこい）をしていたが、途中でやめてデスクトップに戻ってきたところ（{0}月まで、あなた{1}文、相手{2}文だった）。そのことを話し相手にひと言（1〜2文）",
                                      L"You were playing hanafuda (koi-koi) with them, but they stopped midway and you have come back to the desktop (month {0}; you had {1} points, they had {2}). Say a word about it to them (one or two sentences)"),
                                    {std::to_wstring(t.Round()),std::to_wstring(t.Of(Hanafuda::Her).score),std::to_wstring(t.Of(Hanafuda::You).score)});
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
            state.table.NewGame(device(),std::max(LeastCare,Care-LessPerBond*state.her.bond-LessPerLoss*state.lost));
            state.callsSeen=state.table.HerCalls();
            state.selected=state.hover=-1;
            state.confirmQuit=false;
            state.said.clear();
            state.nextAt=GetTickCount64()+HerPlayWait;
            state.seen={};
            state.words.waiting=false;
            state.words.text.clear();
            state.afterword.clear();
            state.afterwordLine.clear();
            Prefetch();
        }

        // After a move of yours: the next waits a moment.
        void Moved(ULONGLONG wait)
        {
            state.selected=state.hover=-1;
            state.nextAt=GetTickCount64()+wait;
            if(state.table.Now()==Phase::RoundOver)state.nextAt=GetTickCount64()+(state.renderer&&state.table.Ended().winner>=0?EndWait3D:EndWait);
            Redraw();
        }

        void Do(const Button& b)
        {
            state.table.Koi(b.act==Act::Koi);
            Moved(HerPlayWait);
        }

        bool CanPlay(int index)
        {
            return YourPlayNow()&&index>=0&&index<static_cast<int>(state.table.Of(Hanafuda::You).hand.size());
        }

        void PlayAt(int index)
        {
            if(!CanPlay(index))return;
            state.table.Play(index);
            Moved(YourPileWait);
        }

        void PickAt(int place)
        {
            if(!PickNow())return;
            const auto m=state.table.Matches(state.table.Pending());
            if(std::find(m.begin(),m.end(),place)==m.end())return;
            const bool drawn=state.table.Now()==Phase::YourDrawPick;
            state.table.Pick(place);
            Moved(drawn?HerPlayWait:YourPileWait);
        }

        void Continue()
        {
            auto& t=state.table;
            if(t.Now()==Phase::RoundOver&&!Waiting()){t.NextRound();state.nextAt=GetTickCount64()+HerPlayWait;state.selected=state.hover=-1;}
            else if(t.Now()==Phase::GameOver)
            {
                state.lost=t.Of(Hanafuda::You).score<t.Of(Hanafuda::Her).score?state.lost+1:0;
                NewGame();
            }
            Redraw();
        }

        // In 2D, the cards' sounds as soon as they move (at the 3D table, as they land).
        void Listen()
        {
            if(state.renderer)return;
            auto& t=state.table;
            if(t.PileLeft()>state.heardPile&&state.heardPile>=0)PlayMahjongSound(MahjongSound::CardDeal);
            else if(t.Moves()!=state.heardMoves&&state.heardMoves>=0)
            {
                const auto& m=t.LastMove();
                PlayMahjongSound(m.fromPile?MahjongSound::CardFlip:MahjongSound::CardPlace);
                if(!m.took.empty())PlayMahjongSound(MahjongSound::CardTake);
            }
            state.heardMoves=t.Moves();
            state.heardPile=t.PileLeft();
        }

        // ---- Her words and the watchers' (as at mahjong) ----

        constexpr ULONGLONG WordsHold=3500;
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

        // A line of the scene, said now (no chance rolled here).
        void SayScene(const wchar_t* name,int weight)
        {
            GameLine line;
            if(state.lines.Pick(name,state.words.recent,state.sparkRandom,line))SayLine(line,weight);
        }

        // A line of the scene, said now and then (chance: how often, unless the user's lines
        // file says otherwise).
        void Say(const wchar_t* name,int weight=2,double chance=1)
        {
            if(state.lines.Roll(name,chance,state.sparkRandom))SayScene(name,weight);
        }

        // Something happened: she speaks of it, and those watching hear of it. facts: for her (she
        // is "you"); feel: how she is to feel about it; seen: the same for one watching; scene:
        // the scene of her lines (null: she says nothing); theirs: the watchers'.
        void Happened(int weight,const std::wstring& facts,const std::wstring& feel,const wchar_t* scene,
                      const std::wstring& seen,const std::vector<std::wstring>& theirs)
        {
            auto& t=state.table;
            const int you=t.Of(Hanafuda::You).score,her=t.Of(Hanafuda::Her).score;
            state.words.told+=Fmt(L"  {0} s, weight {1}: {2}\n",{std::to_wstring(static_cast<int>(state.clock)),std::to_wstring(weight),seen});
            if(state.her.onEvent&&!theirs.empty())
                state.her.onEvent(seen+Fmt(T(L"（{0}月目、{1}が{2}文、持ち主が{3}文）",L" (month {0}; {1} has {2} points, the owner {3})"),
                                           {std::to_wstring(t.Round()),HerName(),std::to_wstring(her),std::to_wstring(you)}),weight,Pick(theirs));
            if(!scene)return;
            auto& w=state.words;
            const ULONGLONG now=GetTickCount64();
            if((w.waiting||now-w.since<WordsHold)&&weight<w.weight)return;
            Say(scene,weight);
        }

        // The next scene not yet in her own words, asked of the LLM.
        void Prefetch()
        {
            if(state.prefetching||!state.her.prefetch)return;
            const std::wstring ask=state.lines.NextAsk();
            if(!ask.empty())state.prefetching=state.her.prefetch(ask);
        }

        std::wstring YakuText(const std::vector<Hanafuda::Yaku>& yaku)
        {
            std::wstring out;
            for(size_t i=0;i<yaku.size();++i)out+=(i?T(L"・",L", "):L"")+std::wstring(English()?yaku[i].en:yaku[i].ja);
            return out;
        }

        // What is new in the game since the last tick, for words: its start, the last month, a
        // koi-koi, a round's end, the game's end.
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
            if(Flying())return;   // the cards are still on their way
            const std::wstring name=HerName();
            const auto& you=t.Of(Hanafuda::You);
            const auto& her=t.Of(Hanafuda::Her);
            if(!seen.started)
            {
                seen.started=true;
                seen.round=t.Round();
                Happened(6,T(L"これから話し相手と花札（こいこい）の勝負が始まる。",L"A game of hanafuda (koi-koi) with them is about to begin."),
                         T(L"わくわくする気持ち",L"how you look forward to it"),
                         L"始まり",
                         Fmt(T(L"{0}と、このパソコンの持ち主の花札（こいこい）の勝負が始まる。",L"A game of hanafuda (koi-koi) between {0} and the owner of this computer is about to begin."),{name}),
                         {T(L"がんばれー！",L"Good luck, you two!"),T(L"どっちが勝つかな？",L"Who's going to win?")});
            }
            const bool over=t.Now()==Phase::RoundOver||t.Now()==Phase::GameOver;
            if(!over&&t.Round()!=seen.round)
            {
                seen.round=t.Round();
                seen.koi={};
                if(t.Round()==Hanafuda::Rounds)
                    Happened(5,T(L"最後の月（12月）が始まった。",L"The last month (December) has begun."),
                             T(L"最後の月に気を引き締める気持ち",L"how you mean to play the last month"),
                             L"最後の月",
                             Fmt(T(L"{0}と持ち主の花札は、最後の月に入った。",L"The hanafuda game between {0} and the owner is in its last month."),{name}),
                             {T(L"いよいよ最後の月だ。",L"Here comes the last month."),T(L"最後だ、どうなるかな。",L"Last round. How will it go?")});
            }
            if(you.koi>seen.koi[Hanafuda::You])
                Happened(5,T(L"花札で、話し相手が役をそろえたのに、上がらずに「こいこい」をした（もっと大きな役を狙って続ける。あなたが先に上がれば点は倍）。",
                             L"At hanafuda, they made a yaku but did not stop: they called koi-koi, playing on for more (if you stop first, your points are doubled)."),
                         T(L"驚くか、受けて立つ気持ち",L"your surprise, or that you take up the challenge"),
                         L"こいこいされた",
                         Fmt(T(L"花札で、持ち主が{0}を相手に「こいこい」をした。",L"In the hanafuda game, the owner called koi-koi against {0}."),{name}),
                         {T(L"おっ、こいこいだ！",L"Ooh, koi-koi!"),T(L"強気だね！",L"Bold move!")});
            if(her.koi>seen.koi[Hanafuda::Her])
                Happened(4,L"",L"",nullptr,
                         Fmt(T(L"花札で、{0}が「こいこい」をした。",L"In the hanafuda game, {0} called koi-koi."),{name}),
                         {Fmt(T(L"{0}のこいこいだ！",L"{0} called koi-koi!"),{name}),T(L"攻めるねえ。",L"Going for it!")});
            seen.koi={you.koi,her.koi};
            if(over&&!seen.over)
            {
                const auto& e=t.Ended();
                const std::wstring yaku=YakuText(e.yaku),points=std::to_wstring(e.points);
                if(e.winner<0)
                    Happened(3,T(L"花札のこの月は、あなたも話し相手も役で上がれずに流れた（どちらも0文）。",L"At hanafuda, neither you nor they stopped with a yaku this month: it is a draw (no points)."),
                             T(L"残念か、ほっとした気持ち",L"whether you are sorry or relieved"),
                             L"月が流れた",
                             Fmt(T(L"{0}と持ち主の花札は、この月は流れた。",L"In the hanafuda game between {0} and the owner, the month was a draw."),{name}),
                             {T(L"流れたね。",L"A draw."),T(L"惜しかったね、どっちも。",L"Close, both of you.")});
                else if(e.winner==Hanafuda::Her)
                    Happened(8,Fmt(T(L"花札のこの月は、あなたが役（{0}、{1}文）をそろえて勝った。話し相手は負けた。",L"At hanafuda, you won this month with your yaku ({0}; {1} points). They lost it."),{yaku,points}),
                             T(L"勝ってうれしい気持ち",L"how glad you are to have won"),
                             L"月に勝った",
                             Fmt(T(L"花札で、{0}が上がった（{1}、{2}文）。",L"In the hanafuda game, {0} won the month ({1}; {2} points)."),{name,yaku,points}),
                             {Fmt(T(L"{0}、すごい！",L"Nice one, {0}!"),{name}),Fmt(T(L"{0}文だ！",L"{0} points!"),{points})});
                else
                    Happened(8,Fmt(T(L"花札のこの月は、話し相手が役（{0}、{1}文）をそろえて勝った。あなたは負けた。",L"At hanafuda, they won this month with their yaku ({0}; {1} points). You lost it."),{yaku,points}),
                             T(L"負けて悔しい気持ちか、話し相手の役をほめる気持ち",L"how sorry you are to have lost, or praise their yaku"),
                             L"月に負けた",
                             Fmt(T(L"花札で、持ち主が上がった（{0}、{1}文）。",L"In the hanafuda game, the owner won the month ({0}; {1} points)."),{yaku,points}),
                             {T(L"おお、上がった！",L"Oh, a win!"),Fmt(T(L"{0}文かあ、大きいね。",L"{0} points, that's big."),{points})});
                if(t.Round()==Hanafuda::Rounds)
                {
                    const bool won=her.score>you.score,tie=her.score==you.score;
                    state.afterword=Fmt(T(L"いま、話し相手と花札（こいこい）を12か月まで打って、デスクトップに戻ってきたところ。結果は{0}（あなた{1}文、相手{2}文）。その勝負の感想を、話し相手にひと言（1〜2文）",
                                          L"You have just played hanafuda (koi-koi) through all twelve months with them and come back to the desktop. {0}: you have {1} points, they have {2}. Tell them what you thought of the game (one or two sentences)"),
                                        {won?T(L"あなたの勝ち",L"You won"):tie?T(L"引き分け",L"A tie"):T(L"あなたの負け",L"You lost"),std::to_wstring(her.score),std::to_wstring(you.score)});
                    state.afterwordLine=won?T(L"花札、楽しかったね！またやろう。",L"That was fun! Let's play again sometime."):T(L"くやしいなあ…次は勝つからね！",L"So close... I'll win next time!");
                    const std::wstring score=Fmt(T(L"あなたは{0}文、話し相手は{1}文",L"you have {0} points, they have {1}"),{std::to_wstring(her.score),std::to_wstring(you.score)});
                    if(won)Happened(9,T(L"花札の12か月が終わって、あなたの勝ち（",L"All twelve months of hanafuda are played, and you won (")+score+T(L"）。",L")."),
                                    T(L"勝ってうれしい気持ち",L"how glad you are to have won"),
                                    L"勝負に勝った",
                                    Fmt(T(L"{0}と持ち主の花札の勝負が終わり、{0}が勝った。",L"The hanafuda game between {0} and the owner is over: {0} won."),{name}),
                                    {T(L"おつかれさま！いい勝負だったね。",L"Well played, both of you!"),Fmt(T(L"{0}の勝ちだ！",L"{0} wins!"),{name})});
                    else Happened(9,(tie?T(L"花札の12か月が終わって、引き分け（",L"All twelve months of hanafuda are played: a tie ("):T(L"花札の12か月が終わって、あなたの負け（",L"All twelve months of hanafuda are played, and you lost ("))+score+T(L"）。",L")."),
                                  tie?T(L"引き分けの気持ち",L"how you feel about a tie"):T(L"負けて悔しい気持ち",L"how sorry you are to have lost"),
                                  tie?L"勝負が引き分け":L"勝負に負けた",
                                  Fmt(T(L"{0}と持ち主の花札の勝負が終わった。",L"The hanafuda game between {0} and the owner is over."),{name}),
                                  {T(L"おつかれさま！いい勝負だったね。",L"Well played, both of you!"),T(L"おめでとう！",L"Congratulations!")});
                }
            }
            seen.over=over;
        }

        void Tick()
        {
            auto& t=state.table;
            const ULONGLONG now=GetTickCount64();
            if(Flying())state.nextAt=std::max(state.nextAt,now+120);   // play waits for the cards
            if(t.HerCalls()!=state.callsSeen)
            {
                state.callsSeen=t.HerCalls();
                state.said=std::wstring(English()?t.HerCallEn():t.HerCallJa())+T(L"！",L"!");
                state.saidAt=now;
                state.nextAt=std::max(state.nextAt,now+SaidShown);
                Redraw();
            }
            if(!state.said.empty()&&now-state.saidAt>SaidShown){state.said.clear();Redraw();}
            // Her turn coming, at the 3D table once she is seated: time for her hand to go out
            // to her card, and now and then to stop and think first.
            if(t.Now()==Phase::HerTurn&&!t.HerPlayed()&&state.plannedFor!=t.Moves()&&state.renderer&&state.entrance==5&&!Flying())
            {
                state.plannedFor=t.Moves();
                state.nextAt=std::max(state.nextAt,now+HerPlayWait3D);
                if(state.lines.Roll(L"考える",0.15,state.sparkRandom))
                {
                    state.nextAt+=ThinkExtra;
                    state.thinkFor=(HerPlayWait3D+ThinkExtra-ReachLead)/1000.0-0.1;
                    if(!state.words.waiting&&GetTickCount64()-state.words.since>WordsHold)SayScene(L"考える",2);
                }
            }
            if(now>=state.nextAt)
            {
                if(t.Now()==Phase::HerTurn)
                {
                    const bool played=t.HerPlayed();
                    const int took=static_cast<int>(t.Of(Hanafuda::Her).taken.size());
                    t.Step();
                    // A bright or an animal taken now and then gets a little word.
                    const auto& taken=t.Of(Hanafuda::Her).taken;
                    if(static_cast<int>(taken.size())>took)
                    {
                        const bool good=std::any_of(taken.begin()+took,taken.end(),[](int c){return Hanafuda::KindOf(c)==Hanafuda::Kind::Bright;});
                        if(good&&!state.words.waiting&&GetTickCount64()-state.words.since>WordsHold)Say(L"取る",2,0.35);
                    }
                    if(t.Now()==Phase::HerTurn)state.nextAt=now+(played?HerPlayWait:HerPileWait);
                    else if(t.Now()==Phase::RoundOver)state.nextAt=now+(state.renderer&&t.Ended().winner>=0?EndWait3D:EndWait);
                    else state.nextAt=now+200;
                }
                else if(t.Now()==Phase::YourDraw)
                {
                    t.Draw();
                    Moved(t.Now()==Phase::HerTurn?HerPlayWait:200);
                }
                else if(t.Now()==Phase::RoundOver||t.Now()==Phase::GameOver)Redraw();
            }
            Redraw();
            Listen();
            if(!state.renderer)state.voice.Tick(0.04);   // (at the 3D table the model's frame ticks it)
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

        // A list of yaku with their points in two columns from y; returns the bottom.
        float YakuList(Graphics& g,const std::vector<Hanafuda::Yaku>& yaku,float left,float y,float width,float s,Color white,Color soft)
        {
            const float colW=width/2;
            for(size_t i=0;i<yaku.size();++i)
            {
                const auto& yk=yaku[i];
                const float cx=left+(i%2)*colW,cy=y+(i/2)*28*s;
                std::wstring name=English()?yk.en:yk.ja;
                const bool counted=std::wstring(yk.ja)==L"タネ"||std::wstring(yk.ja)==L"タン"||std::wstring(yk.ja)==L"カス";
                if(counted)name+=Fmt(T(L"（{0}枚）",L" ({0})"),{std::to_wstring(yk.cards)});
                Text(g,name,RectF(cx,cy,colW,28*s),20*s,white,false,StringAlignmentNear,StringAlignmentNear,true);
                Text(g,Mon(yk.points),RectF(cx,cy,colW-24*s,28*s),20*s,soft,false,StringAlignmentFar,StringAlignmentNear,true);
            }
            return y+((yaku.size()+1)/2)*28*s;
        }

        void PaintEnd(Graphics& g,float cw,float ch,float s)
        {
            auto& t=state.table;
            SolidBrush shade(Color(150,0,0,0));
            g.FillRectangle(&shade,0.0f,0.0f,cw,ch);
            const float w=std::min(cw-40*s,820*s),h=std::min(ch-40*s,500*s);
            const RectF box((cw-w)/2,(ch-h)/2,w,h);
            FillRound(g,box,14*s,Color(255,40,22,22),Color(255,200,170,90),2*s);
            const float left=box.X+28*s;
            float y=box.Y+20*s;
            const Color gold(255,240,206,120),white(255,240,240,236),soft(255,200,190,186);
            if(t.Now()==Phase::GameOver)
            {
                const int you=t.Of(Hanafuda::You).score,her=t.Of(Hanafuda::Her).score;
                Text(g,T(L"勝負あり",L"Game over"),RectF(box.X,y,w,44*s),34*s,gold,true,StringAlignmentCenter,StringAlignmentNear,true);
                y+=70*s;
                Runs(g,{{T(L"あなた  ",L"You  ")},{Mon(you)}},RectF(box.X,y,w,36*s),26*s,white);
                y+=44*s;
                Runs(g,{{HerName()},{L"  "+Mon(her)}},RectF(box.X,y,w,36*s),26*s,white);
                y+=70*s;
                if(you<her)Runs(g,{{HerName()},{T(L"の勝ち",L" wins")}},RectF(box.X,y,w,48*s),34*s,gold);
                else Runs(g,{{you>her?T(L"あなたの勝ち！",L"You win!"):T(L"引き分け",L"A tie")}},RectF(box.X,y,w,48*s),34*s,gold);
                Text(g,T(L"Enter：もう一度　Esc：閉じる",L"Enter: play again   Esc: close"),RectF(box.X,box.GetBottom()-44*s,w,30*s),17*s,soft,false,StringAlignmentCenter);
                return;
            }
            const auto& e=t.Ended();
            Text(g,MonthLabel(t.Round()),RectF(left,y+6*s,w,26*s),18*s,soft,false,StringAlignmentNear,StringAlignmentNear,true);
            if(e.winner<0)
            {
                y+=30*s;
                Text(g,T(L"流れ",L"No one won"),RectF(box.X,y,w,44*s),34*s,gold,true,StringAlignmentCenter,StringAlignmentNear,true);
                y+=60*s;
                Text(g,T(L"どちらも役で上がらないまま、手札がなくなりました。",L"The hands ran out with no one stopping on a yaku."),RectF(box.X,y,w,30*s),19*s,white,false,StringAlignmentCenter);
            }
            else
            {
                const std::wstring who=e.winner==Hanafuda::You?std::wstring(T(L"あなた",L"You")):HerName();
                y+=26*s;
                Runs(g,{{who},{T(L"の上がり",L" won")}},RectF(box.X,y,w,44*s),34*s,gold);
                y+=56*s;
                if(e.dealt)
                {
                    // Won at the deal: the hand, shown.
                    const float cw2=SmallW*1.2f*s,chh=SmallH*1.2f*s;
                    float x=box.X+(w-e.hand.size()*(cw2+4*s))/2;
                    for(int c:e.hand){DrawCard(g,x,y,cw2,chh,c);x+=cw2+4*s;}
                    y+=chh+14*s;
                }
                else
                {
                    // The cards of the winner's pile taken that make the yaku.
                    const auto& taken=t.Of(e.winner).taken;
                    std::vector<int> shown(taken.begin(),taken.end());
                    std::sort(shown.begin(),shown.end(),[](int a,int b){return std::pair(static_cast<int>(Hanafuda::KindOf(a)),a)<std::pair(static_cast<int>(Hanafuda::KindOf(b)),b);});
                    const float cw2=SmallW*1.1f*s,chh=cw2*SmallH/SmallW;
                    const float step=std::min(cw2+2*s,(w-56*s-cw2)/(shown.size()>1?shown.size()-1:1));
                    float x=left;
                    for(int c:shown){DrawCard(g,x,y,cw2,chh,c);x+=step;}
                    y+=chh+14*s;
                }
                y=YakuList(g,e.yaku,left,y,w-56*s,s,white,soft)+8*s;
                std::wstring how=Mon(e.base);
                if(e.doubledSeven)how+=T(L" ×2（7文以上）",L" x2 (7 or more)");
                if(e.doubledKoi)how+=T(L" ×2（こいこい返し）",L" x2 (after their koi-koi)");
                if(e.doubledSeven||e.doubledKoi)how+=L" = "+Mon(e.points);
                Text(g,how,RectF(box.X,y,w,40*s),28*s,gold,true,StringAlignmentCenter,StringAlignmentNear,true);
            }
            Runs(g,{{std::wstring(T(L"あなた ",L"You "))+Mon(t.Of(Hanafuda::You).score)+L"　　"},{HerName()},{L" "+Mon(t.Of(Hanafuda::Her).score)}},
                 RectF(box.X,box.GetBottom()-86*s,w,32*s),21*s,white);
            Text(g,T(L"Enter：次へ",L"Enter: next"),RectF(box.X,box.GetBottom()-44*s,w,30*s),17*s,soft,false,StringAlignmentCenter);
        }

        // The cards taken by who, four groups side by side from (x, y), each overlapping.
        void DrawTaken(Graphics& g,int who,float x,float y,float s)
        {
            const auto& taken=state.table.Of(who).taken;
            for(int k=0;k<4;++k)
            {
                std::vector<int> row;
                for(int c:taken)if(static_cast<int>(Hanafuda::KindOf(c))==k)row.push_back(c);
                for(size_t i=0;i<row.size();++i)DrawCard(g,x+i*12*s,y,SmallW*s,SmallH*s,row[i]);
                x+=(row.empty()?0:SmallW+12*(row.size()-1))*s+(row.empty()?0:14*s);
            }
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
            const auto& you=t.Of(Hanafuda::You);
            const auto& her=t.Of(Hanafuda::Her);
            const Color white(255,240,240,236),soft(255,206,196,186),gold(255,240,206,120),panel(185,40,18,16),edge(220,170,120,100);
            UpdateButtons();

            // Her: name, points, the dealer, koi-koi and her yaku (at the top left over the 3D
            // table; with her face, her hand's backs and her cards taken when there is none).
            const float nameX=hud?28*s:112*s,nameY=hud?20*s:14*s;
            const auto herYaku=Hanafuda::YakuOf(her.taken);
            if(hud)FillRound(g,RectF(14*s,12*s,300*s,herYaku.empty()?92*s:118*s),10*s,panel,edge,1.2f*s);
            else
            {
                const RectF faceBox(16*s,14*s,84*s,84*s);
                FillRound(g,faceBox,10*s,Color(255,14,50,36),Color(255,120,160,130),1.5f*s);
                if(state.face)g.DrawImage(state.face.get(),faceBox.X+3*s,faceBox.Y+3*s,faceBox.Width-6*s,faceBox.Height-6*s);
                else Text(g,HerName().substr(0,1),faceBox,40*s,white,true,StringAlignmentCenter,StringAlignmentCenter);
                for(size_t i=0;i<her.hand.size();++i)DrawCard(g,W-310*s-(her.hand.size()-i)*(SmallW+4)*s,18*s,SmallW*s,SmallH*s,-1,false);
                DrawTaken(g,Hanafuda::Her,20*s,112*s,s);
            }
            Text(g,HerName(),RectF(nameX,nameY,280*s,28*s),20*s,white,true);
            Text(g,Mon(her.score)+(t.Dealer()==Hanafuda::Her?T(L"　親",L"  dealer"):L"")+(her.koi?Fmt(T(L"　こいこい×{0}",L"  koi-koi x{0}"),{std::to_wstring(her.koi)}):L""),
                 RectF(nameX,nameY+30*s,280*s,24*s),17*s,soft);
            if(!herYaku.empty())Text(g,YakuText(herYaku)+L" "+Mon(Hanafuda::PointsOf(herYaku)),RectF(nameX,nameY+56*s,280*s,24*s),16*s,gold);
            // Her words: a bubble just right of her head (as at mahjong).
            if(!state.words.text.empty())
            {
                const std::wstring& said=state.words.text;
                const float most=std::max(160*s,W*0.4f);
                const WordsBubble bubble(g,said,s,most);
                const float bw=bubble.Width(),bh=bubble.Height();
                const bool beside=hud&&state.besideHead.x>=0;
                const float x=beside?std::clamp(static_cast<float>(state.besideHead.x),14*s,W-14*s-bw):W-14*s-bw;
                const float y=beside?std::clamp(static_cast<float>(state.headScreen.y)-bh*0.6f,96*s,std::max(96*s,H*0.6f-bh)):hud?96*s:96*s;
                const float ty=beside?std::clamp(static_cast<float>(state.headScreen.y),y+12*s,y+bh-12*s):y+std::min(bh*0.5f,30*s);
                const std::wstring shown=said.substr(0,std::min(said.size(),std::max<size_t>(1,static_cast<size_t>(state.words.shown))));
                bubble.Draw(g,x,y,ty,shown);
            }
            if(!state.said.empty())
            {
                const float tw=TextWidth(g,state.said,30*s,true)+30*s;
                const RectF bubble(hud?328*s:262*s,hud?30*s:44*s,tw,52*s);
                FillRound(g,bubble,14*s,Color(255,255,252,240),Color(255,230,120,60),2*s);
                Text(g,state.said,bubble,30*s,Color(255,200,40,40),true,StringAlignmentCenter,StringAlignmentCenter);
            }

            // The month and the pile (at the top right).
            {
                const RectF info(W-294*s,12*s,280*s,64*s);
                FillRound(g,info,10*s,Color(hud?215:200,40,18,16),edge,1.2f*s);
                Text(g,MonthLabel(t.Round())+Fmt(L"  {0}/{1}",{std::to_wstring(t.Round()),std::to_wstring(Hanafuda::Rounds)}),RectF(info.X+12*s,info.Y+6*s,260*s,26*s),18*s,white,true);
                Text(g,Fmt(T(L"山札 {0}枚",L"Pile {0}"),{std::to_wstring(t.PileLeft())}),RectF(info.X+12*s,info.Y+34*s,200*s,24*s),15*s,soft);
            }

            // The field and the pile (on the 3D table when there is one).
            const auto& field=t.Field();
            const auto glowing=Glowing();
            if(!hud)
            {
                state.fieldBoxes.assign(field.size(),RectF());
                const float fy=268*s;
                for(size_t i=0;i<field.size();++i)
                {
                    const float x=mid+(FieldColumn(static_cast<int>(i))-0.5f)*(FieldW+12)*s-FieldW*s/2,y=fy+(i%2==0?0:(FieldH+12)*s);
                    if(field[i]<0)continue;
                    const bool glow=std::find(glowing.begin(),glowing.end(),static_cast<int>(i))!=glowing.end();
                    DrawCard(g,x,y-(glow&&PickNow()&&static_cast<int>(i)==state.hover?8*s:0),FieldW*s,FieldH*s,field[i],true,false,glow);
                    state.fieldBoxes[i]=RectF(x,y,FieldW*s,FieldH*s);
                }
                const float px=mid-5.6f*(FieldW+12)*s,py=fy+(FieldH*0.55f)*s;
                for(int k=std::min(4,t.PileLeft());k-->0;)DrawCard(g,px+k*2*s,py-k*2*s,FieldW*s,FieldH*s,-1,false);
                Text(g,std::to_wstring(t.PileLeft()),RectF(px,py+FieldH*s+4*s,FieldW*s,22*s),15*s,soft,false,StringAlignmentCenter);
                if(t.Pending()>=0)DrawCard(g,mid+5.0f*(FieldW+12)*s,py-20*s,FieldW*1.2f*s,FieldH*1.2f*s,t.Pending());
                DrawTaken(g,Hanafuda::You,20*s,BaseHeight*s-118*s-SmallH*s-150*s,s);
            }

            // You: points, the dealer, koi-koi and your yaku; your hand (in 2D).
            const float handY=(BaseHeight-136)*s;
            const auto yourYaku=Hanafuda::YakuOf(you.taken);
            {
                const float boxY=hud?H-112*s:handY-20*s,boxW=hud?150*s:112*s;
                if(hud)FillRound(g,RectF(8*s,boxY,boxW,100*s),10*s,panel,edge,1.2f*s);
                Text(g,T(L"あなた",L"You"),RectF(16*s,boxY+8*s,boxW,26*s),18*s,white,true);
                Text(g,Mon(you.score)+(t.Dealer()==Hanafuda::You?T(L"　親",L"  dealer"):L""),RectF(16*s,boxY+34*s,boxW,24*s),16*s,soft);
                if(you.koi)Text(g,Fmt(T(L"こいこい×{0}",L"koi-koi x{0}"),{std::to_wstring(you.koi)}),RectF(16*s,boxY+60*s,boxW,24*s),15*s,gold);
                if(!yourYaku.empty())
                {
                    const std::wstring line=YakuText(yourYaku)+L" "+Mon(Hanafuda::PointsOf(yourYaku));
                    const RectF at=hud?RectF(8*s,boxY-40*s,TextWidth(g,line,15*s)+24*s,32*s):RectF(20*s,478*s,700*s,24*s);
                    if(hud)FillRound(g,at,8*s,panel,edge,1.2f*s);
                    Text(g,line,hud?RectF(at.X+12*s,at.Y,at.Width,at.Height):at,15*s,gold,false,StringAlignmentNear,StringAlignmentCenter);
                }
            }
            if(!hud)
            {
                state.handBoxes.assign(you.hand.size(),RectF());
                for(size_t i=0;i<you.hand.size();++i)
                {
                    const float x=140*s+i*(HandW+6)*s;
                    const bool raised=YourPlayNow()&&(static_cast<int>(i)==state.hover||static_cast<int>(i)==state.selected);
                    DrawCard(g,x,handY-(raised?12*s:0),HandW*s,HandH*s,you.hand[i],true,false,CanTake(you.hand[i]));
                    state.handBoxes[i]=RectF(x,handY-12*s,HandW*s,HandH*s+12*s);
                }
            }

            // What to do now: a line above your hand, and the koi-koi panel.
            {
                std::wstring prompt;
                if(PickNow())prompt=T(L"どちらの札を取りますか（クリック、←→とEnter）",L"Which card do you take? (click, or ←→ and Enter)");
                else if(YourPlayNow()&&you.taken.empty()&&t.Round()==1&&t.Moves()<4)prompt=T(L"手札から1枚出してください（金色に縁取られた札は、場の同じ月の札を取れます）",L"Play a card from your hand (one rimmed in gold takes a card of the same month)");
                if(!prompt.empty())
                {
                    const float tw=TextWidth(g,prompt,17*s)+28*s,y=hud?H-268*s:handY-64*s;
                    FillRound(g,RectF(mid-tw/2,y,tw,34*s),10*s,panel,edge,1.2f*s);
                    Text(g,prompt,RectF(mid-tw/2,y,tw,34*s),17*s,gold,false,StringAlignmentCenter,StringAlignmentCenter);
                }
            }
            if(!state.buttons.empty())
            {
                const float pw=std::min(W-40*s,560*s),ph=196*s;
                const RectF box(mid-pw/2,hud?H*0.40f:250*s,pw,ph);
                FillRound(g,box,14*s,Color(235,40,22,22),Color(255,200,170,90),2*s);
                Text(g,T(L"役ができました",L"You made a yaku"),RectF(box.X,box.Y+12*s,pw,34*s),24*s,gold,true,StringAlignmentCenter,StringAlignmentNear,true);
                Text(g,YakuText(yourYaku)+L"  "+Mon(Hanafuda::PointsOf(yourYaku)),RectF(box.X,box.Y+52*s,pw,30*s),19*s,white,false,StringAlignmentCenter);
                const float gap=20*s,width=(pw-3*gap)/2;
                for(size_t i=0;i<state.buttons.size();++i)
                {
                    Button& b=state.buttons[i];
                    b.box=RectF(box.X+gap+i*(width+gap),box.Y+96*s,width,52*s);
                    const bool stop=b.act==Act::Stop;
                    FillRound(g,b.box,10*s,stop?Color(255,206,60,50):Color(255,236,226,196),Color(255,90,60,30),1.5f*s);
                    Text(g,std::to_wstring(i+1)+L"  "+b.label,b.box,22*s,stop?white:Color(255,40,30,20),true,StringAlignmentCenter,StringAlignmentCenter);
                }
                Text(g,her.koi?T(L"相手はこいこい中：いま勝負すると倍",L"She has called koi-koi: stopping now doubles it"):
                               T(L"こいこい：続けてもっと役を狙う（相手が上がると倍取られる）",L"Koi-koi: play on for more (if she then wins, she gets double)"),
                     RectF(box.X,box.Y+158*s,pw,26*s),14*s,soft,false,StringAlignmentCenter);
            }

            if((t.Now()==Phase::RoundOver&&!Waiting()&&!Flying())||t.Now()==Phase::GameOver)PaintEnd(g,W,H,s);
            if(state.confirmQuit)
            {
                SolidBrush shade(Color(160,0,0,0));
                g.FillRectangle(&shade,0.0f,0.0f,W,H);
                const RectF box(mid-220*s,ch/2.0f-70*s,440*s,140*s);
                FillRound(g,box,14*s,Color(250,40,22,22),Color(255,200,170,90),2*s);
                Text(g,T(L"花札をやめますか？",L"Stop playing?"),RectF(box.X,box.Y+22*s,box.Width,36*s),24*s,white,true,StringAlignmentCenter);
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

        int HandAt(float x,float y)
        {
            for(size_t i=state.handBoxes.size();i-->0;)   // the right one on top where they overlap
                if(state.handBoxes[i].Width>0&&state.handBoxes[i].Contains(x,y))return static_cast<int>(i);
            return -1;
        }

        int FieldAt(float x,float y)
        {
            for(size_t i=0;i<state.fieldBoxes.size();++i)
                if(state.fieldBoxes[i].Width>0&&state.fieldBoxes[i].Contains(x,y))return static_cast<int>(i);
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
                if(key==VK_RETURN||key==VK_SPACE)Continue();
                else if(key==VK_ESCAPE)Finish();
                return;
            }
            if(key==VK_ESCAPE){state.confirmQuit=true;Redraw();return;}
            if(t.Now()==Phase::RoundOver){if((key==VK_RETURN||key==VK_SPACE)&&!Flying())Continue();return;}
            if(key>='1'&&key<='9')
            {
                const size_t i=key-'1';
                if(i<state.buttons.size())Do(state.buttons[i]);
                return;
            }
            if(PickNow())
            {
                const auto m=t.Matches(t.Pending());
                if(key==VK_LEFT||key==VK_RIGHT)
                {
                    const auto at=std::find(m.begin(),m.end(),state.hover);
                    const size_t i=at==m.end()?0:(static_cast<size_t>(at-m.begin())+1)%m.size();
                    state.hover=m[i];
                    Redraw();
                }
                else if(key==VK_RETURN)PickAt(state.hover>=0?state.hover:m[0]);
                return;
            }
            if(!YourPlayNow())return;
            const int n=static_cast<int>(t.Of(Hanafuda::You).hand.size());
            if(n==0)return;
            if(key==VK_LEFT||key==VK_RIGHT)
            {
                state.selected=state.selected<0?(key==VK_LEFT?n-1:0):(state.selected+(key==VK_LEFT?n-1:1))%n;
                state.hover=-1;
                Redraw();
            }
            else if(key==VK_RETURN&&state.selected>=0)PlayAt(state.selected);
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
                int at=-1;
                if(PickNow())
                {
                    const auto m=state.table.Matches(state.table.Pending());
                    const int f=FieldAt(x,y);
                    if(std::find(m.begin(),m.end(),f)!=m.end())at=f;
                }
                else at=HandAt(x,y);
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
                if(t.Now()==Phase::GameOver||(t.Now()==Phase::RoundOver&&!Waiting()&&!Flying())){Continue();return 0;}
                for(const Button& b:state.buttons)if(b.box.Contains(x,y)){Do(b);return 0;}
                if(PickNow()){PickAt(FieldAt(x,y));return 0;}
                const int at=HandAt(x,y);
                if(at>=0)PlayAt(at);
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

    bool HanafudaWindowOpen(){return state.window!=nullptr;}
    HWND HanafudaWindowHandle(){return state.window;}
    bool HanafudaWindowRect(RECT& r)
    {
        return state.window&&IsWindowVisible(state.window)&&!IsIconic(state.window)&&GetWindowRect(state.window,&r);
    }

    namespace
    {
        // Diagnostics: your steps played as she would play them, for moves steps (hers between).
        void PlaySimply(int moves)
        {
            auto& t=state.table;
            for(int i=0;i<moves&&t.Now()!=Phase::RoundOver&&t.Now()!=Phase::GameOver;++i)
            {
                if(t.Now()==Phase::HerTurn)t.Step();
                else t.PlayForYou();
            }
            state.callsSeen=t.HerCalls();
            state.nextAt=0;
            state.selected=state.hover=-1;
        }
    }

    void HanafudaLines(const std::wstring& answer)
    {
        state.prefetching=false;
        if(!state.window)return;
        state.lines.Answered(answer);
        Prefetch();
    }

    bool HanafudaAfterword(std::wstring& situation,std::wstring& fallback)
    {
        if(state.afterword.empty())return false;
        situation=std::move(state.afterword);
        fallback=std::move(state.afterwordLine);
        state.afterword.clear();
        state.afterwordLine.clear();
        return true;
    }

    void HanafudaHeard(const std::wstring& line,bool done)
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

    void PlayHanafudaMoves(int moves,float yaw)
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

        // The cards on their way since the table was last seen: a deal (from the pile to the
        // hands and the field, one after another), or a move (the card to where it goes; when it
        // took cards, held on them a moment, then all into the pile taken), and any other card
        // that moved sliding to its new place.
        void StartFlights(const std::array<Pose,Cards>& before,bool deal,bool moved)
        {
            const double now=state.clock;
            auto& t=state.table;
            const auto& after=state.poses;
            std::array<bool,Cards> done{};
            auto add=[&](int card,std::vector<std::pair<double,Pose>> keys,bool arc=true){
                done[static_cast<size_t>(card)]=true;
                state.flightsUntil=std::max(state.flightsUntil,keys.back().first);
                state.flights.erase(std::remove_if(state.flights.begin(),state.flights.end(),[&](const Flight& f){return f.card==card;}),state.flights.end());
                state.flights.push_back({card,std::move(keys),arc});
            };
            if(deal)
            {
                // Dealt as at the table: two to the other, two to the field, two to the dealer, four times.
                std::vector<int> order;
                const auto& field=t.Field();
                const auto& dealer=t.Of(t.Dealer()).hand;
                const auto& other=t.Of(1-t.Dealer()).hand;
                for(size_t k=0;k<8;k+=2)
                    for(const auto* from:{&other,&field,&dealer})
                        for(size_t j=k;j<k+2&&j<from->size();++j)if((*from)[j]>=0)order.push_back((*from)[j]);
                Pose pile=Lying(PileX(),PileZ(),0,Thick()*(t.PileLeft()+order.size()));
                for(size_t i=0;i<order.size();++i)
                {
                    const int c=order[i];
                    Pose from=pile;
                    from.flip=1;
                    const double start=now+0.05+i*DealEach;
                    add(c,{{start,from},{start+0.32,after[static_cast<size_t>(c)]}});
                }
                Due(now+0.05,MahjongSound::CardDeal);
                return;
            }
            const auto& m=t.LastMove();
            if(moved&&m.card>=0)
            {
                const int c=m.card;
                const Pose from=before[static_cast<size_t>(c)];
                if(m.fromPile)Due(now,MahjongSound::CardFlip);
                if(m.took.empty())
                {
                    add(c,{{now,from},{now+Travel,after[static_cast<size_t>(c)]}});
                    if(!m.fromPile)Due(now+Travel,MahjongSound::CardPlace);
                }
                else
                {
                    Pose onto=before[static_cast<size_t>(m.took[0])];
                    onto.x+=0.022f*state.upper;
                    onto.z-=0.02f*state.upper;
                    onto.y+=Thick()*2;
                    onto.yaw=0;onto.pitch=0;onto.flip=0;
                    onto.w=LyingW()*FieldScale;onto.l=LyingL()*FieldScale;
                    const double held=now+Travel+Hold,end=held+Gather;
                    add(c,{{now,from},{now+Travel,onto},{held,onto},{end,after[static_cast<size_t>(c)]}});
                    for(int k:m.took)add(k,{{now,before[static_cast<size_t>(k)]},{held,before[static_cast<size_t>(k)]},{end,after[static_cast<size_t>(k)]}});
                    Due(now+Travel,MahjongSound::CardPlace);
                    Due(end,MahjongSound::CardTake);
                    // Hers: gold sparkles as she takes a bright.
                    if(m.who==Hanafuda::Her&&std::any_of(m.took.begin(),m.took.end(),[](int k){return Hanafuda::KindOf(k)==Hanafuda::Kind::Bright;}))
                        state.sparkAt=end;
                }
            }
            // The rest that moved: a short slide (a hand closing up, a card held for a pick).
            for(int c=0;c<Cards;++c)
            {
                const Pose& a=before[static_cast<size_t>(c)];
                const Pose& b=after[static_cast<size_t>(c)];
                if(done[static_cast<size_t>(c)]||!b.shown)continue;
                const float moved=std::abs(a.x-b.x)+std::abs(a.y-b.y)+std::abs(a.z-b.z)+std::abs(a.pitch-b.pitch)+std::abs(a.flip-b.flip);
                if(moved<0.002f*state.upper)continue;
                const bool fromPile=!a.shown&&a.flip>0.5f;
                if(fromPile)Due(now,MahjongSound::CardFlip);
                add(c,{{now,a},{now+(fromPile?Travel:Slide),b}},fromPile);
            }
        }
    }

    void TickHanafudaWindow(MascotModel& model,double dt)
    {
        if(!state.window||!state.renderer)return;
        auto& r=*state.renderer;
        using namespace DirectX;
        const float D=state.upper,top=Top();
        dt=std::min(dt,0.1);
        auto& t=state.table;
        // What changed on the table since the last frame: its cards set on their way. A deal
        // waits for a few frames drawn first, so the cards go smoothly.
        {
            const bool deal=t.PileLeft()>state.pileSeen;
            if(deal&&!state.dealPending){state.dealPending=true;state.frames=0;}
            if(state.dealPending&&++state.frames<=3)state.movesSeen=t.Moves();
            else
            {
                const bool moved=t.Moves()!=state.movesSeen,held=t.Pending()!=state.pendingSeen;
                if(state.dealPending||moved||held)
                {
                    const std::array<Pose,Cards> before=state.poses;
                    PlaceCards();
                    StartFlights(before,state.dealPending,moved);
                }
                state.dealPending=false;
                state.movesSeen=t.Moves();
                state.pileSeen=t.PileLeft();
                state.pendingSeen=t.Pending();
            }
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
            const auto& m=t.LastMove();
            if(m.card>=0)Sparkle(At(state.poses[static_cast<size_t>(m.card)],0,0,0));
        }
        // Her hand goes out to the card she will play (tried again next frame while a gesture
        // of hers ends).
        if(t.Now()==Phase::HerTurn&&!t.HerPlayed()&&state.entrance==5&&!Flying()&&state.plannedFor==t.Moves()&&state.reachedFor!=t.Moves()&&
           GetTickCount64()+ReachLead>=state.nextAt)
        {
            const int k=t.HerNextPlay();
            if(k<0||k>=static_cast<int>(state.herHandSpots.size())||ReachFor(model,state.herHandSpots[static_cast<size_t>(k)]))state.reachedFor=t.Moves();
        }
        if(state.thinkFor>0&&state.entrance==5){model.EmoteSeated(L"ponder",state.thinkFor);state.thinkFor=0;}
        {
            const bool over=t.Now()==Phase::RoundOver||t.Now()==Phase::GameOver;
            if(over&&!state.seenOver&&t.Ended().winner>=0)
            {
                std::uniform_real_distribution<double> any(0,1);
                state.endGesture=t.Ended().winner==Hanafuda::Her?(any(state.sparkRandom)<0.5?L"cheeks":L"clap"):L"oops";
                state.endGestureUntil=state.clock+1.5;
            }
            state.seenOver=over;
            if(!state.endGesture.empty()&&state.entrance==5&&(model.EmoteSeated(state.endGesture,3.2)||state.clock>state.endGestureUntil))
                state.endGesture.clear();
        }
        // What moves: her chair, and the cards on their way.
        {
            Mesh moving;
            moving.out=BuildChair(state.chairZ);
            for(const auto& f:state.flights)
            {
                Pose p;
                if(PoseAt(f,state.clock,p)&&state.clock>=f.keys.front().first)AddCard(moving,p,f.card,false,false,Hidden(f.card));
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
        // Your cards and the field's as they are seen, for pointing and clicking.
        auto boxes=[&](const std::vector<std::array<std::array<float,3>,4>>& corners,std::vector<RectF>& out){
            out.assign(corners.size(),RectF());
            for(size_t i=0;i<corners.size();++i)
            {
                LONG l=LONG_MAX,tp=LONG_MAX,rr=LONG_MIN,b=LONG_MIN;
                for(const auto& k:corners[i])
                {
                    const POINT q=r.Project(k);
                    l=std::min(l,q.x);tp=std::min(tp,q.y);rr=std::max(rr,q.x);b=std::max(b,q.y);
                }
                out[i]=RectF(static_cast<REAL>(l),static_cast<REAL>(tp),static_cast<REAL>(rr-l),static_cast<REAL>(b-tp));
            }
        };
        boxes(state.handCorners,state.handBoxes);
        boxes(state.fieldCorners,state.fieldBoxes);
        // Field places with no card are not clicked.
        for(size_t i=0;i<state.fieldBoxes.size()&&i<t.Field().size();++i)if(t.Field()[i]<0)state.fieldBoxes[i]=RectF();
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

    std::wstring DescribeHanafuda()
    {
        if(!state.window)return L"";
        auto& t=state.table;
        std::wstring out=Fmt(L"hanafuda: entrance {0}, month {1}, phase {2}, pile {3}, field {4}, flights {5}, scores you {6} her {7}\n",
                             {std::to_wstring(state.entrance),std::to_wstring(t.Round()),std::to_wstring(static_cast<int>(t.Now())),std::to_wstring(t.PileLeft()),
                              std::to_wstring(t.Field().size()),std::to_wstring(state.flights.size()),std::to_wstring(t.Of(Hanafuda::You).score),std::to_wstring(t.Of(Hanafuda::Her).score)});
        return out+(state.words.told.empty()?std::wstring():L"hanafuda words:\n"+state.words.told);
    }

    bool SaveHanafudaView(const std::wstring& path)
    {
        return state.renderer&&state.renderer->SaveLastFrame(path);
    }

    bool SaveHanafudaShot(const std::wstring& path,int moves,std::uint32_t seed,const std::wstring& stop)
    {
        GdiplusStartupInput input;
        if(!state.gdiplus)GdiplusStartup(&state.gdiplus,&input,nullptr);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc=DefWindowProcW;
        wc.hInstance=GetModuleHandleW(nullptr);
        wc.lpszClassName=L"MikuMikuDesktopPerchHanafudaShot";
        RegisterClassExW(&wc);
        const int dpi=static_cast<int>(GetDpiForSystem());
        state.window=CreateWindowExW(0,wc.lpszClassName,L"",WS_OVERLAPPEDWINDOW,0,0,MulDiv(static_cast<int>(BaseWidth)+16,dpi,96),MulDiv(static_cast<int>(BaseHeight)+39,dpi,96),
                                     nullptr,nullptr,wc.hInstance,nullptr);
        state.her=HanafudaOpponent{L"ミク"};
        auto& t=state.table;
        t.NewGame(seed,Care);
        if(stop.empty())PlaySimply(moves);
        else
        {
            for(int i=0;i<moves&&t.Now()!=Phase::GameOver;++i)
            {
                if(stop==L"koi"&&t.Now()==Phase::YourKoi)break;
                if(stop==L"pick"&&t.Now()==Phase::YourPick)break;
                if(stop==L"end"&&t.Now()==Phase::RoundOver&&t.Ended().winner>=0&&!t.Ended().dealt)break;
                if(t.Now()==Phase::HerTurn)t.Step();
                else if(t.Now()==Phase::RoundOver)t.NextRound();
                else if(stop==L"pick"&&t.Now()==Phase::YourPlay)
                {
                    // A card that matches two on the field, when there is one.
                    const auto& hand=t.Of(Hanafuda::You).hand;
                    int two=-1;
                    for(size_t k=0;k<hand.size();++k)if(t.Matches(hand[k]).size()==2)two=static_cast<int>(k);
                    if(two>=0)t.Play(two);else t.PlayForYou();
                }
                else t.PlayForYou();
            }
            state.callsSeen=t.HerCalls();
            state.nextAt=0;
            state.selected=state.hover=-1;
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

    bool SaveHanafudaCards(const std::wstring& path)
    {
        GdiplusStartupInput input;
        if(!state.gdiplus)GdiplusStartup(&state.gdiplus,&input,nullptr);
        constexpr float S=1.5f;
        Bitmap sheet(static_cast<INT>(40+12*(HandW+6)*S),static_cast<INT>(40+4*(HandH+8)*S+4*(SmallH+8)*S),PixelFormat32bppARGB);
        Graphics g(&sheet);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Color(255,22,74,52));
        // Each month a column, its four cards down it; below, the same at the piles' size.
        for(int c=0;c<Cards;++c)
        {
            const int m=Hanafuda::MonthOf(c),k=c%4;
            DrawCard(g,20+m*(HandW+6)*S,20+k*(HandH+8)*S,HandW*S,HandH*S,c);
            DrawCard(g,20+m*(HandW+6)*S,20+4*(HandH+8)*S+k*(SmallH+8)*S,SmallW*S,SmallH*S,c,true,false,k==0);
        }
        return SavePng(sheet,path);
    }

    void ShowHanafudaWindow(HWND owner,const HanafudaOpponent& her,std::function<void()> onEnd)
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
        state.heardMoves=state.heardPile=-1;
        state.movesSeen=state.pileSeen=state.pendingSeen=-1;
        state.plannedFor=-1;
        state.dealPending=false;
        state.reachedFor=-1;
        state.poses={};
        state.lines.Load(L"HANAFUDALINES",L"hanafuda",her.ownLinesFile,her.linesFile,her.styleKey,English(),
                         {L"花札（こいこい）",L"hanafuda (koi-koi)",L"",L""});
        state.voice.Stop();
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
        const std::wstring title=std::wstring(T(L"花札（こいこい）",L"Hanafuda (koi-koi)"))+L" - "+HerName();
        state.window=CreateWindowExW(0,ClassName,title.c_str(),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                                     work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,
                                     nullptr,nullptr,instance,nullptr);
        if(CreateView())PlaceView();
        SetTimer(state.window,TimerId,40,nullptr);
        ShowWindow(state.window,SW_SHOWNORMAL);
        SetForegroundWindow(state.window);
    }
}
