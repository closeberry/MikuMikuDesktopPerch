#define NOMINMAX
#include "MahjongWindow.hpp"
#include "Mahjong.hpp"
#include "MahjongSound.hpp"
#include "Log.hpp"
#include "MascotModel.hpp"
#include "MascotRenderer.hpp"
#include "Text.hpp"
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
        using Mahjong::Piece;
        using Mahjong::Phase;
        constexpr wchar_t ClassName[]=L"MikuMikuDesktopPerchMahjong";
        constexpr wchar_t ViewClassName[]=L"MikuMikuDesktopPerchMahjongView";
        constexpr UINT_PTR TimerId=1;
        // How carefully she plays (Mahjong::Table::NewGame): a little less as you grow closer, and
        // after games you lost in a row.
        constexpr double Care=0.85,LessPerBond=0.03,LessPerLoss=0.08,LeastCare=0.4;
        // Her pace: before her draw, before her discard, after a call of hers, before a hand's
        // end is shown, and your discards after riichi.
        constexpr ULONGLONG HerDrawWait=350,HerDiscardWait=450,CallWait=900,EndWait=900,RiichiDiscardWait=350;
        constexpr ULONGLONG SaidShown=1600;
        constexpr ULONGLONG RiichiShowDrawn=450;   // after riichi, the drawn tile is seen this long before it goes
        constexpr ULONGLONG ReachLead=600;         // her hand starts out this long before her discard (it is out 0.6 s in)
        constexpr ULONGLONG HerDiscardWait3D=650;  // at the 3D table: time for her hand to go out after her draw
        constexpr double FadeSeconds=0.6;          // her discard fading into her river
        constexpr ULONGLONG ThinkExtra=2200;       // her discard later by this when she stops to think (time to settle into it and out)
        constexpr ULONGLONG EndWait3D=3000;        // at the 3D table: a won hand's score waits for her reaction
        // Sizes in logical pixels (at 96 dpi, at the size the window opens).
        constexpr float BaseWidth=1180,BaseHeight=840;
        constexpr float HandW=46,HandH=62,RiverW=30,RiverH=40,SmallW=26,SmallH=35,BackW=26,BackH=36;
        constexpr int RiverRow=13;          // two rows hold a river (24 draws each)

        enum class Act {Tsumo,Riichi,Kan,Ron,Pon,OpenKan,Pass,LetGo,CancelRiichi};
        struct Button {Act act;int kind=-1;std::wstring label;RectF box;};

        struct State
        {
            HWND window=nullptr;
            ULONG_PTR gdiplus=0;
            MahjongOpponent her;
            std::unique_ptr<Bitmap> face;
            std::function<void()> onEnd;
            Mahjong::Table table;
            int lost=0;   // games you lost in a row
            int selected=-1,hover=-1;
            bool riichiMode=false,confirmQuit=false;
            ULONGLONG nextAt=0;
            int callsSeen=0;
            std::wstring said;
            ULONGLONG saidAt=0;
            std::vector<RectF> handBoxes;   // by hand index (the drawn tile last)
            std::vector<Button> buttons;
            ULONGLONG autoAt=0;              // after riichi: when the drawn tile goes by itself
            std::unique_ptr<Bitmap> canvas;  // drawn into, then the changed part shown
            // Her at the table in 3D (none: her face and tiles drawn in 2D at the top).
            HWND view=nullptr;
            std::unique_ptr<MascotRenderer> renderer;
            // Her coming to the table: 0 not yet, 1 walking in before her chair (pulled back), 2
            // turning to you, 3 sitting down, 4 moving in with the chair, 5 seated. Where she is along the table, the way she faces.
            int entrance=0;
            float herX=0,herZ=0,herAngle=0,entranceTime=0,chairZ=0;
            // The camera, turned as in MMD: around where it looks (yaw, pitch in radians), how
            // far, and where it looks from the default (the last two in her upper body's lengths).
            struct Camera {float yaw=0,pitch=0.40f,distance=3.0f,x=0,y=0,z=0;} camera;
            POINT dragFrom{};
            int dragging=0;                  // 0, or the button held: 1 turns, 2 moves
            float upper=10,hips=10;          // her standing model: the hips' height, and from there to the head
            std::vector<int> scene;         // what the table's geometry was built from
            std::vector<std::array<std::array<float,3>,8>> handCorners;   // your tiles on the table (the drawn one last), to find them on screen
            // Where her tiles are on the table (world units, on top of each), as last built: each
            // river's last tile and the place of its next, her drawn tile, her newest meld.
            std::array<std::array<float,3>,2> lastRiverSpot{},nextRiverSpot{};
            std::array<float,3> herDrawnSpot{},herMeldSpot{};
            std::vector<std::array<float,3>> herHandSpots;   // on top of each of her standing tiles (the drawn one last)
            std::array<float,3> touched{};   // the tile of hers she reached for (it vanishes from there)
            // Her newest river tile fading in (left out of the table, drawn each frame) since
            // fadeSince (seconds of the clock below), where and how it lies.
            bool fading=false;
            double fadeSince=0,clock=0;
            struct Lying {float x=0,z=0;Piece piece;int turn=0;bool dim=false;} fadingTile;
            // What of hers was seen last frame (sparkles where her tiles come), and the river
            // she reached for last (one reach a discard).
            int seenRiver=-1,seenMelds=-1;
            bool seenDrawn=false;
            size_t reachedRiver=static_cast<size_t>(-1);
            // Sparkles where her tiles appear (her hand cannot always reach: the tiles warp).
            struct Spark {std::array<float,3> at,velocity;float age,life,size,angle,spin;std::array<float,4> color;bool glow;};
            std::vector<Spark> sparks;
            std::mt19937 sparkRandom{12345};
            bool testReach=false;            // diagnostics: reach once seated
            // Her gestures at the table: thinking before a discard (for seconds, 0: none), and
            // at a hand's end (her joy or "oops"), asked for until playing or given up.
            double thinkFor=0;
            std::wstring endGesture;
            double endGestureUntil=0;
            bool seenOver=false;
            double testVanishAt=0;           // and then (on the clock) the tile goes
            std::wstring testGesture;        // diagnostics: a gesture of hers once seated
            // What the table was when last heard (sounds for what changed since).
            struct Heard {bool valid=false,youDrawn=false,herDrawn=false,over=false,endShown=false,gameOver=false;size_t youRiver=0,herRiver=0,youMelds=0,herMelds=0,kans=0;} heard;
        } state;

        // Where the parts of the window go, in logical pixels: her side on top, her river, the
        // round, your river, your hand (with the 3D table, only your hand: the rest is on it).
        struct Places {float top,herRiver,info,yourRiver,hand;};
        Places Layout()
        {
            Places p;
            if(state.renderer){p={0,0,0,0,BaseHeight-118};return p;}
            p.top=112;
            p.herRiver=p.top+6;
            p.info=p.herRiver+2*(RiverH+3)+8;
            p.yourRiver=p.info+64+10;
            p.hand=p.yourRiver+2*(RiverH+3)+70;
            return p;
        }

        float Scale()
        {
            if(!state.window)return 1;
            const float dpi=static_cast<float>(GetDpiForWindow(state.window))/96.0f;
            RECT client{};
            GetClientRect(state.window,&client);
            const float grow=std::min(static_cast<float>(client.right)/(BaseWidth*dpi),static_cast<float>(client.bottom)/(BaseHeight*dpi));
            return dpi*std::clamp(grow,0.6f,2.2f);
        }

        // Parts of the window to draw again: her side at the top, the rivers and the round in the
        // middle, your hand and the buttons at the bottom.
        enum Part {Top=1,Middle=2,Bottom=4,Everything=7};
        void Redraw(int parts)
        {
            if(!state.window||!parts)return;
            if(parts==Everything||state.renderer){InvalidateRect(state.window,nullptr,FALSE);return;}
            RECT client{};
            GetClientRect(state.window,&client);
            const float s=Scale();
            const Places at=Layout();
            const LONG top=static_cast<LONG>(at.top*s),bottom=static_cast<LONG>((at.hand-72)*s);
            const RECT bands[]={{0,0,client.right,top},{0,top,client.right,bottom},{0,bottom,client.right,client.bottom}};
            for(int i=0;i<3;++i)if(parts&(1<<i))InvalidateRect(state.window,&bands[i],FALSE);
        }

        // The result screen's brush hand (res/mahjong/brush.ttf: Yuji Syuku, cut down to the
        // characters this game writes; licenses/YujiSyuku-OFL.txt), null when it cannot be loaded.
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

        const FontFamily* Family()
        {
            static FontFamily gothic(L"Yu Gothic UI");
            return gothic.IsAvailable()?&gothic:FontFamily::GenericSansSerif();
        }

        // brush: in the brush hand (at its own weight; its characters only, so never a name).
        void Text(Graphics& g,const std::wstring& text,const RectF& box,float size,Color color,bool bold=false,
                  StringAlignment across=StringAlignmentNear,StringAlignment down=StringAlignmentNear,bool brush=false)
        {
            const FontFamily* family=brush&&Brush()?Brush():Family();
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
            const FontFamily* family=brush&&Brush()?Brush():Family();
            if(family!=Family()){bold=false;size*=1.08f;}
            Font font(family,size,bold?FontStyleBold:FontStyleRegular,UnitPixel);
            RectF box;
            StringFormat format;
            format.SetFormatFlags(StringFormatFlagsMeasureTrailingSpaces);   // the gaps between Runs' pieces count
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

        // The tiles' faces (res/mahjong/tiles.png): TileSide by
        // TileTop each, ten across, the 34 kinds in order and then the red fives of each suit.
        constexpr int TileSide=150,TileTop=200,AtlasColumns=10;

        Bitmap* Faces()
        {
            static std::unique_ptr<Bitmap> atlas=[]{
                std::unique_ptr<Bitmap> loaded;
                const HRSRC info=FindResourceW(nullptr,L"TILES",L"MAHJONGTILES");
                const HGLOBAL data=info?LoadResource(nullptr,info):nullptr;
                if(!data)return loaded;
                IStream* stream=SHCreateMemStream(static_cast<const BYTE*>(LockResource(data)),SizeofResource(nullptr,info));
                if(!stream)return loaded;
                std::unique_ptr<Bitmap> read(Bitmap::FromStream(stream));
                stream->Release();
                // A copy of its own (GDI+ keeps reading the stream it was made from).
                if(read&&read->GetLastStatus()==Ok)loaded.reset(read->Clone(0,0,read->GetWidth(),read->GetHeight(),PixelFormat32bppPARGB));
                return loaded;
            }();
            return atlas.get();
        }

        // A tile's face at w by h pixels, made once from the atlas (scaling it each time it is
        // drawn is slow).
        Bitmap* Face(int at,int w,int h)
        {
            static std::map<std::tuple<int,int,int>,std::unique_ptr<Bitmap>> made;
            Bitmap* faces=Faces();
            if(!faces||w<1||h<1)return nullptr;
            if(made.size()>600)made.clear();   // sizes the window had before
            auto& face=made[{at,w,h}];
            if(!face)
            {
                face=std::make_unique<Bitmap>(w,h,PixelFormat32bppPARGB);
                Graphics g(face.get());
                g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                g.SetPixelOffsetMode(PixelOffsetModeHalf);
                g.DrawImage(faces,Rect(0,0,w,h),at%AtlasColumns*TileSide,at/AtlasColumns*TileTop,TileSide,TileTop,UnitPixel);
            }
            return face.get();
        }

        // A tile standing up at (x, y), w by h: its face (or its back).
        void DrawTile(Graphics& g,float x,float y,float w,float h,const Piece& p,bool faceUp=true,bool dim=false)
        {
            const float r=w*0.14f;
            FillRound(g,RectF(x+w*0.05f,y+h*0.06f,w,h),r,Color(70,0,0,0));
            if(!faceUp)
            {
                FillRound(g,RectF(x,y,w,h),r,Color(255,214,120,52),Color(255,150,80,30));
                FillRound(g,RectF(x+w*0.12f,y+h*0.1f,w*0.76f,h*0.8f),r*0.6f,Color(255,232,146,70));
                return;
            }
            const int at=p.red&&p.kind<Mahjong::East?Mahjong::Kinds+p.kind/9:p.kind;
            if(Bitmap* face=Face(at,static_cast<int>(std::lround(w)),static_cast<int>(std::lround(h))))
                g.DrawImage(face,RectF(x,y,static_cast<REAL>(face->GetWidth()),static_cast<REAL>(face->GetHeight())));
            else
            {
                FillRound(g,RectF(x,y,w,h),r,Color(255,251,248,240),Color(255,170,160,140));
                Text(g,Mahjong::TileText(p.kind),RectF(x,y,w,h),h*0.3f,Color(255,30,30,36),true,StringAlignmentCenter,StringAlignmentCenter);
            }
            if(dim)FillRound(g,RectF(x,y,w,h),r,Color(130,30,30,40));
        }

        // Lying on its side (a riichi discard): the box is h wide, w tall.
        void DrawSideways(Graphics& g,float x,float y,float w,float h,const Piece& p,bool dim=false)
        {
            const GraphicsState saved=g.Save();
            g.TranslateTransform(x+h/2,y+w/2);
            g.RotateTransform(-90);
            DrawTile(g,-w/2,-h/2,w,h,p,true,dim);
            g.Restore(saved);
        }

        // A river, rows of RiverRow from (x, y); taken tiles are faded.
        void DrawRiver(Graphics& g,const std::vector<Mahjong::RiverTile>& river,float x,float y,float s)
        {
            float cx=x,cy=y;
            int inRow=0;
            for(const auto& r:river)
            {
                if(inRow==RiverRow){cx=x;cy+=(RiverH+3)*s;inRow=0;}
                if(r.sideways){DrawSideways(g,cx,cy+(RiverH-RiverW)*s,RiverW*s,RiverH*s,r.piece,r.taken);cx+=(RiverH+1)*s;}
                else {DrawTile(g,cx,cy,RiverW*s,RiverH*s,r.piece,true,r.taken);cx+=(RiverW+1)*s;}
                ++inRow;
            }
        }

        // Melds from the right edge leftward; returns their left edge.
        float DrawMelds(Graphics& g,const std::vector<Mahjong::Meld>& melds,float right,float y,float s,float w,float h)
        {
            float x=right;
            for(auto m=melds.rbegin();m!=melds.rend();++m)
            {
                const int n=m->kind==Mahjong::MeldKind::Pon?3:4;
                x-=n*(w+s)+8*s;
                for(int i=0;i<n;++i)
                {
                    const bool back=m->kind==Mahjong::MeldKind::ClosedKan&&(i==0||i==3);
                    DrawTile(g,x+i*(w+s),y,w,h,{m->tile,false},!back);
                }
            }
            return x;
        }

        std::wstring HerName(){return state.her.name.empty()?std::wstring(T(L"相手",L"Her")):state.her.name;}

        std::wstring Points(int n){return std::to_wstring(n)+T(L"点",L"");}

        std::wstring Signed(int n){return (n>0?L"+":L"")+std::to_wstring(n);}

        bool Waiting(){return state.nextAt>GetTickCount64();}

        void UpdateButtons()
        {
            auto& t=state.table;
            const auto& you=t.Of(Mahjong::You);
            std::vector<Button> b;
            if(!Waiting()&&t.Now()==Phase::YourTurn)
            {
                if(state.riichiMode)b.push_back({Act::CancelRiichi,-1,T(L"やめる",L"Cancel")});
                else
                {
                    if(t.CanTsumo())b.push_back({Act::Tsumo,-1,T(L"ツモ",L"Tsumo")});
                    if(!t.RiichiDiscards().empty())b.push_back({Act::Riichi,-1,T(L"リーチ",L"Riichi")});
                    for(int k:t.KanKinds())b.push_back({Act::Kan,k,std::wstring(T(L"カン ",L"Kan "))+Mahjong::TileText(k)});
                    if(you.riichi&&t.CanTsumo())b.push_back({Act::LetGo,-1,T(L"ツモ切り",L"Discard it")});
                }
            }
            else if(!Waiting()&&t.Now()==Phase::YourCall)
            {
                if(t.CanRon())b.push_back({Act::Ron,-1,T(L"ロン",L"Ron")});
                if(t.CanPon())b.push_back({Act::Pon,-1,T(L"ポン",L"Pon")});
                if(t.CanOpenKan())b.push_back({Act::OpenKan,-1,T(L"カン",L"Kan")});
                b.push_back({Act::Pass,-1,T(L"見送り",L"Pass")});
            }
            state.buttons=std::move(b);
        }

        LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam);
        int HandAt(float x,float y);

        // Whether something is drawn over the table at (x, y) (your hand, a button, the words).
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
            // The camera as in MMD: drag with the left or right button to turn it around her,
            // with the middle one (or Shift) to move it, the wheel to come nearer; a double-click
            // puts it back. A click on what is drawn over the table (your hand, the buttons) is
            // the window's; any click gives the window the keys.
            auto& c=state.camera;
            if((message==WM_LBUTTONDOWN||message==WM_LBUTTONDBLCLK)&&
               (OverHud(lParam)||HandAt(static_cast<float>(GET_X_LPARAM(lParam)),static_cast<float>(GET_Y_LPARAM(lParam)))>=0))
            {
                SetFocus(state.window);
                return Proc(state.window,WM_LBUTTONDOWN,wParam,lParam);
            }
            switch(message)
            {
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
                SetFocus(state.window);
                state.dragging=message==WM_MBUTTONDOWN||(wParam&MK_SHIFT)?2:1;
                state.dragFrom={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
                SetCapture(hwnd);
                return 0;
            case WM_MOUSEMOVE:
                if(!state.dragging)return Proc(state.window,message,wParam,lParam);   // pointing at your tiles
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
            case WM_LBUTTONUP:
            case WM_RBUTTONUP:
            case WM_MBUTTONUP:
                state.dragging=0;
                ReleaseCapture();
                return 0;
            case WM_CAPTURECHANGED:
                state.dragging=0;
                return 0;
            case WM_MOUSEWHEEL:
                c.distance=std::clamp(c.distance*std::pow(0.9f,GET_WHEEL_DELTA_WPARAM(wParam)/120.0f),0.5f,4.5f);   // inside the room
                return 0;
            case WM_LBUTTONDBLCLK:
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
                Log(L"mahjong: resize: %s",error.c_str());
            InvalidateRect(state.window,nullptr,FALSE);   // the picture over the table, at the new size
        }

        void ReleaseView()
        {
            if(state.renderer){state.renderer->WaitIdle();state.renderer.reset();Log(L"mahjong: 3D view released");}
            state.view=nullptr;
        }

        // The tiles' faces for the 3D table: the atlas on ivory (its clear corners filled), with
        // its mipmaps.
        ImageRgba8 FacesImage()
        {
            ImageRgba8 image;
            Bitmap* faces=Faces();
            if(!faces)return image;
            const int w=static_cast<int>(faces->GetWidth()),h=static_cast<int>(faces->GetHeight());
            BitmapData bits{};
            Rect whole(0,0,w,h);
            if(faces->LockBits(&whole,ImageLockModeRead,PixelFormat32bppARGB,&bits)!=Ok)return image;
            image.levels.resize(1);
            auto& level=image.levels[0];
            level.width=static_cast<unsigned>(w);level.height=static_cast<unsigned>(h);
            level.pixels.resize(static_cast<size_t>(w)*h*4);
            const int ivory[3]={251,248,240};
            for(int y=0;y<h;++y)
            {
                const BYTE* from=static_cast<const BYTE*>(bits.Scan0)+static_cast<size_t>(y)*bits.Stride;
                BYTE* to=level.pixels.data()+static_cast<size_t>(y)*w*4;
                for(int x=0;x<w;++x,from+=4,to+=4)
                {
                    const int a=from[3];
                    for(int k=0;k<3;++k)to[k]=static_cast<BYTE>((from[2-k]*a+ivory[k]*(255-a))/255);
                    to[3]=255;
                }
            }
            faces->UnlockBits(&bits);
            GenerateMips(image);
            return image;
        }

        // Her model drawn at the table, over the whole window: a renderer of its own (no blur
        // or self-shadow, opaque), your hand and the words laid over it. False: 2D only.
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
                Log(L"mahjong: no 3D view (%s)",error.c_str());
                renderer->WaitIdle();
                DestroyWindow(state.view);
                state.view=nullptr;
                return false;
            }
            renderer->SetLight(20,45);
            if(!renderer->SetGeometryTexture(FacesImage()))Log(L"mahjong: no tile faces on the 3D table");
            // Her size, standing as she came from the desktop: the table is made to fit her.
            std::array<float,3> lo{},hi{},legLo{},legHi{},head{};
            if(state.her.model->BoneBounds(lo,hi)&&state.her.model->LegBounds(legLo,legHi)&&state.her.model->HeadPosition(head)&&head[1]>legHi[1])
            {
                state.hips=std::max(legHi[1]-lo[1],0.1f);
                state.upper=head[1]-legHi[1];
            }
            Log(L"mahjong: 3D view %ux%u on %s (hips %.1f, upper body %.1f)",w,h,renderer->AdapterName().c_str(),state.hips,state.upper);
            state.renderer=std::move(renderer);
            state.entrance=0;
            state.scene.clear();
            state.camera={};
            state.dragging=0;
            state.seenRiver=-1;
            state.seenMelds=-1;
            state.seenDrawn=false;
            state.reachedRiver=static_cast<size_t>(-1);
            state.sparks.clear();
            state.fading=false;
            state.thinkFor=0;
            state.endGesture.clear();
            state.seenOver=false;
            return true;
        }

        bool CanDiscard(int index);

        bool YourTurnNow(){return state.table.Now()==Phase::YourTurn&&!Waiting();}

        // Your tile at index (the drawn one at the hand's size) lifted: pointed at or chosen (not
        // after riichi, when the drawn tile goes by itself).
        bool Lifted(int i)
        {
            return YourTurnNow()&&CanDiscard(i)&&(i==state.hover||(i==state.selected&&!state.table.Of(Mahjong::You).riichi));
        }

        // Dimmed while choosing the riichi discard: one that does not leave you waiting.
        bool Dimmed(int i){return state.riichiMode&&YourTurnNow()&&!CanDiscard(i);}

        // What the table's geometry shows: the hands, the rivers, the melds and riichi.
        std::vector<int> Scene()
        {
            std::vector<int> out;
            for(int who:{Mahjong::You,Mahjong::Her})
            {
                const auto& seat=state.table.Of(who);
                out.push_back(static_cast<int>(seat.hand.size())*2+(seat.hasDrawn?1:0));
                out.push_back(seat.riichi?1:0);
                for(const auto& r:seat.river)out.push_back(((r.piece.kind*2+(r.piece.red?1:0))*2+(r.sideways?1:0))*2+(r.taken?1:0));
                out.push_back(-1);
                for(const auto& m:seat.melds)out.push_back(static_cast<int>(m.kind)*64+m.tile);
                out.push_back(-2);
            }
            out.push_back(state.fading?1:0);
            const auto& you=state.table.Of(Mahjong::You);
            const int n=static_cast<int>(you.hand.size());
            for(int i=0;i<n+(you.hasDrawn?1:0);++i)
            {
                const Piece& p=i<n?you.hand[i]:you.drawn;
                out.push_back(((p.kind*2+(p.red?1:0))*2+(Lifted(i)?1:0))*2+(Dimmed(i)?1:0));
            }
            return out;
        }

        constexpr float Wood=0.02f;   // the geometry shader's wood (its pattern 0, look 1)

        // Plain lit geometry being made: quads and boxes, wood (its colour's alpha Wood) grained
        // along x, z or y (grain 0, 1, 2), the grain and the boards' seams grainScale apart as
        // the shader draws them for 1.
        struct Mesh
        {
            using V=std::array<float,3>;
            std::vector<MascotRenderer::GeometryVertex> out;
            int grain=0;
            float grainScale=1;
            // uv: the geometry's texture at a, b, c, d (u, v each).
            void Quad(const V& a,const V& b,const V& c,const V& d,const V& n,const float (&col)[4],const float* uv=nullptr)
            {
                const V* p[4]={&a,&b,&c,&d};
                for(int k:{0,1,2,0,2,3})
                {
                    const V& q=*p[k];
                    MascotRenderer::GeometryVertex v{{q[0],q[1],q[2]},{n[0],n[1],n[2]},{col[0],col[1],col[2],col[3]}};
                    if(col[3]==Wood)
                    {
                        const float along=q[grain==0?0:grain==1?2:1],up=q[grain==2?0:1],across=q[grain==1?0:2];
                        v.wall[0]=along/grainScale;v.wall[1]=up/grainScale;v.wall[3]=across/grainScale;
                    }
                    if(uv){v.uv[0]=uv[k*2];v.uv[1]=uv[k*2+1];v.uv[2]=1;}
                    out.push_back(v);
                }
            }
            // Without a bottom (nothing is seen from below); topFace false: without a top either.
            void Box(float x0,float y0,float z0,float x1,float y1,float z1,const float (&c)[4],bool topFace=true)
            {
                if(topFace)Quad({x0,y1,z0},{x1,y1,z0},{x1,y1,z1},{x0,y1,z1},{0,1,0},c);
                Quad({x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},{0,0,-1},c);
                Quad({x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1},{0,0,1},c);
                Quad({x0,y0,z0},{x0,y0,z1},{x0,y1,z1},{x0,y1,z0},{-1,0,0},c);
                Quad({x1,y0,z0},{x1,y0,z1},{x1,y1,z1},{x1,y1,z0},{1,0,0},c);
            }
        };

        // Her chair's seat: how deep it is, and how far it reaches in front of where she sits
        // (her hips sit as on a window's edge, her thighs forward over it). Set back by half its
        // depth from reaching her knees, so they stay clear of it.
        float SeatDepth(){return 0.27f*state.hips+0.22f*state.upper;}
        float SeatFront(){return 0.27f*state.hips-SeatDepth()/2;}

        // Where she sits and the floor she walks on (world units: the seat at y = 0, she faces -z).
        // A chair's depth further back than up against the table, so she can see her own tiles
        // and her hands clear them.
        float SeatZ(){return 0.15f*state.upper+SeatDepth();}
        float FloorY(){return -0.5f*state.hips;}


        // A tile's face on the atlas (u, v at its corners: top left, top right, bottom right,
        // bottom left), turned by turn corners.
        void FaceUv(const Piece& p,float (&uv)[8],int turn)
        {
            const int at=p.red&&p.kind<Mahjong::East?Mahjong::Kinds+p.kind/9:p.kind;
            Bitmap* faces=Faces();
            const float aw=faces?static_cast<float>(faces->GetWidth()):1,ah=faces?static_cast<float>(faces->GetHeight()):1;
            const float u0=(at%AtlasColumns*TileSide+2)/aw,u1=((at%AtlasColumns+1)*TileSide-2)/aw;
            const float v0=(at/AtlasColumns*TileTop+2)/ah,v1=((at/AtlasColumns+1)*TileTop-2)/ah;
            const float corners[8]={u0,v0,u1,v0,u1,v1,u0,v1};
            for(int k=0;k<4;++k){uv[k*2]=corners[((k+turn)%4)*2];uv[k*2+1]=corners[((k+turn)%4)*2+1];}
        }

        // A tile lying face up (or down) on the table, its middle at (x, z): turn 0 with its top
        // away from you, 2 toward you (hers), 1 and 3 on its side (a riichi discard, yours and
        // hers). appear below 1: coming into being, flat and the felt's colour at 0.
        void LyingTile(Mesh& m,float x,float z,const Piece& p,int turn,bool faceUp,bool dim,float appear=1)
        {
            using V=std::array<float,3>;
            const float D=state.upper,top=0.12f*state.hips+0.30f*D;
            const float tw=0.085f*D,tl=0.115f*D,tt=0.06f*D*std::max(appear,0.05f);
            const float felt[3]={0.09f,0.33f,0.22f};
            auto tint=[&](float r,float g,float b,float (&c)[4]){
                const float rgb[3]={r,g,b};
                for(int k=0;k<3;++k)c[k]=felt[k]+(rgb[k]-felt[k])*appear;
                c[3]=1;
            };
            float back[4],ivory[4],face[4];
            tint(0.84f,0.47f,0.20f,back);
            tint(0.95f,0.92f,0.84f,ivory);
            dim?tint(0.62f,0.62f,0.64f,face):tint(1,1,1,face);
            const float hx=(turn%2?tl:tw)/2,hz=(turn%2?tw:tl)/2;
            const float y=top+tt*0.4f;
            m.Box(x-hx,top,z-hz,x+hx,y,z+hz,back);
            m.Box(x-hx,y,z-hz,x+hx,top+tt,z+hz,faceUp?ivory:back,false);
            // The face: its corners from the far left round, as seen from your side.
            const V a{x-hx,top+tt,z+hz},b{x+hx,top+tt,z+hz},c{x+hx,top+tt,z-hz},d{x-hx,top+tt,z-hz};
            if(!faceUp){m.Quad(d,c,b,a,{0,1,0},back);return;}
            float uv[8];
            // Corner k of the face takes the picture's corner k+turn (0 top left, clockwise).
            FaceUv(p,uv,turn);
            m.Quad(a,b,c,d,{0,1,0},face,uv);
        }

        // Her chair, where she sits at z: a cushioned seat from under her knees to behind her,
        // a back with two rails, four legs. Drawn on its own, as it moves in with her when she
        // sits down.
        std::vector<MascotRenderer::GeometryVertex> BuildChair(float z)
        {
            const float D=state.upper,floor=FloorY(),cw=0.34f*D;
            z-=SeatFront();   // the seat's front edge
            const float back=SeatDepth();   // to the back of the seat
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
                m.Box(lx,floor,z+back-0.06f*D,lx+0.06f*D,0.75f*D,z+back,legs);   // the back legs rise into the back
            }
            m.grain=0;
            for(float y:{0.30f*D,0.62f*D})m.Box(-cw,y,z+back-0.05f*D,cw,y+0.09f*D,z+back-0.01f*D,frame);
            return std::move(m.out);
        }

        // The table's solid wood (a thick frame round the felt, a skirt, four legs) and the room
        // round it (her chair is BuildChair's): plank floor, plaster walls over a wooden wainscot with
        // pillars, a dark wooden ceiling, a lit shoji screen behind her, a hanging scroll and a
        // sideboard at the sides, a lamp over the table.
        template<class Box,class Quad>
        void BuildRoom(Box& box,Quad& quad,int& grain,float& grainScale,float x0,float x1,float zNear,float zFar,float zc)
        {
            using V=std::array<float,3>;
            const float D=state.upper,top=0.12f*state.hips+0.30f*D,floor=FloorY();
            const float frame[4]={0.34f,0.19f,0.10f,Wood},skirt[4]={0.26f,0.14f,0.07f,Wood},planks[4]={0.46f,0.32f,0.20f,Wood};
            const float trim[4]={0.30f,0.18f,0.10f,Wood},beams[4]={0.20f,0.12f,0.07f,Wood};
            const float plaster[4]={0.74f,0.68f,0.58f,1},paper[4]={1.0f,0.93f,0.78f,0.25f};
            const float brass[4]={0.80f,0.62f,0.30f,1},middle[4]={0.16f,0.10f,0.06f,1};

            // The table: a frame standing proud of the felt, a skirt under it, legs at the corners.
            grainScale=0.35f;
            const float rw=0.11f*D,frameTop=top+0.04f*D,frameBottom=top-0.11f*D,skirtBottom=top-0.32f*D;
            grain=0;
            box(x0-rw,frameBottom,zFar-rw,x1+rw,frameTop,zFar,frame);
            box(x0-rw,frameBottom,zNear,x1+rw,frameTop,zNear+rw,frame);
            grain=1;
            box(x0-rw,frameBottom,zFar,x0,frameTop,zNear,frame);
            box(x1,frameBottom,zFar,x1+rw,frameTop,zNear,frame);
            grain=0;
            box(x0-rw+0.02f*D,skirtBottom,zFar-rw+0.02f*D,x1+rw-0.02f*D,frameBottom,zFar-rw+0.05f*D,skirt);
            box(x0-rw+0.02f*D,top-0.16f*D,zNear+rw-0.05f*D,x1+rw-0.02f*D,frameBottom,zNear+rw-0.02f*D,skirt);   // shallow on her side: her thighs go under it
            grain=1;
            box(x0-rw+0.02f*D,skirtBottom,zFar-rw+0.02f*D,x0-rw+0.05f*D,frameBottom,zNear+rw-0.02f*D,skirt);
            box(x1+rw-0.05f*D,skirtBottom,zFar-rw+0.02f*D,x1+rw-0.02f*D,frameBottom,zNear+rw-0.02f*D,skirt);
            grain=2;
            const float leg=0.15f*D;
            for(float lx:{x0-rw+0.02f*D,x1+rw-0.02f*D-leg})
                for(float lz:{zFar-rw+0.02f*D,zNear+rw-0.02f*D-leg})
                    box(lx,floor,lz,lx+leg,skirtBottom,lz+leg,skirt);
            box(-0.09f*D,top,zc-0.09f*D,0.09f*D,top+0.02f*D,zc+0.09f*D,middle);   // the middle of the table

            // The room: floor, walls, ceiling (big enough for the camera to turn round in).
            const float rx=6.0f*D,zBack=5.0f*D,zFront=-7.5f*D,ceiling=floor+1.9f*(state.hips+state.upper);
            grain=0;grainScale=1;
            quad({-rx,floor,zBack},{rx,floor,zBack},{rx,floor,zFront},{-rx,floor,zFront},{0,1,0},planks);
            quad({-rx,ceiling,zBack},{rx,ceiling,zBack},{rx,ceiling,zFront},{-rx,ceiling,zFront},{0,-1,0},beams);
            const float wain=floor+0.55f*state.hips,proud=0.04f*D;
            auto wall=[&](const V& a,const V& b,const V& n){
                // From a to b along the floor, facing n: plaster over a wooden wainscot, a rail
                // and a skirting board.
                const bool alongX=std::abs(a[0]-b[0])>std::abs(a[2]-b[2]);
                grain=alongX?0:1;grainScale=0.6f;
                quad({a[0],wain,a[2]},{b[0],wain,b[2]},{b[0],ceiling,b[2]},{a[0],ceiling,a[2]},n,plaster);
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
            // Pillars at the corners and along the walls.
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

            // Behind her, between the pillars: shoji lit from outside, a lattice of thin bars.
            {
                const float sx=rx/3-pw/2-0.05f*D,sy0=wain+0.08f*D,sy1=ceiling-0.5f*D,sz=zBack-0.06f*D;
                quad({-sx,sy0,sz},{sx,sy0,sz},{sx,sy1,sz},{-sx,sy1,sz},{0,0,-1},paper);
                grain=0;grainScale=0.4f;
                const float bar=0.025f*D;
                for(int i=0;i<=4;++i)
                {
                    const float y=sy0+(sy1-sy0)*i/4;
                    box(-sx,y-bar,sz-0.05f*D,sx,y+bar,sz-0.005f*D,trim);
                }
                grain=2;
                for(int i=0;i<=8;++i)
                {
                    const float x=-sx+2*sx*i/8;
                    box(x-bar,sy0,sz-0.05f*D,x+bar,sy1,sz-0.005f*D,trim);
                }
            }
            // On her right (your left): a hanging scroll; on her left: a low sideboard.
            {
                const float wx=-rx+0.05f*D,sz=-0.6f*D,h0=wain+0.25f*D,h1=ceiling-0.6f*D;
                const float scroll[4]={0.90f,0.86f,0.76f,1},ink[4]={0.12f,0.10f,0.09f,1},cloth[4]={0.30f,0.36f,0.30f,1};
                quad({wx,h0-0.1f*D,sz-0.62f*D},{wx,h0-0.1f*D,sz+0.62f*D},{wx,h1+0.1f*D,sz+0.62f*D},{wx,h1+0.1f*D,sz-0.62f*D},{1,0,0},cloth);
                quad({wx+0.005f*D,h0,sz-0.5f*D},{wx+0.005f*D,h0,sz+0.5f*D},{wx+0.005f*D,h1,sz+0.5f*D},{wx+0.005f*D,h1,sz-0.5f*D},{1,0,0},scroll);
                // A few strokes of ink down its middle.
                for(int i=0;i<3;++i)
                {
                    const float y=h1-0.35f*D-i*(h1-h0-0.7f*D)/3;
                    quad({wx+0.01f*D,y-0.28f*D,sz-0.06f*D+i*0.03f*D},{wx+0.01f*D,y-0.28f*D,sz+0.06f*D+i*0.03f*D},{wx+0.01f*D,y,sz+0.04f*D},{wx+0.01f*D,y,sz-0.08f*D},{1,0,0},ink);
                }
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

            // The lamp over the table: a cord from the ceiling, a paper shade glowing, a wooden rim.
            {
                const float ly=top+2.4f*D,half=0.28f*D;   // high enough to keep out of the camera's way
                grain=2;grainScale=0.35f;
                box(-0.012f*D,ly+0.36f*D,zc-0.012f*D,0.012f*D,ceiling,zc+0.012f*D,beams);
                const float glow[4]={1.0f,0.86f,0.60f,0.25f};
                box(-half,ly,zc-half,half,ly+0.36f*D,zc+half,glow);
                quad({-half,ly,zc-half},{half,ly,zc-half},{half,ly,zc+half},{-half,ly,zc+half},{0,-1,0},glow);
                grain=0;
                box(-half-0.02f*D,ly+0.33f*D,zc-half-0.02f*D,half+0.02f*D,ly+0.38f*D,zc+half+0.02f*D,frame);
                box(-half-0.02f*D,ly-0.02f*D,zc-half-0.02f*D,half+0.02f*D,ly+0.02f*D,zc+half+0.02f*D,frame);
            }
        }

        // The table and what is on it (world units; she sits facing -Z at the origin, the seat
        // at y = 0): green felt framed in wood; her tiles standing with their backs to you, her
        // melds lying face up at her right; your tiles standing at your edge facing you, leaning
        // back a little, your melds at your right; both rivers face up, six to a row from the
        // middle outward (hers turned to her), a riichi stick before each who declared.
        std::vector<MascotRenderer::GeometryVertex> BuildTable()
        {
            using V=std::array<float,3>;
            Mesh mesh;
            int& grain=mesh.grain;
            float& grainScale=mesh.grainScale;
            auto quad=[&](const V& a,const V& b,const V& c,const V& d,const V& n,const float (&col)[4],const float* uv=nullptr){mesh.Quad(a,b,c,d,n,col,uv);};
            auto box=[&](float x0,float y0,float z0,float x1,float y1,float z1,const float (&c)[4],bool topFace=true){mesh.Box(x0,y0,z0,x1,y1,z1,c,topFace);};
            const float D=state.upper,top=0.12f*state.hips+0.30f*D;
            const float felt[4]={0.09f,0.33f,0.22f,1},back[4]={0.84f,0.47f,0.20f,1},ivory[4]={0.95f,0.92f,0.84f,1};
            const float white[4]={1,1,1,1},faded[4]={0.62f,0.62f,0.64f,1};
            const float x0=-1.3f*D,x1=1.3f*D,zNear=-0.30f*D,zFar=-2.15f*D;
            const float zc=(zNear-1.9f*D)/2;   // between the hands' rows (yours is nearer the edge)
            BuildRoom(box,quad,grain,grainScale,x0,x1,zNear,zFar,zc);
            box(x0,top-0.05f*D,zFar,x1,top,zNear,felt);

            // A tile lying face up, its middle at (x, z): turn 0 with its top away from you, 2
            // toward you (hers), 1 and 3 on its side (a riichi discard, yours and hers).
            const float tw=0.085f*D,tl=0.115f*D,tt=0.06f*D,gap=0.004f*D;
            auto atlas=[&](const Piece& p,float (&uv)[8],int turn){FaceUv(p,uv,turn);};
            auto lying=[&](float x,float z,const Piece& p,int turn,bool faceUp,bool dim){LyingTile(mesh,x,z,p,turn,faceUp,dim);};

            // The rivers, from the middle outward: yours toward you, hers toward her.
            for(int who:{Mahjong::You,Mahjong::Her})
            {
                const auto& river=state.table.Of(who).river;
                const float side=who==Mahjong::You?-1.0f:1.0f;   // along z, away from the middle
                const float startX=3*(tw+gap)*side;               // from the left as each sees it
                float x=startX;
                int inRow=0,row=0;
                for(size_t i=0;i<river.size();++i)
                {
                    const auto& r=river[i];
                    if(inRow==6){++row;inRow=0;x=startX;}
                    const float w=(r.sideways?tl:tw)+gap;
                    const float z=zc+side*(0.16f*D+(row+0.5f)*(tl+gap)+(r.sideways?(tl-tw)/2:0));
                    const int turn=r.sideways?(who==Mahjong::You?1:3):(who==Mahjong::You?0:2);
                    if(who==Mahjong::Her&&i+1==river.size()&&state.fading)state.fadingTile={x-side*w/2,z,r.piece,turn,r.taken};
                    else lying(x-side*w/2,z,r.piece,turn,true,r.taken);
                    state.lastRiverSpot[who]={x-side*w/2,top+tt,z};
                    x-=side*w;
                    ++inRow;
                }
                if(inRow==6){++row;x=startX;}
                state.nextRiverSpot[who]={x-side*(tw+gap)/2,top+tt,zc+side*(0.16f*D+(row+0.5f)*(tl+gap))};
                if(state.table.Of(who).riichi)
                {
                    const float stick[4]={0.96f,0.96f,0.94f,1},red[4]={0.8f,0.1f,0.12f,1};
                    const float z=zc+side*0.125f*D;
                    box(-0.17f*D,top,z-0.012f*D,0.17f*D,top+0.012f*D,z+0.012f*D,stick);
                    box(-0.012f*D,top+0.012f*D,z-0.01f*D,0.012f*D,top+0.016f*D,z+0.01f*D,red);
                }
            }

            const auto& her=state.table.Of(Mahjong::Her);
            // Her tiles standing in a row, the drawn one a little apart: the back toward you.
            const float w=0.09f*D,h=0.12f*D,d=0.065f*D;
            const int n=static_cast<int>(her.hand.size());
            const bool drawn=her.hasDrawn;
            state.herHandSpots.clear();
            const float rowRight=(14*(w+gap)+0.05f*D)/2;   // a full hand's right end: her tiles stay put (her melds are at your left)
            const float zb=zNear-0.05f*D;   // the tiles' side toward her
            for(int i=0;i<n+(drawn?1:0);++i)
            {
                const float x=i==n?rowRight-w:rowRight-w-0.05f*D-(n-i)*(w+gap);
                box(x,top,zb-d,x+w,top+h,zb-d*0.45f,back);
                box(x,top,zb-d*0.45f,x+w,top+h,zb,ivory);
                if(i==n)state.herDrawnSpot={x+w/2,top+h,zb-d/2};
                state.herHandSpots.push_back({x+w/2,top+h,zb-d/2});
            }
            // Her melds lying face up at her right (your left), before her hand, from the edge in.
            float mx=x0+0.08f*D;
            const float mz=zb-d-0.04f*D-tl/2;
            for(const auto& m:her.melds)
            {
                const int k=m.kind==Mahjong::MeldKind::Chi||m.kind==Mahjong::MeldKind::Pon?3:4;
                const float from=mx;
                for(int i=0;i<k;++i)
                {
                    const bool faceDown=m.kind==Mahjong::MeldKind::ClosedKan&&(i==0||i==3);
                    lying(mx+tw/2,mz,{m.tile,false},2,!faceDown,false);
                    mx+=tw+gap;
                }
                state.herMeldSpot={(from+mx)/2,top+tt,mz};
                mx+=0.03f*D;
            }

            // Your melds lying face up at your right, from the edge in.
            const auto& you=state.table.Of(Mahjong::You);
            const float front=zFar+0.07f*D;   // your tiles' faces stand here
            float yx=x1-0.06f*D;
            for(const auto& m:you.melds)
            {
                const int k=m.kind==Mahjong::MeldKind::Chi||m.kind==Mahjong::MeldKind::Pon?3:4;
                for(int i=0;i<k;++i)
                {
                    const bool faceDown=m.kind==Mahjong::MeldKind::ClosedKan&&(i==0||i==3);
                    yx-=tw+gap;
                    lying(yx+tw/2,front+tl/2,{m.tile,false},0,!faceDown,false);
                }
                yx-=0.03f*D;
            }
            // Your tiles standing, faces to you, leaning back; the drawn one a little apart; the
            // one pointed at or chosen lifted. Kept clear of your melds.
            {
                const float hw=0.112f*D,hh=0.15f*D,hd=0.075f*D,lean=0.28f,lift=0.035f*D;
                const float up[3]={0,std::cos(lean),std::sin(lean)},in[3]={0,-std::sin(lean),std::cos(lean)};
                const int yn=static_cast<int>(you.hand.size());
                const float yrow=yn*(hw+gap)+(you.hasDrawn?hw+0.04f*D:0);
                float start=-(14*(hw+gap)+0.04f*D)/2;   // where a full hand starts: the tiles stay put as tiles come and go
                if(!you.melds.empty())start=std::min(start,yx-0.05f*D-yrow);
                state.handCorners.assign(static_cast<size_t>(yn)+1,{});
                for(int i=0;i<yn+(you.hasDrawn?1:0);++i)
                {
                    const Piece& p=i<yn?you.hand[i]:you.drawn;
                    const float x=start+i*(hw+gap)+(i==yn?0.04f*D:0),y=top+(Lifted(i)?lift:0);
                    // A point on the tile: across (0 to hw), up its face (0 to hh), into it (0 to hd).
                    auto at=[&](float a,float b,float c)->V{return {x+a,y+b*up[1]+c*in[1],front+b*up[2]+c*in[2]};};
                    const float split=hd*0.55f;
                    auto slab=[&](float c0,float c1,const float (&col)[4],bool face){
                        const V p000=at(0,0,c0),p100=at(hw,0,c0),p110=at(hw,hh,c0),p010=at(0,hh,c0);
                        const V p001=at(0,0,c1),p101=at(hw,0,c1),p111=at(hw,hh,c1),p011=at(0,hh,c1);
                        const V n{0,-in[1],-in[2]},t{0,up[1],up[2]};
                        if(face)quad(p010,p110,p100,p000,n,col);
                        quad(p001,p101,p111,p011,{0,in[1],in[2]},col);
                        quad(p010,p110,p111,p011,t,col);
                        quad(p000,p001,p011,p010,{-1,0,0},col);
                        quad(p100,p101,p111,p110,{1,0,0},col);
                    };
                    slab(0,split,ivory,false);
                    slab(split,hd,back,false);
                    float uv[8];
                    atlas(p,uv,0);
                    quad(at(0,hh,0),at(hw,hh,0),at(hw,0,0),at(0,0,0),{0,-in[1],-in[2]},Dimmed(i)?faded:white,uv);
                    auto& c=state.handCorners[static_cast<size_t>(i)];
                    int k=0;
                    for(float a:{0.0f,hw})for(float b:{0.0f,hh})for(float cc:{0.0f,hd})c[k++]=at(a,b,cc);
                }
            }
            return std::move(mesh.out);
        }

        void Finish()
        {
            if(!state.window)return;
            KillTimer(state.window,TimerId);
            auto onEnd=std::move(state.onEnd);
            DestroyWindow(state.window);
            if(onEnd)onEnd();
        }

        void NewGame()
        {
            std::random_device device;
            state.table.NewGame(device(),std::max(LeastCare,Care-LessPerBond*state.her.bond-LessPerLoss*state.lost));
            state.callsSeen=state.table.HerCalls();
            state.selected=-1;
            state.riichiMode=state.confirmQuit=false;
            state.said.clear();
            state.nextAt=GetTickCount64()+HerDrawWait;
        }

        // After a move of yours: her turn waits a moment.
        void Moved(ULONGLONG wait=HerDrawWait)
        {
            state.riichiMode=false;
            state.selected=-1;
            state.nextAt=GetTickCount64()+wait;
            if(state.table.Now()==Phase::RoundOver)state.nextAt=GetTickCount64()+(state.renderer&&!state.table.Ended().draw?EndWait3D:EndWait/2);
            InvalidateRect(state.window,nullptr,FALSE);
        }

        void Do(const Button& b)
        {
            auto& t=state.table;
            switch(b.act)
            {
            case Act::Tsumo:t.Tsumo();break;
            case Act::Riichi:
                state.riichiMode=true;
                state.selected=t.RiichiDiscards().back();
                InvalidateRect(state.window,nullptr,FALSE);
                return;
            case Act::CancelRiichi:state.riichiMode=false;InvalidateRect(state.window,nullptr,FALSE);return;
            case Act::Kan:t.Kan(b.kind);Moved(0);return;
            case Act::Ron:t.Ron();break;
            case Act::Pon:t.Pon();Moved(0);return;
            case Act::OpenKan:t.OpenKan();Moved(0);return;
            case Act::Pass:t.Pass();Moved(0);return;
            case Act::LetGo:t.Discard(static_cast<int>(t.Of(Mahjong::You).hand.size()));break;
            }
            Moved();
        }

        bool CanDiscard(int index)
        {
            auto& t=state.table;
            const auto& you=t.Of(Mahjong::You);
            const int n=static_cast<int>(you.hand.size());
            if(Waiting()||t.Now()!=Phase::YourTurn||index<0||index>n||(index==n&&!you.hasDrawn))return false;
            if(state.riichiMode)
            {
                const auto can=t.RiichiDiscards();
                return std::find(can.begin(),can.end(),index)!=can.end();
            }
            return !you.riichi||index==n;
        }

        void DiscardAt(int index)
        {
            if(!CanDiscard(index))return;
            state.table.Discard(index,state.riichiMode);
            Moved();
        }

        void Continue()
        {
            auto& t=state.table;
            if(t.Now()==Phase::RoundOver&&!Waiting()){t.NextRound();state.nextAt=GetTickCount64()+HerDrawWait;state.selected=-1;}
            else if(t.Now()==Phase::GameOver)
            {
                state.lost=t.Of(Mahjong::You).points<t.Of(Mahjong::Her).points?state.lost+1:0;
                NewGame();
            }
            InvalidateRect(state.window,nullptr,FALSE);
        }

        // The table's sounds, for what changed since the last tick: the tiles shuffled as a game
        // begins and dealt as each later hand does, draws, discards (hers vanishing and appearing
        // at the 3D table, a riichi set down hard and its stick), pon and kan, a win declared, and
        // the jingle as a hand's end or the game's is shown.
        void Listen()
        {
            auto& t=state.table;
            auto& h=state.heard;
            const auto& you=t.Of(Mahjong::You);
            const auto& her=t.Of(Mahjong::Her);
            const bool over=t.Now()==Phase::RoundOver||t.Now()==Phase::GameOver;
            const bool endShown=(t.Now()==Phase::RoundOver&&!Waiting())||t.Now()==Phase::GameOver;
            size_t kans=0;
            for(const auto* seat:{&you,&her})
                for(const auto& m:seat->melds)kans+=m.kind==Mahjong::MeldKind::Kan||m.kind==Mahjong::MeldKind::ClosedKan;
            auto discarded=[](const Mahjong::Seat& seat,bool hers3d){
                // Hers at the 3D table is set down by Appear.
                if(!hers3d)PlayMahjongSound(seat.river.back().sideways?MahjongSound::PlaceHard:MahjongSound::Place);
                if(seat.river.back().sideways)PlayMahjongSound(MahjongSound::Riichi);
            };
            if(!h.valid||(h.gameOver&&!over))PlayMahjongSound(MahjongSound::Shuffle);
            else
            {
                if(h.over&&!over)PlayMahjongSound(MahjongSound::Deal);
                if(you.hasDrawn&&!h.youDrawn)PlayMahjongSound(MahjongSound::Draw);
                if(her.hasDrawn&&!h.herDrawn)PlayMahjongSound(MahjongSound::Draw);
                if(you.river.size()>h.youRiver)discarded(you,false);
                if(her.river.size()>h.herRiver)
                {
                    if(state.renderer){PlayMahjongSound(MahjongSound::Vanish);PlayMahjongSound(MahjongSound::Appear);}
                    discarded(her,state.renderer!=nullptr);
                }
                if(kans>h.kans)PlayMahjongSound(MahjongSound::Kan);
                else if(you.melds.size()>h.youMelds||her.melds.size()>h.herMelds)PlayMahjongSound(MahjongSound::Pon);
                if(over&&!h.over&&!t.Ended().draw)PlayMahjongSound(MahjongSound::Win);
                if(endShown&&!h.endShown)
                {
                    if(t.Now()==Phase::GameOver)PlayMahjongSound(you.points>=her.points?MahjongSound::GameWon:MahjongSound::GameLost);
                    else if(t.Ended().draw)PlayMahjongSound(MahjongSound::Drawn);
                    else PlayMahjongSound(t.Ended().winner==Mahjong::You?MahjongSound::YouWon:MahjongSound::YouLost);
                }
            }
            h={true,you.hasDrawn,her.hasDrawn,over,endShown,t.Now()==Phase::GameOver,you.river.size(),her.river.size(),you.melds.size(),her.melds.size(),kans};
        }

        void Tick()
        {
            auto& t=state.table;
            const ULONGLONG now=GetTickCount64();
            int parts=0;
            if(t.HerCalls()!=state.callsSeen)
            {
                state.callsSeen=t.HerCalls();
                state.said=std::wstring(English()?t.HerCallEn():t.HerCallJa())+T(L"！",L"!");
                state.saidAt=now;
                state.nextAt=std::max(state.nextAt,now+CallWait);
                parts|=Top;
            }
            if(!state.said.empty()&&now-state.saidAt>SaidShown){state.said.clear();parts|=Top;}
            if(now>=state.nextAt)
            {
                const auto& you=t.Of(Mahjong::You);
                if(t.Now()==Phase::HerTurn)
                {
                    t.Step();
                    state.nextAt=now+(t.Now()==Phase::HerTurn?(state.renderer?HerDiscardWait3D:HerDiscardWait):0);
                    // At the 3D table, now and then she stops to think (more often against your
                    // riichi) before her discard.
                    if(state.renderer&&state.entrance==5&&t.Now()==Phase::HerTurn)
                    {
                        std::uniform_real_distribution<double> any(0,1);
                        if(any(state.sparkRandom)<(t.Of(Mahjong::You).riichi?0.45:0.15))
                        {
                            state.nextAt+=ThinkExtra;
                            state.thinkFor=(HerDiscardWait3D+ThinkExtra-ReachLead)/1000.0-0.1;
                        }
                    }
                    // Her draw changes only her side; her discard the middle and your side too.
                    parts|=t.Now()==Phase::HerTurn?Top:t.Now()==Phase::RoundOver?0:Everything;
                    if(t.Now()==Phase::RoundOver)state.nextAt=now+(state.renderer&&!t.Ended().draw?EndWait3D:EndWait);
                }
                else if(t.Now()==Phase::YourTurn&&you.riichi&&you.hasDrawn&&!t.CanTsumo()&&t.KanKinds().empty())
                {
                    if(!state.autoAt)state.autoAt=now+RiichiShowDrawn;
                    else if(now>=state.autoAt)
                    {
                        state.autoAt=0;
                        t.Discard(static_cast<int>(you.hand.size()));
                        state.nextAt=now+RiichiDiscardWait;
                        parts|=Middle|Bottom;
                    }
                }
                else if(state.nextAt&&now-state.nextAt<100)
                    parts|=t.Now()==Phase::RoundOver?Everything:Bottom;   // what waited (the buttons, the end) shows now
            }
            Redraw(parts);
            Listen();
        }

        std::wstring ScoreLine(const Mahjong::Score& s)
        {
            if(*s.limitJa)return std::wstring(English()?s.limitEn:s.limitJa)+L"  "+Points(s.points);
            return English()?std::to_wstring(s.fu)+L" fu "+std::to_wstring(s.han)+L" han  "+std::to_wstring(s.points)
                            :std::to_wstring(s.fu)+L"符 "+std::to_wstring(s.han)+L"翻  "+Points(s.points);
        }

        // Whether the brush hand has every character of text: the kana, ASCII and full-width forms
        // are all in it (a name in kanji is written in the plain hand).
        bool BrushCan(const std::wstring& text)
        {
            return std::all_of(text.begin(),text.end(),[](wchar_t c){return (c>=0x20&&c<0x7f)||(c>=0x3000&&c<0x3100)||(c>=0xff01&&c<0xff5f);});
        }

        // A line of pieces side by side, centred in box: the brush hand for each but names
        // (which it may not have the characters of).
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
            const float w=std::min(cw-40*s,860*s),h=std::min(ch-40*s,520*s);
            const RectF box((cw-w)/2,(ch-h)/2,w,h);
            FillRound(g,box,14*s,Color(255,24,40,36),Color(255,200,170,90),2*s);
            const float left=box.X+28*s;
            float y=box.Y+20*s;
            const Color gold(255,240,206,120),white(255,240,240,236),soft(255,190,196,190);
            if(t.Now()==Phase::GameOver)
            {
                const int you=t.Of(Mahjong::You).points,her=t.Of(Mahjong::Her).points;
                Text(g,T(L"対局終了",L"Game over"),RectF(box.X,y,w,44*s),34*s,gold,true,StringAlignmentCenter,StringAlignmentNear,true);
                y+=70*s;
                Runs(g,{{T(L"あなた  ",L"You  ")},{Points(you)}},RectF(box.X,y,w,36*s),26*s,white);
                y+=44*s;
                Runs(g,{{HerName(),BrushCan(HerName())},{L"  "+Points(her)}},RectF(box.X,y,w,36*s),26*s,white);
                y+=70*s;
                if(you<her)Runs(g,{{HerName(),BrushCan(HerName())},{T(L"の勝ち",L" wins")}},RectF(box.X,y,w,48*s),34*s,gold);
                else Runs(g,{{you>her?T(L"あなたの勝ち！",L"You win!"):T(L"引き分け",L"A tie")}},RectF(box.X,y,w,48*s),34*s,gold);
                Text(g,T(L"Enter：もう一度　Esc：閉じる",L"Enter: play again   Esc: close"),RectF(box.X,box.GetBottom()-44*s,w,30*s),17*s,soft,false,StringAlignmentCenter);
                return;
            }
            const auto& e=t.Ended();
            if(e.draw)
            {
                Text(g,T(L"流局",L"Drawn hand"),RectF(box.X,y,w,44*s),34*s,gold,true,StringAlignmentCenter,StringAlignmentNear,true);
                y+=60*s;
                Text(g,std::wstring(T(L"あなた：",L"You: "))+(e.tenpai[Mahjong::You]?T(L"テンパイ",L"waiting"):T(L"ノーテン",L"not waiting")),RectF(left,y,w,30*s),21*s,white,false,StringAlignmentNear,StringAlignmentNear,true);
                y+=36*s;
                const float nameW=TextWidth(g,HerName(),21*s,false,BrushCan(HerName()));
                Text(g,HerName(),RectF(left,y,w,30*s),21*s,white,false,StringAlignmentNear,StringAlignmentNear,BrushCan(HerName()));
                Text(g,std::wstring(T(L"：",L": "))+(e.tenpai[Mahjong::Her]?T(L"テンパイ",L"waiting"):T(L"ノーテン",L"not waiting")),RectF(left+nameW,y,w,30*s),21*s,white,false,StringAlignmentNear,StringAlignmentNear,true);
                y+=40*s;
                for(size_t i=0;i<e.herHand.size();++i)DrawTile(g,left+i*(HandW*0.8f+1)*s,y,HandW*0.8f*s,HandH*0.8f*s,e.herHand[i]);
                y+=HandH*s+16*s;
            }
            else
            {
                const std::wstring who=e.winner==Mahjong::You?std::wstring(T(L"あなた",L"You")):HerName();
                Runs(g,{{who,e.winner==Mahjong::You||BrushCan(who)},{std::wstring(L"  ")+(e.tsumo?T(L"ツモ",L"Tsumo"):T(L"ロン",L"Ron"))}},RectF(box.X,y,w,44*s),34*s,gold);
                y+=56*s;
                // The hand: its concealed tiles, the winning one apart, then the melds.
                const float tw=HandW*0.8f*s,th=HandH*0.8f*s;
                float x=left;
                for(size_t i=0;i<e.hand.size();++i)
                {
                    if(i+1==e.hand.size())x+=10*s;
                    DrawTile(g,x,y,tw,th,e.hand[i]);
                    x+=tw+s;
                }
                DrawMelds(g,e.melds,box.GetRight()-28*s,y,s,tw,th);
                y+=th+18*s;
                // The yaku in two columns.
                const float colW=(w-56*s)/2;
                for(size_t i=0;i<e.score.yaku.size();++i)
                {
                    const auto& yk=e.score.yaku[i];
                    const float cx=left+(i%2)*colW,cy=y+(i/2)*28*s;
                    Text(g,English()?yk.en:yk.ja,RectF(cx,cy,colW,28*s),20*s,white,false,StringAlignmentNear,StringAlignmentNear,true);
                    const std::wstring han=e.score.yakuman?std::wstring(T(L"役満",L"yakuman")):std::to_wstring(yk.han)+T(L"翻",L" han");
                    Text(g,han,RectF(cx,cy,colW-24*s,28*s),20*s,soft,false,StringAlignmentFar,StringAlignmentNear,true);
                }
                y+=((e.score.yaku.size()+1)/2)*28*s+12*s;
                Text(g,ScoreLine(e.score),RectF(box.X,y,w,44*s),32*s,gold,true,StringAlignmentCenter,StringAlignmentNear,true);
                y+=48*s;
                if(!e.ura.empty())
                {
                    Text(g,T(L"裏ドラ表示",L"Ura indicators"),RectF(left,y+6*s,140*s,30*s),18*s,soft,false,StringAlignmentNear,StringAlignmentNear,true);
                    for(size_t i=0;i<e.ura.size();++i)DrawTile(g,left+140*s+i*(SmallW+2)*s,y,SmallW*s,SmallH*s,{e.ura[i],false});
                    y+=SmallH*s+10*s;
                }
            }
            Runs(g,{{std::wstring(T(L"あなた ",L"You "))+Signed(e.change[Mahjong::You])+L"　　"},{HerName(),BrushCan(HerName())},{L" "+Signed(e.change[Mahjong::Her])}},
                 RectF(box.X,box.GetBottom()-86*s,w,32*s),21*s,white);
            Text(g,T(L"Enter：次へ",L"Enter: next"),RectF(box.X,box.GetBottom()-44*s,w,30*s),17*s,soft,false,StringAlignmentCenter);
        }

        // dirty: the part of the window to draw (the rest of the picture is left as it was).
        void Paint(HDC dc,const RECT& dirty)
        {
            RECT client{};
            GetClientRect(state.window,&client);
            const int cw=std::max(1L,client.right),ch=std::max(1L,client.bottom);
            if(!state.canvas||static_cast<int>(state.canvas->GetWidth())!=cw||static_cast<int>(state.canvas->GetHeight())!=ch)
                state.canvas=std::make_unique<Bitmap>(cw,ch,PixelFormat32bppPARGB);
            Bitmap& canvas=*state.canvas;
            Graphics g(&canvas);
            // With the 3D table this is a picture laid over it (clear where the table shows), all
            // of it drawn again each time.
            const bool hud=state.renderer!=nullptr;
            if(!hud)g.SetClip(Rect(dirty.left,dirty.top,dirty.right-dirty.left,dirty.bottom-dirty.top));
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(hud?TextRenderingHintAntiAlias:TextRenderingHintAntiAliasGridFit);
            g.Clear(hud?Color(0,0,0,0):Color(255,22,74,52));
            const float s=Scale(),W=static_cast<float>(cw),mid=W/2;
            auto& t=state.table;
            const auto& you=t.Of(Mahjong::You);
            const auto& her=t.Of(Mahjong::Her);
            const Color white(255,240,240,236),soft(255,186,206,190),gold(255,240,206,120);
            UpdateButtons();

            // Her: name, points, riichi and her words (at the top left over the 3D table, or
            // at the top with her face, her tiles face down and her melds when there is none).
            const Places at=Layout();
            const float nameX=hud?28*s:112*s,nameY=hud?20*s:14*s;
            if(hud)FillRound(g,RectF(14*s,12*s,250*s,92*s),10*s,Color(185,10,30,24),Color(220,120,160,130),1.2f*s);
            else
            {
                const RectF faceBox(16*s,14*s,84*s,84*s);
                FillRound(g,faceBox,10*s,Color(255,14,50,36),Color(255,120,160,130),1.5f*s);
                if(state.face)g.DrawImage(state.face.get(),faceBox.X+3*s,faceBox.Y+3*s,faceBox.Width-6*s,faceBox.Height-6*s);
                else Text(g,HerName().substr(0,1),faceBox,40*s,white,true,StringAlignmentCenter,StringAlignmentCenter);
                const int backs=static_cast<int>(her.hand.size())+(her.hasDrawn?1:0);
                const float meldLeft=DrawMelds(g,her.melds,W-20*s,22*s,s,BackW*s,BackH*s);
                float x=meldLeft-(her.melds.empty()?0:12*s)-backs*(BackW+1)*s;
                for(int i=0;i<backs;++i){DrawTile(g,x,22*s,BackW*s,BackH*s,{},false);x+=(BackW+1)*s;}
            }
            Text(g,HerName(),RectF(nameX,nameY,240*s,28*s),20*s,white,true);
            Text(g,Points(her.points)+(t.Dealer()==Mahjong::Her?T(L"　親",L"  dealer"):L""),RectF(nameX,nameY+30*s,240*s,24*s),17*s,soft);
            if(her.riichi){FillRound(g,RectF(nameX,nameY+60*s,90*s,8*s),3*s,Color(255,245,245,240));SolidBrush dot(Color(255,210,30,40));g.FillEllipse(&dot,nameX+41*s,nameY+60*s,8*s,8*s);}
            if(!state.said.empty())
            {
                const float tw=TextWidth(g,state.said,30*s,true)+30*s;
                const RectF bubble(hud?278*s:262*s,hud?30*s:18*s,tw,52*s);
                FillRound(g,bubble,14*s,Color(255,255,252,240),Color(255,230,120,60),2*s);
                Text(g,state.said,bubble,30*s,Color(255,200,40,40),true,StringAlignmentCenter,StringAlignmentCenter);
            }

            // The rivers (on the 3D table when there is one), hers above the middle and yours
            // below; the round between them (at the top right over the table).
            const float riverLeft=mid-RiverRow*(RiverW+1)*s/2;
            if(!hud)DrawRiver(g,her.river,riverLeft,at.herRiver*s,s);
            const RectF info=hud?RectF(W-354*s,12*s,340*s,64*s):RectF(mid-170*s,at.info*s,340*s,64*s);
            FillRound(g,info,10*s,Color(hud?215:200,12,40,28),Color(255,90,130,100),1.2f*s);
            {
                const bool south=t.RoundWind()==Mahjong::South;
                const std::wstring round=Fmt(T(L"{0}{1}局 {2}本場",L"{0} {1}, {2} honba"),
                                             {south?T(L"南",L"South"):T(L"東",L"East"),std::to_wstring((t.RoundNumber()-1)%2+1),std::to_wstring(t.Honba())});
                Text(g,round,RectF(info.X+12*s,info.Y+6*s,200*s,26*s),18*s,white,true);
                Text(g,Fmt(T(L"残り {0}　供託 {1}",L"Wall {0}  Sticks {1}"),{std::to_wstring(t.WallLeft()),std::to_wstring(t.Sticks())}),
                     RectF(info.X+12*s,info.Y+34*s,200*s,24*s),15*s,soft);
                Text(g,T(L"ドラ",L"Dora"),RectF(info.X+204*s,info.Y+20*s,40*s,24*s),15*s,soft);
                for(size_t i=0;i<t.Dora().size();++i)DrawTile(g,info.X+240*s+i*(SmallW*0.85f+2)*s,info.Y+12*s,SmallW*0.85f*s,SmallH*0.85f*s,{t.Dora()[i],false});
            }
            if(!hud)DrawRiver(g,you.river,riverLeft,at.yourRiver*s,s);

            // Your points, riichi, the buttons and your hand.
            const float handY=at.hand*s;
            if(hud)FillRound(g,RectF(8*s,handY-14*s,112*s,100*s),10*s,Color(185,10,30,24),Color(220,120,160,130),1.2f*s);
            Text(g,T(L"あなた",L"You"),RectF(16*s,handY-6*s,110*s,26*s),18*s,white,true);
            Text(g,Points(you.points),RectF(16*s,handY+20*s,110*s,24*s),16*s,soft);
            if(t.Dealer()==Mahjong::You)Text(g,T(L"親",L"dealer"),RectF(16*s,handY+44*s,110*s,24*s),15*s,gold);
            if(you.riichi){FillRound(g,RectF(16*s,handY+72*s,90*s,8*s),3*s,Color(255,245,245,240));SolidBrush dot(Color(255,210,30,40));g.FillEllipse(&dot,57*s,handY+72*s,8*s,8*s);}
            {
                float x=W-20*s;
                for(size_t i=state.buttons.size();i-->0;)
                {
                    Button& b=state.buttons[i];
                    const std::wstring label=std::to_wstring(i+1)+L"  "+b.label;
                    const float bw=std::max(96*s,TextWidth(g,label,18*s,true)+28*s);
                    x-=bw;
                    b.box=RectF(x,hud?handY+24*s:handY-62*s,bw,40*s);
                    const bool main=b.act==Act::Tsumo||b.act==Act::Ron;
                    FillRound(g,b.box,10*s,main?Color(255,206,60,50):Color(255,236,226,196),Color(255,90,60,30),1.5f*s);
                    Text(g,label,b.box,18*s,main?white:Color(255,40,30,20),true,StringAlignmentCenter,StringAlignmentCenter);
                    x-=8*s;
                }
            }
            {
                const int n=static_cast<int>(you.hand.size());
                const float handLeft=130*s;
                if(!hud)state.handBoxes.assign(static_cast<size_t>(n)+1,RectF());
                std::vector<int> riichiOk;
                if(state.riichiMode)riichiOk=t.RiichiDiscards();
                const bool yourTurn=t.Now()==Phase::YourTurn&&!Waiting();
                if(yourTurn&&state.selected<0)state.selected=you.hasDrawn?n:n-1;
                for(int i=0;!hud&&i<=n;++i)
                {
                    if(i==n&&!you.hasDrawn)break;
                    const Piece& p=i<n?you.hand[i]:you.drawn;
                    const float x=handLeft+i*(HandW+1)*s+(i==n?12*s:0);
                    // After riichi the drawn tile is not lifted (it goes by itself), only when pointed at.
                    const bool raised=yourTurn&&CanDiscard(i)&&(i==state.hover||(i==state.selected&&!you.riichi));
                    const float y=handY-(raised?10*s:0);
                    const bool dim=yourTurn&&!CanDiscard(i);
                    DrawTile(g,x,y,HandW*s,HandH*s,p,true,dim&&state.riichiMode);
                    state.handBoxes[i]=RectF(x,handY-10*s,HandW*s,HandH*s+10*s);
                }
                if(!hud)DrawMelds(g,you.melds,W-20*s,handY+(HandH-SmallH)*s,s,SmallW*s,SmallH*s);
            }

            // What you wait on (and whether you are furiten).
            {
                const std::vector<int> waits=t.Now()==Phase::YourTurn?std::vector<int>{}:t.Waits();
                const float y=handY+HandH*s+14*s;
                if(!waits.empty())
                {
                    if(hud)FillRound(g,RectF(122*s,y-6*s,(68+waits.size()*(SmallW*0.8f+2)+(t.Furiten()?130:0))*s,SmallH*0.8f*s+12*s),8*s,Color(185,10,30,24));
                    float x=130*s;
                    Text(g,T(L"待ち",L"Waits"),RectF(x,y+4*s,60*s,24*s),16*s,soft);
                    x+=50*s;
                    for(int w:waits){DrawTile(g,x,y,SmallW*0.8f*s,SmallH*0.8f*s,{w,false});x+=(SmallW*0.8f+2)*s;}
                    if(t.Furiten())Text(g,T(L"フリテン",L"Furiten"),RectF(x+10*s,y+4*s,120*s,24*s),16*s,Color(255,255,120,110),true);
                }
                else if(state.riichiMode)
                {
                    if(hud)FillRound(g,RectF(122*s,y-2*s,TextWidth(g,T(L"リーチ：切る牌を選んでください（Escでやめる）",L"Riichi: choose the tile to discard (Esc to cancel)"),16*s)+20*s,32*s),8*s,Color(185,10,30,24));
                    Text(g,T(L"リーチ：切る牌を選んでください（Escでやめる）",L"Riichi: choose the tile to discard (Esc to cancel)"),RectF(130*s,y+4*s,600*s,24*s),16*s,gold);
                }
            }

            if((t.Now()==Phase::RoundOver&&!Waiting())||t.Now()==Phase::GameOver)PaintEnd(g,W,static_cast<float>(ch),s);
            if(state.confirmQuit)
            {
                SolidBrush shade(Color(160,0,0,0));
                g.FillRectangle(&shade,0.0f,0.0f,W,static_cast<float>(ch));
                const RectF box(mid-220*s,ch/2.0f-70*s,440*s,140*s);
                FillRound(g,box,14*s,Color(250,24,40,36),Color(255,200,170,90),2*s);
                Text(g,T(L"麻雀をやめますか？",L"Stop playing?"),RectF(box.X,box.Y+22*s,box.Width,36*s),24*s,white,true,StringAlignmentCenter);
                Text(g,T(L"Y：やめる　N：続ける",L"Y: stop   N: keep playing"),RectF(box.X,box.Y+80*s,box.Width,30*s),18*s,soft,false,StringAlignmentCenter);
            }

            if(hud)
            {
                // Laid over the table: premultiplied BGRA to RGBA.
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
            for(size_t i=0;i<state.handBoxes.size();++i)
                if(state.handBoxes[i].Width>0&&state.handBoxes[i].Contains(x,y))return static_cast<int>(i);
            return -1;
        }

        void OnKey(WPARAM key)
        {
            auto& t=state.table;
            if(state.confirmQuit)
            {
                if(key=='Y')Finish();
                else if(key=='N'||key==VK_ESCAPE){state.confirmQuit=false;InvalidateRect(state.window,nullptr,FALSE);}
                return;
            }
            if(t.Now()==Phase::GameOver)
            {
                if(key==VK_RETURN||key==VK_SPACE)Continue();
                else if(key==VK_ESCAPE)Finish();
                return;
            }
            if(key==VK_ESCAPE)
            {
                if(state.riichiMode)state.riichiMode=false;
                else state.confirmQuit=true;
                InvalidateRect(state.window,nullptr,FALSE);
                return;
            }
            if(t.Now()==Phase::RoundOver){if(key==VK_RETURN||key==VK_SPACE)Continue();return;}
            if(key>='1'&&key<='9')
            {
                const size_t i=key-'1';
                if(i<state.buttons.size())Do(state.buttons[i]);
                return;
            }
            if(key==VK_SPACE&&t.Now()==Phase::YourCall&&!Waiting()){t.Pass();Moved(0);return;}
            if(t.Now()!=Phase::YourTurn||Waiting())return;
            const int n=static_cast<int>(t.Of(Mahjong::You).hand.size())+(t.Of(Mahjong::You).hasDrawn?1:0);
            if(key==VK_LEFT||key==VK_RIGHT)
            {
                // To the next tile that can be discarded.
                int i=state.selected;
                for(int step=0;step<n;++step)
                {
                    i=(i+(key==VK_LEFT?n-1:1))%n;
                    if(CanDiscard(i))break;
                }
                state.selected=i;
                InvalidateRect(state.window,nullptr,FALSE);
            }
            else if(key==VK_RETURN)DiscardAt(state.selected);
        }

        LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            switch(message)
            {
            case WM_MOUSEWHEEL:
                // The wheel goes to the window with the keys: over the 3D view, it is the view's.
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
                const int at=HandAt(static_cast<float>(GET_X_LPARAM(lParam)),static_cast<float>(GET_Y_LPARAM(lParam)));
                if(at!=state.hover){state.hover=at;Redraw(Bottom);}
                return 0;
            }
            case WM_LBUTTONDOWN:
            {
                const float x=static_cast<float>(GET_X_LPARAM(lParam)),y=static_cast<float>(GET_Y_LPARAM(lParam));
                auto& t=state.table;
                if(state.confirmQuit)return 0;
                if(t.Now()==Phase::GameOver||(t.Now()==Phase::RoundOver&&!Waiting())){Continue();return 0;}
                for(const Button& b:state.buttons)if(b.box.Contains(x,y)){Do(b);return 0;}
                const int at=HandAt(x,y);
                if(at>=0)DiscardAt(at);
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

    bool MahjongWindowOpen(){return state.window!=nullptr;}

    namespace
    {
        // Diagnostics: a simple player of yours for moves steps (or to the hand's end): discards
        // what it draws, wins when it can, calls a pon now and then.
        void PlaySimply(int moves)
        {
            auto& t=state.table;
            for(int i=0;i<moves&&t.Now()!=Phase::RoundOver&&t.Now()!=Phase::GameOver;++i)
            {
                const auto& you=t.Of(Mahjong::You);
                if(t.Now()==Phase::HerTurn)t.Step();
                else if(t.Now()==Phase::YourCall){if(t.CanRon())t.Ron();else if(t.CanPon())t.Pon();else t.Pass();}
                else if(t.CanTsumo())t.Tsumo();
                else if(!t.RiichiDiscards().empty()&&!you.riichi)t.Discard(t.RiichiDiscards()[0],true);
                else t.Discard(you.hasDrawn?static_cast<int>(you.hand.size()):0);
            }
            state.callsSeen=t.HerCalls();
            state.said=t.HerCalls()?std::wstring(t.HerCallJa())+L"！":L"";
            state.nextAt=0;
            state.selected=-1;
        }
    }

    void ReachMahjongForTest(){state.testReach=true;}
    void GestureMahjongForTest(const std::wstring& name){state.testGesture=name;}

    void SetMahjongCamera(float pitch,float distance,float y,float z,float yaw)
    {
        state.camera.yaw=yaw;
        state.camera.pitch=pitch;
        state.camera.distance=distance;
        state.camera.y=y;
        state.camera.z=z;
    }

    void PlayMahjongMoves(int moves)
    {
        if(!state.window)return;
        PlaySimply(moves);
        InvalidateRect(state.window,nullptr,FALSE);
    }

    namespace
    {
        // A burst of sparkles rising from a tile (world units): stars, gold and white, and a
        // few soft glows; scale makes it smaller or bigger.
        void Sparkle(const std::array<float,3>& at,float scale=1,bool vanish=false)
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
                k.color=i%3==0?std::array<float,4>{1,1,1,1}:vanish?std::array<float,4>{0.62f,0.84f,1,1}:std::array<float,4>{1,0.84f,0.42f,1};
                state.sparks.push_back(k);
            }
        }

        // Her light pat from above toward a spot on the table (world units), the nearer hand, in
        // front of her chest: the tile warps (it is a touch of magic, not a reach). False when
        // she cannot now (another gesture playing).
        bool ReachFor(MascotModel& model,const std::array<float,3>& spot)
        {
            std::array<float,3> head{};
            if(!model.HeadPosition(head))return false;
            // Seated, she is drawn moved by (herX, 0, herZ), her walking's sideways shift taken out.
            const float hx=head[0]-model.RootOffset()+state.herX,hz=head[2]+state.herZ;
            // The wrist a hand's height over the tile, so the fingers stop above the table.
            const std::array<float,3> from{spot[0]-hx,spot[1]+0.07f*state.hips-head[1],spot[2]-hz};
            return model.Reach(spot[0]>0.25f*state.upper?0:1,from,1.65,true);
        }
    }

    void TickMahjongWindow(MascotModel& model,double dt)
    {
        if(!state.window||!state.renderer)return;
        auto& r=*state.renderer;
        using namespace DirectX;
        const float D=state.upper,top=0.12f*state.hips+0.30f*D;
        // She comes in walking on the floor (a chair's seat below the seat she sits on) from
        // behind on her right to the front of her chair, pulled back from the table, turns to
        // you, sits down on it (on its front edge, as on a window's), and moves in with it to a
        // little back from the table.
        const float seatZ=SeatZ(),seatHeight=-FloorY(),step=static_cast<float>(dt);
        // She stands clear of the table and just before the pulled chair's seat (her legs clear
        // of its front edge), and sits back onto it, her thighs over the seat.
        const float standX=0,standZ=0.25f*D+SeatDepth(),pulled=standZ+0.25f*D+SeatFront(),startX=-1.8f*D,startZ=standZ;
        float lift=0,standing=1;   // standing: 1 until she sits, the camera looking higher to keep her head in view
        float lean=0;              // forward, in radians, about her hips (as she shifts her chair in)
        auto ease=[](float u){return u*u*(3-2*u);};
        if(state.entrance==0)
        {
            model.SetFacingWalk(true);
            model.SetActivity(Activity::Walk,1);
            state.herX=startX;
            state.herZ=startZ;
            state.herAngle=std::atan2(startX-standX,startZ-standZ);   // facing the way she walks (toward +x)
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
            // Lowered back onto the seat as she bends into sitting.
            const float u=ease(std::min(1.0f,(state.entranceTime+=step)/0.7f));
            lift=-seatHeight*(1-u);
            state.herZ=standZ+(pulled-standZ)*u;
            standing=1-u;
            if(u>=1){state.entrance=4;state.entranceTime=0;}
        }
        else if(state.entrance==4)
        {
            // A beat once seated, then she and the chair move in together: her hips lifted a
            // little off the seat and leaning forward, most at the middle of it.
            const float t=std::clamp(((state.entranceTime+=step)-0.45f)/0.8f,0.0f,1.0f),u=ease(t);
            const float bump=std::sin(t*XM_PI);
            lift=0.05f*state.hips*bump;
            lean=0.22f*bump;
            standing=0;
            state.herZ=state.chairZ=pulled+(seatZ-pulled)*u;
            if(u>=1)state.entrance=5;
        }
        else if(state.entrance==5){standing=0;state.herX=0;state.herZ=state.chairZ=seatZ;}
        const auto& her=state.table.Of(Mahjong::Her);
        if(auto scene=Scene();scene!=state.scene||state.scene.empty()){state.scene=std::move(scene);r.SetGeometry(BuildTable());}
        // Sparkles where her tiles come: her draw, her discard, her call (which she reaches for).
        if(state.seenRiver>=0)
        {
            if(static_cast<int>(her.river.size())>state.seenRiver)
            {
                // The tile she touched goes, and fades into her river.
                Sparkle(state.touched,1.2f,true);
                state.fading=true;
                state.fadeSince=state.clock;
            }
            if(her.hasDrawn&&!state.seenDrawn)Sparkle(state.herDrawnSpot,0.6f);
            if(static_cast<int>(her.melds.size())>state.seenMelds)
            {
                Sparkle(state.herMeldSpot);
                if(state.entrance==5)ReachFor(model,state.herMeldSpot);
            }
        }
        state.seenRiver=static_cast<int>(her.river.size());
        state.seenMelds=static_cast<int>(her.melds.size());
        state.seenDrawn=her.hasDrawn;
        // Her thinking before a discard; at a hand's end, joy at her win, "oops" at yours.
        if(state.thinkFor>0&&state.entrance==5){model.EmoteSeated(L"ponder",state.thinkFor);state.thinkFor=0;}
        {
            const bool over=state.table.Now()==Phase::RoundOver||state.table.Now()==Phase::GameOver;
            if(over&&!state.seenOver)
            {
                const auto& e=state.table.Ended();
                if(!e.draw)
                {
                    std::uniform_real_distribution<double> any(0,1);
                    state.endGesture=e.winner==Mahjong::Her?(any(state.sparkRandom)<0.5?L"cheeks":L"clap"):L"oops";
                    state.endGestureUntil=state.clock+1.5;
                }
            }
            state.seenOver=over;
            if(!state.testGesture.empty()){state.endGesture=state.testGesture;state.endGestureUntil=1e9;state.testGesture.clear();}
            if(!state.endGesture.empty()&&state.entrance==5&&(model.EmoteSeated(state.endGesture,3.2)||state.clock>state.endGestureUntil))
                state.endGesture.clear();
        }
        // Diagnostics: as at her discard, she touches a tile of hers; 0.6 s on it vanishes and
        // her newest river tile fades in again.
        if(state.testReach&&state.entrance==5&&!state.herHandSpots.empty())
        {
            state.testReach=false;
            state.touched=state.herHandSpots[state.herHandSpots.size()/3];
            ReachFor(model,state.touched);
            state.testVanishAt=state.clock+ReachLead/1000.0;
        }
        if(state.testVanishAt>0&&state.clock>=state.testVanishAt&&!her.river.empty())
        {
            state.testVanishAt=0;
            Sparkle(state.touched,1.2f,true);
            state.fading=true;
            state.fadeSince=state.clock;
        }
        // Before her discard, her hand goes out to touch the tile in her hand (it vanishes
        // from there and fades into her river).
        if(state.table.Now()==Phase::HerTurn&&state.table.HerTurnDrawn()&&state.reachedRiver!=her.river.size()&&
           GetTickCount64()+ReachLead>=state.nextAt)
        {
            const int k=state.table.HerNextDiscard();
            if(k>=0&&k<static_cast<int>(state.herHandSpots.size()))
            {
                state.touched=state.herHandSpots[static_cast<size_t>(k)];
                // Tried again next frame while a gesture of hers ends.
                if(state.entrance!=5||ReachFor(model,state.touched))state.reachedRiver=her.river.size();
            }
            else state.reachedRiver=her.river.size();
        }
        // What moves: her chair, and her newest river tile fading in (gold sparkles rising from
        // it as it comes; once whole, it joins the table).
        state.clock+=dt;
        {
            Mesh moving;
            moving.out=BuildChair(state.chairZ);
            if(state.fading)
            {
                const float u=static_cast<float>((state.clock-state.fadeSince)/FadeSeconds);
                if(u>=1)state.fading=false;
                else
                {
                    const auto& f=state.fadingTile;
                    LyingTile(moving,f.x,f.z,f.piece,f.turn,true,f.dim,u*u*(3-2*u));
                    if(state.clock-dt<state.fadeSince+FadeSeconds*0.35&&state.clock>=state.fadeSince+FadeSeconds*0.35)
                        Sparkle({f.x,0.12f*state.hips+0.30f*state.upper+0.06f*state.upper,f.z});
                }
            }
            r.SetMovingGeometry(std::move(moving.out));
        }
        {
            std::vector<MascotRenderer::Particle> shown;
            const float fall=0.9f*D,step=static_cast<float>(dt);
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
            r.SetParticles(shown);
        }
        // Her walks on the desktop leave センター shifted sideways in the model: taken back out.
        const float hip=0.12f*state.hips;   // her hips above the seat
        r.SetModelTransform(XMMatrixTranslation(-model.RootOffset(),-hip,0)*XMMatrixRotationX(-lean)*XMMatrixTranslation(0,hip,0)*XMMatrixRotationY(state.herAngle)*XMMatrixTranslation(state.herX,lift,state.entrance==0?seatZ:state.herZ));
        // From your side of the table, above her, looking down at her and the table between you
        // (until the camera is turned).
        const auto& c=state.camera;
        const XMVECTOR target=XMVectorSet(c.x*D,top+(0.47f+c.y+0.7f*standing)*D,(c.z-1.17f)*D,0);
        const XMVECTOR toEye=XMVectorSet(std::sin(c.yaw)*std::cos(c.pitch),std::sin(c.pitch),-std::cos(c.yaw)*std::cos(c.pitch),0);
        r.SetCamera(XMVectorAdd(target,XMVectorScale(toEye,c.distance*(1+0.22f*standing)*D)),target,XMConvertToRadians(42.0f));
        // Your tiles as they are seen, for pointing and clicking.
        state.handBoxes.assign(state.handCorners.size(),RectF());
        for(size_t i=0;i<state.handCorners.size();++i)
        {
            if(state.handCorners[i][7][1]==0&&state.handCorners[i][0][1]==0)continue;   // no drawn tile
            LONG l=LONG_MAX,t=LONG_MAX,rr=LONG_MIN,b=LONG_MIN;
            for(size_t k=0;k<8;k+=2)   // its face (the box round it would overlap the next tile's)
            {
                const POINT q=r.Project(state.handCorners[i][k]);
                l=std::min(l,q.x);t=std::min(t,q.y);rr=std::max(rr,q.x);b=std::max(b,q.y);
            }
            state.handBoxes[i]=RectF(static_cast<REAL>(l),static_cast<REAL>(t),static_cast<REAL>(rr-l),static_cast<REAL>(b-t));
        }
        {
            MascotRenderer::PointLight lamp;
            lamp.position[0]=0;lamp.position[1]=top+2.3f*D;lamp.position[2]=(-0.30f*D-1.9f*D)/2;
            lamp.radius=4.2f*D;
            lamp.color[0]=0.42f;lamp.color[1]=0.30f;lamp.color[2]=0.16f;
            lamp.onModel=false;   // her face is lit as on the desktop (the lamp washed it out)
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

    bool SaveMahjongView(const std::wstring& path)
    {
        return state.renderer&&state.renderer->SaveLastFrame(path);
    }

    bool SaveMahjongShot(const std::wstring& path,int moves,std::uint32_t seed)
    {
        GdiplusStartupInput input;
        if(!state.gdiplus)GdiplusStartup(&state.gdiplus,&input,nullptr);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc=DefWindowProcW;
        wc.hInstance=GetModuleHandleW(nullptr);
        wc.lpszClassName=L"MikuMikuDesktopPerchMahjongShot";
        RegisterClassExW(&wc);
        const int dpi=static_cast<int>(GetDpiForSystem());
        state.window=CreateWindowExW(0,wc.lpszClassName,L"",WS_OVERLAPPEDWINDOW,0,0,MulDiv(static_cast<int>(BaseWidth)+16,dpi,96),MulDiv(static_cast<int>(BaseHeight)+39,dpi,96),
                                     nullptr,nullptr,wc.hInstance,nullptr);
        state.her=MahjongOpponent{L"ミク"};
        auto& t=state.table;
        t.NewGame(seed,Care);
        PlaySimply(moves);
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

    bool SaveMahjongTiles(const std::wstring& path)
    {
        GdiplusStartupInput input;
        if(!state.gdiplus)GdiplusStartup(&state.gdiplus,&input,nullptr);
        constexpr float S=2;   // as on a 192 dpi screen
        Bitmap sheet(static_cast<INT>(40+10*(HandW+4)*S),static_cast<INT>(40+4*(HandH+8)*S+3*(RiverH+8)*S),PixelFormat32bppARGB);
        Graphics g(&sheet);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Color(255,22,74,52));
        // Each suit in a row (with its red five last), the honors in the fourth; below, the same at the river's size.
        auto row=[&](int r,float w,float h,float top)
        {
            for(int i=0;i<10;++i)
            {
                Piece p{};
                if(r<3){if(i==9)p={r*9+4,true};else p={r*9+i,false};}
                else if(i<7)p={Mahjong::East+i,false};
                else continue;
                DrawTile(g,20+i*(w+4*S),top,w,h,p);
            }
        };
        for(int r=0;r<4;++r)row(r,HandW*S,HandH*S,20+r*(HandH+8)*S);
        for(int r=0;r<3;++r)row(r,RiverW*S,RiverH*S,20+4*(HandH+8)*S+r*(RiverH+8)*S);
        return SavePng(sheet,path);
    }

    void ShowMahjongWindow(HWND owner,const MahjongOpponent& her,std::function<void()> onEnd)
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
            // A copy the bitmap owns (GDI+ keeps pointing at the pixels it is given).
            Bitmap view(her.faceSize,her.faceSize,her.faceSize*4,PixelFormat32bppPARGB,const_cast<BYTE*>(state.her.face.data()));
            state.face.reset(view.Clone(0,0,her.faceSize,her.faceSize,PixelFormat32bppPARGB));
        }
        NewGame();

        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&info);
        const RECT work=info.rcWork;
        const int dpi=static_cast<int>(GetDpiForSystem());
        const int w=std::min(static_cast<int>(work.right-work.left)*9/10,MulDiv(static_cast<int>(BaseWidth)+16,dpi,96));
        const int h=std::min(static_cast<int>(work.bottom-work.top)*9/10,MulDiv(static_cast<int>(BaseHeight)+39,dpi,96));
        const std::wstring title=std::wstring(T(L"麻雀",L"Mahjong"))+L" - "+HerName();
        state.window=CreateWindowExW(0,ClassName,title.c_str(),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                                     work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,
                                     nullptr,nullptr,instance,nullptr);
        if(CreateView())PlaceView();
        SetTimer(state.window,TimerId,40,nullptr);
        ShowWindow(state.window,SW_SHOWNORMAL);
        SetForegroundWindow(state.window);
    }
}
