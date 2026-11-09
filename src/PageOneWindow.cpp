#define NOMINMAX
#include "PageOneWindow.hpp"
#include "ShownKey.hpp"
#include "PageOne.hpp"
#include "MahjongSound.hpp"
#include "Log.hpp"
#include "MascotModel.hpp"
#include "MascotRenderer.hpp"
#include "Text.hpp"
#include "BubbleText.hpp"
#include "GameLines.hpp"
#include "WesternRoom.hpp"
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
        using PageOne::Phase;
        constexpr wchar_t ClassName[]=L"MikuMikuDesktopPerchPageOne";
        constexpr wchar_t ViewClassName[]=L"MikuMikuDesktopPerchPageOneView";
        constexpr UINT_PTR TimerId=1;
        // Her pace: before each step of hers (in 2D, and at the 3D table where her hand goes out
        // first), after a move of yours.
        constexpr ULONGLONG HerPlayWait=650,HerPlayWait3D=1000,YourWait=250;
        constexpr ULONGLONG SaidShown=2400;
        constexpr ULONGLONG HandWait=500,StreetWait=800,RunOutWait=1300;   // before cards are dealt
        constexpr ULONGLONG ReachLead=600;         // her hand starts out this long before her card
        // The cards on their way at the 3D table (seconds): out of the shoe to where they go, each
        // one dealt this long after the one before; turned over in place; gathered to the pile.
        constexpr double Travel=0.34,DealEach=0.20,Turn=0.30,Gather=0.40;
        // Sizes in logical pixels (at 96 dpi, at the size the window opens).
        constexpr float BaseWidth=1180,BaseHeight=840;
        constexpr float CardW2D=81,CardH2D=149;   // a card on the 2D table

        // Where a card is at the 3D table (world units): its middle, turned about the upright by
        // yaw, turned over about its length by flip (1: face down), w wide and l long.
        struct Pose {float x=0,y=0,z=0,yaw=0,flip=1,w=0,l=0;bool shown=false;};
        // A card on its way: where it is at each time (seconds of the clock); arc: lifted on the way.
        struct Flight {int card=-1;std::vector<std::pair<double,Pose>> keys;bool arc=true;};
        enum class Act {Draw,Keep,Call,Catch,Next,Suit};
        struct Button {Act act;int value;std::wstring label;RectF box;bool on=true;};

        // How carefully she plays (PageOne::Table::NewGame): a little less as you grow closer,
        // and after games you lost.
        constexpr double Care=0.85,LessPerBond=0.03,LessPerLoss=0.08,LeastCare=0.4;
        // How often the look on her face is put on, the other way about from what she holds.
        constexpr double PutOnShare=0.2;
        constexpr float HandStep=64,SuitGap=18;   // your cards along the bottom of the window, a gap between suits

        // The suits' marks and names (spade, heart, diamond, club), and their colours.
        const wchar_t* SuitMark(int s){static const wchar_t* marks[]={L"♠",L"♥",L"♦",L"♣"};return s>=0&&s<4?marks[s]:L"";}
        const wchar_t* SuitName(int s)
        {
            static const wchar_t* ja[]={L"スペード",L"ハート",L"ダイヤ",L"クラブ"};
            static const wchar_t* en[]={L"spades",L"hearts",L"diamonds",L"clubs"};
            return s>=0&&s<4?(English()?en[s]:ja[s]):L"";
        }
        Gdiplus::Color SuitInk(int s){return s==1||s==2?Gdiplus::Color(255,214,52,58):Gdiplus::Color(255,30,30,40);}

        struct State
        {
            HWND window=nullptr;
            ULONG_PTR gdiplus=0;
            PageOneOpponent her;
            std::unique_ptr<Bitmap> face;
            std::function<void()> onEnd;
            PageOne::Table table;
            int lost=0;                // games you lost in a row
            bool gameOverTold=false;
            bool confirmQuit=false;
            bool help=false;           // the rules shown
            RectF helpBox;
            RectF quitButtons[2];
            std::vector<Button> buttons;
            int hover=-1;              // the button under the mouse
            ULONGLONG nextAt=0;
            std::wstring said;         // the blinds going up, in the big bubble
            ULONGLONG saidAt=0;
            bool calling=false;        // "page one" said with your next card
            int choosing=-1;           // the wild of yours whose suit is being chosen
            std::vector<std::pair<int,RectF>> cardBoxes;   // your cards as drawn, left to right
            int hoverCard=-1;          // the card under the mouse
            RectF topBox,stockBox;     // the pile's top card and the stock as drawn
            bool nearTold=false;       // she has spoken of your last card
            int herLastSeen=-1;        // her move last spoken of (Moves)
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
            // Her lines by scene (res/pageone/lines.txt, said again her way by the LLM, and the
            // user's own), and the voice of a line that has one.
            GameLines lines;
            GameVoice voice;
            bool prefetching=false;
            POINT headScreen{-1,-1},besideHead{-1,-1};
            std::wstring afterword,afterwordLine;
            struct Seen
            {
                bool started=false,over=false;
                int hand=0;               // the hand whose end was spoken of
                int dealt=0;              // and whose deal
            } seen;
            std::unique_ptr<Bitmap> canvas;
            bool overlayDue=true;      // (3D) the 2D layer to be drawn again with the next frame
            std::uint64_t shown=0;     // (3D) what it showed when last drawn (Shown)
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
            // Where each card is (world units), as the table is now.
            std::array<Pose,PageOne::Cards> poses{};
            // The cards on their way, and until when (on the clock); sounds due on the way.
            std::vector<Flight> flights;
            double flightsUntil=0;
            std::vector<std::pair<double,MahjongSound>> sounds;
            int movesSeen=-1;
            double sparkAt=0;         // gold sparkles then, over a pageone of yours
            std::array<float,3> sparkSpot{};
            double clock=0;
            int plannedFor=-1;        // the move count her step's wait was last set at
            int reachedFor=-1;        // and her hand last went out at
            struct Spark {std::array<float,3> at,velocity;float age,life,size,angle,spin;std::array<float,4> color;bool glow;};
            std::vector<Spark> sparks;
            std::vector<MascotRenderer::Particle> roomGlows;
            std::mt19937 sparkRandom{12345};
            double thinkFor=0;
            std::wstring endGesture;
            double endGestureUntil=0;
            int gestureHand=0;        // the hand her reaction was last given for
            int heardMoves=-1;        // in 2D, the move last heard (its sound)
            Expression tell=Expression::Neutral;   // the look on her face
            int tellFor=-1;           // the hand, street and bet it was chosen for
            Expression tellFace=Expression::Neutral;
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
        std::wstring Chips(int n){return std::to_wstring(n)+T(L"枚",L" chips");}
        std::wstring Signed(int n){return (n>0?L"+":L"")+std::to_wstring(n);}

        bool Waiting(){return state.nextAt>GetTickCount64();}
        // The cards still on their way at the 3D table (a move not yet shown counts).
        bool Flying()
        {
            return state.renderer&&(state.movesSeen!=state.table.Moves()||state.clock<state.flightsUntil);
        }
        bool Ready(){return !Waiting()&&!Flying();}
        // Your turn (a card to play or the stock to draw from), or a card just drawn.
        bool YourTurnNow(){return state.table.Now()==Phase::YourTurn&&Ready();}
        bool YourDrawnNow(){return state.table.Now()==Phase::YourDrawn&&Ready();}
        bool YourPlayNow(){return YourTurnNow()||YourDrawnNow();}
        bool HandShown(){return state.table.Now()==Phase::HandOver&&Ready();}
        bool EndShown(){return state.table.Now()==Phase::GameOver&&Ready();}

        // Whose cards in order: by suit (spades, hearts, clubs, diamonds, so that the colours
        // alternate), each by rank, the ace low; the wilds at the end.
        std::vector<int> Sorted(int who)
        {
            std::vector<int> out(state.table.HandOf(who));
            auto key=[](int c){
                if(PageOne::IsJoker(c))return 1000;
                if(PageOne::RankOf(c)==8)return 900+PageOne::SuitOf(c);
                static const int order[4]={0,1,3,2};
                return order[PageOne::SuitOf(c)]*20+PageOne::RankOf(c);
            };
            std::sort(out.begin(),out.end(),[&](int a,int b){return key(a)<key(b);});
            return out;
        }

        // "Page one" may be said with the next card: you have two (one of them to be played).
        bool CanCall()
        {
            auto& t=state.table;
            return YourPlayNow()&&t.HandOf(PageOne::You).size()==2&&t.Penalty()==0;
        }

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
                Log(L"pageone: resize: %s",error.c_str());
            InvalidateRect(state.window,nullptr,FALSE);
        }

        void ReleaseView()
        {
            if(state.renderer){state.renderer->WaitIdle();state.renderer.reset();Log(L"pageone: 3D view released");}
            state.view=nullptr;
        }

        // The cards' faces (res/pageone/cards.jpg): CardSide by CardTop each, eight across,
        // card n at column n%8, row n/8 (the back at PageOne::Back).
        constexpr int CardSide=176,CardTop=324,AtlasColumns=8;

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
            if(!faceUp||card<0)card=PageOne::Back;
            if(Bitmap* face=Face(card,static_cast<int>(std::lround(w)),static_cast<int>(std::lround(h))))
                g.DrawImage(face,RectF(x,y,static_cast<REAL>(face->GetWidth()),static_cast<REAL>(face->GetHeight())));
            else
            {
                FillRound(g,RectF(x,y,w,h),r,Color(255,242,233,208),Color(255,30,26,24),2);
                Text(g,std::to_wstring(PageOne::RankOf(card)),RectF(x,y,w,h),h*0.2f,Color(255,30,30,36),true,StringAlignmentCenter,StringAlignmentCenter);
            }
            if(dim)FillRound(g,RectF(x,y,w,h),r,Color(130,30,30,40));
        }

        // ---- The room (as the mahjong table's) ----


        float FloorY(){return state.floorY;}

        // The faces for the 3D table, with their mipmaps.
        ImageRgba8 CardsImage()
        {
            ImageRgba8 image;
            Bitmap* faces=Faces();
            if(!faces)return image;
            const int w=static_cast<int>(faces->GetWidth()),facesH=static_cast<int>(faces->GetHeight()),h=facesH;
            Bitmap atlas(w,h,PixelFormat32bppARGB);
            {
                Graphics g(&atlas);
                g.Clear(Color(255,236,231,218));
                g.DrawImage(faces,0,0,w,facesH);
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

        float FieldZ();

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
                Log(L"pageone: no 3D view (%s)",error.c_str());
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
            Log(L"pageone: 3D view %ux%u on %s (hips %.1f, upper body %.1f, floor %.1f)",w,h,renderer->AdapterName().c_str(),state.hips,state.upper,state.floorY);
            {
                std::vector<MascotRenderer::GeometryVertex> room;
                BuildWesternRoom(room,state.roomGlows,{state.upper,state.hips,state.floorY,0.12f*state.hips+0.30f*state.upper,FieldZ()});
                if(!renderer->SetRoomGeometry(room))Log(L"pageone: no room");
            }
            if(!renderer->SetGeometryTexture(CardsImage()))Log(L"pageone: no card faces on the 3D table");
            state.renderer=std::move(renderer);
            state.overlayDue=true;
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
            state.gestureHand=0;
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


        // A card's face on the atlas (u, v at its corners: top left, top right, bottom right,
        // bottom left).
        void CardUv(int card,float (&uv)[8])
        {
            Bitmap* faces=Faces();
            const float aw=faces?static_cast<float>(faces->GetWidth()):1,ah=faces?static_cast<float>(faces->GetHeight()):1;
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

        // The table's wood (the chair is BuildChair's; the room round it, made once, is WesternRoom's).
        void BuildRoom(Mesh& m)
        {
            auto box=[&](float x0,float y0,float z0,float x1,float y1,float z1,const float (&c)[4]){m.Box(x0,y0,z0,x1,y1,z1,c);};
            int& grain=m.grain;
            float& grainScale=m.grainScale;
            const float D=state.upper,floor=FloorY();
            const float frame[4]={0.20f,0.09f,0.05f,Wood},skirt[4]={0.15f,0.07f,0.04f,Wood};
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
        }

        // ---- The cards on the table ----
        // Her cards in a row before her, the stock and the discards between you. Yours are drawn
        // along the bottom of the window: on the table they only pass by on their way there.

        float Top(){return 0.12f*state.hips+0.30f*state.upper;}
        float FieldZ(){return -1.10f*state.upper;}
        float Thick(){return 0.003f*state.upper;}
        float CardW(){return 0.149f*state.upper;}
        float CardL(){return 0.275f*state.upper;}
        std::array<float,2> ShoeAt(){return {-0.22f*state.upper,FieldZ()};}
        std::array<float,2> PileAt(){return {0.22f*state.upper,FieldZ()};}
        float HerZ(){return -0.70f*state.upper;}
        float YourZ(){return -2.40f*state.upper;}
        // Her cards' spacing in their row: closer as they grow many, to stay on the table.
        float HerStep(size_t count){return std::min(0.10f*state.upper,1.9f*state.upper/std::max<size_t>(1,count));}

        Pose Lying(float x,float z,bool faceUp,float above=0)
        {
            Pose p;
            p.x=x;p.y=Top()+Thick()/2+above;p.z=z;
            p.flip=faceUp?0.0f:1.0f;
            p.w=CardW();p.l=CardL();
            p.shown=true;
            return p;
        }

        // A point on the card: across (-w/2 to w/2), out of its face (thickness), along it
        // toward its top (-l/2 to l/2).
        std::array<float,3> At(const Pose& p,float u,float v,float w)
        {
            using namespace DirectX;
            const XMMATRIX turn=XMMatrixRotationZ(p.flip*XM_PI)*XMMatrixRotationY(p.yaw);
            const XMVECTOR q=XMVector3TransformNormal(XMVectorSet(u,v,w,0),turn);
            return {p.x+XMVectorGetX(q),p.y+XMVectorGetY(q),p.z+XMVectorGetZ(q)};
        }

        // Where every card is, as the table is now (state.poses): those in the stock at its top,
        // face down and not shown (the stock is drawn as one); yours off the table's near edge.
        void PlaceCards()
        {
            auto& t=state.table;
            const float D=state.upper;
            {
                Pose top=Lying(ShoeAt()[0],ShoeAt()[1],false,std::max<int>(0,static_cast<int>(t.Stock().size())-1)*Thick());
                top.shown=false;
                for(auto& p:state.poses)p=top;
            }
            // The pile: each card a little turned and off the middle, as played by hand.
            const auto& pile=t.Pile();
            for(size_t i=0;i<pile.size();++i)
            {
                const int k=static_cast<int>((pile[i]*31+i*17)%23);
                Pose p=Lying(PileAt()[0]+(k%5-2)*0.006f*D,PileAt()[1]+(k%7-3)*0.005f*D,true,i*Thick());
                p.yaw=static_cast<float>(k-11)*0.03f;
                state.poses[static_cast<size_t>(pile[i])]=p;
            }
            const auto& mine=t.HandOf(PageOne::You);
            for(size_t i=0;i<mine.size();++i)
            {
                Pose p=Lying((i-(mine.size()-1)/2.0f)*0.05f*D,YourZ(),true);
                p.shown=false;
                state.poses[static_cast<size_t>(mine[i])]=p;
            }
            // Hers in a row before her, turned up once the hand is over.
            const bool over=t.Now()==Phase::HandOver;
            const std::vector<int> hers=over?Sorted(PageOne::Her):t.HandOf(PageOne::Her);
            const float step=HerStep(hers.size());
            for(size_t i=0;i<hers.size();++i)
                state.poses[static_cast<size_t>(hers[i])]=Lying((i-(hers.size()-1)/2.0f)*step,HerZ(),over,i*Thick());
        }

        // A card at pose, its corners rounded: its face and its back from the atlas, cream edges.
        void AddCard(Mesh& m,const Pose& p,int card)
        {
            using V=std::array<float,3>;
            using namespace DirectX;
            constexpr int Bend=5;   // steps round each corner
            const float hw=p.w/2,hl=p.l/2,ht=Thick()/2,r=p.w*CornerShare;
            const XMMATRIX turn=XMMatrixRotationZ(p.flip*XM_PI)*XMMatrixRotationY(p.yaw);
            auto at=[&](float u,float v,float w)->V{return At(p,u,v,w);};
            auto dir=[&](float u,float v,float w)->V{
                const XMVECTOR q=XMVector3TransformNormal(XMVectorSet(u,v,w,0),turn);
                return {XMVectorGetX(q),XMVectorGetY(q),XMVectorGetZ(q)};
            };
            const float white[4]={1,1,1,1},edge[4]={0.86f,0.83f,0.76f,1};
            float face[8],back[8];
            CardUv(card,face);
            CardUv(PageOne::Back,back);
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
            out.push_back(Ready()?1:0);
            for(const auto& f:state.flights)out.push_back(1000+f.card);
            return out;
        }

        std::vector<MascotRenderer::GeometryVertex> BuildTable()
        {
            Mesh mesh;
            const float D=state.upper;
            const float felt[4]={0.09f,0.33f,0.22f,1};
            const TableShape t=Table3D();
            BuildRoom(mesh);
            mesh.Box(t.x0,t.top-0.05f*D,t.zFar,t.x1,t.top,t.zNear,felt);
            PlaceCards();
            auto& table=state.table;
            // The stock: a stack of backs.
            if(!table.Stock().empty())
            {
                const float white[4]={1,1,1,1},core[4]={0.92f,0.89f,0.82f,1};
                const float hw=CardW()/2,hl=CardL()/2,h=static_cast<float>(table.Stock().size())*Thick(),x=ShoeAt()[0],z=ShoeAt()[1];
                mesh.Box(x-hw,t.top,z-hl,x+hw,t.top+h,z+hl,core,false);
                float uv[8];
                CardUv(PageOne::Back,uv);
                const float y=t.top+h;
                mesh.Quad({x-hw,y,z+hl},{x+hw,y,z+hl},{x+hw,y,z-hl},{x-hw,y,z-hl},{0,1,0},white,uv);
            }
            // The cards on the table (those on their way are drawn where they are going).
            if(state.movesSeen==table.Moves())
                for(int c=0;c<PageOne::Cards;++c)
                {
                    const Pose& p=state.poses[static_cast<size_t>(c)];
                    if(p.shown&&!InFlight(c))AddCard(mesh,p,c);
                }
            // A rim round the pile in the suit to follow, once a wild has named it.
            if(table.Top()>=0&&PageOne::IsWild(table.Top())&&table.Now()!=Phase::HandOver&&state.movesSeen==table.Moves())
            {
                const bool red=table.Suit()==1||table.Suit()==2;
                const float rim[4]={red?0.85f:0.16f,red?0.22f:0.18f,red?0.24f:0.26f,1};
                const float m=0.03f*D,y=Top()+0.0006f*D,hw=CardW()/2+m,hl=CardL()/2+m,x=PileAt()[0],z=PileAt()[1];
                mesh.Quad({x-hw,y,z+hl},{x+hw,y,z+hl},{x+hw,y,z-hl},{x-hw,y,z-hl},{0,1,0},rim);
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
                out={mix(a.x,b.x),mix(a.y,b.y),mix(a.z,b.z),mix(a.yaw,b.yaw),mix(a.flip,b.flip),mix(a.w,b.w),mix(a.l,b.l),true};
                if(f.arc)out.y+=std::sin(u*DirectX::XM_PI)*std::max(0.06f*state.upper,0.15f*std::hypot(b.x-a.x,b.z-a.z));
                return true;
            }
            return false;   // arrived
        }

        void Finish()
        {
            if(!state.window)return;
            KillTimer(state.window,TimerId);
            state.voice.Stop();
            if(state.her.model)state.her.model->SetSolids({});
            if(state.her.model&&state.tell!=Expression::Neutral)state.her.model->SetExpression(Expression::Neutral,false);
            state.tell=Expression::Neutral;
            if(state.afterword.empty()&&state.seen.started&&state.table.Now()!=Phase::GameOver)
            {
                const auto& t=state.table;
                state.afterword=Fmt(T(L"いま、話し相手とトランプのページワンをしていたが、途中でやめてデスクトップに戻ってきたところ（話し相手は{0}点、あなたは{1}点だった）。そのことを話し相手にひと言（1〜2文）",
                                      L"You were playing Page One (a card game) with them, but they stopped midway and you have come back to the desktop (they had {0} points, you {1}). Say a word about it to them (one or two sentences)"),
                                    {std::to_wstring(t.Score(PageOne::You)),std::to_wstring(t.Score(PageOne::Her))});
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
            state.hover=-1;
            state.hoverCard=-1;
            state.calling=false;
            state.choosing=-1;
            state.nearTold=false;
            state.herLastSeen=-1;
            state.confirmQuit=false;
            state.help=false;
            state.said.clear();
            state.nextAt=GetTickCount64()+300;
            state.seen={};
            state.words.waiting=false;
            state.words.text.clear();
            state.afterword.clear();
            state.afterwordLine.clear();
            state.plannedFor=state.reachedFor=-1;
            state.tellFor=-1;
            Prefetch();
        }

        // After a move of yours: the next waits a moment.
        void Moved(ULONGLONG wait=YourWait)
        {
            state.nextAt=GetTickCount64()+wait;
            Redraw();
        }

        void Say(const wchar_t* name,int weight=2,double chance=1);

        void Continue()
        {
            auto& t=state.table;
            if(HandShown()){t.NextHand();state.calling=false;state.choosing=-1;state.nearTold=false;Moved();}
            else if(EndShown())NewGame();
            Redraw();
        }

        // Your card played (a wild with the suit you named); what it does gets a word from her.
        void PlayNow(int card,int suit)
        {
            auto& t=state.table;
            const bool call=state.calling&&t.HandOf(PageOne::You).size()==2;
            const int herBefore=static_cast<int>(t.HandOf(PageOne::Her).size());
            t.Play(card,suit,call);
            state.calling=false;
            state.choosing=-1;
            state.hoverCard=-1;
            if(call){state.said=T(L"あなた「ページワン！」",L"You: \"Page one!\"");state.saidAt=GetTickCount64();}
            Moved();
            if(t.Now()==Phase::HandOver)return;
            const auto kind=PageOne::KindOf(card);
            if(kind==PageOne::Kind::DrawTwo||kind==PageOne::Kind::DrawFour)Say(L"攻められた",2,kind==PageOne::Kind::DrawFour?0.8:0.35);
            else if(kind==PageOne::Kind::Skip||kind==PageOne::Kind::Reverse)Say(L"飛ばされた",2,0.3);
            else if(herBefore<=2&&kind==PageOne::Kind::Wild)Say(L"相手が色を変えた",2,0.4);
        }

        // A card of yours clicked: played, or for a wild, the suit to name asked first.
        void PlayYours(int card)
        {
            auto& t=state.table;
            if(!YourPlayNow()||!t.CanPlay(card))return;
            if(PageOne::IsWild(card)&&t.HandOf(PageOne::You).size()>1){state.choosing=card;Redraw();return;}
            PlayNow(card,PageOne::IsWild(card)?0:-1);
        }

        void Do(Act act,int value=0)
        {
            auto& t=state.table;
            if(act==Act::Next){Continue();return;}
            if(act==Act::Suit)
            {
                if(state.choosing>=0&&value>=0&&YourPlayNow())PlayNow(state.choosing,value);
                else {state.choosing=-1;Redraw();}
                return;
            }
            if(state.choosing>=0)return;
            if(act==Act::Call)
            {
                if(CanCall()){state.calling=!state.calling;Redraw();}
                return;
            }
            if(act==Act::Catch)
            {
                if(YourTurnNow()&&t.CanCatch(PageOne::You))
                {
                    t.Catch(PageOne::You);
                    state.said=Fmt(T(L"あなた「ページワンって言ってない！」",L"You: \"You didn't say page one!\""),{});
                    state.saidAt=GetTickCount64();
                    Moved();
                    Say(L"指摘された",3,1);
                }
                return;
            }
            if(act==Act::Draw&&YourTurnNow()&&t.CanDraw())
            {
                const int penalty=t.Penalty();
                t.Draw();
                state.calling=false;
                Moved(t.Now()==Phase::YourDrawn&&!t.CanPlay(t.JustDrawn())?700:YourWait);   // (one that can't be played is kept by itself)
                if(penalty>=4)Say(L"たくさん引かせた",2,0.6);
            }
            else if(act==Act::Keep&&YourDrawnNow()&&t.CanKeep()){t.Keep();state.calling=false;Moved();}
        }

        // In 2D, a card's sound as soon as it is dealt (at the 3D table, as it lands).
        void Listen()
        {
            if(state.renderer)return;
            auto& t=state.table;
            if(t.Moves()>state.heardMoves&&state.heardMoves>=0)PlayMahjongSound(MahjongSound::CardPlace);
            state.heardMoves=t.Moves();
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

        // Something happened: she speaks of it, and those watching hear of it. facts: for her (she
        // is "you"); feel: how she is to feel about it; seen: the same for one watching; scene:
        // the scene of her lines (null: she says nothing); theirs: the watchers'.
        void Happened(int weight,const std::wstring& facts,const std::wstring& feel,const wchar_t* scene,
                      const std::wstring& seen,const std::vector<std::wstring>& theirs)
        {
            auto& t=state.table;
            state.words.told+=Fmt(L"  {0} s, weight {1}: {2}\n",{std::to_wstring(static_cast<int>(state.clock)),std::to_wstring(weight),seen});
            if(state.her.onEvent&&!theirs.empty())
                state.her.onEvent(seen+Fmt(T(L"（持ち主は{0}点、{1}は{2}点）",L" (the owner has {0} points, {1} {2})"),{std::to_wstring(t.Score(PageOne::You)),HerName(),std::to_wstring(t.Score(PageOne::Her))}),weight,Pick(theirs));
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

        // A line of the scene, said now and then (chance: how often, unless the user's lines
        // file says otherwise).
        void Say(const wchar_t* name,int weight,double chance)
        {
            if(!state.lines.Roll(name,chance,state.sparkRandom))return;
            GameLine line;
            if(state.lines.Pick(name,state.words.recent,state.sparkRandom,line))SayLine(line,weight);
        }

        // The look on her face. Through a hand it comes from how near she is to going out against
        // you: a smile with few cards left, a worried look when she holds many more than you, or
        // now and then the other way about, put on.
        void UpdateTell()
        {
            auto& t=state.table;
            Expression e=Expression::Neutral;
            if(t.Now()==Phase::HandOver)
            {
                if(!Flying()&&t.Result().winner>=0)e=t.Result().winner==PageOne::Her?Expression::Smile:Expression::Sad;
            }
            else if(t.Now()==Phase::GameOver)e=t.Score(PageOne::Her)>t.Score(PageOne::You)?Expression::Smile:Expression::Sad;
            else if(!t.HandOf(PageOne::Her).empty())
            {
                const int hers=static_cast<int>(t.HandOf(PageOne::Her).size()),yours=static_cast<int>(t.HandOf(PageOne::You).size());
                const int band=hers<=2?2:hers>=yours+4?0:1;
                const int key=t.HandNumber()*1000+hers*10+band;
                if(state.tellFor!=key)
                {
                    state.tellFor=key;
                    std::uniform_real_distribution<double> any(0,1);
                    Expression look=band==2?Expression::Smile:band==0?Expression::Troubled:Expression::Neutral;
                    if(any(state.sparkRandom)<PutOnShare)look=look==Expression::Smile?Expression::Troubled:look==Expression::Troubled?Expression::Smile:look;
                    state.tellFace=look;
                }
                e=state.tellFace;
            }
            if(e==state.tell)return;
            state.tell=e;
            if(state.her.model&&state.renderer)state.her.model->SetExpression(e,false);
        }

        // What is new in the game since the last tick, for words: its start, a hand's end, the
        // game's end.
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
            if(!seen.started)
            {
                seen.started=true;
                Happened(6,T(L"これから話し相手とトランプのページワンの対戦が始まる。",L"A game of Page One (a card game) with them is about to begin. "),
                         T(L"わくわくする気持ち",L"how you look forward to it"),L"始まり",
                         Fmt(T(L"{0}とこのパソコンの持ち主のページワンが始まる。",L"{0} and the owner of this computer are starting a game of Page One."),{name}),
                         {T(L"がんばれー！",L"Good luck!"),T(L"どっちが勝つかな？",L"Who will win?")});
            }
            // A hand over: a word on how.
            if(t.Now()==Phase::HandOver&&seen.hand!=t.HandNumber())
            {
                seen.hand=t.HandNumber();
                const auto& r=t.Result();
                const std::wstring points=std::to_wstring(r.points);
                if(r.winner<0)Happened(2,T(L"ページワンで、勝負がつかずにこの回はなしになった。",L"At Page One, the hand went on too long and was void."),L"",L"流れた",L"",{});
                else if(r.winner==PageOne::Her)
                    Happened(r.points>=60?6:4,Fmt(T(L"ページワンで、あなたが先に手札を出し切って{0}点取った。",L"At Page One, you played out your cards first and scored {0} points."),{points}),L"",L"上がり",
                             Fmt(T(L"ページワンで{0}が上がった。",L"At Page One, {0} went out."),{name}),{T(L"上がった！",L"Out!")});
                else
                    Happened(r.points>=60?6:4,Fmt(T(L"ページワンで、話し相手が先に手札を出し切って{0}点取った。",L"At Page One, they played out their cards first and scored {0} points."),{points}),L"",L"相手の上がり",
                             T(L"ページワンで持ち主が上がった。",L"At Page One, the owner went out."),{T(L"上がりだ！",L"They're out!")});
            }
            const bool over=t.Now()==Phase::GameOver;
            if(over&&!seen.over)
            {
                const int you=t.Score(PageOne::You),her=t.Score(PageOne::Her);
                // How she goes easier next game comes from how this one ended.
                state.lost=her>you?state.lost+1:0;
                const std::wstring result=Fmt(T(L"話し相手は{0}点、あなたは{1}点",L"they have {0} points, you {1}"),{std::to_wstring(you),std::to_wstring(her)});
                state.afterword=Fmt(T(L"いま、話し相手とトランプのページワンをして、デスクトップに戻ってきたところ。結果は{0}。その勝負の感想を、話し相手にひと言（1〜2文）",
                                      L"You have just played Page One (a card game) with them and come back to the desktop. In the end {0}. Tell them what you thought of the game (one or two sentences)"),{result});
                state.afterwordLine=you>her?T(L"ページワン、強かったね！次は負けないよ。",L"You were good at that! I'll win next time."):T(L"ページワン、楽しかったね！またやろう。",L"That was fun! Let's play again sometime.");
                if(you>her)Happened(9,Fmt(T(L"ページワンが終わって、話し相手の勝ち（{0}）。",L"The Page One game is over and they won ({0})."),{result}),
                                    T(L"悔しい気持ちか、話し相手をほめる気持ち",L"how sorry you are, or praise them"),L"負けて終わる",
                                    Fmt(T(L"{0}と持ち主のページワンが終わり、持ち主が勝った。",L"The Page One game between {0} and the owner is over: the owner won."),{name}),
                                    {T(L"おめでとう！",L"Congratulations!"),T(L"おつかれさま！",L"Well played!")});
                else Happened(9,Fmt(T(L"ページワンが終わって、あなたの勝ち（{0}）。",L"The Page One game is over and you won ({0})."),{result}),
                              T(L"うれしい気持ち",L"how pleased you are"),L"勝って終わる",
                              Fmt(T(L"{0}と持ち主のページワンが終わり、{0}が勝った。",L"The Page One game between {0} and the owner is over: {0} won."),{name}),
                              {T(L"おつかれさま！いい勝負だったね。",L"Well played, both of you!"),Fmt(T(L"{0}の勝ちだ！",L"{0} wins!"),{name})});
            }
            seen.over=over;
        }

        // Her turn or cards to come: time for what comes next (at the 3D table once she is seated).
        void Plan(ULONGLONG now)
        {
            auto& t=state.table;
            state.plannedFor=t.Moves();
            std::uniform_int_distribution<int> extra(0,500);
            ULONGLONG wait=HandWait;
            if(t.Now()!=Phase::Dealing)
            {
                wait=(state.renderer?HerPlayWait3D:HerPlayWait)+extra(state.sparkRandom);
                if(t.Now()==Phase::HerTurn&&!state.words.waiting&&GetTickCount64()-state.words.since>WordsHold)Say(L"考える",1,0.08);
            }
            state.nextAt=std::max(state.nextAt,now+wait);
        }

        // What her step did, told: her "page one", a suit named, a card that makes you draw or
        // passes you over, yours pointed out, many drawn.
        void Heard(int yoursBefore,int hersBefore)
        {
            auto& t=state.table;
            const auto did=t.LastDid(PageOne::Her);
            const ULONGLONG now=GetTickCount64();
            if(t.Now()==Phase::HandOver)return;   // (her going out is spoken of with the hand)
            if(did==PageOne::Did::Caught&&static_cast<int>(t.HandOf(PageOne::You).size())>yoursBefore)
            {
                state.said=Fmt(T(L"{0}「ページワンって言ってないよ！」",L"{0}: \"You didn't say page one!\""),{HerName()});
                state.saidAt=now;
                Say(L"指摘する",3,1);
                return;
            }
            if(did==PageOne::Did::Penalty){if(static_cast<int>(t.HandOf(PageOne::Her).size())-hersBefore>=4)Say(L"たくさん引いた",2,0.7);return;}
            if(did==PageOne::Did::Drew){Say(L"出せない",1,0.2);return;}
            if(did!=PageOne::Did::Played||static_cast<int>(t.HandOf(PageOne::Her).size())>=hersBefore)return;
            const int card=t.LastPlayed(PageOne::Her);
            const auto kind=PageOne::KindOf(card);
            if(PageOne::IsWild(card))
            {
                state.said=Fmt(T(L"{0}は{1}{2}を指定",L"{0} names {2} {1}"),{HerName(),SuitMark(t.Suit()),SuitName(t.Suit())});
                state.saidAt=now;
            }
            if(t.Called(PageOne::Her))
            {
                state.said=Fmt(T(L"{0}「ページワン！」",L"{0}: \"Page one!\""),{HerName()})+(PageOne::IsWild(card)?Fmt(T(L"（{0}を指定）",L" ({0} named)"),{SuitMark(t.Suit())}):L"");
                state.saidAt=now;
                Say(L"ページワン",3,1);
            }
            else if(kind==PageOne::Kind::DrawTwo||kind==PageOne::Kind::DrawFour)Say(L"攻める",2,kind==PageOne::Kind::DrawFour?0.8:0.4);
            else if(kind==PageOne::Kind::Skip||kind==PageOne::Kind::Reverse)Say(L"飛ばす",2,0.3);
            else if(kind==PageOne::Kind::Wild)Say(L"色を変える",2,0.4);
        }

        void Tick()
        {
            auto& t=state.table;
            const ULONGLONG now=GetTickCount64();
            if(Flying())state.nextAt=std::max(state.nextAt,now+120);   // play waits for the cards
            if(!state.said.empty()&&now-state.saidAt>SaidShown){state.said.clear();Redraw();}
            const bool hers=t.Now()==Phase::Dealing||t.Now()==Phase::HerTurn||t.Now()==Phase::HerDrawn;
            if(hers&&state.plannedFor!=t.Moves()&&!Flying()&&(!state.renderer||state.entrance==5))Plan(now);
            if(hers&&state.plannedFor==t.Moves()&&now>=state.nextAt&&(!state.renderer||state.entrance==5))
            {
                const bool dealing=t.Now()==Phase::Dealing;
                const int yoursBefore=static_cast<int>(t.HandOf(PageOne::You).size()),hersBefore=static_cast<int>(t.HandOf(PageOne::Her).size());
                t.Step();
                state.nextAt=now+150;
                if(!dealing)Heard(yoursBefore,hersBefore);
            }
            // A card you drew that can't be played: your turn ends by itself.
            if(YourDrawnNow()&&!t.CanPlay(t.JustDrawn())&&state.choosing<0){t.Keep();state.calling=false;Moved();}
            // You are down to your last card: now and then she lets it be known it worries her.
            if(t.HandOf(PageOne::You).size()>1)state.nearTold=false;
            else if(t.Now()==Phase::HerTurn&&!state.nearTold&&!Flying()&&t.HandOf(PageOne::You).size()==1)
            {
                state.nearTold=true;
                if(!state.words.waiting&&now-state.words.since>WordsHold)Say(L"相手がもうすぐ",2,0.45);
            }
            // A deal now and then gets a word.
            if(!t.HandOf(PageOne::You).empty()&&t.Turns()==0&&state.seen.dealt!=t.HandNumber()&&!Flying())
            {
                state.seen.dealt=t.HandNumber();
                if(!state.words.waiting&&now-state.words.since>WordsHold)Say(L"配る",2,0.3);
            }
            UpdateTell();
            if(!state.renderer)Redraw();
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

        void PaintEnd(Graphics& g,float cw,float ch,float s)
        {
            auto& t=state.table;
            SolidBrush shade(Color(150,0,0,0));
            g.FillRectangle(&shade,0.0f,0.0f,cw,ch);
            const float w=std::min(cw-40*s,600*s),h=std::min(ch-40*s,320*s);
            const RectF box((cw-w)/2,(ch-h)/2,w,h);
            FillRound(g,box,14*s,Color(255,40,22,22),Color(255,200,170,90),2*s);
            float y=box.Y+22*s;
            const Color gold(255,240,206,120),white(255,240,240,236),soft(255,200,190,186);
            Text(g,T(L"勝負あり",L"Game over"),RectF(box.X,y,w,44*s),32*s,gold,true,StringAlignmentCenter,StringAlignmentNear,true);
            y+=66*s;
            const int you=t.Score(PageOne::You),her=t.Score(PageOne::Her);
            Text(g,Fmt(T(L"あなた {0}点　{1} {2}点",L"You {0}   {1} {2}"),{std::to_wstring(you),HerName(),std::to_wstring(her)}),RectF(box.X,y,w,40*s),26*s,white,false,StringAlignmentCenter);
            y+=82*s;
            if(you>her)Runs(g,{{T(L"あなたの勝ち！",L"You win!")}},RectF(box.X,y,w,48*s),32*s,gold);
            else Runs(g,{{HerName()},{T(L"の勝ち",L" wins")}},RectF(box.X,y,w,48*s),32*s,gold);
            Text(g,T(L"Enter：もう一度　Esc：閉じる",L"Enter: play again   Esc: close"),RectF(box.X,box.GetBottom()-44*s,w,30*s),17*s,soft,false,StringAlignmentCenter);
        }

        // How the game goes.
        void PaintHelp(Graphics& g,float W,float H,float s)
        {
            SolidBrush shade(Color(150,0,0,0));
            g.FillRectangle(&shade,0.0f,0.0f,W,H);
            const float w=std::min(W-40*s,760*s),h=std::min(H-40*s,560*s);
            const RectF box((W-w)/2,(H-h)/2,w,h);
            FillRound(g,box,14*s,Color(250,40,22,22),Color(255,200,170,90),2*s);
            const Color gold(255,240,206,120),white(255,240,240,236),soft(255,200,190,186);
            Text(g,T(L"ページワンの遊び方",L"How to play Page One"),RectF(box.X,box.Y+14*s,w,36*s),24*s,gold,true,StringAlignmentCenter);
            struct Row {std::wstring name,about;};
            const Row rows[]={
                {T(L"進め方",L"Your turn"),T(L"場札と同じマークか同じ数のカードを1枚出す。出せないときは山札から1枚",L"play a card of the pile's suit or rank; if you can't, draw one")},
                {T(L"引いたとき",L"After drawing"),T(L"引いたカードが出せればそのまま出せる。出さずに終えてもよい",L"the card drawn may be played at once, or kept")},
                {T(L"2",L"2"),T(L"次の人は2枚引く。2を出して返すと、合わせて次の人へ",L"the next draws two, unless they answer with a 2 (adding up)")},
                {T(L"J・A",L"J and A"),T(L"Jは相手を飛ばし、Aは順番を逆に（二人ではどちらも続けて出す）",L"J skips, A turns back the order: with two, you play again")},
                {T(L"8",L"8"),T(L"いつでも出せて、次のマークを決められる",L"goes on anything; you name the next suit")},
                {T(L"ジョーカー",L"Joker"),T(L"いつでも出せて、マークを決め、次の人は4枚引く",L"goes on anything, names the suit, and the next draws four")},
                {T(L"ページワン",L"Page one"),T(L"残り1枚になるカードを出すときに宣言（O）。忘れて指摘されると2枚引く",L"say it (O) as you come down to one card, or draw two if caught")},
                {T(L"指摘",L"Catching"),T(L"相手が言い忘れたら、自分の番に「言ってない！」（C）",L"if they forget it, call it out on your turn (C)")},
                {T(L"点数",L"Scoring"),T(L"上がった人に、相手の残り札の点（数そのまま、Q・K 10、2・J・A 20、8・ジョーカー 50）",L"the one out scores the other's cards: numbers as they are, Q K 10, 2 J A 20, 8 joker 50")},
                {T(L"勝ち",L"To win"),Fmt(T(L"先に{0}点。手札を出し切った人がその回の勝ち",L"first to {0}; a hand is won by playing out all your cards"),{std::to_wstring(PageOne::Target)})}};
            float y=box.Y+60*s;
            for(const Row& r:rows)
            {
                Text(g,r.name,RectF(box.X+24*s,y,160*s,30*s),17*s,white,true);
                Text(g,r.about,RectF(box.X+182*s,y+1*s,w-196*s,30*s),15*s,soft);
                y+=42*s;
            }
            Text(g,T(L"Tab：閉じる",L"Tab: close"),RectF(box.X,box.GetBottom()-34*s,w,26*s),15*s,soft,false,StringAlignmentCenter);
        }

        // The buttons for now: the suits for a wild; to draw (or the cards to draw); to keep a
        // card drawn; "page one" with your next card; calling out hers; Next when a hand is over.
        void LayButtons(Graphics& g,float W,float H,float s)
        {
            auto& t=state.table;
            std::vector<Button> b;
            if(state.choosing>=0)
            {
                for(int k=0;k<4;++k)
                    b.push_back({Act::Suit,k,Fmt(L"{0} {1}（{2}）",{SuitMark(k),SuitName(k),std::to_wstring(k+1)}),RectF(),YourPlayNow()});
                b.push_back({Act::Suit,-1,T(L"やめる（Esc）",L"Back (Esc)"),RectF(),true});
            }
            else if(t.Now()==Phase::YourTurn||t.Now()==Phase::YourDrawn)
            {
                if(t.Now()==Phase::YourTurn)
                {
                    const std::wstring draw=t.Penalty()>0?Fmt(T(L"{0}枚引く（D）",L"Draw {0} (D)"),{std::to_wstring(t.Penalty())}):std::wstring(T(L"山札から引く（D）",L"Draw a card (D)"));
                    b.push_back({Act::Draw,0,draw,RectF(),YourTurnNow()});
                }
                else b.push_back({Act::Keep,0,T(L"出さずに終える（K）",L"Keep it (K)"),RectF(),YourDrawnNow()});
                if(t.HandOf(PageOne::You).size()==2)
                    b.push_back({Act::Call,0,state.calling?T(L"ページワン！をやめる（O）",L"Don't say it (O)"):T(L"ページワン！（O）",L"Page one! (O)"),RectF(),CanCall()});
                if(t.CanCatch(PageOne::You))b.push_back({Act::Catch,0,T(L"言ってない！（C）",L"You didn't say it! (C)"),RectF(),YourTurnNow()});
            }
            else if(HandShown())b.push_back({Act::Next,0,T(L"次へ（Enter）",L"Next (Enter)"),RectF(),true});
            float total=0;
            std::vector<float> widths;
            for(const auto& k:b)
            {
                const float bw=std::max(120*s,TextWidth(g,k.label,16*s,true)+26*s);
                widths.push_back(bw);
                total+=bw+10*s;
            }
            float x=W/2-(total-10*s)/2;
            const float y=H-(CardH2D+12+56)*s;
            for(size_t i=0;i<b.size();++i)
            {
                Button& k=b[i];
                k.box=RectF(x,y,widths[i],46*s);
                x+=widths[i]+10*s;
                const bool lit=k.on&&static_cast<int>(i)==state.hover;
                const bool strong=k.act==Act::Next||((k.act==Act::Call||k.act==Act::Catch)&&k.on)||(k.act==Act::Call&&state.calling);
                Color fill=!k.on?Color(170,60,50,46):lit?Color(250,250,236,200):Color(240,236,226,196);
                if(strong)fill=lit?Color(250,226,90,70):Color(245,206,60,50);
                FillRound(g,k.box,10*s,fill,Color(255,90,60,30),1.5f*s);
                Color ink=!k.on?Color(255,150,140,130):strong?Color(255,255,250,240):Color(255,40,30,20);
                if(k.act==Act::Suit&&k.value>=0&&k.on)ink=SuitInk(k.value);
                Text(g,k.label,k.box,16*s,ink,true,StringAlignmentCenter,StringAlignmentCenter);
            }
            state.buttons=std::move(b);
        }

        // Your cards along the bottom of the window, sorted by suit (a little gap between suits,
        // the wilds last), closer together as they grow many; those you cannot play now dimmed,
        // the one you point at lifted, the one just drawn rimmed in gold.
        void PaintHand(Graphics& g,float W,float H,float s)
        {
            auto& t=state.table;
            state.cardBoxes.clear();
            if(t.HandOf(PageOne::You).empty()||t.Now()==Phase::GameOver)return;
            const std::vector<int> cards=Sorted(PageOne::You);
            const float cw2=CardW2D*s,ch2=CardH2D*s;
            auto group=[](int c){return PageOne::IsWild(c)?4:PageOne::SuitOf(c);};
            int gaps=0;
            for(size_t i=1;i<cards.size();++i)if(group(cards[i])!=group(cards[i-1]))++gaps;
            // The hand fits between the boxes at the sides (yours at the left, the pile's at the right in 3D).
            const float room=W-2*250*s;
            float step=HandStep*s,gap=SuitGap*s;
            if(cards.size()>1)
            {
                const float wide=cw2+(cards.size()-1)*step+gaps*gap;
                if(wide>room)
                {
                    const float k=std::max(0.15f,(room-cw2)/std::max(1.0f,(cards.size()-1)*step+gaps*gap));
                    step*=k;gap*=k;
                }
            }
            const float width=cw2+(cards.size()-1)*step+gaps*gap;
            float x=(W-width)/2;
            const float y=H-ch2-12*s;
            const bool playing=t.Now()==Phase::YourTurn||t.Now()==Phase::YourDrawn;
            for(size_t i=0;i<cards.size();++i)
            {
                const int c=cards[i];
                if(i>0&&group(c)!=group(cards[i-1]))x+=gap;
                const bool can=playing&&t.CanPlay(c)&&state.choosing<0;
                const bool up=YourPlayNow()&&can&&c==state.hoverCard;
                const bool dim=playing&&!can&&c!=state.choosing;
                const float lift=c==state.choosing?26*s:up?18*s:0;
                DrawCard(g,x,y-lift,cw2,ch2,c,true,dim,c==t.JustDrawn()||c==state.choosing);
                state.cardBoxes.push_back({c,RectF(x,y-lift,cw2,ch2+lift)});
                x+=step;
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
            const Color white(255,240,240,236),soft(255,206,196,186),gold(255,240,206,120),panel(185,40,18,16),edge(220,170,120,100);
            // Her: name, score, how many cards she holds.
            const float nameX=hud?28*s:112*s,nameY=hud?20*s:14*s;
            if(hud)FillRound(g,RectF(14*s,12*s,260*s,92*s),10*s,panel,edge,1.2f*s);
            else
            {
                const RectF faceBox(16*s,14*s,84*s,84*s);
                FillRound(g,faceBox,10*s,Color(255,14,50,36),Color(255,120,160,130),1.5f*s);
                if(state.face)g.DrawImage(state.face.get(),faceBox.X+3*s,faceBox.Y+3*s,faceBox.Width-6*s,faceBox.Height-6*s);
                else Text(g,HerName().substr(0,1),faceBox,40*s,white,true,StringAlignmentCenter,StringAlignmentCenter);
            }
            Text(g,HerName()+(t.Dealer()==PageOne::Her?T(L"（親）",L" (dealer)"):L""),RectF(nameX,nameY,240*s,28*s),20*s,white,true);
            Text(g,Fmt(T(L"{0}点",L"{0} points"),{std::to_wstring(t.Score(PageOne::Her))}),RectF(nameX,nameY+30*s,200*s,24*s),17*s,gold);
            if(!t.HandOf(PageOne::Her).empty()&&t.Now()!=Phase::HandOver&&t.Now()!=Phase::GameOver)
            {
                Text(g,Fmt(T(L"手札 {0}枚",L"{0} cards"),{std::to_wstring(t.HandOf(PageOne::Her).size())}),RectF(nameX,nameY+56*s,120*s,24*s),16*s,soft);
                if(t.Called(PageOne::Her))Text(g,T(L"ページワン！",L"Page one!"),RectF(nameX+110*s,nameY+56*s,130*s,24*s),16*s,Color(255,255,170,90),true);
            }
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

            // The hand and the stock (at the top right), and how to play.
            {
                const RectF info(W-254*s,12*s,240*s,64*s);
                FillRound(g,info,10*s,Color(hud?215:200,40,18,16),edge,1.2f*s);
                Text(g,Fmt(T(L"ページワン  {0}回目",L"Page One  hand {0}"),{std::to_wstring(t.HandNumber())}),RectF(info.X+12*s,info.Y+6*s,228*s,26*s),18*s,white,true);
                Text(g,Fmt(T(L"山札 {0}枚　{1}点先取",L"Stock {0}   first to {1}"),{std::to_wstring(t.Stock().size()),std::to_wstring(PageOne::Target)}),RectF(info.X+12*s,info.Y+34*s,224*s,24*s),15*s,soft);
                state.helpBox=RectF(W-254*s,82*s,240*s,30*s);
                FillRound(g,state.helpBox,8*s,Color(hud?215:200,40,18,16),edge,1.2f*s);
                Text(g,T(L"遊び方（Tab）",L"How to play (Tab)"),state.helpBox,14*s,soft,false,StringAlignmentCenter,StringAlignmentCenter);
            }

            // The table's cards, in 2D (on the 3D table when there is one).
            state.stockBox=RectF();
            if(!hud)
            {
                const float cw2=CardW2D*s,ch2=CardH2D*s;
                // Hers face down, or sorted and turned up once the hand is over.
                const bool shownHers=t.Now()==Phase::HandOver;
                const std::vector<int> hers=shownHers?Sorted(PageOne::Her):t.HandOf(PageOne::Her);
                const float step=std::min(44*s,(W-360*s)/std::max<size_t>(1,hers.size()));
                for(size_t i=0;i<hers.size();++i)
                    DrawCard(g,mid-(hers.size()*step+cw2-step)/2+i*step,104*s,cw2,ch2,hers[i],shownHers);
                if(!t.Stock().empty())
                {
                    state.stockBox=RectF(mid-180*s,300*s,cw2,ch2);
                    DrawCard(g,mid-180*s,300*s,cw2,ch2,-1,false,false,YourTurnNow()&&state.choosing<0&&!t.CanPlayAny());
                    Text(g,T(L"山札",L"Stock"),RectF(mid-200*s,276*s,cw2+40*s,22*s),14*s,white,true,StringAlignmentCenter);
                }
            }

            // The pile's top card, the suit to follow (a wild's named one), the cards to draw.
            state.topBox=RectF();
            if(t.Top()>=0&&t.Now()!=Phase::GameOver)
            {
                const float cw2=CardW2D*s,ch2=CardH2D*s;
                const float x=hud?W-cw2-14*s:mid-20*s,y=hud?H-ch2-12*s:300*s;
                state.topBox=RectF(x,y,cw2,ch2);
                DrawCard(g,x,y,cw2,ch2,t.Top(),true);
                Text(g,T(L"場札",L"Pile"),RectF(x-20*s,y-24*s,cw2+40*s,22*s),14*s,white,true,StringAlignmentCenter);
                if(t.Now()!=Phase::HandOver)
                {
                    // The suit to follow, beside it: lit when a wild has named it.
                    const bool named=PageOne::IsWild(t.Top());
                    const RectF mark(x-64*s,y+ch2/2-54*s,54*s,54*s);
                    FillRound(g,mark,10*s,named?Color(250,255,248,232):Color(200,236,230,214),named?Color(255,240,190,80):edge,named?2.5f*s:1.2f*s);
                    Text(g,SuitMark(t.Suit()),mark,32*s,SuitInk(t.Suit()),true,StringAlignmentCenter,StringAlignmentCenter);
                    if(t.Penalty()>0)
                    {
                        const RectF owe(x-64*s,y+ch2/2+6*s,54*s,40*s);
                        FillRound(g,owe,10*s,Color(245,200,40,40),Color(255,255,210,150),1.5f*s);
                        Text(g,L"+"+std::to_wstring(t.Penalty()),owe,22*s,white,true,StringAlignmentCenter,StringAlignmentCenter);
                    }
                }
            }

            // You: score, how many cards.
            {
                const float boxW=230*s,boxH=92*s,boxY=H-CardH2D*s-12*s-boxH-14*s;
                FillRound(g,RectF(8*s,boxY,boxW,boxH),10*s,panel,edge,1.2f*s);
                Text(g,std::wstring(T(L"あなた",L"You"))+(t.Dealer()==PageOne::You?T(L"（親）",L" (dealer)"):L""),RectF(22*s,boxY+8*s,boxW,26*s),18*s,white,true);
                Text(g,Fmt(T(L"{0}点",L"{0} points"),{std::to_wstring(t.Score(PageOne::You))}),RectF(22*s,boxY+34*s,boxW,24*s),17*s,gold);
                if(!t.HandOf(PageOne::You).empty()&&t.Now()!=Phase::HandOver)
                {
                    Text(g,Fmt(T(L"手札 {0}枚",L"{0} cards"),{std::to_wstring(t.HandOf(PageOne::You).size())}),RectF(22*s,boxY+60*s,120*s,24*s),16*s,soft);
                    if(t.Called(PageOne::You))Text(g,T(L"ページワン！",L"Page one!"),RectF(122*s,boxY+60*s,120*s,24*s),16*s,Color(255,255,170,90),true);
                }
            }

            // A hand over: who went out, with what, and what it scored, at the side.
            if(HandShown())
            {
                const auto& r=t.Result();
                const float pw=std::min(W-40*s,360*s),ph=(r.winner<0?110:r.drawnAtEnd>0?190:162)*s;
                const RectF box(W-pw-14*s,hud?std::max(120*s,H*0.34f-ph/2):270*s,pw,ph);
                FillRound(g,box,14*s,Color(235,40,22,22),Color(255,200,170,90),2*s);
                const std::wstring how=r.winner<0?std::wstring(T(L"勝負がつきませんでした",L"No one went out")):
                                       r.winner==PageOne::You?std::wstring(T(L"あなたの上がり！",L"You're out!")):Fmt(T(L"{0}の上がり",L"{0} is out"),{HerName()});
                Text(g,how,RectF(box.X,box.Y+12*s,pw,36*s),24*s,gold,true,StringAlignmentCenter,StringAlignmentNear,true);
                if(r.winner<0)Text(g,T(L"この回はなしです",L"This hand doesn't count"),RectF(box.X,box.Y+58*s,pw,30*s),18*s,white,false,StringAlignmentCenter);
                else
                {
                    const int loser=1-r.winner;
                    const std::wstring who=r.winner==PageOne::You?std::wstring(T(L"あなた",L"You")):HerName();
                    const std::wstring other=loser==PageOne::You?std::wstring(T(L"あなた",L"you")):HerName();
                    Text(g,Fmt(T(L"{0} +{1}点",L"{0} +{1}"),{who,std::to_wstring(r.points)}),RectF(box.X,box.Y+54*s,pw,32*s),22*s,r.winner==PageOne::You?gold:Color(255,236,150,140),true,StringAlignmentCenter);
                    Text(g,Fmt(T(L"{0}の残り {1}枚",L"{1} cards left with {0}"),{other,std::to_wstring(t.HandOf(loser).size())}),RectF(box.X,box.Y+96*s,pw,26*s),17*s,white,false,StringAlignmentCenter);
                    if(r.drawnAtEnd>0)Text(g,Fmt(T(L"最後の{0}で{1}枚引いた分も数えます",L"with {1} drawn for the last card, {0}"),{r.lastCard>=0&&PageOne::IsJoker(r.lastCard)?std::wstring(T(L"ジョーカー",L"the joker")):std::wstring(L"2"),std::to_wstring(r.drawnAtEnd)}),
                                           RectF(box.X,box.Y+126*s,pw,26*s),15*s,soft,false,StringAlignmentCenter);
                }
            }

            // What to do now.
            {
                std::wstring prompt;
                if(state.choosing>=0)prompt=T(L"次のマークを選びます（1〜4）",L"Name the next suit (1-4)");
                else if(YourTurnNow()&&t.Penalty()>0)
                    prompt=t.CanPlayAny()?Fmt(T(L"2かジョーカーで返すか、{0}枚引きます",L"Answer with a 2 or the joker, or draw {0}"),{std::to_wstring(t.Penalty())}):
                                          Fmt(T(L"返せるカードがありません。{0}枚引きます（D）",L"Nothing to answer it with: draw {0} (D)"),{std::to_wstring(t.Penalty())});
                else if(YourTurnNow()&&t.CanCatch(PageOne::You))prompt=Fmt(T(L"{0}は「ページワン」と言っていません（C で指摘）",L"{0} didn't say \"page one\" (C to call it out)"),{HerName()});
                else if(YourPlayNow()&&state.calling)prompt=T(L"「ページワン！」と言って出すカードを選びます",L"Choose the card to play as you say \"page one\"");
                else if(YourTurnNow()&&!t.CanPlayAny())prompt=T(L"出せるカードがありません。山札から引きます（D）",L"Nothing to play: draw a card (D)");
                else if(YourTurnNow())prompt=Fmt(T(L"{0}か同じ数のカードを出します（Tabで遊び方）",L"Play a card of {0} or the same rank (Tab: how to play)"),{std::wstring(SuitMark(t.Suit()))+SuitName(t.Suit())});
                else if(YourDrawnNow())prompt=t.CanPlay(t.JustDrawn())?T(L"引いたカードを出すか、出さずに終えます（K）",L"Play the card drawn, or keep it (K)"):T(L"引いたカードは出せません（K で終える）",L"The card drawn can't be played (K to end your turn)");
                if(!prompt.empty())
                {
                    const float tw=TextWidth(g,prompt,16*s)+24*s,y=H-(CardH2D+12+56+40)*s;
                    FillRound(g,RectF(mid-tw/2,y,tw,30*s),10*s,panel,edge,1.2f*s);
                    Text(g,prompt,RectF(mid-tw/2,y,tw,30*s),16*s,gold,false,StringAlignmentCenter,StringAlignmentCenter);
                }
            }
            PaintHand(g,W,H,s);
            LayButtons(g,W,H,s);
            if(!state.said.empty())
            {
                const float tw=TextWidth(g,state.said,26*s,true)+40*s;
                const RectF bubble(mid-tw/2,H*0.30f,tw,52*s);
                FillRound(g,bubble,14*s,Color(245,255,252,240),Color(255,230,120,60),2*s);
                Text(g,state.said,bubble,26*s,Color(255,200,40,40),true,StringAlignmentCenter,StringAlignmentCenter);
            }

            if(EndShown())PaintEnd(g,W,H,s);
            if(state.help)PaintHelp(g,W,H,s);
            if(state.confirmQuit)
            {
                SolidBrush shade(Color(160,0,0,0));
                g.FillRectangle(&shade,0.0f,0.0f,W,H);
                const RectF box(mid-220*s,ch/2.0f-70*s,440*s,140*s);
                FillRound(g,box,14*s,Color(250,40,22,22),Color(255,200,170,90),2*s);
                Text(g,T(L"ページワンをやめますか？",L"Stop playing?"),RectF(box.X,box.Y+22*s,box.Width,36*s),24*s,white,true,StringAlignmentCenter);
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

        // All the 2D layer over the 3D view shows.
        std::uint64_t Shown()
        {
            auto& t=state.table;
            ShownKey k;
            k(t.Now(),t.Moves(),t.HandNumber(),t.Dealer(),t.Score(PageOne::You),t.Score(PageOne::Her),t.Top(),t.JustDrawn(),t.Turns(),t.Suit(),t.Penalty(),t.Stock().size(),t.HandOf(PageOne::You));
            k(Waiting(),Flying(),state.hover,state.hoverCard,state.calling,state.choosing,state.said,state.help,state.confirmQuit);
            k.Words(state.words.text,state.words.shown,static_cast<float>(state.besideHead.x),static_cast<float>(state.headScreen.y),2*Scale());
            return k.Value();
        }

        // Over the 3D view the 2D layer is drawn from the frame (TickPageOneWindow), and only when what it shows changed:
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

        int ButtonAt(float x,float y)
        {
            for(size_t i=0;i<state.buttons.size();++i)if(state.buttons[i].box.Contains(x,y))return static_cast<int>(i);
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
            if(key==VK_TAB){state.help=!state.help;Redraw();return;}
            if(state.help){if(key==VK_ESCAPE){state.help=false;Redraw();}return;}
            if(t.Now()==Phase::GameOver)
            {
                if((key==VK_RETURN||key==VK_SPACE)&&EndShown())Continue();
                else if(key==VK_ESCAPE)Finish();
                return;
            }
            if(state.choosing>=0)
            {
                if(key>='1'&&key<='4')Do(Act::Suit,static_cast<int>(key-'1'));
                else if(key==VK_ESCAPE)Do(Act::Suit,-1);
                return;
            }
            if(key==VK_ESCAPE){state.confirmQuit=true;Redraw();return;}
            if(t.Now()==Phase::YourTurn||t.Now()==Phase::YourDrawn)
            {
                if(key=='D'){Do(Act::Draw);return;}
                if(key=='K'){Do(Act::Keep);return;}
                if(key=='O'){Do(Act::Call);return;}
                if(key=='C'){Do(Act::Catch);return;}
                // The arrow keys go along your cards, Enter plays.
                const auto& boxes=state.cardBoxes;
                if(boxes.empty())return;
                int at=-1;
                for(size_t i=0;i<boxes.size();++i)if(boxes[i].first==state.hoverCard)at=static_cast<int>(i);
                const int n=static_cast<int>(boxes.size());
                if(key==VK_LEFT){state.hoverCard=boxes[static_cast<size_t>(at<0?n-1:(at+n-1)%n)].first;Redraw();}
                else if(key==VK_RIGHT){state.hoverCard=boxes[static_cast<size_t>(at<0?0:(at+1)%n)].first;Redraw();}
                else if((key==VK_RETURN||key==VK_SPACE)&&at>=0)PlayYours(state.hoverCard);
                return;
            }
            if(t.Now()==Phase::HandOver&&(key==VK_RETURN||key==VK_SPACE))Continue();
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
                const int at=ButtonAt(x,y);
                if(at!=state.hover){state.hover=at;Redraw();}
                int card=-1;
                for(const auto& [c,box]:state.cardBoxes)if(box.Contains(x,y))card=c;   // (the later ones lie on top)
                if(card!=state.hoverCard){state.hoverCard=card;Redraw();}
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
                if(state.help){state.help=false;Redraw();return 0;}
                if(state.helpBox.Contains(x,y)){state.help=true;Redraw();return 0;}
                if(t.Now()==Phase::GameOver){if(EndShown())Continue();return 0;}
                if(const int b=ButtonAt(x,y);b>=0&&state.buttons[static_cast<size_t>(b)].on)
                {
                    const Button k=state.buttons[static_cast<size_t>(b)];
                    Do(k.act,k.value);
                    return 0;
                }
                if(state.choosing>=0)return 0;
                if(state.stockBox.Contains(x,y)&&YourTurnNow()){Do(Act::Draw);return 0;}
                if(YourPlayNow())
                {
                    int card=-1;
                    for(const auto& [c,box]:state.cardBoxes)if(box.Contains(x,y))card=c;
                    if(card>=0){PlayYours(card);return 0;}
                }
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
                return 0;
            }
            return DefWindowProcW(hwnd,message,wParam,lParam);
        }
    }

    bool PageOneWindowOpen(){return state.window!=nullptr;}
    HWND PageOneWindowHandle(){return state.window;}
    bool PageOneWindowRect(RECT& r)
    {
        return state.window&&IsWindowVisible(state.window)&&!IsIconic(state.window)&&GetWindowRect(state.window,&r);
    }

    namespace
    {
        // Diagnostics: steps played for both (yours with her own judgment).
        void PlaySimply(int moves)
        {
            auto& t=state.table;
            for(int i=0;i<moves&&t.Now()!=Phase::GameOver;++i)t.PlayForYou();
            state.nextAt=0;
            state.hover=-1;
        }
    }

    void PageOneLines(const std::wstring& answer)
    {
        state.prefetching=false;
        if(!state.window)return;
        state.lines.Answered(answer);
        Prefetch();
    }

    bool PageOneAfterword(std::wstring& situation,std::wstring& fallback)
    {
        if(state.afterword.empty())return false;
        situation=std::move(state.afterword);
        fallback=std::move(state.afterwordLine);
        state.afterword.clear();
        state.afterwordLine.clear();
        return true;
    }

    void PageOneHeard(const std::wstring& line,bool done)
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

    void PlayPageOneMoves(int moves,float yaw)
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

        // The cards on their way since the table was last seen: those out of the stock one after
        // another in the order dealt; the rest sliding to where they go (or turning, her cards
        // shown; or back into the stock for the next hand).
        void StartFlights(const std::array<Pose,PageOne::Cards>& before,bool fresh)
        {
            const double now=state.clock;
            auto& t=state.table;
            const auto& after=state.poses;
            if(fresh){state.flights.clear();return;}
            std::array<bool,PageOne::Cards> done{};
            auto add=[&](int card,std::vector<std::pair<double,Pose>> keys,bool arc){
                done[static_cast<size_t>(card)]=true;
                state.flightsUntil=std::max(state.flightsUntil,keys.back().first);
                state.flights.erase(std::remove_if(state.flights.begin(),state.flights.end(),[&](const Flight& f){return f.card==card;}),state.flights.end());
                state.flights.push_back({card,std::move(keys),arc});
            };
            auto apart=[&](const Pose& a,const Pose& b){return std::abs(a.x-b.x)+std::abs(a.z-b.z);};
            // The order dealt: in turn from the one not dealing, then the upcard.
            std::vector<int> order;
            const auto& first=t.HandOf(1-t.Dealer());
            const auto& second=t.HandOf(t.Dealer());
            for(size_t i=0;i<first.size()||i<second.size();++i)
            {
                if(i<first.size())order.push_back(first[i]);
                if(i<second.size())order.push_back(second[i]);
            }
            if(t.Top()>=0)order.push_back(t.Top());
            const std::array<float,2> shoe=ShoeAt();
            double at=now;
            for(int c:order)
            {
                const Pose& a=before[static_cast<size_t>(c)];
                const Pose& b=after[static_cast<size_t>(c)];
                const bool fromStock=!a.shown&&std::abs(a.x-shoe[0])+std::abs(a.z-shoe[1])<0.01f*state.upper;
                if(!fromStock||apart(a,b)<0.05f*state.upper)continue;
                Pose from=a;
                from.flip=1;
                add(c,{{at,from},{at+Travel,b}},true);
                Due(at+Travel,MahjongSound::CardPlace);
                at+=DealEach*0.55;
            }
            // The rest that moved.
            double gather=now;
            int moved=0;
            for(int c=0;c<PageOne::Cards;++c)
            {
                const Pose& a=before[static_cast<size_t>(c)];
                const Pose& b=after[static_cast<size_t>(c)];
                if(done[static_cast<size_t>(c)])continue;
                const float by=apart(a,b);
                const bool turned=std::abs(a.flip-b.flip)>0.5f;
                if(by<0.002f*state.upper&&!turned)continue;
                if(!a.shown&&!b.shown&&by<0.05f*state.upper)continue;
                if(by>0.2f*state.upper)++moved;
                add(c,{{gather,a},{gather+Gather,b}},by>0.2f*state.upper);
                gather+=0.02;
            }
            if(moved>4)Due(now+Gather,MahjongSound::CardTake);
            else if(moved>0)Due(now+Gather,MahjongSound::CardPlace);
            // You went out: gold sparkles once all is down.
            if(t.Now()==Phase::HandOver&&t.Result().winner==PageOne::You)
            {
                state.sparkAt=std::max(at,gather)+0.2;
                state.sparkSpot={0,Top(),FieldZ()};
            }
        }
    }

    void TickPageOneWindow(MascotModel& model,double dt)
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
            const std::array<Pose,PageOne::Cards> before=state.poses;
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
        // The flights done: off the list, the table built again with them in place.
        state.flights.erase(std::remove_if(state.flights.begin(),state.flights.end(),[](const Flight& f){return f.keys.back().first<=state.clock;}),state.flights.end());
        if(auto scene=Scene();scene!=state.scene||state.scene.empty()){state.scene=std::move(scene);r.SetGeometry(BuildTable());}
        for(auto k=state.sounds.begin();k!=state.sounds.end();)
            if(k->first<=state.clock){PlayMahjongSound(k->second);k=state.sounds.erase(k);}else ++k;
        if(state.sparkAt>0&&state.clock>=state.sparkAt)
        {
            state.sparkAt=0;
            Sparkle(state.sparkSpot,1.2f);
        }
        // Her hand goes out to the card she is about to take, or to the discard (tried again next
        // frame while a gesture of hers ends).
        if((t.Now()==Phase::HerTurn||t.Now()==Phase::HerDrawn)&&state.entrance==5&&!Flying()&&state.plannedFor==t.Moves()&&state.reachedFor!=t.Moves()&&
           GetTickCount64()+ReachLead>=state.nextAt)
        {
            // To the pile with a card to play, to the stock to draw; no hand to keep the card drawn
            // or to point out yours.
            int named=-1;
            bool call=true;
            const bool catching=t.Now()==Phase::HerTurn&&t.HerCatches();
            const int card=catching?-1:t.HerPlay(named,call);
            const bool toPile=card>=0;
            if(catching||(!toPile&&t.Now()==Phase::HerDrawn))state.reachedFor=t.Moves();
            if(state.reachedFor!=t.Moves())
            {
                const std::array<float,2> spot=toPile?PileAt():ShoeAt();
                if(ReachFor(model,{spot[0],Top(),spot[1]}))state.reachedFor=t.Moves();
            }
        }
        if(state.thinkFor>0&&state.entrance==5){model.EmoteSeated(L"ponder",state.thinkFor);state.thinkFor=0;}
        {
            // A hand over: a clap (or her cheeks) when she went out with many points, "oops" when you did.
            if(t.Now()==Phase::HandOver&&!Flying()&&state.gestureHand!=t.HandNumber())
            {
                state.gestureHand=t.HandNumber();
                std::uniform_real_distribution<double> any(0,1);
                const auto& r=t.Result();
                if(r.winner==PageOne::You)state.endGesture=L"oops";
                else if(r.winner==PageOne::Her&&(r.points>=40||any(state.sparkRandom)<0.5))state.endGesture=any(state.sparkRandom)<0.5?L"cheeks":L"clap";
                if(!state.endGesture.empty())state.endGestureUntil=state.clock+1.5;
            }
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
                if(PoseAt(f,state.clock,p)&&state.clock>=f.keys.front().first)AddCard(moving,p,f.card);
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
            ShowRoomGlows(shown,state.roomGlows,state.clock);
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

    std::wstring DescribePageOne()
    {
        if(!state.window)return L"";
        auto& t=state.table;
        std::wstring out=Fmt(L"pageone: entrance {0}, phase {1}, hand {2}, turns {3}, score you {4} her {5}, cards you {6} her {7}, stock {8}, top {9}, suit {10}, penalty {11}, flights {12}, face {13}\n",
                             {std::to_wstring(state.entrance),std::to_wstring(static_cast<int>(t.Now())),std::to_wstring(t.HandNumber()),std::to_wstring(t.Turns()),
                              std::to_wstring(t.Score(PageOne::You)),std::to_wstring(t.Score(PageOne::Her)),std::to_wstring(t.HandOf(PageOne::You).size()),std::to_wstring(t.HandOf(PageOne::Her).size()),
                              std::to_wstring(t.Stock().size()),std::to_wstring(t.Top()),std::to_wstring(t.Suit()),std::to_wstring(t.Penalty()),std::to_wstring(state.flights.size()),std::to_wstring(static_cast<int>(state.tell))});
        return out+(state.words.told.empty()?std::wstring():L"pageone words:\n"+state.words.told);
    }

    bool SavePageOneView(const std::wstring& path)
    {
        return state.renderer&&state.renderer->SaveLastFrame(path);
    }

    bool SavePageOneShot(const std::wstring& path,int moves,std::uint32_t seed,bool help)
    {
        GdiplusStartupInput input;
        if(!state.gdiplus)GdiplusStartup(&state.gdiplus,&input,nullptr);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc=DefWindowProcW;
        wc.hInstance=GetModuleHandleW(nullptr);
        wc.lpszClassName=L"MikuMikuDesktopPerchPageOneShot";
        RegisterClassExW(&wc);
        const int dpi=static_cast<int>(GetDpiForSystem());
        state.window=CreateWindowExW(0,wc.lpszClassName,L"",WS_OVERLAPPEDWINDOW,0,0,MulDiv(static_cast<int>(BaseWidth)+16,dpi,96),MulDiv(static_cast<int>(BaseHeight)+39,dpi,96),
                                     nullptr,nullptr,wc.hInstance,nullptr);
        state.her=PageOneOpponent{L"ミク"};
        state.table.NewGame(seed);
        PlaySimply(moves);
        state.help=help;
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

    void ShowPageOneWindow(HWND owner,const PageOneOpponent& her,std::function<void()> onEnd)
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
        state.gestureHand=0;
        state.lost=0;
        state.lines.Load(L"PAGEONELINES",L"pageone",her.ownLinesFile,her.linesFile,her.styleKey,English(),
                         {L"トランプのページワン",L"Page One (a card game)",L"",L""});
        state.voice.Stop();
        state.prefetching=false;
        state.tell=Expression::Neutral;
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
        const std::wstring title=std::wstring(T(L"ページワン",L"Page One"))+L" - "+HerName();
        state.window=CreateWindowExW(0,ClassName,title.c_str(),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                                     work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,
                                     nullptr,nullptr,instance,nullptr);
        if(CreateView())PlaceView();
        SetTimer(state.window,TimerId,40,nullptr);
        ShowWindow(state.window,SW_SHOWNORMAL);
        SetForegroundWindow(state.window);
    }
}
