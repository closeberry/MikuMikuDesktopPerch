#define NOMINMAX
#include "PokerWindow.hpp"
#include "Poker.hpp"
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
        using Poker::Phase;
        constexpr wchar_t ClassName[]=L"MikuMikuDesktopPerchPoker";
        constexpr wchar_t ViewClassName[]=L"MikuMikuDesktopPerchPokerView";
        constexpr UINT_PTR TimerId=1;
        // Her pace: before each step of hers (in 2D, and at the 3D table where her hand goes out
        // first), after a move of yours.
        constexpr ULONGLONG HerPlayWait=650,HerPlayWait3D=1000,YourWait=250;
        constexpr ULONGLONG SaidShown=1600;
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
        enum class Act {Fold,Call,Raise,Next};
        struct Button {Act act;int value;std::wstring label;RectF box;bool on=true;};

        // How carefully she plays (Poker::Table::NewGame): a little less as you grow closer,
        // and after games you lost.
        constexpr double Care=0.85,LessPerBond=0.03,LessPerLoss=0.08,LeastCare=0.4;
        // How often the look on her face is put on, the other way about from what she holds.
        constexpr double PutOnShare=0.3;
        constexpr int BigPot=500;       // a pot this large is talked of as a big one

        struct State
        {
            HWND window=nullptr;
            ULONG_PTR gdiplus=0;
            PokerOpponent her;
            std::unique_ptr<Bitmap> face;
            std::function<void()> onEnd;
            Poker::Table table;
            int lost=0;                // games you lost in a row
            bool gameOverTold=false;
            bool confirmQuit=false;
            bool help=false;           // the list of hands shown
            RectF helpBox;
            RectF quitButtons[2];
            std::vector<Button> buttons;
            int hover=-1;              // the button under the mouse
            ULONGLONG nextAt=0;
            std::wstring said;         // the blinds going up, in the big bubble
            ULONGLONG saidAt=0;
            int blindSeen=0;
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
            // Her lines by scene (res/poker/lines.txt, said again her way by the LLM, and the
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
            std::array<Pose,Poker::Cards> poses{};
            // The cards on their way, and until when (on the clock); sounds due on the way.
            std::vector<Flight> flights;
            double flightsUntil=0;
            std::vector<std::pair<double,MahjongSound>> sounds;
            int movesSeen=-1;
            double sparkAt=0;         // gold sparkles then, over a poker of yours
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
        std::wstring Chips(int n){return std::to_wstring(n)+T(L"枚",L" chips");}
        std::wstring Signed(int n){return (n>0?L"+":L"")+std::to_wstring(n);}

        bool Waiting(){return state.nextAt>GetTickCount64();}
        // The cards still on their way at the 3D table (a move not yet shown counts).
        bool Flying()
        {
            return state.renderer&&(state.movesSeen!=state.table.Moves()||state.clock<state.flightsUntil);
        }
        bool Ready(){return !Waiting()&&!Flying();}
        bool YourTurnNow(){return state.table.Now()==Phase::YourTurn&&Ready();}
        bool HandShown(){return state.table.Now()==Phase::HandOver&&Ready();}
        bool EndShown(){return state.table.Now()==Phase::GameOver&&Ready();}

        const wchar_t* KindName(int kind)
        {
            switch(kind)
            {
            case 0:return T(L"ハイカード",L"High card");
            case 1:return T(L"ワンペア",L"One pair");
            case 2:return T(L"ツーペア",L"Two pair");
            case 3:return T(L"スリーカード",L"Three of a kind");
            case 4:return T(L"ストレート",L"Straight");
            case 5:return T(L"フラッシュ",L"Flush");
            case 6:return T(L"フルハウス",L"Full house");
            case 7:return T(L"フォーカード",L"Four of a kind");
            default:return T(L"ストレートフラッシュ",L"Straight flush");
            }
        }

        // The hand who holds with the board as it is now ("" with no cards yet); with two cards
        // alone, only whether they are a pair.
        std::wstring HandName(int who)
        {
            const auto& hole=state.table.Holes()[who];
            if(hole.size()<2)return {};
            std::vector<int> all(hole);
            all.insert(all.end(),state.table.Board().begin(),state.table.Board().end());
            if(all.size()<5)return Poker::RankOf(hole[0])==Poker::RankOf(hole[1])?KindName(1):KindName(0);
            return KindName(Poker::KindOf(Poker::Score(all.data(),static_cast<int>(all.size()))));
        }

        std::wstring DidText(int who)
        {
            const auto& t=state.table;
            const std::wstring amount=std::to_wstring(t.LastAmount(who));
            switch(t.LastDid(who))
            {
            case Poker::Did::Check:return T(L"チェック",L"Check");
            case Poker::Did::Call:return std::wstring(T(L"コール ",L"Call "))+amount;
            case Poker::Did::Raise:return Fmt(T(L"{0}まで賭ける",L"Bets to {0}"),{std::to_wstring(t.BetOf(who))});
            case Poker::Did::AllIn:return T(L"オールイン",L"All in");
            case Poker::Did::Fold:return T(L"フォールド",L"Fold");
            case Poker::Did::Blind:return Fmt(T(L"ブラインド {0}",L"Blind {0}"),{std::to_wstring(t.BetOf(who))});
            default:return {};
            }
        }

        // The raises offered: least, half the pot, the pot, all in.
        int RaiseChoice(int i)
        {
            auto& t=state.table;
            switch(i)
            {
            case 0:return t.MinRaiseTo(Poker::You);
            case 1:return t.RaiseToShare(Poker::You,0.5);
            case 2:return t.RaiseToShare(Poker::You,1);
            default:return t.MaxRaiseTo(Poker::You);
            }
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
                Log(L"poker: resize: %s",error.c_str());
            InvalidateRect(state.window,nullptr,FALSE);
        }

        void ReleaseView()
        {
            if(state.renderer){state.renderer->WaitIdle();state.renderer.reset();Log(L"poker: 3D view released");}
            state.view=nullptr;
        }

        // The cards' faces (res/poker/cards.jpg): CardSide by CardTop each, eight across,
        // card n at column n%8, row n/8 (the back at Poker::Back).
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
            if(!faceUp||card<0)card=Poker::Back;
            if(Bitmap* face=Face(card,static_cast<int>(std::lround(w)),static_cast<int>(std::lround(h))))
                g.DrawImage(face,RectF(x,y,static_cast<REAL>(face->GetWidth()),static_cast<REAL>(face->GetHeight())));
            else
            {
                FillRound(g,RectF(x,y,w,h),r,Color(255,242,233,208),Color(255,30,26,24),2);
                Text(g,std::to_wstring(Poker::RankOf(card)),RectF(x,y,w,h),h*0.2f,Color(255,30,30,36),true,StringAlignmentCenter,StringAlignmentCenter);
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
                Log(L"poker: no 3D view (%s)",error.c_str());
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
            Log(L"poker: 3D view %ux%u on %s (hips %.1f, upper body %.1f, floor %.1f)",w,h,renderer->AdapterName().c_str(),state.hips,state.upper,state.floorY);
            {
                std::vector<MascotRenderer::GeometryVertex> room;
                BuildWesternRoom(room,state.roomGlows,{state.upper,state.hips,state.floorY,0.12f*state.hips+0.30f*state.upper,FieldZ()});
                if(!renderer->SetRoomGeometry(room))Log(L"poker: no room");
            }
            if(!renderer->SetGeometryTexture(CardsImage()))Log(L"poker: no card faces on the 3D table");
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

        // ---- The cards and the chips on the table ----
        // Her two cards before her, yours before you, the board between them, the deck at her
        // left (your right); each one's chips and bet beside their cards, the pot beside the board.

        float Top(){return 0.12f*state.hips+0.30f*state.upper;}
        float FieldZ(){return -1.10f*state.upper;}
        float Thick(){return 0.003f*state.upper;}
        float CardW(){return 0.149f*state.upper;}
        float CardL(){return 0.275f*state.upper;}
        float BoardStep(){return 0.168f*state.upper;}
        float HoleStep(){return 0.160f*state.upper;}
        std::array<float,2> ShoeAt(){return {0.92f*state.upper,-0.78f*state.upper};}
        float HerZ(){return -0.72f*state.upper;}
        float YourZ(){return -1.48f*state.upper;}
        float ChipR(){return 0.042f*state.upper;}
        float ChipH(){return 0.008f*state.upper;}

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

        // Where every card is, as the table is now (state.poses): those in the deck at its top,
        // face down and not shown (the deck is drawn as one).
        void PlaceCards()
        {
            auto& t=state.table;
            {
                Pose top=Lying(ShoeAt()[0],ShoeAt()[1],false,std::max(0,t.DeckLeft()-1)*Thick());
                top.shown=false;
                for(auto& p:state.poses)p=top;
            }
            const auto& holes=t.Holes();
            for(size_t i=0;i<holes[Poker::Her].size();++i)
                state.poses[static_cast<size_t>(holes[Poker::Her][i])]=Lying((i-0.5f)*HoleStep(),HerZ(),t.HerCardsShown());
            for(size_t i=0;i<holes[Poker::You].size();++i)
                state.poses[static_cast<size_t>(holes[Poker::You][i])]=Lying((i-0.5f)*HoleStep(),YourZ(),true);
            const auto& board=t.Board();
            for(size_t i=0;i<board.size();++i)
                state.poses[static_cast<size_t>(board[i])]=Lying((i-2.0f)*BoardStep(),FieldZ(),true);
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
            CardUv(Poker::Back,back);
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

        // A stack of count chips of value at (x, z) on the table: each a disc of its colour with
        // white flecks round its edge.
        void AddChips(Mesh& m,float x,float z,int value,int count)
        {
            constexpr int N=18;
            const float r=ChipR(),h=ChipH();
            float c[4]={0.70f,0.12f,0.10f,1};
            if(value==20){c[0]=0.12f;c[1]=0.50f;c[2]=0.22f;}
            else if(value==50){c[0]=0.14f;c[1]=0.26f;c[2]=0.62f;}
            else if(value>=100){c[0]=0.10f;c[1]=0.10f;c[2]=0.11f;}
            else if(value==0){c[0]=0.92f;c[1]=0.90f;c[2]=0.84f;}   // the dealer's button
            const float fleck[4]={0.95f,0.94f,0.90f,1};
            const float top=Top();
            for(int k=0;k<count;++k)
            {
                const float y0=top+k*h,y1=y0+h*0.92f;
                // Each a little off the one below, as a stack is.
                const float ox=x+0.0025f*state.upper*std::sin(k*2.1f),oz=z+0.0025f*state.upper*std::cos(k*1.7f);
                for(int i=0;i<N;++i)
                {
                    const float a0=i*DirectX::XM_2PI/N,a1=(i+1)*DirectX::XM_2PI/N,am=(a0+a1)/2;
                    const float c0=std::cos(a0)*r,s0=std::sin(a0)*r,c1=std::cos(a1)*r,s1=std::sin(a1)*r;
                    m.Quad({ox+c0,y0,oz+s0},{ox+c1,y0,oz+s1},{ox+c1,y1,oz+s1},{ox+c0,y1,oz+s0},{std::cos(am),0,std::sin(am)},i%3==0?fleck:c);
                    m.Quad({ox,y1,oz},{ox+c0,y1,oz+s0},{ox+c1,y1,oz+s1},{ox,y1,oz},{0,1,0},c);
                }
                // A white ring on the top one.
                if(k==count-1)
                    for(int i=0;i<N;++i)
                    {
                        const float a0=i*DirectX::XM_2PI/N,a1=(i+1)*DirectX::XM_2PI/N,y=y1+0.0004f*state.upper;
                        const float r0=0.62f*r,r1=0.74f*r;
                        m.Quad({ox+std::cos(a0)*r0,y,oz+std::sin(a0)*r0},{ox+std::cos(a1)*r0,y,oz+std::sin(a1)*r0},{ox+std::cos(a1)*r1,y,oz+std::sin(a1)*r1},{ox+std::cos(a0)*r1,y,oz+std::sin(a0)*r1},{0,1,0},fleck);
                    }
            }
        }

        // An amount laid out as chips (the largest first), stacks side by side from (x, z).
        void AddAmount(Mesh& m,float x,float z,int amount,int perStack=10)
        {
            float at=x;
            for(int value:{100,50,20,10})
            {
                int n=amount/value;
                amount-=n*value;
                while(n>0)
                {
                    const int k=std::min(n,perStack);
                    AddChips(m,at,z,value,k);
                    at+=2.3f*ChipR();
                    n-=k;
                }
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
            out.push_back(t.Chips(Poker::You));
            out.push_back(t.Chips(Poker::Her));
            out.push_back(t.BetOf(Poker::You));
            out.push_back(t.BetOf(Poker::Her));
            out.push_back(t.Collected());
            out.push_back(t.Button());
            out.push_back(t.HerCardsShown()?1:0);
            out.push_back(Ready()?1:0);
            for(const auto& f:state.flights)out.push_back(1000+f.card);
            return out;
        }

        // The five that won at a showdown (both hands' when they tied).
        std::vector<int> WinningCards()
        {
            auto& t=state.table;
            std::vector<int> out;
            if(t.Now()!=Phase::HandOver||t.ByFold())return out;
            for(int who:{Poker::You,Poker::Her})
                if(t.Winner()==who||t.Winner()<0)
                {
                    std::vector<int> all(t.Holes()[who]);
                    all.insert(all.end(),t.Board().begin(),t.Board().end());
                    const auto five=Poker::BestFive(all);
                    out.insert(out.end(),five.begin(),five.end());
                }
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
            // The deck: a stack of backs in a dark wooden holder.
            {
                const float wood[4]={0.22f,0.12f,0.06f,Wood},white[4]={1,1,1,1};
                const float hw=CardW()/2,hl=CardL()/2,h=std::max(1,table.DeckLeft())*Thick(),x=ShoeAt()[0],z=ShoeAt()[1];
                mesh.grain=1;mesh.grainScale=0.35f;
                mesh.Box(x-hw-0.02f*D,t.top,z-hl-0.02f*D,x+hw+0.02f*D,t.top+0.012f*D,z+hl+0.02f*D,wood);
                mesh.Box(x-hw-0.02f*D,t.top,z+hl,x+hw+0.02f*D,t.top+h+0.03f*D,z+hl+0.02f*D,wood);
                const float core[4]={0.92f,0.89f,0.82f,1};
                mesh.Box(x-hw,t.top+0.012f*D,z-hl,x+hw,t.top+0.012f*D+h,z+hl,core,false);
                float uv[8];
                CardUv(Poker::Back,uv);
                const float y=t.top+0.012f*D+h;
                mesh.Quad({x-hw,y,z+hl},{x+hw,y,z+hl},{x+hw,y,z-hl},{x-hw,y,z-hl},{0,1,0},white,uv);
            }
            // The cards on the table (those on their way are drawn where they are going).
            if(state.movesSeen==table.Moves())
                for(int c=0;c<Poker::Cards;++c)
                {
                    const Pose& p=state.poses[static_cast<size_t>(c)];
                    if(p.shown&&!InFlight(c))AddCard(mesh,p,c);
                }
            // A gold rim round the five that won, once they are all turned.
            if(HandShown())
                for(int c:WinningCards())
                {
                    const float gold[4]={1.0f,0.80f,0.32f,1};
                    const Pose& p=state.poses[static_cast<size_t>(c)];
                    const float m=0.011f*D,y=Top()+0.0006f*D,hw=p.w/2+m,hl=p.l/2+m;
                    mesh.Quad({p.x-hw,y,p.z+hl},{p.x+hw,y,p.z+hl},{p.x+hw,y,p.z-hl},{p.x-hw,y,p.z-hl},{0,1,0},gold);
                }
            // The chips: each one's before them, what they bet nearer the middle, the pot by the board.
            AddAmount(mesh,0.46f*D,-1.62f*D,table.Chips(Poker::You),12);
            AddAmount(mesh,-0.92f*D,-0.60f*D,table.Chips(Poker::Her),12);
            AddAmount(mesh,-0.56f*D,-1.36f*D,table.BetOf(Poker::You));
            AddAmount(mesh,0.30f*D,-0.80f*D,table.BetOf(Poker::Her));
            AddAmount(mesh,-0.86f*D,FieldZ(),table.Collected());
            // The dealer's button, by whoever has it.
            if(table.Now()!=Phase::GameOver)
            {
                if(table.Button()==Poker::You)AddChips(mesh,0.26f*D,-1.34f*D,0,1);
                else AddChips(mesh,-0.26f*D,-0.64f*D,0,1);
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
                state.afterword=Fmt(T(L"いま、話し相手とポーカーをしていたが、途中でやめてデスクトップに戻ってきたところ（{0}回目、話し相手のチップは{1}枚、あなたは{2}枚だった）。そのことを話し相手にひと言（1〜2文）",
                                      L"You were playing poker with them, but they stopped midway and you have come back to the desktop (hand {0}; they had {1} chips, you {2}). Say a word about it to them (one or two sentences)"),
                                    {std::to_wstring(t.Hand()),std::to_wstring(t.Chips(Poker::You)),std::to_wstring(t.Chips(Poker::Her))});
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
            state.blindSeen=state.table.SmallBlind();
            state.hover=-1;
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
            if(HandShown()){t.NextHand();Moved();}
            else if(EndShown())NewGame();
            Redraw();
        }

        void Do(Act act,int value)
        {
            auto& t=state.table;
            if(act==Act::Next){Continue();return;}
            if(!YourTurnNow())return;
            switch(act)
            {
            case Act::Fold:if(t.ToCall(Poker::You)>0){t.Fold();Moved();}return;
            case Act::Call:t.CheckCall();Moved();break;
            case Act::Raise:
                if(!t.CanRaise(Poker::You))return;
                t.RaiseTo(value);
                Moved();
                break;
            default:return;
            }
            if(t.LastDid(Poker::You)==Poker::Did::AllIn&&t.Now()!=Phase::HandOver&&act==Act::Raise)Say(L"相手のオールイン",4);
            else if(act==Act::Raise)Say(L"相手のレイズ",3,0.35);
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
                state.her.onEvent(seen+Fmt(T(L"（{0}回目、持ち主のチップは{1}枚）",L" (hand {0}; the owner has {1} chips)"),{std::to_wstring(t.Hand()),std::to_wstring(t.Chips(Poker::You))}),weight,Pick(theirs));
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

        // The look on her face. Through a hand it comes from how good her cards look to her, a
        // smile on a strong hand and a worried look on a weak one, or (a street at a time, now and
        // then) the other way about, put on; a bet on little more often behind a smile.
        void UpdateTell()
        {
            auto& t=state.table;
            Expression e=Expression::Neutral;
            if(t.Now()==Phase::HandOver)
            {
                if(!Flying()&&t.Winner()>=0&&!(t.ByFold()&&t.Winner()==Poker::You))e=t.Winner()==Poker::Her?Expression::Smile:Expression::Sad;
            }
            else if(t.Now()==Phase::GameOver)e=t.GameGain()<0?Expression::Smile:t.GameGain()>0?Expression::Sad:Expression::Neutral;
            else if(!t.Holes()[Poker::Her].empty())
            {
                const int key=t.Hand()*20+static_cast<int>(t.Board().size())*2+(t.Bluffing()?1:0);
                if(state.tellFor!=key)
                {
                    state.tellFor=key;
                    std::uniform_real_distribution<double> any(0,1);
                    const double strength=t.HerStrength();
                    Expression look=strength>=0.68?Expression::Smile:strength<=0.42?Expression::Troubled:Expression::Neutral;
                    if(any(state.sparkRandom)<PutOnShare)look=look==Expression::Smile?Expression::Troubled:look==Expression::Troubled?Expression::Smile:look;
                    if(t.Bluffing()&&any(state.sparkRandom)<0.5)look=Expression::Smile;
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
                Happened(6,T(L"これから話し相手とポーカー（テキサスホールデム）の対戦が始まる。",L"A game of poker (Texas hold'em) with them is about to begin. "),
                         T(L"わくわくする気持ち",L"how you look forward to it"),L"始まり",
                         Fmt(T(L"{0}とこのパソコンの持ち主のポーカーが始まる。",L"{0} and the owner of this computer are starting a game of poker."),{name}),
                         {T(L"がんばれー！",L"Good luck!"),T(L"どっちが勝つかな？",L"Who will win?")});
            }
            // A hand over: a word on how.
            if(t.Now()==Phase::HandOver&&seen.hand!=t.Hand())
            {
                seen.hand=t.Hand();
                const int won=t.Won();
                const std::wstring pot=std::to_wstring(won);
                if(t.ByFold())
                {
                    if(t.Winner()==Poker::Her)
                        Happened(3,Fmt(T(L"ポーカーで話し相手が降りて、あなたがチップ{0}枚の山を取った。",L"At poker, they folded and you took the pot of {0}."),{pot}),
                                 T(L"うれしい気持ち",L"how pleased you are"),t.Bluffing()?L"ブラフ成功":L"相手が降りた",L"",{});
                    else
                        Happened(3,Fmt(T(L"ポーカーであなたが降りて、話し相手がチップ{0}枚の山を取った。",L"At poker, you folded and they took the pot of {0}."),{pot}),
                                 T(L"くやしい気持ち",L"how you feel about it"),nullptr,L"",{});
                }
                else if(t.Winner()<0)
                    Happened(3,T(L"ポーカーでカードを見せあって、同じ強さで山を分けた。",L"At poker, the hands tied and the pot was split."),
                             T(L"驚く気持ち",L"your surprise"),L"分けた",L"",{});
                else if(t.Winner()==Poker::Her)
                    Happened(won>=BigPot?6:3,Fmt(T(L"ポーカーでカードを見せあって、あなたの{0}が話し相手の{1}に勝ち、チップ{2}枚の山を取った。",L"At poker, your {0} beat their {1} at the showdown, a pot of {2}."),{HandName(Poker::Her),HandName(Poker::You),pot}),
                             T(L"うれしい気持ち",L"how pleased you are"),won>=BigPot?L"大きく勝った":L"勝負に勝った",
                             Fmt(T(L"ポーカーで{0}が{1}でチップ{2}枚の山を取った。",L"At poker, {0} took a pot of {2} with {1}."),{name,HandName(Poker::Her),pot}),
                             {Fmt(T(L"{0}の勝ちだ！",L"{0} takes it!"),{name})});
                else
                    Happened(won>=BigPot?6:3,Fmt(T(L"ポーカーでカードを見せあって、話し相手の{0}があなたの{1}に勝ち、チップ{2}枚の山を取られた。",L"At poker, their {0} beat your {1} at the showdown, a pot of {2}."),{HandName(Poker::You),HandName(Poker::Her),pot}),
                             T(L"くやしい気持ち",L"how annoyed you are"),won>=BigPot?L"大きく負けた":L"勝負に負けた",
                             Fmt(T(L"ポーカーで持ち主が{0}でチップ{1}枚の山を取った（相手は{2}）。",L"At poker, the owner took a pot of {1} with {0} (against {2})."),{HandName(Poker::You),pot,name}),
                             {T(L"おお、取った！",L"Ooh, they took it!")});
            }
            const bool over=t.Now()==Phase::GameOver;
            if(over&&!seen.over)
            {
                const int gain=t.GameGain();
                // How she goes easier next game comes from how this one ended.
                state.lost=gain<0?state.lost+1:0;
                const std::wstring result=Fmt(T(L"話し相手のチップは{0}枚、あなたは{1}枚",L"they have {0} chips, you {1}"),{std::to_wstring(t.Chips(Poker::You)),std::to_wstring(t.Chips(Poker::Her))});
                state.afterword=Fmt(T(L"いま、話し相手とポーカーをして、デスクトップに戻ってきたところ。結果は{0}。その勝負の感想を、話し相手にひと言（1〜2文）",
                                      L"You have just played poker with them and come back to the desktop. In the end {0}. Tell them what you thought of the game (one or two sentences)"),{result});
                state.afterwordLine=gain>0?T(L"ポーカー、強かったね！次は負けないよ。",L"You were good at that! I'll win next time."):T(L"ポーカー、楽しかったね！またやろう。",L"That was fun! Let's play again sometime.");
                if(gain>0)Happened(9,Fmt(T(L"ポーカーが終わって、話し相手の勝ち（{0}）。",L"The poker game is over and they won ({0})."),{result}),
                                   T(L"悔しい気持ちか、話し相手をほめる気持ち",L"how sorry you are, or praise them"),L"負けて終わる",
                                   Fmt(T(L"{0}と持ち主のポーカーが終わり、持ち主が勝った。",L"The poker game between {0} and the owner is over: the owner won."),{name}),
                                   {T(L"おめでとう！",L"Congratulations!"),T(L"おつかれさま！",L"Well played!")});
                else if(gain<0)Happened(9,Fmt(T(L"ポーカーが終わって、あなたの勝ち（{0}）。",L"The poker game is over and you won ({0})."),{result}),
                              T(L"うれしい気持ち",L"how pleased you are"),L"勝って終わる",
                              Fmt(T(L"{0}と持ち主のポーカーが終わり、{0}が勝った。",L"The poker game between {0} and the owner is over: {0} won."),{name}),
                              {T(L"おつかれさま！いい勝負だったね。",L"Well played, both of you!"),Fmt(T(L"{0}の勝ちだ！",L"{0} wins!"),{name})});
                else Happened(9,Fmt(T(L"ポーカーが終わって、引き分け（{0}）。",L"The poker game is over, even ({0})."),{result}),
                              T(L"驚く気持ち",L"your surprise"),L"引き分けで終わる",
                              Fmt(T(L"{0}と持ち主のポーカーが終わり、引き分けだった。",L"The poker game between {0} and the owner ended even."),{name}),
                              {T(L"おつかれさま！",L"Well played!")});
            }
            seen.over=over;
        }

        // Her turn or cards to come: time for what comes next (at the 3D table once she is seated).
        void Plan(ULONGLONG now)
        {
            auto& t=state.table;
            state.plannedFor=t.Moves();
            std::uniform_int_distribution<int> extra(0,700);
            ULONGLONG wait=0;
            if(t.Now()==Phase::Dealing)wait=t.Holes()[0].empty()?HandWait:t.HerCardsShown()?RunOutWait:StreetWait;
            else
            {
                const auto& order=t.HerOrder();
                wait=(state.renderer?HerPlayWait3D:HerPlayWait)+extra(state.sparkRandom);
                // A big price: she takes her time over it.
                const int call=t.ToCall(Poker::Her);
                if(call>0&&call*2>=t.Pot()&&call>=t.BigBlind()*3)
                {
                    wait+=1500;
                    state.thinkFor=1.3;
                    Say(L"考える",2,0.6);
                }
                else if(order.did==Poker::Did::Check&&!state.words.waiting&&GetTickCount64()-state.words.since>WordsHold)
                    Say(L"チェック",1,0.12);
            }
            state.nextAt=std::max(state.nextAt,now+wait);
        }

        void HerActed(Poker::Did did)
        {
            auto& t=state.table;
            if(did==Poker::Did::Fold)Say(L"降りる",3);
            else if(did==Poker::Did::AllIn&&t.LastAmount(Poker::Her)>0&&t.ToCall(Poker::You)>0)Say(L"オールイン",4);
            else if(did==Poker::Did::Raise)Say(L"レイズ",2,0.4);
            else if(did==Poker::Did::Call||did==Poker::Did::AllIn)Say(L"コール",2,0.25);
        }

        void Tick()
        {
            auto& t=state.table;
            const ULONGLONG now=GetTickCount64();
            if(Flying())state.nextAt=std::max(state.nextAt,now+120);   // play waits for the cards
            if(t.SmallBlind()!=state.blindSeen)
            {
                state.blindSeen=t.SmallBlind();
                state.said=Fmt(T(L"ブラインドが {0}/{1} に上がりました",L"The blinds go up to {0}/{1}"),{std::to_wstring(t.SmallBlind()),std::to_wstring(t.BigBlind())});
                state.saidAt=now;
                Redraw();
            }
            if(!state.said.empty()&&now-state.saidAt>SaidShown){state.said.clear();Redraw();}
            const bool hers=t.Now()==Phase::HerTurn||t.Now()==Phase::Dealing;
            if(hers&&state.plannedFor!=t.Moves()&&!Flying()&&(!state.renderer||state.entrance==5))Plan(now);
            if(hers&&state.plannedFor==t.Moves()&&now>=state.nextAt&&(!state.renderer||state.entrance==5))
            {
                if(t.Now()==Phase::HerTurn)
                {
                    const Poker::Did did=t.HerOrder().did;
                    t.Step();
                    HerActed(did);
                }
                else t.Step();
                state.nextAt=now+150;
            }
            // A deal now and then gets a word.
            if(!t.Holes()[0].empty()&&t.Board().empty()&&state.seen.dealt!=t.Hand()&&!Flying())
            {
                state.seen.dealt=t.Hand();
                if(!state.words.waiting&&now-state.words.since>WordsHold)Say(L"配る",2,0.25);
            }
            UpdateTell();
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
            const bool outOfChips=t.Chips(Poker::You)==0||t.Chips(Poker::Her)==0;
            Text(g,outOfChips?T(L"勝負あり",L"Game over"):Fmt(T(L"{0}回終わりました",L"{0} hands played"),{std::to_wstring(t.Hand())}).c_str(),RectF(box.X,y,w,44*s),32*s,gold,true,StringAlignmentCenter,StringAlignmentNear,true);
            y+=66*s;
            const int gain=t.GameGain();
            Text(g,Fmt(T(L"チップ {0}枚（{1}）",L"Chips {0} ({1})"),{std::to_wstring(t.Chips(Poker::You)),Signed(gain)}),RectF(box.X,y,w,40*s),26*s,white,false,StringAlignmentCenter);
            y+=82*s;
            if(gain>0)Runs(g,{{T(L"あなたの勝ち！",L"You win!")}},RectF(box.X,y,w,48*s),32*s,gold);
            else if(gain<0)Runs(g,{{HerName()},{T(L"の勝ち",L" wins")}},RectF(box.X,y,w,48*s),32*s,gold);
            else Runs(g,{{T(L"引き分け",L"Even")}},RectF(box.X,y,w,48*s),32*s,gold);
            Text(g,T(L"Enter：もう一度　Esc：閉じる",L"Enter: play again   Esc: close"),RectF(box.X,box.GetBottom()-44*s,w,30*s),17*s,soft,false,StringAlignmentCenter);
        }

        // The list of hands, the best first.
        void PaintHelp(Graphics& g,float W,float H,float s)
        {
            SolidBrush shade(Color(150,0,0,0));
            g.FillRectangle(&shade,0.0f,0.0f,W,H);
            const float w=std::min(W-40*s,680*s),h=std::min(H-40*s,540*s);
            const RectF box((W-w)/2,(H-h)/2,w,h);
            FillRound(g,box,14*s,Color(250,40,22,22),Color(255,200,170,90),2*s);
            const Color gold(255,240,206,120),white(255,240,240,236),soft(255,200,190,186);
            Text(g,T(L"役の一覧（強い順）",L"The hands, best first"),RectF(box.X,box.Y+14*s,w,36*s),24*s,gold,true,StringAlignmentCenter);
            const wchar_t* about[9]={T(L"同じマークで5枚続く",L"five in a row, one suit"),T(L"同じ数が4枚",L"four of a rank"),T(L"同じ数3枚と、同じ数2枚",L"three of a rank and a pair"),
                                     T(L"同じマークが5枚",L"five of one suit"),T(L"数が5枚続く（Aは上にも下にも）",L"five in a row (ace high or low)"),T(L"同じ数が3枚",L"three of a rank"),
                                     T(L"同じ数2枚が2組",L"two pairs"),T(L"同じ数が2枚",L"a pair"),T(L"どれでもない（高いカードで比べる）",L"none of these: the highest cards count")};
            float y=box.Y+60*s;
            for(int i=0;i<9;++i)
            {
                Text(g,KindName(8-i),RectF(box.X+28*s,y,230*s,30*s),18*s,white,true);
                Text(g,about[i],RectF(box.X+262*s,y+1*s,w-280*s,30*s),16*s,soft);
                y+=36*s;
            }
            Text(g,T(L"自分の2枚と場の5枚の7枚から、いちばん強い5枚で比べます",L"Your two cards and the five on the table: the best five of the seven count"),RectF(box.X,y+6*s,w,26*s),15*s,gold,false,StringAlignmentCenter);
            Text(g,T(L"Tab：閉じる",L"Tab: close"),RectF(box.X,box.GetBottom()-34*s,w,26*s),15*s,soft,false,StringAlignmentCenter);
        }

        // The buttons for now: Fold, Check or Call and the raises in your turn; Next when a hand
        // is over.
        void LayButtons(Graphics& g,float W,float H,float s)
        {
            auto& t=state.table;
            std::vector<Button> b;
            if(t.Now()==Phase::YourTurn)
            {
                const int call=t.ToCall(Poker::You);
                const bool ready=YourTurnNow(),raise=t.CanRaise(Poker::You);
                b.push_back({Act::Fold,0,T(L"降りる（F）",L"Fold (F)"),RectF(),ready&&call>0});
                b.push_back({Act::Call,0,call>0?Fmt(T(L"コール {0}（C）",L"Call {0} (C)"),{std::to_wstring(call)}):std::wstring(T(L"チェック（C）",L"Check (C)")),RectF(),ready});
                const wchar_t* names[4]={T(L"最小",L"Min"),T(L"半ポット",L"1/2 pot"),T(L"ポット",L"Pot"),T(L"オールイン",L"All in")};
                for(int i=0;i<4;++i)
                {
                    const int total=raise?RaiseChoice(i):0;
                    const std::wstring amount=i<3&&raise?L" "+std::to_wstring(total):L"";
                    b.push_back({Act::Raise,total,std::wstring(names[i])+amount+T(L"（",L" (")+std::to_wstring(i+1)+T(L"）",L")"),RectF(),ready&&raise});
                }
            }
            else if(HandShown())b.push_back({Act::Next,0,T(L"次へ（Enter）",L"Next (Enter)"),RectF(),true});
            float total=0;
            std::vector<float> widths;
            for(const auto& k:b)
            {
                const float bw=std::max(86*s,TextWidth(g,k.label,16*s,true)+22*s);
                widths.push_back(bw);
                total+=bw+8*s;
            }
            float x=std::max(W/2-(total-8*s)/2,286*s);   // clear of your panel
            const float y=H-74*s;
            for(size_t i=0;i<b.size();++i)
            {
                Button& k=b[i];
                k.box=RectF(x,y,widths[i],46*s);
                x+=widths[i]+8*s;
                const bool lit=k.on&&static_cast<int>(i)==state.hover;
                Color fill=!k.on?Color(170,60,50,46):lit?Color(250,250,236,200):Color(240,236,226,196);
                if(k.act==Act::Next||k.act==Act::Call)fill=!k.on?Color(170,60,50,46):lit?Color(250,226,90,70):Color(245,206,60,50);
                FillRound(g,k.box,10*s,fill,Color(255,90,60,30),1.5f*s);
                const Color ink=!k.on?Color(255,150,140,130):(k.act==Act::Next||k.act==Act::Call)?Color(255,255,250,240):Color(255,40,30,20);
                Text(g,k.label,k.box,16*s,ink,true,StringAlignmentCenter,StringAlignmentCenter);
            }
            state.buttons=std::move(b);
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
            const bool settled=t.Now()==Phase::HandOver||t.Now()==Phase::GameOver;

            // Her: name, chips, what she last did.
            const float nameX=hud?28*s:112*s,nameY=hud?20*s:14*s;
            if(hud)FillRound(g,RectF(14*s,12*s,260*s,92*s),10*s,panel,edge,1.2f*s);
            else
            {
                const RectF faceBox(16*s,14*s,84*s,84*s);
                FillRound(g,faceBox,10*s,Color(255,14,50,36),Color(255,120,160,130),1.5f*s);
                if(state.face)g.DrawImage(state.face.get(),faceBox.X+3*s,faceBox.Y+3*s,faceBox.Width-6*s,faceBox.Height-6*s);
                else Text(g,HerName().substr(0,1),faceBox,40*s,white,true,StringAlignmentCenter,StringAlignmentCenter);
            }
            Text(g,HerName()+(t.Button()==Poker::Her?L" (D)":L""),RectF(nameX,nameY,240*s,28*s),20*s,white,true);
            Text(g,T(L"チップ ",L"Chips ")+std::to_wstring(t.Chips(Poker::Her)),RectF(nameX,nameY+30*s,120*s,24*s),17*s,gold);
            if(!Flying()&&!settled)Text(g,DidText(Poker::Her),RectF(nameX,nameY+54*s,230*s,24*s),16*s,soft);
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

            // The hand, the blinds and the pot (at the top right), and the list of hands.
            {
                const RectF info(W-254*s,12*s,240*s,92*s);
                FillRound(g,info,10*s,Color(hud?215:200,40,18,16),edge,1.2f*s);
                Text(g,Fmt(T(L"ポーカー  {0}/{1}",L"Poker  {0}/{1}"),{std::to_wstring(t.Hand()),std::to_wstring(Poker::Hands)}),RectF(info.X+12*s,info.Y+6*s,228*s,26*s),18*s,white,true);
                Text(g,Fmt(T(L"ブラインド {0}/{1}",L"Blinds {0}/{1}"),{std::to_wstring(t.SmallBlind()),std::to_wstring(t.BigBlind())}),RectF(info.X+12*s,info.Y+34*s,220*s,24*s),15*s,soft);
                Text(g,Fmt(T(L"ポット {0}",L"Pot {0}"),{std::to_wstring(t.Pot())}),RectF(info.X+12*s,info.Y+58*s,220*s,26*s),18*s,gold,true);
                state.helpBox=RectF(W-254*s,110*s,240*s,30*s);
                FillRound(g,state.helpBox,8*s,Color(hud?215:200,40,18,16),edge,1.2f*s);
                Text(g,T(L"役の一覧（Tab）",L"Hands (Tab)"),state.helpBox,14*s,soft,false,StringAlignmentCenter,StringAlignmentCenter);
            }

            // The cards (on the 3D table when there is one).
            if(!hud)
            {
                const float cw2=CardW2D*s,ch2=CardH2D*s,step=90*s;
                const auto& holes=t.Holes();
                for(size_t i=0;i<holes[Poker::Her].size();++i)
                    DrawCard(g,mid+(i-1.0f)*step+4*s,118*s,cw2,ch2,holes[Poker::Her][i],t.HerCardsShown());
                const auto& board=t.Board();
                const std::vector<int> winning=HandShown()?WinningCards():std::vector<int>();
                auto rim=[&](int c){return std::find(winning.begin(),winning.end(),c)!=winning.end();};
                for(size_t i=0;i<board.size();++i)
                    DrawCard(g,mid+(i-2.5f)*step+4*s,292*s,cw2,ch2,board[i],true,false,rim(board[i]));
                for(size_t i=0;i<holes[Poker::You].size();++i)
                    DrawCard(g,mid+(i-1.0f)*step+4*s,466*s,cw2,ch2,holes[Poker::You][i],true,false,rim(holes[Poker::You][i]));
                if(t.BetOf(Poker::Her)>0)Text(g,std::to_wstring(t.BetOf(Poker::Her)),RectF(mid+110*s,170*s,120*s,30*s),22*s,gold,true);
                if(t.BetOf(Poker::You)>0)Text(g,std::to_wstring(t.BetOf(Poker::You)),RectF(mid+110*s,520*s,120*s,30*s),22*s,gold,true);
                for(size_t i=0;i<3&&t.DeckLeft()>0;++i)DrawCard(g,W-160*s+i*2*s,300*s-i*2*s,cw2*0.8f,ch2*0.8f,-1,false);
            }

            // You: chips, your bet, what you hold.
            {
                const float boxW=hud?270*s:260*s,boxH=112*s,boxY=H-boxH-12*s;
                FillRound(g,RectF(8*s,boxY,boxW,boxH),10*s,panel,edge,1.2f*s);
                Text(g,std::wstring(T(L"あなた",L"You"))+(t.Button()==Poker::You?L" (D)":L""),RectF(22*s,boxY+8*s,boxW,26*s),18*s,white,true);
                Text(g,T(L"チップ ",L"Chips ")+std::to_wstring(t.Chips(Poker::You)),RectF(120*s,boxY+10*s,boxW-126*s,24*s),17*s,gold);
                if(t.BetOf(Poker::You)>0)Text(g,Fmt(T(L"賭け {0}",L"Bet {0}"),{std::to_wstring(t.BetOf(Poker::You))}),RectF(22*s,boxY+40*s,boxW,24*s),16*s,soft);
                if(!Flying()&&!t.Holes()[Poker::You].empty())Text(g,HandName(Poker::You),RectF(22*s,boxY+70*s,boxW-30*s,28*s),19*s,white,true);
            }

            // A hand over: who took the pot and how, at the side.
            if(HandShown())
            {
                const float pw=std::min(W-40*s,340*s),ph=(t.ByFold()?106:150)*s;
                const RectF box(W-pw-14*s,std::max(150*s,H*0.40f-ph/2),pw,ph);
                FillRound(g,box,14*s,Color(235,40,22,22),Color(255,200,170,90),2*s);
                const int gain=t.HandGain();
                const std::wstring title=t.Winner()==Poker::You?Fmt(T(L"あなたの勝ち +{0}",L"You win +{0}"),{std::to_wstring(gain)}):
                                         t.Winner()==Poker::Her?Fmt(T(L"{0}の勝ち {1}",L"{0} wins {1}"),{HerName(),Signed(-gain)}):
                                         std::wstring(T(L"引き分け",L"Split pot"));
                Text(g,title,RectF(box.X,box.Y+12*s,pw,36*s),24*s,t.Winner()==Poker::You?gold:t.Winner()==Poker::Her?Color(255,236,150,140):white,true,StringAlignmentCenter,StringAlignmentNear,true);
                if(t.ByFold())
                    Text(g,t.Winner()==Poker::You?Fmt(T(L"{0}が降りました",L"{0} folded"),{HerName()}):std::wstring(T(L"あなたが降りました",L"You folded")),RectF(box.X,box.Y+58*s,pw,30*s),18*s,white,false,StringAlignmentCenter);
                else
                {
                    Text(g,T(L"あなた：",L"You: ")+HandName(Poker::You),RectF(box.X,box.Y+58*s,pw,30*s),18*s,white,false,StringAlignmentCenter);
                    Text(g,HerName()+T(L"：",L": ")+HandName(Poker::Her),RectF(box.X,box.Y+94*s,pw,30*s),18*s,white,false,StringAlignmentCenter);
                }
            }

            // What to do now, early on.
            if(YourTurnNow()&&t.Hand()==1&&t.Board().empty())
            {
                const std::wstring prompt=T(L"賭けるか降りるかを選びます（相手の顔も見て。Tabで役の一覧）",L"Bet or fold, and watch her face (Tab: the list of hands)");
                const float tw=TextWidth(g,prompt,17*s)+28*s,y=H-120*s;
                FillRound(g,RectF(mid-tw/2,y,tw,34*s),10*s,panel,edge,1.2f*s);
                Text(g,prompt,RectF(mid-tw/2,y,tw,34*s),17*s,gold,false,StringAlignmentCenter,StringAlignmentCenter);
            }
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
                Text(g,T(L"ポーカーをやめますか？",L"Stop playing?"),RectF(box.X,box.Y+22*s,box.Width,36*s),24*s,white,true,StringAlignmentCenter);
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
            if(key==VK_ESCAPE){state.confirmQuit=true;Redraw();return;}
            if(t.Now()==Phase::YourTurn)
            {
                if(key=='F')Do(Act::Fold,0);
                else if(key=='C'||key=='K')Do(Act::Call,0);
                else if(key>='1'&&key<='4'&&t.CanRaise(Poker::You))Do(Act::Raise,RaiseChoice(static_cast<int>(key-'1')));
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
                }
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

    bool PokerWindowOpen(){return state.window!=nullptr;}
    HWND PokerWindowHandle(){return state.window;}
    bool PokerWindowRect(RECT& r)
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

    void PokerLines(const std::wstring& answer)
    {
        state.prefetching=false;
        if(!state.window)return;
        state.lines.Answered(answer);
        Prefetch();
    }

    bool PokerAfterword(std::wstring& situation,std::wstring& fallback)
    {
        if(state.afterword.empty())return false;
        situation=std::move(state.afterword);
        fallback=std::move(state.afterwordLine);
        state.afterword.clear();
        state.afterwordLine.clear();
        return true;
    }

    void PokerHeard(const std::wstring& line,bool done)
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

    void PlayPokerMoves(int moves,float yaw)
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

        // The cards on their way since the table was last seen: those out of the deck one after
        // another in the order dealt, her cards turned in place, those of a hand played sliding
        // back into the deck.
        void StartFlights(const std::array<Pose,Poker::Cards>& before,bool fresh)
        {
            const double now=state.clock;
            auto& t=state.table;
            const auto& after=state.poses;
            if(fresh){state.flights.clear();return;}
            std::array<bool,Poker::Cards> done{};
            auto add=[&](int card,std::vector<std::pair<double,Pose>> keys,bool arc){
                done[static_cast<size_t>(card)]=true;
                state.flightsUntil=std::max(state.flightsUntil,keys.back().first);
                state.flights.erase(std::remove_if(state.flights.begin(),state.flights.end(),[&](const Flight& f){return f.card==card;}),state.flights.end());
                state.flights.push_back({card,std::move(keys),arc});
            };
            // The order dealt: the big blind's and the button's in turn, then the board.
            std::vector<int> order;
            const auto& holes=t.Holes();
            const int big=1-t.Button();
            for(size_t i=0;i<holes[big].size()&&i<holes[t.Button()].size();++i){order.push_back(holes[big][i]);order.push_back(holes[t.Button()][i]);}
            order.insert(order.end(),t.Board().begin(),t.Board().end());
            double at=now;
            for(int c:order)
            {
                const Pose& a=before[static_cast<size_t>(c)];
                const Pose& b=after[static_cast<size_t>(c)];
                if(done[static_cast<size_t>(c)]||a.shown||!b.shown)continue;
                Pose from=a;
                from.flip=1;
                add(c,{{at,from},{at+Travel,b}},true);
                Due(at+Travel,MahjongSound::CardPlace);
                at+=DealEach;
            }
            // The rest that moved.
            double gather=now;
            for(int c=0;c<Poker::Cards;++c)
            {
                const Pose& a=before[static_cast<size_t>(c)];
                const Pose& b=after[static_cast<size_t>(c)];
                if(done[static_cast<size_t>(c)]||!a.shown)continue;
                const float moved=std::abs(a.x-b.x)+std::abs(a.z-b.z)+std::abs(a.y-b.y)+std::abs(a.yaw-b.yaw);
                if(b.shown&&std::abs(a.flip-b.flip)>0.5f&&moved<0.02f*state.upper)
                {
                    // Turned over where it lies (her cards), after the cards before it land.
                    add(c,{{at,a},{at+Turn,b}},true);
                    Due(at,MahjongSound::CardFlip);
                    at+=Turn;
                }
                else if(moved>0.002f*state.upper||std::abs(a.flip-b.flip)>0.01f)
                {
                    add(c,{{gather,a},{gather+Gather,b}},false);
                    gather+=0.03;
                }
            }
            if(gather>now)Due(now+Gather,MahjongSound::CardTake);
            // A strong hand of yours taking the pot: gold sparkles once all is down.
            if(t.Now()==Phase::HandOver&&t.Winner()==Poker::You&&!t.ByFold()&&t.Won()>=BigPot)
            {
                state.sparkAt=std::max(at,now)+0.2;
                state.sparkSpot={0,Top(),YourZ()};
            }
        }
    }

    void TickPokerWindow(MascotModel& model,double dt)
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
            const std::array<Pose,Poker::Cards> before=state.poses;
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
        // Her hand goes out to what she is about to do (tried again next frame while a gesture of
        // hers ends): to her bet's place for chips, a tap for a check, to her cards to give them up.
        if(t.Now()==Phase::HerTurn&&state.entrance==5&&!Flying()&&state.plannedFor==t.Moves()&&state.reachedFor!=t.Moves()&&
           GetTickCount64()+ReachLead>=state.nextAt)
        {
            const Poker::Did did=t.HerOrder().did;
            const std::array<float,3> spot=did==Poker::Did::Fold?std::array<float,3>{0,Top(),HerZ()}:
                                           did==Poker::Did::Check?std::array<float,3>{0.18f*D,Top(),-0.70f*D}:std::array<float,3>{0.36f*D,Top(),-0.80f*D};
            if(ReachFor(model,spot))state.reachedFor=t.Moves();
        }
        if(state.thinkFor>0&&state.entrance==5){model.EmoteSeated(L"ponder",state.thinkFor);state.thinkFor=0;}
        {
            // A hand over: a clap (or her cheeks) for a big pot of hers, "oops" for a big one of yours.
            if(t.Now()==Phase::HandOver&&!Flying()&&state.gestureHand!=t.Hand())
            {
                state.gestureHand=t.Hand();
                std::uniform_real_distribution<double> any(0,1);
                if(t.Won()>=BigPot&&t.Winner()==Poker::You)state.endGesture=L"oops";
                else if(t.Won()>=BigPot&&t.Winner()==Poker::Her)state.endGesture=any(state.sparkRandom)<0.5?L"cheeks":L"clap";
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

    std::wstring DescribePoker()
    {
        if(!state.window)return L"";
        auto& t=state.table;
        std::wstring out=Fmt(L"poker: entrance {0}, phase {1}, hand {2}, chips you {3} her {4}, pot {5}, board {6}, deck {7}, flights {8}, face {9}, her strength {10}\n",
                             {std::to_wstring(state.entrance),std::to_wstring(static_cast<int>(t.Now())),std::to_wstring(t.Hand()),std::to_wstring(t.Chips(Poker::You)),std::to_wstring(t.Chips(Poker::Her)),
                              std::to_wstring(t.Pot()),std::to_wstring(t.Board().size()),std::to_wstring(t.DeckLeft()),std::to_wstring(state.flights.size()),std::to_wstring(static_cast<int>(state.tell)),
                              std::to_wstring(t.Holes()[Poker::Her].empty()?0.0:t.HerStrength())});
        return out+(state.words.told.empty()?std::wstring():L"poker words:\n"+state.words.told);
    }

    bool SavePokerView(const std::wstring& path)
    {
        return state.renderer&&state.renderer->SaveLastFrame(path);
    }

    bool SavePokerShot(const std::wstring& path,int moves,std::uint32_t seed,bool help)
    {
        GdiplusStartupInput input;
        if(!state.gdiplus)GdiplusStartup(&state.gdiplus,&input,nullptr);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc=DefWindowProcW;
        wc.hInstance=GetModuleHandleW(nullptr);
        wc.lpszClassName=L"MikuMikuDesktopPerchPokerShot";
        RegisterClassExW(&wc);
        const int dpi=static_cast<int>(GetDpiForSystem());
        state.window=CreateWindowExW(0,wc.lpszClassName,L"",WS_OVERLAPPEDWINDOW,0,0,MulDiv(static_cast<int>(BaseWidth)+16,dpi,96),MulDiv(static_cast<int>(BaseHeight)+39,dpi,96),
                                     nullptr,nullptr,wc.hInstance,nullptr);
        state.her=PokerOpponent{L"ミク"};
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

    void ShowPokerWindow(HWND owner,const PokerOpponent& her,std::function<void()> onEnd)
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
        state.lines.Load(L"POKERLINES",L"poker",her.ownLinesFile,her.linesFile,her.styleKey,English(),
                         {L"ポーカー",L"poker",L"",L""});
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
        const std::wstring title=std::wstring(T(L"ポーカー",L"Poker"))+L" - "+HerName();
        state.window=CreateWindowExW(0,ClassName,title.c_str(),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                                     work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,
                                     nullptr,nullptr,instance,nullptr);
        if(CreateView())PlaceView();
        SetTimer(state.window,TimerId,40,nullptr);
        ShowWindow(state.window,SW_SHOWNORMAL);
        SetForegroundWindow(state.window);
    }
}
