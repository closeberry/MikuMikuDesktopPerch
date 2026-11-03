#define NOMINMAX
#include "ShogiWindow.hpp"
#include "Shogi.hpp"
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
        using Shogi::Phase;
        using Shogi::Move;
        constexpr int Squares=Shogi::Squares;
        constexpr int HandBase=100;   // a kind in your hand, pointed at or chosen: HandBase + kind
        constexpr wchar_t ClassName[]=L"MikuMikuDesktopPerchShogi";
        constexpr wchar_t ViewClassName[]=L"MikuMikuDesktopPerchShogiView";
        constexpr UINT_PTR TimerId=1;
        // How carefully she plays (Shogi::Table::NewGame): a little less as you grow closer,
        // and after games you lost in a row.
        constexpr double Care=0.85,LessPerBond=0.03,LessPerLoss=0.08,LeastCare=0.4;
        constexpr double ThinkSeconds=1.0;         // how long she may read a move
        // Her pace: before her move (in 2D), before the game's end is shown.
        constexpr ULONGLONG HerPlayWait=700,EndWait=900;
        constexpr ULONGLONG SaidShown=1800;
        constexpr ULONGLONG ReachLead=600;         // her hand starts out this long before her piece moves
        constexpr ULONGLONG HerPlayWait3D=1100;    // at the 3D table: time for her hand to go out
        constexpr ULONGLONG ThinkExtra=2000;       // her move later by this when she stops to think
        constexpr ULONGLONG EndWait3D=2200;        // at the 3D table: the end waits for her reaction
        // A piece moved at the 3D table goes to its square in this long (seconds); one taken
        // goes to the taker's stand after.
        constexpr double Glide=0.38,ToStand=0.45;
        // Sizes in logical pixels (at 96 dpi, at the size the window opens).
        constexpr float BaseWidth=1180,BaseHeight=840;
        constexpr float HerColumn=0.27f;           // her column at the right, a share of the window's width
        // The kinds in a hand, in the order the stands show them.
        constexpr int HandOrder[7]={Shogi::Rook,Shogi::Bishop,Shogi::Gold,Shogi::Silver,Shogi::Knight,Shogi::Lance,Shogi::Pawn};

        // A piece at the 3D table (world units): the middle of its bottom, the way its point
        // goes (yaw 0: toward her, pi: toward you), its kind and owner.
        struct Piece {float x=0,y=0,z=0,yaw=0;int kind=0,who=0;bool shown=false;};
        // A piece on its way: where it is at each time (seconds of the clock). square: where
        // it comes to rest on the board, or -1 for a stand (hand: whose, of handKind).
        struct Flight {int square=-1,hand=-1,handKind=0;std::vector<std::pair<double,Piece>> keys;};

        struct State
        {
            HWND window=nullptr;
            ULONG_PTR gdiplus=0;
            ShogiOpponent her;
            std::unique_ptr<Bitmap> face;
            std::function<void()> onEnd;
            Shogi::Table table;
            int lost=0;                // games you lost in a row
            int nextFirst=-1;          // who moves first next game (-1: drawn by lot)
            bool gameOverTold=false;
            int hover=-1,cursor=-1;    // under the mouse (a square, or HandBase + a kind), and the keys' square
            int selected=-1;           // the piece of yours chosen to move (a square, or HandBase + a kind)
            Move promoting;            // a move that may promote or not, asked about
            RectF promoteButtons[2];
            bool confirmResign=false;
            RectF quitButtons[2];
            ULONGLONG nextAt=0;
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
            // Her lines by scene (res/shogi/lines.txt, said again her way by the LLM, and the
            // user's own lines file), and her voice for a line that has one.
            GameLines lines;
            GameVoice voice;
            bool prefetching=false;
            Expression tell=Expression::Neutral;   // her face while she sees a mate, or is far behind
            POINT headScreen{-1,-1};
            std::wstring afterword,afterwordLine;
            struct Seen
            {
                bool started=false,over=false;
                int moves=0;
                bool sureWin=false,behind=false;   // said once each game
            } seen;
            std::array<float,2> boardOrigin{};                     // the 2D board's top left corner
            float cellW=0,cellH=0;                                 // and its squares
            std::array<RectF,Shogi::HandKinds> handBoxes{};        // your hand's kinds on the 2D stand
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
            // The pieces on their way, and until when (on the clock); sounds due on the way.
            std::vector<Flight> flights;
            double flightsUntil=0;
            std::vector<std::pair<double,MahjongSound>> sounds;
            int movesSeen=-1;
            double sparkAt=0;         // gold sparkles then, round the mated king
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
        // Who moves first, in words.
        std::wstring FirstOf(int who){return state.table.First()==who?T(L"先手",L"Sente (first)"):T(L"後手",L"Gote (second)");}

        bool Waiting(){return state.nextAt>GetTickCount64();}
        // A piece still on its way at the 3D table (a move not yet shown counts).
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
                Log(L"shogi: resize: %s",error.c_str());
            InvalidateRect(state.window,nullptr,FALSE);
        }

        void ReleaseView()
        {
            if(state.renderer){state.renderer->WaitIdle();state.renderer.reset();Log(L"shogi: 3D view released");}
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


        // ---- The pieces' faces ----

        // A face's characters: two stacked on the 3D pieces, one in 2D. Slot 13 (no kind) is
        // her king, 王将 (yours is 玉将).
        const wchar_t* FaceText(int slot,bool two)
        {
            static const wchar_t* const twos[16]={L"",L"歩兵",L"香車",L"桂馬",L"銀将",L"金将",L"角行",L"飛車",L"玉将",L"と",L"杏",L"圭",L"全",L"王将",L"龍馬",L"龍王"};
            static const wchar_t* const ones[16]={L"",L"歩",L"香",L"桂",L"銀",L"金",L"角",L"飛",L"玉",L"と",L"杏",L"圭",L"全",L"王",L"馬",L"龍"};
            return (two?twos:ones)[slot&15];
        }
        int FaceSlot(int kind,int who){return kind==Shogi::King&&who==Shogi::Her?13:kind;}
        bool RedFace(int slot){return slot>=Shogi::Tokin&&slot!=13;}

        const FontFamily* Mincho()
        {
            static FontFamily yu(L"Yu Mincho"),ms(L"MS Mincho");
            return yu.IsAvailable()?&yu:ms.IsAvailable()?&ms:FontFamily::GenericSerif();
        }

        // A face's characters in box (its point at the top): ink, red when promoted.
        // A face's characters in box (its point at the top), in the brush hand (Mincho where
        // it lacks one), carved into the wood: the stroke's floor sunk a little below its
        // edges, the far wall in shadow and the near lip catching the light. Ink, red when
        // promoted.
        void FaceInk(Graphics& g,const RectF& box,int slot,bool two)
        {
            const std::wstring text=FaceText(slot,two);
            if(text.empty())return;
            const bool brush=Brush()&&BrushHas(text);
            const FontFamily* family=brush?Brush():Mincho();
            const INT style=brush?FontStyleRegular:FontStyleBold;
            StringFormat format;
            format.SetAlignment(StringAlignmentCenter);
            format.SetLineAlignment(StringAlignmentCenter);
            format.SetFormatFlags(StringFormatFlagsNoWrap|StringFormatFlagsNoClip);
            GraphicsPath path;
            float size;
            if(text.size()==2)
            {
                size=box.Height*(brush?0.32f:0.29f);
                for(int i=0;i<2;++i)
                    path.AddString(text.c_str()+i,1,family,style,size,RectF(box.X,box.Y+box.Height*(0.34f+0.30f*i)-size*0.6f,box.Width,size*1.2f),&format);
            }
            else
            {
                size=box.Height*(two?0.44f:0.50f)*(brush?1.1f:1.0f);
                path.AddString(text.c_str(),1,family,style,size,RectF(box.X,box.Y+box.Height*0.52f-size*0.62f,box.Width,size*1.24f),&format);
            }
            const bool red=RedFace(slot);
            const float d=std::max(0.9f,size*0.05f);
            auto shifted=[&](float dx,float dy){
                std::unique_ptr<GraphicsPath> moved(path.Clone());
                Matrix by(1,0,0,1,dx,dy);
                moved->Transform(&by);
                return moved;
            };
            const GraphicsState saved=g.Save();
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            // The lip at the near (lower right) edge, lit; the wood just past the far edge, shaded.
            SolidBrush lip(Color(210,255,248,220)),shade(Color(90,90,50,16));
            g.FillPath(&lip,shifted(d,d).get());
            g.FillPath(&shade,shifted(-0.5f*d,-0.5f*d).get());
            // The groove: dark at its far wall, the floor (seen past the wall) a shade lighter
            // and darkening toward its middle.
            SolidBrush wall(red?Color(255,96,14,10):Color(255,8,5,3));
            g.FillPath(&wall,&path);
            g.SetClip(&path);
            const RectF bounds=[&]{RectF r;path.GetBounds(&r);return r;}();
            LinearGradientBrush floor(PointF(bounds.X,bounds.Y),PointF(bounds.GetRight(),bounds.GetBottom()),
                                     red?Color(255,196,46,34):Color(255,52,38,26),red?Color(255,150,28,20):Color(255,26,18,10));
            g.FillPath(&floor,shifted(0.8f*d,0.8f*d).get());
            g.Restore(saved);
        }

        // A piece's outline in box (its point at the top): a pentagon, its shoulders 0.22 down.
        void PieceOutline(GraphicsPath& path,const RectF& box)
        {
            const PointF p[5]={{box.X+box.Width*0.5f,box.Y},{box.X+box.Width*0.91f,box.Y+box.Height*0.22f},{box.GetRight(),box.GetBottom()},
                               {box.X,box.GetBottom()},{box.X+box.Width*0.09f,box.Y+box.Height*0.22f}};
            path.AddPolygon(p,5);
        }

        // ---- The room's texture ----

        // The 3D table's texture: the scroll's painting at the top left, the pieces' faces in
        // the middle (4 by 4, slot n at column n%4, row n/4) and the board's face (its lines
        // and star points painted, so its mipmaps keep them from breaking up into stray pixels
        // far off or at a low angle) at the right.
        constexpr int AtlasW=2048,AtlasH=1024,BoardX=1024,BoardSide=1024;
        constexpr int FaceX=336,FaceW=168,FaceH=252;
        constexpr float BoardMargin=0.32f;   // the board's face beyond the lines, in squares

        void PaintBoard(Graphics& g)
        {
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            LinearGradientBrush wood(PointF(0,0),PointF(static_cast<float>(BoardSide),static_cast<float>(BoardSide)),Color(255,222,178,104),Color(255,206,160,88));
            g.FillRectangle(&wood,0,0,BoardSide,BoardSide);
            const float span=Shogi::Size+2*BoardMargin,cell=BoardSide/span,edge=BoardMargin*cell;
            Pen line(Color(255,30,20,10),std::max(2.0f,cell*0.022f));
            for(int i=0;i<=Shogi::Size;++i)
            {
                const float p=edge+i*cell;
                g.DrawLine(&line,p,edge,p,BoardSide-edge);
                g.DrawLine(&line,edge,p,BoardSide-edge,p);
            }
            SolidBrush dot(Color(255,30,20,10));
            for(int px:{3,6})for(int py:{3,6})g.FillEllipse(&dot,edge+px*cell-cell*0.05f,edge+py*cell-cell*0.05f,cell*0.1f,cell*0.1f);
        }

        void PaintFaces(Graphics& g)
        {
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            for(int slot=1;slot<16;++slot)
            {
                const RectF box(static_cast<float>(FaceX+slot%4*FaceW),static_cast<float>(slot/4*FaceH),static_cast<float>(FaceW),static_cast<float>(FaceH));
                LinearGradientBrush wood(PointF(box.X,box.Y),PointF(box.X,box.GetBottom()),Color(255,246,214,150),Color(255,234,196,126));
                g.FillRectangle(&wood,box);
                FaceInk(g,RectF(box.X+box.Width*0.08f,box.Y+box.Height*0.06f,box.Width*0.84f,box.Height*0.9f),slot,true);
            }
        }

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
                PaintFaces(g);
                g.TranslateTransform(static_cast<float>(BoardX),0);
                PaintBoard(g);
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
                Log(L"shogi: no 3D view (%s)",error.c_str());
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
            Log(L"shogi: 3D view %ux%u on %s (hips %.1f, upper body %.1f, floor %.1f)",w,h,renderer->AdapterName().c_str(),state.hips,state.upper,state.floorY);
            if(!renderer->SetGeometryTexture(ScrollImage()))Log(L"shogi: no texture for the 3D table");
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

        // ---- The board and the pieces ----
        // A shogi board on the middle of the felt, row 0 at her side and row 8 at yours, column
        // 0 at your left; each side's stand of pieces taken at its right.

        float Top(){return 0.12f*state.hips+0.30f*state.upper;}
        float BoardZ(){return -1.10f*state.upper;}
        float FieldZ(){return BoardZ();}
        float CellW(){return 0.11f*state.upper;}
        float CellH(){return 0.12f*state.upper;}
        float BoardTop(){return Top()+0.05f*state.upper;}
        float StandTop(){return Top()+0.035f*state.upper;}
        float SquareX(int sq){return (sq%Shogi::Size-4)*CellW();}
        float SquareZ(int sq){return BoardZ()+(4-sq/Shogi::Size)*CellH();}
        std::array<float,3> SquareSpot(int sq){return {SquareX(sq),BoardTop(),SquareZ(sq)};}
        // Where each side's stand stands (its middle): at its owner's right.
        std::array<float,2> StandAt(int who)
        {
            const float D=state.upper;
            return who==Shogi::You?std::array<float,2>{0.86f*D,BoardZ()-0.34f*D}:std::array<float,2>{-0.86f*D,BoardZ()+0.34f*D};
        }
        constexpr float StandW=0.36f,StandD=0.46f;   // its size, in upper bodies

        // A piece's size: the king's the largest, the pawn's the least.
        float KindScale(int kind)
        {
            static const float scale[9]={1,0.84f,0.87f,0.90f,0.93f,0.93f,0.96f,0.96f,1};
            return scale[Shogi::Base(kind)];
        }
        float PieceW(int kind){return 0.84f*CellW()*KindScale(kind);}
        float PieceL(int kind){return 0.90f*CellH()*KindScale(kind);}

        float YawOf(int who){return who==Shogi::You?0.0f:DirectX::XM_PI;}

        Piece Resting(int sq)
        {
            const auto& b=state.table.Board();
            Piece p;
            p.x=SquareX(sq);
            p.y=BoardTop();
            p.z=SquareZ(sq);
            p.kind=b.KindAt(sq);
            p.who=b.OwnerAt(sq);
            p.yaw=YawOf(p.who);
            p.shown=p.kind!=0;
            return p;
        }

        // The index-th of count pieces of kind on who's stand (laid in rows, each kind's
        // pieces overlapping a little).
        Piece OnStand(int who,int kind,int index,int count)
        {
            const float D=state.upper;
            const auto c=StandAt(who);
            // Rows from the far end: rook and bishop, gold and silver, knight and lance, pawns.
            float u=0,v=0;
            switch(kind)
            {
            case Shogi::Rook:u=-0.085f;v=0.15f;break;
            case Shogi::Bishop:u=0.085f;v=0.15f;break;
            case Shogi::Gold:u=-0.085f;v=0.04f;break;
            case Shogi::Silver:u=0.085f;v=0.04f;break;
            case Shogi::Knight:u=-0.085f;v=-0.07f;break;
            case Shogi::Lance:u=0.085f;v=-0.07f;break;
            default:u=-0.11f;v=-0.18f;break;
            }
            const float step=kind==Shogi::Pawn?std::min(0.026f,0.22f/std::max(1,count-1)):0.028f;
            u=(u+step*index-(kind==Shogi::Pawn?0:step*(count-1)/2))*D;
            v*=D;
            // Owner's right and forward: yours toward her (+z), hers toward you.
            const float sign=who==Shogi::You?1.0f:-1.0f;
            Piece p;
            p.x=c[0]+sign*u;
            p.z=c[1]+sign*v;
            p.y=StandTop()+0.006f*D*index;
            p.kind=kind;
            p.who=who;
            p.yaw=YawOf(who);
            p.shown=true;
            return p;
        }

        // A piece lying at p: a pentagon of wood, its face from the texture.
        void AddPiece(Mesh& m,const Piece& p,float raise=0)
        {
            using V=std::array<float,3>;
            const float w=PieceW(p.kind),l=PieceL(p.kind),h=0.27f*w;
            const float fx=std::sin(p.yaw),fz=std::cos(p.yaw),rx=std::cos(p.yaw),rz=-std::sin(p.yaw);
            constexpr float Outline[5][2]={{-0.5f,0},{0.5f,0},{0.41f,0.78f},{0,1},{-0.41f,0.78f}};
            V low[5],high[5];
            for(int i=0;i<5;++i)
            {
                // The top a little smaller than the bottom (its edges bevelled).
                for(const auto& [out,shrink,y]:{std::tuple{&low,1.0f,p.y+raise},std::tuple{&high,0.94f,p.y+raise+h}})
                {
                    const float a=Outline[i][0]*w*shrink,b=(Outline[i][1]-0.5f)*l*shrink;
                    (*out)[i]={p.x+rx*a+fx*b,y,p.z+rz*a+fz*b};
                }
            }
            const float side[4]={0.80f,0.62f,0.38f,1},white[4]={1,1,1,1};
            for(int i=0;i<5;++i)
            {
                const int j=(i+1)%5;
                const float dx=low[j][0]-low[i][0],dz=low[j][2]-low[i][2],len=std::sqrt(dx*dx+dz*dz)+1e-6f;
                m.Quad(low[i],low[j],high[j],high[i],{dz/len,0.15f,-dx/len},side);
            }
            const int slot=FaceSlot(p.kind,p.who);
            const float u0=(FaceX+slot%4*FaceW+2.0f)/AtlasW,du=(FaceW-4.0f)/AtlasW,v0=(slot/4*FaceH+2.0f)/AtlasH,dv=(FaceH-4.0f)/AtlasH;
            auto uv=[&](int i,float* out){out[0]=u0+(Outline[i][0]+0.5f)*du;out[1]=v0+(1-Outline[i][1])*dv;};
            float a[8],b[8];
            uv(0,a);uv(1,a+2);uv(2,a+4);uv(4,a+6);
            uv(4,b);uv(2,b+2);uv(3,b+4);uv(3,b+6);
            m.Quad(high[0],high[1],high[2],high[4],{0,1,0},white,a);
            m.Quad(high[4],high[2],high[3],high[3],{0,1,0},white,b);
        }

        // A flat round spot on the board.
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

        // A square's frame on the board (k: how far in from its edge, in squares).
        void AddFrame(Mesh& m,int sq,const float (&colour)[4],float k=0.06f)
        {
            const float x=SquareX(sq),z=SquareZ(sq),hx=CellW()*(0.5f-k),hz=CellH()*(0.5f-k),w=0.0065f*state.upper,y=BoardTop()+0.0012f*state.upper;
            m.Quad({x-hx,y,z-hz},{x+hx,y,z-hz},{x+hx,y,z-hz+w},{x-hx,y,z-hz+w},{0,1,0},colour);
            m.Quad({x-hx,y,z+hz-w},{x+hx,y,z+hz-w},{x+hx,y,z+hz},{x-hx,y,z+hz},{0,1,0},colour);
            m.Quad({x-hx,y,z-hz},{x-hx+w,y,z-hz},{x-hx+w,y,z+hz},{x-hx,y,z+hz},{0,1,0},colour);
            m.Quad({x+hx-w,y,z-hz},{x+hx,y,z-hz},{x+hx,y,z+hz},{x+hx-w,y,z+hz},{0,1,0},colour);
        }

        // A square filled (just inside its lines).
        void AddFill(Mesh& m,int sq,const float (&colour)[4])
        {
            const float x=SquareX(sq),z=SquareZ(sq),hx=CellW()*0.47f,hz=CellH()*0.47f,y=BoardTop()+0.0006f*state.upper;
            m.Quad({x-hx,y,z-hz},{x+hx,y,z-hz},{x+hx,y,z+hz},{x-hx,y,z+hz},{0,1,0},colour);
        }

        bool InFlight(int sq)
        {
            return std::any_of(state.flights.begin(),state.flights.end(),[&](const Flight& f){return f.square==sq;});
        }

        // Your moves from what is chosen (a square, or HandBase + a kind).
        std::vector<Move> Options(int chosen)
        {
            if(chosen<0)return {};
            return chosen>=HandBase?state.table.DropsOf(chosen-HandBase):state.table.MovesFrom(chosen);
        }
        bool Movable(int at){return at>=0&&!Options(at).empty();}
        // The squares the chosen piece may go to.
        std::vector<int> Targets()
        {
            std::vector<int> out;
            for(const Move& m:Options(state.selected))if(std::find(out.begin(),out.end(),m.to)==out.end())out.push_back(m.to);
            return out;
        }

        // What the table's geometry shows.
        std::vector<int> Scene()
        {
            auto& t=state.table;
            const auto& b=t.Board();
            std::vector<int> out;
            for(int sq=0;sq<Squares;++sq)out.push_back(b.At(sq));
            for(int who=0;who<2;++who)for(int kind=Shogi::Pawn;kind<=Shogi::Rook;++kind)out.push_back(b.Hand(who,kind));
            out.push_back(t.Moves());
            out.push_back(state.movesSeen);
            out.push_back(YourMoveNow()?state.hover:-9);
            out.push_back(YourMoveNow()?state.cursor:-9);
            out.push_back(state.selected);
            out.push_back(t.Now()==Phase::GameOver?1:0);
            for(const auto& f:state.flights)out.push_back(1000+f.square*10+f.hand);
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
            // The board: a block of pale wood, its face with the lines and star points from
            // the texture.
            const float hx=(Shogi::Size/2.0f+BoardMargin)*CellW(),hz=(Shogi::Size/2.0f+BoardMargin)*CellH(),bz=BoardZ(),bt=BoardTop();
            const float wood[4]={0.78f,0.58f,0.32f,1},white[4]={1,1,1,1};
            mesh.Box(-hx,t.top,bz-hz,hx,bt-0.0004f*D,bz+hz,wood);
            {
                const float u0=(BoardX+1.0f)/AtlasW,u1=(BoardX+BoardSide-1.0f)/AtlasW,v0=1.0f/AtlasH,v1=(BoardSide-1.0f)/AtlasH;
                // Its top row (the texture's) at her side.
                const float uv[8]={u0,v1,u1,v1,u1,v0,u0,v0};
                mesh.Quad({-hx,bt,bz-hz},{hx,bt,bz-hz},{hx,bt,bz+hz},{-hx,bt,bz+hz},{0,1,0},white,uv);
            }
            // Each side's stand: a low box of dark wood on four short feet.
            for(int who:{Shogi::You,Shogi::Her})
            {
                const auto c=StandAt(who);
                const float stand[4]={0.36f,0.20f,0.10f,Wood},feet[4]={0.26f,0.14f,0.07f,Wood};
                const float sw=StandW/2*D,sd=StandD/2*D,st=StandTop();
                mesh.grain=0;mesh.grainScale=0.35f;
                mesh.Box(c[0]-sw,st-0.012f*D,c[1]-sd,c[0]+sw,st,c[1]+sd,stand);
                for(float fx:{-1.0f,1.0f})
                    for(float fz:{-1.0f,1.0f})
                        mesh.Box(c[0]+fx*(sw-0.03f*D)-0.015f*D,t.top,c[1]+fz*(sd-0.03f*D)-0.015f*D,c[0]+fx*(sw-0.03f*D)+0.015f*D,st-0.012f*D,c[1]+fz*(sd-0.03f*D)+0.015f*D,feet);
                const auto& b=state.table.Board();
                for(int kind=Shogi::Pawn;kind<=Shogi::Rook;++kind)
                {
                    int count=b.Hand(who,kind);
                    // One still on its way here is not on the stand yet.
                    for(const auto& f:state.flights)if(f.hand==who&&f.handKind==kind)--count;
                    for(int i=0;i<count;++i)
                    {
                        const bool chosen=who==Shogi::You&&state.selected==HandBase+kind&&i==count-1;
                        AddPiece(mesh,OnStand(who,kind,i,count),chosen?0.025f*D:0);
                    }
                }
            }
            const auto& b=state.table.Board();
            const bool shown=state.movesSeen==state.table.Moves();
            const float gold[4]={1.0f,0.80f,0.32f,1},red[4]={0.86f,0.22f,0.18f,1},pale[4]={0.95f,0.70f,0.45f,1},soft[4]={1.0f,0.92f,0.62f,1};
            // The last move's square, and a king in check.
            const Move last=state.table.LastMove();
            if(shown&&last.Valid())AddFill(mesh,last.to,pale);
            if(shown&&state.table.InCheck()&&state.table.Now()!=Phase::GameOver)AddFrame(mesh,b.KingOf(b.ToMove()),red);
            // The pieces on the board (one chosen lifted a little).
            for(int sq=0;sq<Squares;++sq)
            {
                if(!b.At(sq)||(shown&&InFlight(sq))||(!shown&&state.movesSeen>=0&&sq==last.to))continue;
                AddPiece(mesh,Resting(sq),state.selected==sq?0.025f*D:0);
            }
            // While you are to move: where the chosen piece may go (gold spots; a frame round a
            // piece it would take), the piece pointed at, and the keys' square.
            if(YourMoveNow())
            {
                for(int to:Targets())
                {
                    if(b.At(to))AddFrame(mesh,to,gold,0.10f);
                    else AddSpot(mesh,SquareX(to),SquareZ(to),0.16f*CellW(),gold);
                }
                if(state.hover>=0&&state.hover<Squares&&state.hover!=state.selected&&(Movable(state.hover)||state.selected>=0))AddFrame(mesh,state.hover,soft);
                if(state.cursor>=0&&state.hover<0)AddFrame(mesh,state.cursor,gold);
            }
            return std::move(mesh.out);
        }

        // The piece between keys at time (eased; it turns and changes face halfway).
        bool PieceAt(const Flight& f,double time,Piece& out)
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
                out=u<0.5f?a:b;
                out.x=mix(a.x,b.x);out.y=mix(a.y,b.y);out.z=mix(a.z,b.z);out.yaw=mix(a.yaw,b.yaw);
                out.shown=true;
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
            if(state.her.model&&state.tell!=Expression::Neutral)state.her.model->SetExpression(Expression::Neutral,false);
            state.tell=Expression::Neutral;
            if(state.afterword.empty()&&state.seen.started&&state.table.Now()!=Phase::GameOver)
            {
                const auto& t=state.table;
                state.afterword=Fmt(T(L"いま、話し相手と将棋を指していたが、途中でやめてデスクトップに戻ってきたところ（{0}手まで指していた）。そのことを話し相手にひと言（1〜2文）",
                                      L"You were playing shogi with them, but they stopped midway and you have come back to the desktop ({0} moves in). Say a word about it to them (one or two sentences)"),
                                    {std::to_wstring(t.Moves())});
                state.afterwordLine=T(L"また続き、指そうね。",L"Let's finish it another time.");
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
            // The first game's first move by lot; then it goes back and forth.
            const int first=state.nextFirst>=0?state.nextFirst:static_cast<int>(device()%2);
            state.nextFirst=1-first;
            state.table.NewGame(device(),std::max(LeastCare,Care-LessPerBond*state.her.bond-LessPerLoss*state.lost),first,ThinkSeconds);
            state.hover=state.cursor=state.selected=-1;
            state.promoting={};
            state.confirmResign=false;
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
            state.hover=state.selected=-1;
            state.promoting={};
            const ULONGLONG now=GetTickCount64();
            state.nextAt=now+(state.table.Now()==Phase::GameOver?(state.renderer?EndWait3D:EndWait):HerPlayWait);
            Redraw();
        }

        void Tell(const std::wstring& why)
        {
            state.said=why;
            state.saidAt=GetTickCount64();
            Redraw();
        }

        void PlayMove(const Move& m)
        {
            if(!YourMoveNow()||!state.table.CanPlay(m))return;
            state.table.Play(m);
            Moved();
        }

        // Why the piece chosen may not go to sq, when it is worth saying (a pawn on a file with
        // one, a pawn dropped to mate, your king in check).
        std::wstring WhyNot(int sq)
        {
            const auto& b=state.table.Board();
            if(state.selected==HandBase+Shogi::Pawn&&!b.At(sq))
            {
                for(int r=0;r<Shogi::Size;++r)
                    if(b.At(r*Shogi::Size+sq%Shogi::Size)==Shogi::Pawn)return T(L"二歩になるので打てません",L"Not there: two pawns on a file");
                if(sq/Shogi::Size>0&&b.At(sq-Shogi::Size)==Shogi::King+16*Shogi::Her)return T(L"打ち歩詰めは禁じ手です",L"Not there: a pawn may not be dropped to mate");
            }
            if(state.table.InCheck())return T(L"王手がかかっています",L"Your king is in check");
            return L"";
        }

        // A click (or Enter) on a square or a kind in your hand: choose a piece of yours, then
        // where it goes (asked whether to promote when it may or not).
        void Click(int at)
        {
            if(!YourMoveNow()||at<0)return;
            if(state.selected>=0&&at<Squares)
            {
                std::vector<Move> here;
                for(const Move& m:Options(state.selected))if(m.to==at)here.push_back(m);
                if(here.size()==1){PlayMove(here[0]);return;}
                if(here.size()>1)
                {
                    state.promoting=here[0];
                    state.promoting.promote=true;
                    Redraw();
                    return;
                }
            }
            if(at!=state.selected&&Movable(at)){state.selected=at;PlayMahjongSound(MahjongSound::CardPlace);Redraw();return;}
            if(state.selected>=0&&at<Squares&&at!=state.selected)
            {
                const auto& b=state.table.Board();
                if(b.OwnerAt(at)!=Shogi::You)
                    if(const std::wstring why=WhyNot(at);!why.empty())Tell(why);
            }
            state.selected=-1;
            Redraw();
        }

        void Promote(bool yes)
        {
            Move m=state.promoting;
            state.promoting={};
            m.promote=yes;
            PlayMove(m);
        }

        void Continue()
        {
            auto& t=state.table;
            if(t.Now()==Phase::GameOver)
            {
                state.lost=t.Winner()==Shogi::Her?state.lost+1:0;
                NewGame();
            }
            Redraw();
        }

        // In 2D, a piece's sound as soon as it is played (at the 3D table, as it lands).
        void Listen()
        {
            if(state.renderer)return;
            auto& t=state.table;
            if(t.Moves()>state.heardMoves&&state.heardMoves>=0)
            {
                PlayMahjongSound(MahjongSound::CardPlace);
            }
            state.heardMoves=t.Moves();
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
        void Say(const wchar_t* name,int weight=2,double chance=1)
        {
            if(!state.lines.Roll(name,chance,state.sparkRandom))return;
            GameLine line;
            if(state.lines.Pick(name,state.words.recent,state.sparkRandom,line))SayLine(line,weight);
        }

        // What she sees now, while the game goes on: 1 when her reading finds a mate for her,
        // -1 when she is far behind, else 0.
        int Certain()
        {
            const auto& t=state.table;
            if(t.Now()==Phase::GameOver||t.Moves()<2)return 0;
            const int score=t.LastThought().score;
            return score>=Shogi::Mate-200?1:score<=-1500?-1:0;
        }

        void SetTell(Expression e)
        {
            if(e==state.tell)return;
            state.tell=e;
            if(state.her.model&&state.renderer)state.her.model->SetExpression(e,false);
        }

        // Her face: smiling when she sees her mate, glum when she is far behind.
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
                state.her.onEvent(seen+Fmt(T(L"（{0}手目）",L" (move {0})"),{std::to_wstring(t.Moves())}),weight,Pick(theirs));
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

        bool BigPiece(int kind){return Shogi::Base(kind)==Shogi::Rook||Shogi::Base(kind)==Shogi::Bishop;}

        // How the game ended, in words (for the end screen and her afterword).
        std::wstring EndingText()
        {
            const auto& t=state.table;
            const std::wstring moves=std::to_wstring(t.Moves());
            switch(t.HowEnded())
            {
            case Shogi::Ending::Mate:return Fmt(T(L"詰み（{0}手）",L"Checkmate ({0} moves)"),{moves});
            case Shogi::Ending::Resigned:return Fmt(T(L"投了（{0}手）",L"Resigned ({0} moves)"),{moves});
            case Shogi::Ending::Repetition:return Fmt(T(L"千日手（{0}手）",L"Fourfold repetition ({0} moves)"),{moves});
            case Shogi::Ending::PerpetualCheck:return Fmt(T(L"連続王手の千日手（{0}手）",L"Perpetual check ({0} moves)"),{moves});
            case Shogi::Ending::MoveLimit:return Fmt(T(L"手数が長くなりました（{0}手）",L"Too many moves ({0})"),{moves});
            default:return L"";
            }
        }

        // What is new in the game since the last tick, for words: its start, a check or a big
        // piece taken either way, the end.
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
            if(Flying())return;   // the piece is still on its way
            const std::wstring name=HerName();
            if(!seen.started)
            {
                seen.started=true;
                const bool first=t.First()==Shogi::Her;
                Happened(6,Fmt(T(L"これから話し相手と将棋の対局が始まる。あなたは{0}。",L"A game of shogi with them is about to begin. You {0}. "),
                               {first?T(L"先手（先に指す）",L"move first (sente)"):T(L"後手（後から指す）",L"move second (gote)")}),
                         T(L"わくわくする気持ち",L"how you look forward to it"),
                         L"始まり",
                         Fmt(T(L"{0}と、このパソコンの持ち主の将棋の対局が始まる。",L"A game of shogi between {0} and the owner of this computer is about to begin."),{name}),
                         {T(L"がんばれー！",L"Good luck, you two!"),T(L"どっちが勝つかな？",L"Who's going to win?")});
            }
            if(t.Moves()!=seen.moves)
            {
                seen.moves=t.Moves();
                const bool over=t.Now()==Phase::GameOver;
                if(!over&&t.LastWho()==Shogi::You&&(t.InCheck()||BigPiece(t.LastCaptured())))
                    Happened(4,t.InCheck()?T(L"将棋で、話し相手に王手をかけられた。",L"At shogi, they have put your king in check."):
                                           T(L"将棋で、話し相手に飛車か角を取られた。",L"At shogi, they have taken your rook or bishop."),
                             T(L"あせる気持ち",L"how alarmed you are"),
                             L"危ない",
                             Fmt(t.InCheck()?T(L"将棋で、持ち主が{0}に王手をかけた。",L"At shogi, the owner put {0}'s king in check."):
                                             T(L"将棋で、持ち主が{0}の大駒を取った。",L"At shogi, the owner took one of {0}'s big pieces."),{name}),
                             {T(L"おっ、王手だ！",L"Ooh, check!"),T(L"いい手だね！",L"Nice move!")});
                else if(!over&&t.LastWho()==Shogi::Her&&(t.InCheck()||BigPiece(t.LastCaptured())))
                    Happened(3,L"",L"",nullptr,
                             Fmt(t.InCheck()?T(L"将棋で、{0}が王手をかけた。",L"At shogi, {0} put the owner's king in check."):
                                             T(L"将棋で、{0}が大駒を取った。",L"At shogi, {0} took a big piece."),{name}),
                             {Fmt(T(L"{0}、攻めてるね！",L"{0} is on the attack!"),{name})});
            }
            // Her mate seen, or her game far behind: said once a game, and her face holds it
            // (UpdateTell).
            const int certain=Certain();
            if(certain>0&&!seen.sureWin){seen.sureWin=true;Say(L"勝ち確定");}
            if(certain<0&&!seen.behind){seen.behind=true;Say(L"苦しい");}
            const bool over=t.Now()==Phase::GameOver;
            if(over&&!seen.over)
            {
                const int winner=t.Winner();
                const bool sheResigned=t.HowEnded()==Shogi::Ending::Resigned&&winner==Shogi::You;
                const std::wstring how=EndingText();
                state.afterword=Fmt(T(L"いま、話し相手と将棋を最後まで指して、デスクトップに戻ってきたところ。結果は{0}（{1}）。その対局の感想を、話し相手にひと言（1〜2文）",
                                      L"You have just played a game of shogi to its end with them and come back to the desktop. {0} ({1}). Tell them what you thought of the game (one or two sentences)"),
                                    {winner==Shogi::Her?T(L"あなたの勝ち",L"You won"):winner<0?T(L"引き分け",L"A draw"):T(L"あなたの負け",L"You lost"),how});
                state.afterwordLine=winner==Shogi::Her?T(L"将棋、楽しかったね！また指そう。",L"That was fun! Let's play again sometime."):T(L"くやしいなあ…次は勝つからね！",L"So close... I'll win next time!");
                if(winner==Shogi::Her)
                    Happened(9,Fmt(T(L"将棋の対局が終わって、あなたが勝った（{0}）。",L"The game of shogi is over: you won ({0})."),{how}),
                             T(L"勝ってうれしい気持ち",L"how glad you are to have won"),
                             L"勝った",
                             Fmt(T(L"{0}と持ち主の将棋の対局が終わり、{0}が勝った。",L"The shogi game between {0} and the owner is over: {0} won."),{name}),
                             {T(L"おつかれさま！いい勝負だったね。",L"Well played, both of you!"),Fmt(T(L"{0}の勝ちだ！",L"{0} wins!"),{name})});
                else Happened(9,winner<0?Fmt(T(L"将棋の対局が終わって、引き分け（{0}）。",L"The game of shogi is over: a draw ({0})."),{how}):
                                sheResigned?Fmt(T(L"将棋の対局で、詰みが見えたのであなたが投了した。あなたの負け（{0}）。",L"At shogi, you saw you were mated and resigned: you lost ({0})."),{how}):
                                            Fmt(T(L"将棋の対局が終わって、あなたの負け（{0}）。",L"The game of shogi is over: you lost ({0})."),{how}),
                              winner<0?T(L"引き分けの気持ち",L"how you feel about a draw"):T(L"負けて悔しい気持ちか、話し相手をほめる気持ち",L"how sorry you are to have lost, or praise them"),
                              winner<0?L"引き分け":sheResigned?L"投了":L"負けた",
                              Fmt(T(L"{0}と持ち主の将棋の対局が終わった。",L"The shogi game between {0} and the owner is over."),{name}),
                              {T(L"おつかれさま！いい勝負だったね。",L"Well played, both of you!"),T(L"おめでとう！",L"Congratulations!")});
            }
            seen.over=over;
        }

        void Tick()
        {
            auto& t=state.table;
            const ULONGLONG now=GetTickCount64();
            if(Flying())state.nextAt=std::max(state.nextAt,now+120);   // play waits for the piece
            if(!state.said.empty()&&now-state.saidAt>SaidShown){state.said.clear();Redraw();}
            // Her turn coming: she starts to read at once; at the 3D table once she is seated,
            // time for her hand to go out to her square, and now and then to stop and think first.
            if(t.Now()==Phase::HerTurn&&state.plannedFor!=t.Moves()&&(!state.renderer||state.entrance==5)&&!Flying())
            {
                state.plannedFor=t.Moves();
                t.Think();
                state.nextAt=std::max(state.nextAt,now+(state.renderer?HerPlayWait3D:HerPlayWait));
                std::uniform_real_distribution<double> any(0,1);
                if(any(state.sparkRandom)<0.15&&t.Moves()>=6)
                {
                    state.nextAt+=ThinkExtra;
                    state.thinkFor=(HerPlayWait3D+ThinkExtra-ReachLead)/1000.0-0.1;
                    if(!state.words.waiting&&GetTickCount64()-state.words.since>WordsHold)Say(L"考える",2);
                }
            }
            if(now>=state.nextAt&&t.Now()==Phase::HerTurn&&state.plannedFor==t.Moves()&&t.Thought())
            {
                t.Step();
                // A check or a big piece taken now and then gets a little word.
                if(t.Now()==Phase::YourMove&&(t.InCheck()||BigPiece(t.LastCaptured()))&&!state.words.waiting&&GetTickCount64()-state.words.since>WordsHold)
                    Say(L"いい手",2,0.6);
                if(t.Now()==Phase::GameOver)state.nextAt=now+(state.renderer?EndWait3D:EndWait);
                else state.nextAt=now+150;
            }
            UpdateTell();
            Redraw();
            Listen();
            if(!state.renderer)state.voice.Tick(0.04);   // (at the 3D table the model's frame ticks it)
            NoticeWords();
            if(t.Now()==Phase::GameOver&&!state.gameOverTold)
            {
                state.gameOverTold=true;
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

        // A piece drawn flat, its middle at (cx, cy) in a square cw by ch: its point away from
        // its owner (hers turned round).
        void DrawPiece(Graphics& g,float cx,float cy,float cw,float ch,int kind,int who,bool lifted=false)
        {
            const float w=0.84f*cw*KindScale(kind),h=0.90f*ch*KindScale(kind);
            const GraphicsState saved=g.Save();
            g.TranslateTransform(cx,cy-(lifted?0.08f*ch:0));
            if(who==Shogi::Her)g.RotateTransform(180);
            const RectF box(-w/2,-h/2,w,h);
            {
                GraphicsPath shadow;
                PieceOutline(shadow,RectF(box.X+w*0.05f,box.Y+h*0.06f,w,h));
                SolidBrush dark(Color(lifted?90:60,0,0,0));
                g.FillPath(&dark,&shadow);
            }
            GraphicsPath path;
            PieceOutline(path,box);
            LinearGradientBrush wood(PointF(0,box.Y),PointF(0,box.GetBottom()),Color(255,248,220,160),Color(255,228,188,116));
            g.FillPath(&wood,&path);
            Pen edge(Color(255,150,108,56),std::max(1.0f,w*0.03f));
            g.DrawPath(&edge,&path);
            FaceInk(g,RectF(box.X,box.Y+h*0.04f,w,h),FaceSlot(kind,who),false);
            g.Restore(saved);
        }

        void PaintEnd(Graphics& g,float cw,float ch,float s)
        {
            auto& t=state.table;
            SolidBrush shade(Color(150,0,0,0));
            g.FillRectangle(&shade,0.0f,0.0f,cw,ch);
            const float w=std::min(cw-40*s,600*s),h=std::min(ch-40*s,300*s);
            const RectF box((cw-w)/2,(ch-h)/2,w,h);
            FillRound(g,box,14*s,Color(255,40,22,22),Color(255,200,170,90),2*s);
            float y=box.Y+22*s;
            const Color gold(255,240,206,120),white(255,240,240,236),soft(255,200,190,186);
            Text(g,T(L"勝負あり",L"Game over"),RectF(box.X,y,w,44*s),34*s,gold,true,StringAlignmentCenter,StringAlignmentNear,true);
            y+=70*s;
            const int winner=t.Winner();
            if(winner==Shogi::Her)Runs(g,{{HerName()},{T(L"の勝ち",L" wins")}},RectF(box.X,y,w,48*s),32*s,gold);
            else Runs(g,{{winner==Shogi::You?T(L"あなたの勝ち！",L"You win!"):T(L"引き分け",L"A draw")}},RectF(box.X,y,w,48*s),32*s,gold);
            y+=60*s;
            std::wstring how=EndingText();
            if(t.HowEnded()==Shogi::Ending::Resigned)
                how=Fmt(T(L"{0}が投了しました（{1}手）",L"{0} resigned ({1} moves)"),{winner==Shogi::You?HerName():std::wstring(T(L"あなた",L"You")),std::to_wstring(t.Moves())});
            Text(g,how,RectF(box.X,y,w,30*s),19*s,white,false,StringAlignmentCenter);
            Text(g,T(L"Enter：もう一局　Esc：閉じる",L"Enter: play again   Esc: close"),RectF(box.X,box.GetBottom()-44*s,w,30*s),17*s,soft,false,StringAlignmentCenter);
        }

        // A stand of pieces in hand, drawn flat in box: the kinds held side by side, each with
        // how many (hers from the left, her right; yours from the right).
        void DrawStand(Graphics& g,const RectF& box,int who,float cw,float ch,float s)
        {
            FillRound(g,box,8*s,Color(255,120,74,38),Color(255,70,40,20),1.5f*s);
            const auto& b=state.table.Board();
            const float slot=std::min(box.Width/7,1.45f*cw);
            int shown=0;
            for(int kind:HandOrder)
            {
                const int n=b.Hand(who,kind);
                if(who==Shogi::You)state.handBoxes[static_cast<size_t>(kind)]=RectF();
                if(!n)continue;
                const float x=who==Shogi::Her?box.X+slot*shown:box.GetRight()-slot*(shown+1);
                ++shown;
                const RectF cell(x+2*s,box.Y+3*s,slot-4*s,box.Height-6*s);
                const bool chosen=who==Shogi::You&&state.selected==HandBase+kind;
                if(chosen)FillRound(g,cell,6*s,Color(120,240,206,120));
                else if(who==Shogi::You&&state.hover==HandBase+kind&&YourMoveNow()&&Movable(HandBase+kind))FillRound(g,cell,6*s,Color(60,240,206,120));
                const float pw=std::min(cw,cell.Width*0.72f);
                DrawPiece(g,cell.X+pw*0.55f,cell.Y+cell.Height/2,pw,std::min(ch,cell.Height*1.05f),kind,who,chosen);
                if(n>1)Text(g,Fmt(L"×{0}",{std::to_wstring(n)}),RectF(cell.X+pw*1.02f,cell.Y,cell.Width-pw,cell.Height),15*s,Color(255,250,236,210),true,StringAlignmentNear,StringAlignmentCenter);
                if(who==Shogi::You)state.handBoxes[static_cast<size_t>(kind)]=cell;
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

        // Where things go: her column at the right (her upper body in 3D, or her face), the
        // board large in the middle of the rest, her stand above it and yours below.
        struct Layout {float her,area,cw,ch,x0,y0,stripH;};
        Layout Place(float W,float H,float s)
        {
            Layout l;
            l.her=std::round(W*HerColumn);
            l.area=W-l.her;
            const float m=BoardMargin;
            l.ch=std::min((H-24*s)/(9+2*m+2*0.95f+0.4f),(l.area-300*s)/((9+2*m)*0.92f));
            l.ch=std::max(l.ch,20*s);
            l.cw=0.92f*l.ch;
            l.stripH=0.95f*l.ch;
            l.x0=std::round(l.area/2-4.5f*l.cw);
            l.y0=std::round(H/2-4.5f*l.ch);
            return l;
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
            const Layout l=Place(W,H,s);
            const float mid=l.area/2;
            auto& t=state.table;
            const auto& b=t.Board();
            const Color white(255,240,240,236),soft(255,206,196,186),gold(255,240,206,120),panel(185,40,18,16),edge(220,170,120,100);
            const Color felt(255,22,74,52);
            // The board's side is felt; her column shows the 3D room with her in it (in 2D,
            // darker felt with her face).
            g.Clear(hud?Color(0,0,0,0):Color(255,16,56,40));
            {
                SolidBrush fill(felt);
                g.FillRectangle(&fill,0.0f,0.0f,l.area,H);
                LinearGradientBrush shade(PointF(l.area-1,0),PointF(l.area+14*s,0),Color(150,0,0,0),Color(0,0,0,0));
                g.FillRectangle(&shade,l.area-1,0.0f,15*s,H);
            }

            // Her: name, first or second and whose turn, at the top of her column.
            const float hx=l.area+10*s,hw=l.her-20*s;
            {
                FillRound(g,RectF(hx,12*s,hw,70*s),10*s,panel,edge,1.2f*s);
                Text(g,HerName(),RectF(hx+12*s,18*s,hw-20*s,28*s),20*s,white,true);
                const bool thinking=t.Now()==Phase::HerTurn&&!t.Thought()&&state.plannedFor==t.Moves();
                Text(g,FirstOf(Shogi::Her)+(t.Now()==Phase::HerTurn?(thinking?T(L"　（考え中…）",L"  (thinking...)"):T(L"　（番）",L"  (to move)")):L""),
                     RectF(hx+12*s,48*s,hw-20*s,26*s),17*s,t.Now()==Phase::HerTurn?gold:soft);
            }
            if(!hud)
            {
                const float side=std::min(hw,240*s);
                const RectF faceBox(hx+(hw-side)/2,96*s,side,side);
                FillRound(g,faceBox,10*s,Color(255,14,50,36),Color(255,120,160,130),1.5f*s);
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
                bubble.Draw(g,x,y,y+std::min(bh*0.5f,26*s),shown);
            }
            // What to do now, early in the game: at the foot of her column.
            if(YourMoveNow()&&t.Moves()<4&&!state.promoting.Valid())
            {
                const RectF box(hx,H-118*s,hw,106*s);
                FillRound(g,box,10*s,panel,edge,1.2f*s);
                Wrapped(g,T(L"自分の駒をクリックし、動かす先をクリックします。持ち駒は盤の下の駒台から（1〜7のキーでも）。Escで選び直し・投了。",
                            L"Click a piece of yours, then where it goes. Pieces in hand are on your stand below the board (or keys 1-7). Esc to choose again or resign."),
                        RectF(box.X+10*s,box.Y+8*s,box.Width-20*s,box.Height-12*s),14*s,gold);
            }

            // The board.
            const float cx=l.cw,cy=l.ch,x0=l.x0,y0=l.y0,m=BoardMargin;
            FillRound(g,RectF(x0-m*cx,y0-m*cy,(9+2*m)*cx,(9+2*m)*cy),6*s,Color(255,222,178,104),Color(255,120,80,40),2*s);
            state.boardOrigin={x0,y0};
            state.cellW=cx;state.cellH=cy;
            const Move last=t.LastMove();
            auto cell=[&](int sq){return RectF(x0+(sq%Shogi::Size)*cx,y0+(sq/Shogi::Size)*cy,cx,cy);};
            if(last.Valid()){SolidBrush lastFill(Color(255,236,166,104));const RectF r=cell(last.to);g.FillRectangle(&lastFill,r.X+1,r.Y+1,r.Width-2,r.Height-2);}
            if(state.selected>=0&&state.selected<Squares&&YourMoveNow()){SolidBrush fill(Color(255,240,206,120));const RectF r=cell(state.selected);g.FillRectangle(&fill,r.X+1,r.Y+1,r.Width-2,r.Height-2);}
            {
                Pen line(Color(255,40,28,16),1.2f*s);
                for(int i=0;i<=Shogi::Size;++i)
                {
                    g.DrawLine(&line,x0+i*cx,y0,x0+i*cx,y0+9*cy);
                    g.DrawLine(&line,x0,y0+i*cy,x0+9*cx,y0+i*cy);
                }
                SolidBrush dot(Color(255,40,28,16));
                for(int px:{3,6})for(int py:{3,6})g.FillEllipse(&dot,x0+px*cx-3*s,y0+py*cy-3*s,6*s,6*s);
            }
            // The files' numbers above and the ranks' at the right, as seen from the one moving
            // first.
            {
                static const wchar_t files[]=L"１２３４５６７８９",ranks[]=L"一二三四五六七八九";
                const Color label(255,190,210,190);
                for(int i=0;i<Shogi::Size;++i)
                {
                    const int file=t.First()==Shogi::You?Shogi::Size-i:i+1,rank=t.First()==Shogi::You?i+1:Shogi::Size-i;
                    Text(g,std::wstring(1,files[file-1]),RectF(x0+i*cx,y0-m*cy,cx,m*cy),std::min(13*s,m*cy*0.9f),Color(255,120,80,40),false,StringAlignmentCenter,StringAlignmentCenter);
                    Text(g,std::wstring(1,ranks[rank-1]),RectF(x0+9*cx+m*cx+2*s,y0+i*cy,22*s,cy),13*s,label,false,StringAlignmentCenter,StringAlignmentCenter);
                }
            }
            if(t.InCheck()&&t.Now()!=Phase::GameOver)
            {
                Pen red(Color(255,220,50,40),3*s);
                const RectF r=cell(b.KingOf(b.ToMove()));
                g.DrawRectangle(&red,r.X+2*s,r.Y+2*s,r.Width-4*s,r.Height-4*s);
            }
            for(int sq=0;sq<Squares;++sq)
            {
                if(!b.At(sq))continue;
                const RectF r=cell(sq);
                DrawPiece(g,r.X+cx/2,r.Y+cy/2,cx,cy,b.KindAt(sq),b.OwnerAt(sq),state.selected==sq&&YourMoveNow());
            }
            if(YourMoveNow())
            {
                for(int to:Targets())
                {
                    const RectF r=cell(to);
                    if(b.At(to)){Pen frame(gold,3*s);g.DrawRectangle(&frame,r.X+3*s,r.Y+3*s,r.Width-6*s,r.Height-6*s);}
                    else {SolidBrush mark(Color(220,240,190,70));const float k=std::min(cx,cy)*0.13f;g.FillEllipse(&mark,r.X+cx/2-k,r.Y+cy/2-k,2*k,2*k);}
                }
                if(state.hover>=0&&state.hover<Squares&&state.hover!=state.selected&&(Movable(state.hover)||state.selected>=0))
                {
                    Pen frame(Color(200,255,236,170),2*s);
                    const RectF r=cell(state.hover);
                    g.DrawRectangle(&frame,r.X+1.5f*s,r.Y+1.5f*s,r.Width-3*s,r.Height-3*s);
                }
                if(state.cursor>=0&&state.hover<0)
                {
                    Pen frame(gold,2.5f*s);
                    const RectF r=cell(state.cursor);
                    g.DrawRectangle(&frame,r.X,r.Y,r.Width,r.Height);
                }
            }
            // The stands: hers above the board, yours below.
            const float boardL=x0-m*cx,boardW=(9+2*m)*cx,gap=0.2f*cy;
            const RectF herStand(boardL,y0-m*cy-gap-l.stripH,boardW,l.stripH),yourStand(boardL,y0+9*cy+m*cy+gap,boardW,l.stripH);
            DrawStand(g,herStand,Shogi::Her,cx,cy,s);
            DrawStand(g,yourStand,Shogi::You,cx,cy,s);

            // The move count and the last move, right of her stand; you, left of yours.
            {
                const float ix=boardL+boardW+8*s,iw=std::max(60*s,l.area-ix-8*s);
                const RectF info(ix,herStand.Y,iw,std::max(l.stripH,58*s));
                FillRound(g,info,10*s,panel,edge,1.2f*s);
                Text(g,Fmt(T(L"{0}手目",L"Move {0}"),{std::to_wstring(t.Moves()+1)}),RectF(info.X+10*s,info.Y+5*s,iw-14*s,24*s),16*s,white,true);
                std::wstring line=t.Moves()>0?t.LastName():L"";
                if(t.InCheck()&&t.Now()!=Phase::GameOver)line+=T(L"　王手",L" check");
                Text(g,line,RectF(info.X+10*s,info.Y+30*s,iw-14*s,22*s),14*s,soft);
                const float yw=std::max(60*s,boardL-16*s),yh=std::max(l.stripH,58*s);
                const RectF you(8*s,yourStand.GetBottom()-yh,yw,yh);
                FillRound(g,you,10*s,panel,edge,1.2f*s);
                Text(g,T(L"あなた",L"You"),RectF(you.X+10*s,you.Y+5*s,yw-14*s,24*s),16*s,white,true);
                Text(g,FirstOf(Shogi::You)+(t.Now()==Phase::YourMove?T(L"（番）",L" (to move)"):L""),RectF(you.X+10*s,you.Y+30*s,yw-14*s,22*s),14*s,t.Now()==Phase::YourMove?gold:soft);
            }
            if(!state.said.empty())
            {
                const float tw=TextWidth(g,state.said,26*s,true)+40*s;
                const RectF bubble(mid-tw/2,H*0.40f,tw,52*s);
                FillRound(g,bubble,14*s,Color(245,255,252,240),Color(255,230,120,60),2*s);
                Text(g,state.said,bubble,26*s,Color(255,200,40,40),true,StringAlignmentCenter,StringAlignmentCenter);
            }

            // Whether to promote: the piece's two faces to choose from.
            if(state.promoting.Valid())
            {
                SolidBrush shade(Color(120,0,0,0));
                g.FillRectangle(&shade,0.0f,0.0f,W,H);
                const RectF box(mid-200*s,H/2-110*s,400*s,220*s);
                FillRound(g,box,14*s,Color(250,40,22,22),Color(255,200,170,90),2*s);
                Text(g,T(L"成りますか？",L"Promote?"),RectF(box.X,box.Y+16*s,box.Width,34*s),24*s,white,true,StringAlignmentCenter);
                const int kind=b.KindAt(state.promoting.from);
                const std::wstring labels[2]={T(L"成る（Y）",L"Promote (Y)"),T(L"成らない（N）",L"Don't (N)")};
                const float gap2=20*s,width=(box.Width-3*gap2)/2;
                for(int i=0;i<2;++i)
                {
                    const RectF button(box.X+gap2+i*(width+gap2),box.Y+62*s,width,140*s);
                    state.promoteButtons[i]=button;
                    FillRound(g,button,10*s,i==0?Color(240,150,46,52):Color(240,46,92,150),Color(255,200,170,90),1.5f*s);
                    DrawPiece(g,button.X+button.Width/2,button.Y+50*s,64*s,70*s,i==0?kind+Shogi::Promoted:kind,Shogi::You);
                    Text(g,labels[i],RectF(button.X,button.Y+100*s,button.Width,28*s),17*s,white,true,StringAlignmentCenter);
                }
            }

            if(EndShown())PaintEnd(g,W,H,s);
            if(state.confirmResign)
            {
                SolidBrush shade(Color(160,0,0,0));
                g.FillRectangle(&shade,0.0f,0.0f,W,H);
                const RectF box(mid-220*s,ch/2.0f-70*s,440*s,140*s);
                FillRound(g,box,14*s,Color(250,40,22,22),Color(255,200,170,90),2*s);
                Text(g,T(L"投了しますか？",L"Resign?"),RectF(box.X,box.Y+22*s,box.Width,36*s),24*s,white,true,StringAlignmentCenter);
                const float gap2=20*s,width=(box.Width-3*gap2)/2;
                const std::wstring labels[2]={T(L"投了する（Y）",L"Resign (Y)"),T(L"続ける（N）",L"Keep playing (N)")};
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

        // What is under (x, y): a square of the board, or a kind on your stand (HandBase +
        // kind); -1: nothing.
        int TargetAt(float x,float y)
        {
            for(int k=Shogi::Pawn;k<=Shogi::Rook;++k)
                if(state.handBoxes[static_cast<size_t>(k)].Width>0&&state.handBoxes[static_cast<size_t>(k)].Contains(x,y))return HandBase+k;
            if(state.cellW<=0)return -1;
            const float fc=(x-state.boardOrigin[0])/state.cellW,fr=(y-state.boardOrigin[1])/state.cellH;
            if(fc<0||fr<0)return -1;
            const int c=static_cast<int>(fc),r=static_cast<int>(fr);
            return r<Shogi::Size&&c<Shogi::Size?r*Shogi::Size+c:-1;
        }

        void OnKey(WPARAM key)
        {
            auto& t=state.table;
            if(key==VK_F1){state.camera={};return;}
            if(state.confirmResign)
            {
                if(key=='Y'){state.confirmResign=false;t.Resign(Shogi::You);Moved();}
                else if(key=='N'||key==VK_ESCAPE){state.confirmResign=false;Redraw();}
                return;
            }
            if(state.promoting.Valid())
            {
                if(key=='Y'||key==VK_RETURN)Promote(true);
                else if(key=='N')Promote(false);
                else if(key==VK_ESCAPE){state.promoting={};Redraw();}
                return;
            }
            if(t.Now()==Phase::GameOver)
            {
                if((key==VK_RETURN||key==VK_SPACE)&&EndShown())Continue();
                else if(key==VK_ESCAPE)Finish();
                return;
            }
            if(key==VK_ESCAPE)
            {
                if(state.selected>=0)state.selected=-1;
                else state.confirmResign=true;
                Redraw();
                return;
            }
            if(!YourMoveNow())return;
            if(key==VK_LEFT||key==VK_RIGHT||key==VK_UP||key==VK_DOWN)
            {
                // A square at a time, from the piece chosen (the last move's square to start).
                if(state.cursor<0)state.cursor=state.selected>=0&&state.selected<Squares?state.selected:t.LastMove().Valid()?t.LastMove().to:Squares/2;
                else
                {
                    int r=state.cursor/Shogi::Size,c=state.cursor%Shogi::Size;
                    r=std::clamp(r+(key==VK_UP?-1:key==VK_DOWN?1:0),0,Shogi::Size-1);
                    c=std::clamp(c+(key==VK_LEFT?-1:key==VK_RIGHT?1:0),0,Shogi::Size-1);
                    state.cursor=r*Shogi::Size+c;
                }
                state.hover=-1;
                Redraw();
            }
            else if((key==VK_RETURN||key==VK_SPACE)&&state.cursor>=0)Click(state.cursor);
            // 1-7: a kind in your hand (pawn to rook).
            else if(key>='1'&&key<='7')Click(HandBase+static_cast<int>(key-'0'));
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
                const int at=TargetAt(x,y);
                if(at!=state.hover){state.hover=at;Redraw();}
                return 0;
            }
            case WM_LBUTTONDOWN:
            {
                const float x=static_cast<float>(GET_X_LPARAM(lParam)),y=static_cast<float>(GET_Y_LPARAM(lParam));
                auto& t=state.table;
                if(state.confirmResign)
                {
                    if(state.quitButtons[0].Contains(x,y)){state.confirmResign=false;t.Resign(Shogi::You);Moved();}
                    else if(state.quitButtons[1].Contains(x,y)){state.confirmResign=false;Redraw();}
                    return 0;
                }
                if(state.promoting.Valid())
                {
                    if(state.promoteButtons[0].Contains(x,y))Promote(true);
                    else if(state.promoteButtons[1].Contains(x,y))Promote(false);
                    return 0;
                }
                if(t.Now()==Phase::GameOver){if(EndShown())Continue();return 0;}
                Click(TargetAt(x,y));
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

    bool ShogiWindowOpen(){return state.window!=nullptr;}
    HWND ShogiWindowHandle(){return state.window;}
    bool ShogiWindowRect(RECT& r)
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
                else t.PlayForYou(0.2);
            }
            state.nextAt=0;
            state.hover=state.cursor=state.selected=-1;
        }
    }

    void ShogiLines(const std::wstring& answer)
    {
        state.prefetching=false;
        if(!state.window)return;
        state.lines.Answered(answer);
        Prefetch();
    }

    bool ShogiAfterword(std::wstring& situation,std::wstring& fallback)
    {
        if(state.afterword.empty())return false;
        situation=std::move(state.afterword);
        fallback=std::move(state.afterwordLine);
        state.afterword.clear();
        state.afterwordLine.clear();
        return true;
    }

    void ShogiHeard(const std::wstring& line,bool done)
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

    void PlayShogiMoves(int moves,float yaw)
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

        // The move made since the board was last seen: the piece goes from its square (or its
        // owner's stand) to its new one in a low arc, and a piece it took goes to the taker's
        // stand after; at a mate, gold sparkles round the king.
        void StartFlights(bool fresh)
        {
            const double now=state.clock;
            auto& t=state.table;
            state.flights.clear();
            if(fresh)return;
            const Move m=t.LastMove();
            if(!m.Valid())return;
            const int who=t.LastWho();
            const auto& b=t.Board();
            Piece from;
            if(m.from>=0)
            {
                from=Resting(m.to);
                from.x=SquareX(m.from);from.z=SquareZ(m.from);
                from.kind=t.LastMoved();
            }
            else from=OnStand(who,m.drop,b.Hand(who,m.drop),b.Hand(who,m.drop)+1);
            const Piece to=Resting(m.to);
            Piece mid=from;
            mid.x=(from.x+to.x)/2;mid.z=(from.z+to.z)/2;mid.yaw=(from.yaw+to.yaw)/2;
            mid.y=std::max(from.y,to.y)+0.55f*CellH();
            state.flights.push_back({m.to,-1,0,{{now,from},{now+Glide*0.5,mid},{now+Glide,to}}});
            double until=now+Glide;
            if(const int taken=t.LastCaptured())
            {
                // It stays on its square until the piece taking it comes down, then goes.
                Piece lying=to;
                lying.kind=taken;
                lying.who=who^1;
                lying.yaw=YawOf(lying.who);
                const int kind=Shogi::Base(taken),n=b.Hand(who,kind);
                const Piece stand=OnStand(who,kind,n-1,n);
                Piece up=lying;
                up.x=(lying.x+stand.x)/2;up.z=(lying.z+stand.z)/2;up.yaw=(lying.yaw+stand.yaw)/2;up.kind=kind;
                up.y=std::max(lying.y,stand.y)+0.6f*CellH();
                const double go=now+Glide*0.9;
                state.flights.push_back({-1,who,kind,{{now,lying},{go,lying},{go+ToStand*0.5,up},{go+ToStand,stand}}});
                until=go+ToStand;
                Due(go+ToStand,MahjongSound::CardPlace);
            }
            state.flightsUntil=std::max(state.flightsUntil,until);
            Due(now+Glide,MahjongSound::CardPlace);
            if(t.Now()==Phase::GameOver&&t.HowEnded()==Shogi::Ending::Mate)state.sparkAt=now+Glide+0.1;
        }
    }

    void TickShogiWindow(MascotModel& model,double dt)
    {
        if(!state.window||!state.renderer)return;
        auto& r=*state.renderer;
        using namespace DirectX;
        const float D=state.upper,top=Top();
        dt=std::min(dt,0.1);
        auto& t=state.table;
        // What changed on the board since the last frame: the piece moved set on its way.
        if(t.Moves()!=state.movesSeen&&(t.Moves()>0||state.entrance==5))
        {
            const bool fresh=t.Moves()==0||state.movesSeen<0||t.Moves()<state.movesSeen;
            if(fresh||t.Moves()==state.movesSeen+1)StartFlights(fresh);
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
            const auto& b=t.Board();
            Sparkle(SquareSpot(b.KingOf(t.Winner()^1)),0.8f);
        }
        // Her hand goes out to the square she will move to, once she has chosen (tried again
        // next frame while a gesture of hers ends).
        if(t.Now()==Phase::HerTurn&&state.entrance==5&&!Flying()&&state.plannedFor==t.Moves()&&state.reachedFor!=t.Moves()&&
           t.Thought()&&GetTickCount64()+ReachLead>=state.nextAt)
        {
            const Move m=t.HerNext();
            if(!m.Valid()||ReachFor(model,SquareSpot(m.to)))state.reachedFor=t.Moves();
        }
        if(state.thinkFor>0&&state.entrance==5){model.EmoteSeated(L"ponder",state.thinkFor);state.thinkFor=0;}
        {
            const bool over=t.Now()==Phase::GameOver;
            if(over&&!state.seenOver&&t.Winner()>=0)
            {
                std::uniform_real_distribution<double> any(0,1);
                state.endGesture=t.Winner()==Shogi::Her?(any(state.sparkRandom)<0.5?L"cheeks":L"clap"):L"oops";
                state.endGestureUntil=state.clock+1.5;
            }
            state.seenOver=over;
            if(!state.endGesture.empty()&&state.entrance==5&&(model.EmoteSeated(state.endGesture,3.2)||state.clock>state.endGestureUntil))
                state.endGesture.clear();
        }
        // What moves: her chair, and the pieces on their way.
        {
            Mesh moving;
            moving.out=BuildChair(state.chairZ);
            for(const auto& f:state.flights)
            {
                Piece p;
                if(PieceAt(f,state.clock,p))AddPiece(moving,p);
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

    std::wstring DescribeShogi()
    {
        if(!state.window)return L"";
        auto& t=state.table;
        std::wstring out=Fmt(L"shogi: entrance {0}, phase {1}, moves {2}, first {3}, winner {4}, ending {5}, flights {6}, last {7}, her depth {8}, score {9}\n",
                             {std::to_wstring(state.entrance),std::to_wstring(static_cast<int>(t.Now())),std::to_wstring(t.Moves()),
                              t.First()==Shogi::You?L"you":L"her",std::to_wstring(t.Winner()),std::to_wstring(static_cast<int>(t.HowEnded())),
                              std::to_wstring(state.flights.size()),t.LastName(),std::to_wstring(t.LastThought().depth),std::to_wstring(t.LastThought().score)});
        return out+(state.words.told.empty()?std::wstring():L"shogi words:\n"+state.words.told);
    }

    bool SaveShogiView(const std::wstring& path)
    {
        return state.renderer&&state.renderer->SaveLastFrame(path);
    }

    bool SaveShogiShot(const std::wstring& path,int moves,std::uint32_t seed)
    {
        GdiplusStartupInput input;
        if(!state.gdiplus)GdiplusStartup(&state.gdiplus,&input,nullptr);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc=DefWindowProcW;
        wc.hInstance=GetModuleHandleW(nullptr);
        wc.lpszClassName=L"MikuMikuDesktopPerchShogiShot";
        RegisterClassExW(&wc);
        const int dpi=static_cast<int>(GetDpiForSystem());
        state.window=CreateWindowExW(0,wc.lpszClassName,L"",WS_OVERLAPPEDWINDOW,0,0,MulDiv(static_cast<int>(BaseWidth)+16,dpi,96),MulDiv(static_cast<int>(BaseHeight)+39,dpi,96),
                                     nullptr,nullptr,wc.hInstance,nullptr);
        state.her=ShogiOpponent{L"ミク"};
        state.table.NewGame(seed,Care,seed%2?Shogi::Her:Shogi::You,0.2);
        PlaySimply(moves);
        // Your move next, when it is: a piece of yours chosen, where it may go shown.
        if(state.table.Now()==Phase::YourMove)
        {
            const auto& b=state.table.Board();
            for(int sq=Squares-1;sq>=0&&state.selected<0;--sq)
                if(b.OwnerAt(sq)==Shogi::You&&b.KindAt(sq)!=Shogi::Pawn&&state.table.MovesFrom(sq).size()>=2)state.selected=sq;
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

    void ShowShogiWindow(HWND owner,const ShogiOpponent& her,std::function<void()> onEnd)
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
        state.nextFirst=-1;
        state.following=false;
        state.lines.Load(L"SHOGILINES",L"shogi",her.ownLinesFile,her.linesFile,her.styleKey,English(),
                         {L"将棋",L"Shogi",L"",L""});
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
        const std::wstring title=std::wstring(T(L"将棋",L"Shogi"))+L" - "+HerName();
        state.window=CreateWindowExW(0,ClassName,title.c_str(),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                                     work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,
                                     nullptr,nullptr,instance,nullptr);
        if(CreateView())PlaceView();
        SetTimer(state.window,TimerId,40,nullptr);
        ShowWindow(state.window,SW_SHOWNORMAL);
        SetForegroundWindow(state.window);
    }
}
