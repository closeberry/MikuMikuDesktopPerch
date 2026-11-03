#define NOMINMAX
#include "ChessWindow.hpp"
#include "Chess.hpp"
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
#include <cstring>
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
        using Chess::Phase;
        using Chess::Squares;
        constexpr wchar_t ClassName[]=L"MikuMikuDesktopPerchChess";
        constexpr wchar_t ViewClassName[]=L"MikuMikuDesktopPerchChessView";
        constexpr UINT_PTR TimerId=1;
        // How carefully she plays (Chess::Table::NewGame): a little less as you grow closer,
        // and after games you lost in a row.
        constexpr double Care=0.85,LessPerBond=0.03,LessPerLoss=0.08,LeastCare=0.4;
        constexpr double ThinkSeconds=1.0;         // how long she may read a move
        // Her pace: before her move (in 2D), before the game's end is shown.
        constexpr ULONGLONG HerPlayWait=700,EndWait=900;
        constexpr ULONGLONG SaidShown=1800;
        constexpr ULONGLONG ReachLead=600;         // her hand starts out this long before her man goes
        constexpr ULONGLONG HerPlayWait3D=1100;    // at the 3D table: time for her hand to go out
        constexpr ULONGLONG ThinkExtra=2000;       // her move later by this when she stops to think
        constexpr ULONGLONG EndWait3D=3400;        // at the 3D table: the end waits for the men's reaction
        // Sizes in logical pixels (at 96 dpi, at the size the window opens).
        constexpr float BaseWidth=1180,BaseHeight=840;
        constexpr float Cell2D=62;   // a square of the 2D board
        constexpr float ModelCell=6.75f;   // a square's side in the men's own units (the king's base is 5.3 across)

        // A man at the 3D table: where it is, its turn about the upright, how far it leans (a
        // rotation vector: x, then z) and how its neck leans (x, z) and twists, its size.
        struct Pose
        {
            float x=0,y=0,z=0,yaw=0,scale=1;
            float tilt[2]{};
            float neck[3]{};
        };
        struct PoseKey {double at=0;Pose pose;};
        struct Figure
        {
            int kind=0,colour=0;
            int square=-1;              // where it stands (-1: taken)
            Pose home;                  // where it rests when it has no keys
            std::vector<PoseKey> keys;  // where it is at each time (seconds of the clock), eased
            float phase=0,look=0;       // its own sway, and when it looks round
            float perk=0,flinch=0,alarm=0,droop=0;   // eased: chosen, about to be taken, in check, taken
            float aura=0;               // eased: how far its aura has come up
        };
        // A shape in the men's mesh.
        struct Shape {UINT indexStart=0,indexCount=0;INT baseVertex=0;float height=0,neckY=0,radius=0;};

        struct State
        {
            HWND window=nullptr;
            ULONG_PTR gdiplus=0;
            ChessOpponent her;
            std::unique_ptr<Bitmap> face;
            std::function<void()> onEnd;
            Chess::Table table;
            int lost=0;                // games you lost in a row
            int nextWhite=-1;          // who has white next game (-1: drawn by lot)
            bool gameOverTold=false;
            int hover=-1,cursor=-1;    // squares: under the mouse, and the keys' choice
            int chosen=-1;             // the square of the man you chose
            int promoting=-1;          // the square your pawn goes to, while you choose what it becomes
            RectF promoteButtons[4];
            bool confirmQuit=false;
            RectF quitButtons[2];
            ULONGLONG nextAt=0;
            std::wstring said;         // in the big bubble
            ULONGLONG saidAt=0;
            // Her words in the bubble (as at mahjong).
            struct Words
            {
                std::wstring text,fallback;
                ULONGLONG since=0,until=0;
                bool waiting=false;
                int weight=0;
                std::vector<std::wstring> recent;
                std::wstring revealing;
                double shown=0;
                ULONGLONG shownAt=0;
                bool voiced=false;   // the words are said by a voice file of their own
                std::wstring told;
            } words;
            // Her lines by scene (res/chess/lines.txt, said again her way by the LLM, and the
            // user's own), and the voice of a line that has one.
            GameLines lines;
            GameVoice voice;
            bool prefetching=false;
            POINT headScreen{-1,-1},besideHead{-1,-1};
            std::wstring afterword,afterwordLine;
            struct Seen
            {
                bool started=false,over=false;
                int moves=0;
            } seen;
            std::array<RectF,Squares> cellBoxes{};                 // the 2D board's squares
            struct ManMark {int square=-1;POINT base{},top{};float width=0,toEye=0;};
            std::vector<ManMark> manMarks;                         // the men standing, on the screen
            std::array<std::array<POINT,4>,Squares> cellQuads{};   // the 3D board's, on the screen
            bool quadsSet=false;
            std::unique_ptr<Bitmap> canvas;
            // Her at the table in 3D (none: 2D only).
            HWND view=nullptr;
            std::unique_ptr<MascotRenderer> renderer;
            int entrance=0;
            float herX=0,herZ=0,herAngle=0,entranceTime=0,chairZ=0;
            struct Camera {float yaw=0,pitch=0.58f,distance=3.25f,x=0,y=0,z=0;} camera;
            POINT dragFrom{};
            int dragging=0;
            float upper=10,hips=10;
            float floorY=-5;
            std::vector<int> scene;
            // The men, and until when some are on their way (on the clock); what happens on the way.
            std::vector<Figure> figures;
            std::array<std::array<Shape,7>,2> shapes{};
            UINT blobStart=0,blobCount=0;
            UINT sheathStart=0,sheathCount=0,ringStart=0,ringCount=0;   // the aura's, in the same mesh
            std::vector<MascotRenderer::Particle> auraMotes;            // its motes going up, this frame
            std::array<int,2> bayCount{};   // how many each side has taken
            double moveUntil=0;
            std::vector<std::pair<double,std::function<void()>>> dues;
            int movesSeen=-1;
            bool endingShown=false;
            bool autoPlay=false;            // (diagnostics) your moves are played too, live
            std::wstring framesDir;         // (diagnostics) frames saved here while the men move
            double frameEvery=0.2,nextFrameAt=0;
            int frameIndex=0;
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
        // Who has which colour, in words.
        std::wstring ColourOf(int who){return state.table.ColourOf(who)==Chess::White?T(L"白",L"white"):T(L"黒",L"black");}
        // What a colour's men on the board are worth (the king left out).
        int Worth(int colour)
        {
            static constexpr int Values[7]={0,100,300,300,500,900,0};
            int sum=0;
            const auto& b=state.table.Board();
            for(int s=0;s<Squares;++s)if(b.ColourAt(s)==colour)sum+=Values[b.KindAt(s)];
            return sum;
        }
        // How the game ended, in words.
        std::wstring EndingWords()
        {
            switch(state.table.HowEnded())
            {
            case Chess::Ending::Checkmate:return T(L"チェックメイト",L"Checkmate");
            case Chess::Ending::Stalemate:return T(L"ステイルメイト",L"Stalemate");
            case Chess::Ending::Repetition:return T(L"同じ局面の3回目",L"The same position three times");
            case Chess::Ending::FiftyMoves:return T(L"50手ルール",L"The fifty-move rule");
            case Chess::Ending::TooLittle:return T(L"駒が足りない",L"Not enough men to mate");
            default:return L"";
            }
        }

        void Sparkle(const std::array<float,3>& at,float scale=1);
        void Due(double at,std::function<void()> call){state.dues.push_back({at,std::move(call)});}
        // The way a man faces: toward the other side (the men's noses are toward -Z).
        float FacingOf(int colour){return colour==state.table.ColourOf(Chess::You)?DirectX::XM_PI:0.0f;}

        bool Waiting(){return state.nextAt>GetTickCount64();}
        // The men still on their way at the 3D table (a move not yet shown counts).
        bool Flying()
        {
            return state.renderer&&(state.movesSeen!=state.table.Moves()||state.clock<state.moveUntil);
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
                Log(L"chess: resize: %s",error.c_str());
            InvalidateRect(state.window,nullptr,FALSE);
        }

        void ReleaseView()
        {
            if(state.renderer){state.renderer->WaitIdle();state.renderer.reset();Log(L"chess: 3D view released");}
            state.view=nullptr;
        }

        // ---- The room (as the mahjong table's) ----

        constexpr int LabelCell=64,AtlasW=320,AtlasH=256;   // the 3D table's texture: the names of the files and ranks
        constexpr float RimColour[3]={0.13f,0.07f,0.035f};   // the board's frame, on top (the names' ground)

        float FloorY(){return state.floorY;}

        // The names of the files and ranks for the 3D table's texture, with its mipmaps.
        ImageRgba8 LabelsImage()
        {
            ImageRgba8 image;
            Bitmap painting(AtlasW,AtlasH,PixelFormat32bppARGB);
            {
                // a to h, 1 to 8, five to a row, cream on the frame's colour.
                Graphics p(&painting);
                p.SetTextRenderingHint(TextRenderingHintAntiAlias);
                auto level8=[](float c){return static_cast<BYTE>(std::lround(std::clamp(c,0.0f,1.0f)*255));};
                SolidBrush ground(Color(255,level8(RimColour[0]),level8(RimColour[1]),level8(RimColour[2])));
                p.FillRectangle(&ground,0,0,AtlasW,AtlasH);
                Font font(Family(),LabelCell*0.66f,FontStyleBold,UnitPixel);
                SolidBrush ink(Color(255,226,206,160));
                StringFormat format;
                format.SetAlignment(StringAlignmentCenter);
                format.SetLineAlignment(StringAlignmentCenter);
                for(int i=0;i<16;++i)
                {
                    const wchar_t name[2]={static_cast<wchar_t>(i<8?L'a'+i:L'1'+i-8),0};
                    p.DrawString(name,1,&font,RectF(static_cast<float>(i%5*LabelCell),static_cast<float>(i/5*LabelCell),LabelCell,LabelCell),&format,&ink);
                }
            }
            BitmapData bits{};
            Rect whole(0,0,AtlasW,AtlasH);
            if(painting.LockBits(&whole,ImageLockModeRead,PixelFormat32bppARGB,&bits)!=Ok)return image;
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
            painting.UnlockBits(&bits);
            GenerateMips(image);
            return image;
        }

        bool LoadMen(MascotRenderer& r);

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
                Log(L"chess: no 3D view (%s)",error.c_str());
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
            Log(L"chess: 3D view %ux%u on %s (hips %.1f, upper body %.1f, floor %.1f)",w,h,renderer->AdapterName().c_str(),state.hips,state.upper,state.floorY);
            {
                std::vector<MascotRenderer::GeometryVertex> room;
                BuildWesternRoom(room,state.roomGlows,{state.upper,state.hips,state.floorY,0.12f*state.hips+0.30f*state.upper,FieldZ()});
                if(!renderer->SetRoomGeometry(room))Log(L"chess: no room");
            }
            if(!renderer->SetGeometryTexture(LabelsImage()))Log(L"chess: no texture for the 3D table");
            if(!LoadMen(*renderer))
            {
                Log(L"chess: no men");
                renderer->WaitIdle();
                DestroyWindow(state.view);
                state.view=nullptr;
                return false;
            }
            state.renderer=std::move(renderer);
            state.entrance=0;
            state.scene.clear();
            state.camera={};
            state.dragging=0;
            state.sparks.clear();
            state.figures.clear();
            state.dues.clear();
            state.moveUntil=0;
            state.endingShown=false;
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

        // ---- The board and the men ----
        // The board lies in the middle of the felt, your side nearest you, whichever colour you
        // have; those taken stand beside it, at the right hand of whoever took them.

        float Top(){return 0.12f*state.hips+0.30f*state.upper;}
        float BoardZ(){return -1.12f*state.upper;}
        float FieldZ(){return BoardZ();}
        float Cell(){return 0.142f*state.upper;}
        float BoardTop(){return Top()+0.022f*state.upper;}
        float BoardHalf(){return 4*Cell();}
        float BoardRim(){return 0.052f*state.upper;}
        // Where a square is seen: row 0 at her side, column 0 at your left.
        int ColumnOf(int s){return state.table.ColourOf(Chess::You)==Chess::White?Chess::FileOf(s):7-Chess::FileOf(s);}
        int RowOf(int s){return state.table.ColourOf(Chess::You)==Chess::White?7-Chess::RankOf(s):Chess::RankOf(s);}
        int SquareAtCell(int column,int row)
        {
            return state.table.ColourOf(Chess::You)==Chess::White?(7-row)*8+column:row*8+(7-column);
        }
        float SquareX(int s){return (ColumnOf(s)-3.5f)*Cell();}
        float SquareZ(int s){return BoardZ()+(3.5f-RowOf(s))*Cell();}
        std::array<float,3> SquareSpot(int s){return {SquareX(s),BoardTop(),SquareZ(s)};}
        float ModelScale(){return Cell()/ModelCell;}
        float GroundAt(float x,float z)
        {
            const float edge=BoardHalf()+BoardRim();
            return std::abs(x)<edge&&std::abs(z-BoardZ())<edge?BoardTop():Top();
        }

        // Where the index-th man taken by who stands.
        Pose BayPose(int who,int index,int colour)
        {
            const float side=who==Chess::You?1.0f:-1.0f;
            const float step=0.78f*Cell(),x0=BoardHalf()+BoardRim()+0.72f*step;
            Pose p;
            p.x=side*(x0+(index/8)*step);
            p.z=BoardZ()-side*(3.5f-index%8)*step*1.05f;
            p.y=Top();
            p.yaw=FacingOf(colour);
            return p;
        }

        Pose RestAt(int square,int colour)
        {
            Pose p;
            p.x=SquareX(square);
            p.z=SquareZ(square);
            p.y=BoardTop();
            p.yaw=FacingOf(colour);
            return p;
        }

        Figure* FigureOn(int square)
        {
            if(square<0)return nullptr;
            for(auto& f:state.figures)if(f.square==square)return &f;
            return nullptr;
        }

        // Every man where the board says, nothing on its way; those missing in the bays.
        void PlaceFigures(bool drop)
        {
            const auto& b=state.table.Board();
            state.figures.clear();
            std::array<int,2> onBoard{};
            std::array<std::array<int,7>,2> counted{};
            std::uniform_real_distribution<float> any(0,1);
            auto add=[&](int kind,int colour,int square,const Pose& at){
                Figure f;
                f.kind=kind;
                f.colour=colour;
                f.square=square;
                f.home=at;
                f.phase=any(state.sparkRandom)*DirectX::XM_2PI;
                f.look=any(state.sparkRandom)*40;
                state.figures.push_back(std::move(f));
            };
            for(int s=0;s<Squares;++s)
            {
                const int k=b.KindAt(s);
                if(!k)continue;
                const int c=b.ColourAt(s);
                ++onBoard[static_cast<size_t>(c)];
                ++counted[static_cast<size_t>(c)][static_cast<size_t>(k)];
                add(k,c,s,RestAt(s,c));
            }
            // Those no longer on the board (a promoted pawn counts as the pawn gone).
            static constexpr int Full[7]={0,8,2,2,2,1,1};
            state.bayCount={};
            for(int c:{Chess::White,Chess::Black})
                for(int k=Chess::Queen;k>=Chess::Pawn;--k)
                {
                    int missing=Full[k]-counted[static_cast<size_t>(c)][static_cast<size_t>(k)];
                    if(k==Chess::Pawn)for(int up=Chess::Knight;up<=Chess::Queen;++up)missing-=std::max(0,counted[static_cast<size_t>(c)][static_cast<size_t>(up)]-Full[up]);
                    for(int i=0;i<missing;++i)
                    {
                        const int who=state.table.WhoHas(c^1);
                        add(k,c,-1,BayPose(who,state.bayCount[static_cast<size_t>(who)]++,c));
                    }
                }
            if(drop)
            {
                // A new game: they come down on their squares, rank by rank from each side's back.
                for(auto& f:state.figures)
                {
                    if(f.square<0)continue;
                    const int row=RowOf(f.square);
                    const int order=row<=1?row:7-row+2;
                    const double start=state.clock+0.25+order*0.22+ColumnOf(f.square)*0.035;
                    Pose above=f.home;
                    above.y+=2.6f*Cell();
                    above.scale=0;
                    f.keys.push_back({start,above});
                    Pose landing=f.home;
                    f.keys.push_back({start+0.26,landing});
                    Pose bounce=f.home;
                    bounce.y+=0.14f*Cell();
                    f.keys.push_back({start+0.37,bounce});
                    f.keys.push_back({start+0.48,f.home});
                    state.moveUntil=std::max(state.moveUntil,start+0.48);
                }
                for(int order=0;order<4;++order)Due(state.clock+0.25+order*0.22+0.34,[]{PlayMahjongSound(MahjongSound::CardPlace);});
            }
        }

        // A man's next poses are put after the last it has (or from where it is now).
        Pose LastOf(const Figure& f){return f.keys.empty()?f.home:f.keys.back().pose;}
        void Key(Figure& f,double at,const Pose& p)
        {
            if(f.keys.empty())f.keys.push_back({state.clock,f.home});
            f.keys.push_back({at,p});
            state.moveUntil=std::max(state.moveUntil,at);
        }
        void Lean(Pose& p,float dx,float dz,float amount)
        {
            p.tilt[0]=dz*amount;
            p.tilt[1]=-dx*amount;
        }
        void NeckLean(Pose& p,float dx,float dz,float amount)
        {
            p.neck[0]=dz*amount;
            p.neck[1]=-dx*amount;
        }

        // How a man of each kind goes: the height of its hops (in cells), and how long a hop takes.
        // The queen glides and the knight leaps (Travel).
        void HopsOf(int kind,float& height,double& each)
        {
            switch(kind)
            {
            case Chess::Pawn:height=0.20f;each=0.26;break;
            case Chess::Rook:height=0.07f;each=0.17;break;
            case Chess::Bishop:height=0.15f;each=0.18;break;
            case Chess::King:height=0.13f;each=0.36;break;
            default:height=0.14f;each=0.18;break;
            }
        }

        // The man goes in a straight line to (x, z), its way depending on its kind; t: when it
        // starts, moved on to when it arrives. It leans into the way it goes, its neck lagging
        // behind as it rises and nodding forward as it lands.
        void Travel(Figure& f,double& t,float x,float z,bool leap)
        {
            const Pose from=LastOf(f);
            const float ox=from.x,oz=from.z,gx=x-ox,gz=z-oz,length=std::sqrt(gx*gx+gz*gz);
            if(length<1e-4f)return;
            const float dx=gx/length,dz=gz/length,C=Cell(),y1=GroundAt(x,z);
            auto base=[&](float u){Pose p=from;p.x=ox+gx*u;p.z=oz+gz*u;p.y=from.y+(y1-from.y)*u;p.tilt[0]=p.tilt[1]=0;p.neck[0]=p.neck[1]=0;return p;};
            if(leap)
            {
                // The knight rears back, flies over the rest in one arc, and lands nodding.
                Pose crouch=base(0);
                NeckLean(crouch,dx,dz,-0.34f);
                Lean(crouch,dx,dz,-0.16f);
                Key(f,t+0.16,crouch);
                Pose up=base(0.5f);
                up.y+=1.5f*C;
                Lean(up,dx,dz,0.30f);
                NeckLean(up,dx,dz,-0.12f);
                Key(f,t+0.16+0.27,up);
                Pose down=base(1);
                NeckLean(down,dx,dz,0.42f);
                Lean(down,dx,dz,0.06f);
                Key(f,t+0.16+0.54,down);
                t+=0.16+0.54;
                Pose settled=base(1);
                Key(f,t+0.22,settled);
                t+=0.22;
                return;
            }
            if(f.kind==Chess::Queen)
            {
                // The queen glides, a little raised, neck held back.
                const double glide=0.30+0.16*length/C;
                Pose lifted=base(0.12f);
                lifted.y+=0.16f*C;
                Lean(lifted,dx,dz,0.13f);
                NeckLean(lifted,dx,dz,-0.20f);
                Key(f,t+glide*0.16,lifted);
                Pose along=base(0.88f);
                along.y+=0.16f*C;
                Lean(along,dx,dz,0.09f);
                NeckLean(along,dx,dz,-0.14f);
                Key(f,t+glide*0.84,along);
                Pose down=base(1);
                Lean(down,dx,dz,-0.05f);
                NeckLean(down,dx,dz,0.22f);
                Key(f,t+glide,down);
                t+=glide;
                Key(f,t+0.20,base(1));
                t+=0.20;
                return;
            }
            float height;
            double each;
            HopsOf(f.kind,height,each);
            const int hops=std::max(1,static_cast<int>(std::lround(length/C)));
            for(int i=0;i<hops;++i)
            {
                const float u0=static_cast<float>(i)/hops,u1=static_cast<float>(i+1)/hops;
                Pose up=base((u0+u1)/2);
                up.y+=height*C;
                Lean(up,dx,dz,0.14f);
                NeckLean(up,dx,dz,-0.20f);
                Key(f,t+each/2,up);
                Pose down=base(u1);
                Lean(down,dx,dz,0.03f);
                NeckLean(down,dx,dz,0.20f);
                Key(f,t+each,down);
                t+=each;
            }
            Key(f,t+0.14,base(1));
            t+=0.14;
        }

        // The man taken: it shrinks from the blow, flies off tumbling and comes to rest in the
        // bay of whoever took it, its head hanging.
        void Knocked(Figure& v,double hit,float dx,float dz,int capturer)
        {
            const float C=Cell();
            Pose cower=LastOf(v);
            NeckLean(cower,dx,dz,0.38f);
            Lean(cower,dx,dz,0.10f);
            Key(v,std::max(state.clock+0.02,hit-0.18),LastOf(v));
            Key(v,hit-0.02,cower);
            Pose bay=BayPose(capturer,state.bayCount[static_cast<size_t>(capturer)]++,v.colour);
            Pose flying=LastOf(v);
            const float gx=bay.x-flying.x,gz=bay.z-flying.z;
            Pose mid=flying;
            mid.x+=gx*0.5f;
            mid.z+=gz*0.5f;
            mid.y=std::max(flying.y,bay.y)+1.7f*C;
            Lean(mid,dx,dz,3.0f);
            mid.neck[0]=mid.neck[1]=0;
            Key(v,hit+0.34,mid);
            Pose landing=bay;
            Lean(landing,dx,dz,6.283f);
            Key(v,hit+0.70,landing);
            Pose bounce=bay;
            bounce.y+=0.22f*C;
            bounce.tilt[0]=bounce.tilt[1]=0;
            // (the turn taken off at once on landing: 2 pi is no turn at all)
            Pose settle=bay;
            settle.tilt[0]=settle.tilt[1]=0;
            Key(v,hit+0.70+0.0001,settle);
            Key(v,hit+0.83,bounce);
            Key(v,hit+0.96,settle);
            Due(hit,[at=std::array<float,3>{flying.x,flying.y+0.5f*C,flying.z}]{
                PlayMahjongSound(MahjongSound::CardFlip);
                Sparkle(at,0.8f);
            });
            Due(hit+0.70,[]{PlayMahjongSound(MahjongSound::CardPlace);});
        }

        // The last move, shown: the men on their way.
        void StartMove()
        {
            const auto& p=state.table.Last();
            Figure* f=FigureOn(p.move.from);
            if(!f)return;
            const int capturer=p.who;
            Figure* v=p.capturedAt>=0?FigureOn(p.capturedAt):nullptr;
            const float tx=SquareX(p.move.to),tz=SquareZ(p.move.to);
            double t=state.clock;
            f->square=p.move.to;
            if(v==f)v=nullptr;
            if(v)
            {
                v->square=-1;
                const Pose target=LastOf(*v);
                const Pose from=LastOf(*f);
                const float gx=target.x-from.x,gz=target.z-from.z,length=std::sqrt(gx*gx+gz*gz);
                const float dx=length>0?gx/length:0,dz=length>0?gz/length:1;
                const bool leap=p.moved==Chess::Knight;
                // Up to a blow's distance from it (a knight comes down right before it).
                const float reach=0.74f*Cell();
                if(length>reach+0.05f*Cell())Travel(*f,t,target.x-dx*reach,target.z-dz*reach,leap);
                // It draws its head back and swings it through.
                Pose back=LastOf(*f);
                NeckLean(back,dx,dz,-0.62f);
                Lean(back,dx,dz,-0.14f);
                Key(*f,t+0.26,back);
                t+=0.26;
                Pose swing=LastOf(*f);
                NeckLean(swing,dx,dz,0.78f);
                Lean(swing,dx,dz,0.26f);
                swing.x+=dx*0.08f*Cell();
                swing.z+=dz*0.08f*Cell();
                Key(*f,t+0.10,swing);
                t+=0.10;
                Knocked(*v,t,dx,dz,capturer);
                // Then it steps into the place.
                t+=0.12;
                Pose onto=LastOf(*f);
                onto.tilt[0]=onto.tilt[1]=0;
                onto.neck[0]=onto.neck[1]=0;
                const float ox=onto.x,oz=onto.z;
                onto.x=(ox+tx)/2;
                onto.z=(oz+tz)/2;
                onto.y=BoardTop()+0.22f*Cell();
                Key(*f,t+0.13,onto);
                Pose there=onto;
                there.x=tx;
                there.z=tz;
                there.y=BoardTop();
                Key(*f,t+0.26,there);
                t+=0.26;
                Due(t,[]{PlayMahjongSound(MahjongSound::CardPlace);});
            }
            else
            {
                Travel(*f,t,tx,tz,p.moved==Chess::Knight);
                Due(t-0.16,[]{PlayMahjongSound(MahjongSound::CardPlace);});
            }
            if(p.rookFrom>=0)
                if(Figure* rook=FigureOn(p.rookFrom))
                {
                    // Castling: the rook hops over its king.
                    rook->square=p.rookTo;
                    double rt=state.clock+0.25;
                    Travel(*rook,rt,SquareX(p.rookTo),SquareZ(p.rookTo),true);
                    Due(rt-0.2,[]{PlayMahjongSound(MahjongSound::CardPlace);});
                }
            if(p.move.promote)
            {
                // Promotion: it rises turning, and comes back down what it has become.
                const int index=static_cast<int>(f-state.figures.data()),kind=p.move.promote;
                Pose high=LastOf(*f);
                high.y+=1.1f*Cell();
                high.yaw+=DirectX::XM_2PI*2;
                high.scale=0.55f;
                Key(*f,t+0.42,high);
                t+=0.42;
                Due(t,[index,kind,at=std::array<float,3>{tx,BoardTop()+1.6f*Cell(),tz}]{
                    state.figures[static_cast<size_t>(index)].kind=kind;
                    Sparkle(at,1.4f);
                    PlayMahjongSound(MahjongSound::CardFlip);
                });
                Pose down=LastOf(*f);
                down.y=BoardTop();
                down.yaw-=DirectX::XM_2PI*2;
                down.scale=1;
                // (the yaw brought back at the top: a whole turn or two looks the same)
                Pose same=LastOf(*f);
                same.yaw=down.yaw;
                Key(*f,t+0.0001,same);
                Key(*f,t+0.36,down);
                t+=0.36;
                Due(t,[]{PlayMahjongSound(MahjongSound::CardPlace);});
            }
            // Her hand went out as it began; hers with a big piece taken get a little sparkle.
            if(p.who==Chess::Her&&p.captured>=Chess::Rook)Due(t,[at=std::array<float,3>{tx,BoardTop(),tz}]{Sparkle(at);});
            state.moveUntil=std::max(state.moveUntil,t);
        }

        // The game's end, shown once the last move has landed: a mated king falls over, the
        // winner's men jump for joy in a wave; at a draw the kings bow to each other.
        void StartEnding()
        {
            auto& t=state.table;
            const double start=std::max(state.clock,state.moveUntil)+0.2;
            if(t.HowEnded()==Chess::Ending::Checkmate)
            {
                const int loser=t.ColourOf(t.Winner()^1);
                if(Figure* king=FigureOn(t.Board().KingOf(loser)))
                {
                    const float toward=king->home.z>BoardZ()?1.0f:-1.0f;
                    double at=start;
                    for(int i=0;i<4;++i)
                    {
                        Pose shake=king->home;
                        shake.neck[1]=(i%2?1:-1)*0.26f;
                        shake.tilt[1]=(i%2?1:-1)*0.05f;
                        at+=0.13;
                        Key(*king,at,shake);
                    }
                    Pose fallen=king->home;
                    Pose tip=king->home;
                    Lean(tip,0,toward,0.42f);
                    tip.neck[0]=toward*0.30f;
                    at+=0.38;
                    Key(*king,at,tip);
                    Lean(fallen,0,toward,DirectX::XM_PIDIV2-0.06f);
                    fallen.neck[0]=toward*0.30f;
                    fallen.y+=state.shapes[0][Chess::King].radius*ModelScale()*0.9f;
                    at+=0.24;
                    Key(*king,at,fallen);
                    Due(at,[at3=std::array<float,3>{fallen.x,BoardTop(),fallen.z+toward*Cell()}]{
                        PlayMahjongSound(MahjongSound::CardPlace);
                        Sparkle(at3,1.2f);
                    });
                    for(auto& f:state.figures)
                    {
                        if(f.colour==loser||f.square<0)continue;
                        double when=at+0.2+ColumnOf(f.square)*0.08+RowOf(f.square)*0.05;
                        for(int hop=0;hop<3;++hop)
                        {
                            Pose up=f.home;
                            up.y+=0.42f*Cell();
                            up.neck[0]=(hop%2?1:-1)*0.18f;
                            Key(f,when+0.16,up);
                            Key(f,when+0.32,f.home);
                            when+=0.32;
                        }
                    }
                }
            }
            else
                for(int c:{Chess::White,Chess::Black})
                    if(Figure* king=FigureOn(t.Board().KingOf(c)))
                    {
                        const float toward=king->home.z>BoardZ()?-1.0f:1.0f;
                        Pose bow=king->home;
                        Lean(bow,0,toward,0.16f);
                        NeckLean(bow,0,toward,0.62f);
                        Key(*king,start+0.5,bow);
                        Key(*king,start+1.3,bow);
                        Key(*king,start+1.8,king->home);
                    }
        }

        // The pose eased between its keys (or none: it rests where it is).
        Pose PoseOf(const Figure& f)
        {
            if(f.keys.empty())return f.home;
            if(state.clock<=f.keys.front().at)return f.keys.front().pose;
            for(size_t i=1;i<f.keys.size();++i)
            {
                const auto& b=f.keys[i];
                if(state.clock>b.at)continue;
                const auto& a=f.keys[i-1];
                const float u=b.at>a.at?static_cast<float>((state.clock-a.at)/(b.at-a.at)):1,e=u*u*(3-2*u);
                auto mix=[&](float p,float q){return p+(q-p)*e;};
                Pose p;
                p.x=mix(a.pose.x,b.pose.x);
                p.y=mix(a.pose.y,b.pose.y);
                p.z=mix(a.pose.z,b.pose.z);
                p.yaw=mix(a.pose.yaw,b.pose.yaw);
                p.scale=mix(a.pose.scale,b.pose.scale);
                for(int k=0;k<2;++k)
                {
                    p.tilt[k]=mix(a.pose.tilt[k],b.pose.tilt[k]);
                    p.neck[k]=mix(a.pose.neck[k],b.pose.neck[k]);
                }
                p.neck[2]=mix(a.pose.neck[2],b.pose.neck[2]);
                return p;
            }
            return f.keys.back().pose;
        }

        // A soft round shadow in the mesh: one unit across its middle, fading out to its rim.
        constexpr int BlobRings=6,BlobSlices=28;
        void AddBlob(std::vector<MascotRenderer::PieceVertex>& v,std::vector<std::uint32_t>& ix)
        {
            const std::uint32_t first=static_cast<std::uint32_t>(v.size());
            for(int r=0;r<=BlobRings;++r)
                for(int s=0;s<BlobSlices;++s)
                {
                    const float u=static_cast<float>(r)/BlobRings,a=s*DirectX::XM_2PI/BlobSlices;
                    const float fade=1-u*u*(3-2*u);
                    MascotRenderer::PieceVertex k{};
                    k.position[0]=std::cos(a)*u;
                    k.position[2]=std::sin(a)*u;
                    k.normal[1]=1;
                    k.color[3]=0.998f*fade;
                    v.push_back(k);
                }
            for(int r=0;r<BlobRings;++r)
                for(int s=0;s<BlobSlices;++s)
                {
                    const std::uint32_t a=first+r*BlobSlices+s,b=first+r*BlobSlices+(s+1)%BlobSlices,c=a+BlobSlices,d=b+BlobSlices;
                    for(std::uint32_t k:{a,c,b,b,c,d})ix.push_back(k);
                }
        }

        // An aura in the mesh: a sheath one unit across and high, widening a little upward, and a
        // flat ring for the ground (see PiecePS.hlsl).
        void AddAura(std::vector<MascotRenderer::PieceVertex>& v,std::vector<std::uint32_t>& ix)
        {
            constexpr int Rings=12,Slices=48;
            auto vertex=[&](float x,float y,float z,float kind){
                MascotRenderer::PieceVertex k{};
                k.position[0]=x;k.position[1]=y;k.position[2]=z;
                k.normal[1]=1;
                k.color[0]=kind;
                k.color[3]=4;
                v.push_back(k);
            };
            auto strip=[&](std::uint32_t first,int rings){
                for(int r=0;r<rings;++r)
                    for(int s=0;s<Slices;++s)
                    {
                        const std::uint32_t a=first+r*Slices+s,b=first+r*Slices+(s+1)%Slices,c=a+Slices,d=b+Slices;
                        for(std::uint32_t k:{a,c,b,b,c,d})ix.push_back(k);
                    }
            };
            state.sheathStart=static_cast<UINT>(ix.size());
            std::uint32_t first=static_cast<std::uint32_t>(v.size());
            for(int r=0;r<=Rings;++r)
                for(int s=0;s<Slices;++s)
                {
                    const float u=static_cast<float>(r)/Rings,a=s*DirectX::XM_2PI/Slices,across=1+0.22f*u;
                    vertex(std::sin(a)*across,u,std::cos(a)*across,0);
                }
            strip(first,Rings);
            state.sheathCount=static_cast<UINT>(ix.size())-state.sheathStart;
            state.ringStart=static_cast<UINT>(ix.size());
            first=static_cast<std::uint32_t>(v.size());
            for(int r=0;r<=2;++r)
                for(int s=0;s<Slices;++s)
                {
                    const float across=r==0?0.0f:r==1?0.6f:1.3f,a=s*DirectX::XM_2PI/Slices;
                    vertex(std::sin(a)*across,0,std::cos(a)*across,1);
                }
            strip(first,2);
            state.ringCount=static_cast<UINT>(ix.size())-state.ringStart;
        }

        // The six shapes (res/chess/pieces.bin) in each colour, for the renderer.
        bool LoadMen(MascotRenderer& r)
        {
            const HRSRC info=FindResourceW(nullptr,L"PIECES",L"CHESSMODEL");
            const HGLOBAL data=info?LoadResource(nullptr,info):nullptr;
            if(!data)return false;
            const auto* bytes=static_cast<const std::uint8_t*>(LockResource(data));
            const size_t size=SizeofResource(nullptr,info);
            if(size<8||std::memcmp(bytes,"CHP1",4)!=0)return false;
            size_t at=8;
            // Body, felt, carving, iris, pupil. White is crystal (its colour what it lets through),
            // black metal (its colour what it mirrors): see PiecePS.hlsl.
            static constexpr float Colours[2][5][3]={
                {{1.0f,1.0f,1.0f},{1.0f,1.0f,1.0f},{0.8f,0.85f,0.9f},{0.10f,0.05f,0.02f},{0.01f,0.008f,0.006f}},
                {{0.36f,0.37f,0.42f},{0.03f,0.10f,0.05f},{0.20f,0.21f,0.25f},{0.10f,0.05f,0.02f},{0.01f,0.008f,0.006f}}};
            std::vector<MascotRenderer::PieceVertex> vertices;
            std::vector<std::uint32_t> indices;
            struct Raw {std::uint16_t x,y,z;std::int16_t u,v;std::uint16_t neck;std::uint8_t material,pad;};
            static_assert(sizeof(Raw)==14);
            for(int kind=Chess::Pawn;kind<=Chess::King;++kind)
            {
                std::uint32_t nv,ni;
                float height,neckY,radius;
                if(at+20>size)return false;
                std::memcpy(&nv,bytes+at,4);
                std::memcpy(&ni,bytes+at+4,4);
                std::memcpy(&height,bytes+at+8,4);
                std::memcpy(&neckY,bytes+at+12,4);
                std::memcpy(&radius,bytes+at+16,4);
                at+=20;
                if(at+nv*14ull+ni*2ull>size)return false;
                const size_t rawAt=at,indexAt=at+nv*14ull;
                at=indexAt+ni*2ull;
                for(int colour:{Chess::White,Chess::Black})
                {
                    auto& shape=state.shapes[colour][kind];
                    shape.indexStart=static_cast<UINT>(indices.size());
                    shape.indexCount=ni;
                    shape.baseVertex=static_cast<INT>(vertices.size());
                    shape.height=height;
                    shape.neckY=neckY;
                    shape.radius=radius;
                    for(std::uint32_t i=0;i<nv;++i)
                    {
                        Raw q;
                        std::memcpy(&q,bytes+rawAt+i*14ull,14);
                        MascotRenderer::PieceVertex k{};
                        k.position[0]=q.x/65535.0f*32-16;
                        k.position[1]=q.y/65535.0f*32-16;
                        k.position[2]=q.z/65535.0f*32-16;
                        float nx=q.u/32767.0f,ny=q.v/32767.0f,nz=1-std::abs(nx)-std::abs(ny);
                        if(nz<0)
                        {
                            const float ox=nx;
                            nx=(1-std::abs(ny))*(ox>=0?1.0f:-1.0f);
                            ny=(1-std::abs(ox))*(ny>=0?1.0f:-1.0f);
                        }
                        const float l=std::sqrt(nx*nx+ny*ny+nz*nz);
                        k.normal[0]=nx/l;k.normal[1]=ny/l;k.normal[2]=nz/l;
                        const auto& c=Colours[colour][std::min<int>(q.material,4)];
                        k.color[0]=c[0];k.color[1]=c[1];k.color[2]=c[2];
                        k.color[3]=q.material>=3?1.0f:colour==Chess::White?2.0f:q.material==1?1.0f:3.0f;
                        k.neck=q.neck/65535.0f;
                        vertices.push_back(k);
                    }
                    for(std::uint32_t i=0;i<ni;++i)
                    {
                        std::uint16_t index;
                        std::memcpy(&index,bytes+indexAt+i*2ull,2);
                        indices.push_back(index);
                    }
                }
            }
            state.blobStart=static_cast<UINT>(indices.size());
            AddBlob(vertices,indices);
            state.blobCount=static_cast<UINT>(indices.size())-state.blobStart;
            AddAura(vertices,indices);
            const bool ok=r.SetPieceMesh(vertices,indices);
            if(ok)Log(L"chess: men loaded (%u vertices, %u triangles)",static_cast<unsigned>(vertices.size()),static_cast<unsigned>(indices.size()/3));
            return ok;
        }

        // A flat round spot on the board.
        void AddSpot(Mesh& m,float x,float z,float r,const float (&colour)[4],float inner=0)
        {
            constexpr int N=24;
            const float y=BoardTop()+0.0016f*state.upper;
            for(int i=0;i<N;++i)
            {
                const float a0=i*DirectX::XM_2PI/N,a1=(i+1)*DirectX::XM_2PI/N;
                const float c0=std::cos(a0),s0=std::sin(a0),c1=std::cos(a1),s1=std::sin(a1);
                m.Quad({x+c0*inner,y,z+s0*inner},{x+c0*r,y,z+s0*r},{x+c1*r,y,z+s1*r},{x+c1*inner,y,z+s1*inner},{0,1,0},colour);
            }
        }

        // A square washed in a colour (the last move, a king in check).
        void AddWash(Mesh& m,int s,const float (&colour)[4],float lift=0.0010f)
        {
            const float x=SquareX(s),z=SquareZ(s),c=Cell()/2,y=BoardTop()+lift*state.upper;
            m.Quad({x-c,y,z-c},{x+c,y,z-c},{x+c,y,z+c},{x-c,y,z+c},{0,1,0},colour);
        }
        void AddFrame(Mesh& m,int s,const float (&colour)[4],float width)
        {
            const float x=SquareX(s),z=SquareZ(s),c=Cell()/2,w=width*state.upper,y=BoardTop()+0.0013f*state.upper;
            m.Quad({x-c,y,z-c},{x+c,y,z-c},{x+c,y,z-c+w},{x-c,y,z-c+w},{0,1,0},colour);
            m.Quad({x-c,y,z+c-w},{x+c,y,z+c-w},{x+c,y,z+c},{x-c,y,z+c},{0,1,0},colour);
            m.Quad({x-c,y,z-c},{x-c+w,y,z-c},{x-c+w,y,z+c},{x-c,y,z+c},{0,1,0},colour);
            m.Quad({x+c-w,y,z-c},{x+c,y,z-c},{x+c,y,z+c},{x+c-w,y,z+c},{0,1,0},colour);
        }

        // The square pointed at (the mouse, or the keys' choice).
        int Pointed(){return state.hover>=0?state.hover:state.cursor;}
        // The moves of the man chosen.
        std::vector<Chess::Move> ChosenMoves(){return state.chosen>=0?state.table.MovesFrom(state.chosen):std::vector<Chess::Move>{};}
        bool IsChosenTarget(int square)
        {
            const auto moves=ChosenMoves();
            return std::any_of(moves.begin(),moves.end(),[&](const Chess::Move& m){return m.to==square;});
        }

        // What the table's geometry shows.
        std::vector<int> Scene()
        {
            auto& t=state.table;
            std::vector<int> out;
            out.push_back(t.Moves());
            out.push_back(t.ColourOf(Chess::You));
            out.push_back(YourMoveNow()?1:0);
            out.push_back(state.chosen);
            out.push_back(Pointed());
            out.push_back(Flying()?1:0);
            out.push_back(t.Now()==Phase::GameOver?1:0);
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
            // The board: a dark wooden frame with a thin pale line inside it, squares of maple and walnut.
            const float half=BoardHalf(),rim=BoardRim(),bz=BoardZ(),bt=BoardTop();
            const float frame[4]={0.20f,0.10f,0.05f,Wood},inlay[4]={0.72f,0.60f,0.38f,1};
            const float light[4]={0.80f,0.66f,0.46f,1},dark[4]={0.30f,0.17f,0.09f,1};
            mesh.grain=0;mesh.grainScale=0.35f;
            mesh.Box(-half-rim,t.top,bz-half-rim,half+rim,bt-0.0004f*D,bz+half+rim,frame,false);
            const float rimTop[4]={RimColour[0],RimColour[1],RimColour[2],1};
            {
                const float y=bt-0.0004f*D;
                mesh.Quad({-half-rim,y,bz-half-rim},{half+rim,y,bz-half-rim},{half+rim,y,bz+half+rim},{-half-rim,y,bz+half+rim},{0,1,0},rimTop);
                // The names along your edges, read from your side.
                const float white[4]={1,1,1,1},r=0.42f*rim,ly=y+0.0003f*D;
                auto label=[&](int index,float x,float z){
                    const float u0=(index%5)/5.0f,u1=(index%5+1)/5.0f,v0=(index/5)*LabelCell/static_cast<float>(AtlasH),v1=(index/5+1)*LabelCell/static_cast<float>(AtlasH);
                    const float uv[8]={u0,v1,u1,v1,u1,v0,u0,v0};
                    mesh.Quad({x-r,ly,z-r},{x+r,ly,z-r},{x+r,ly,z+r},{x-r,ly,z+r},{0,1,0},white,uv);
                };
                for(int i=0;i<8;++i)
                {
                    label(Chess::FileOf(SquareAtCell(i,7)),(i-3.5f)*Cell(),bz-half-rim/2);
                    label(8+Chess::RankOf(SquareAtCell(0,i)),-half-rim/2,bz+(3.5f-i)*Cell());
                }
            }
            const float line=0.006f*D,ly=bt-0.0002f*D;
            mesh.Quad({-half-line,ly,bz-half-line},{half+line,ly,bz-half-line},{half+line,ly,bz+half+line},{-half-line,ly,bz+half+line},{0,1,0},inlay);
            for(int s=0;s<Squares;++s)
            {
                const float x=SquareX(s),z=SquareZ(s),c=Cell()/2;
                const bool isDark=(Chess::FileOf(s)+Chess::RankOf(s))%2==0;
                mesh.Quad({x-c,bt,z-c},{x+c,bt,z-c},{x+c,bt,z+c},{x-c,bt,z+c},{0,1,0},isDark?dark:light);
            }
            auto& g=state.table;
            const auto& last=g.Last();
            if(last.move.Valid())
            {
                const float wash[4]={0.62f,0.72f,0.30f,1};
                for(int s:{last.move.from,last.move.to})
                {
                    const bool isDark=(Chess::FileOf(s)+Chess::RankOf(s))%2==0;
                    const float shade=isDark?0.62f:1.0f;
                    const float c[4]={wash[0]*shade,wash[1]*shade,wash[2]*shade,1};
                    AddWash(mesh,s,c);
                }
            }
            if(g.InCheck()&&!Flying())
            {
                const float red[4]={0.78f,0.16f,0.12f,1};
                AddWash(mesh,g.Board().KingOf(g.Board().ToMove()),red,0.0012f);
            }
            if(YourMoveNow())
            {
                const float gold[4]={1.0f,0.80f,0.32f,1},pale[4]={0.95f,0.86f,0.50f,1},red[4]={0.92f,0.30f,0.22f,1};
                const int at=Pointed();
                if(state.chosen>=0)AddFrame(mesh,state.chosen,gold,0.009f);
                bool seen[Squares]{};
                for(const auto& m:ChosenMoves())
                {
                    if(seen[m.to])continue;
                    seen[m.to]=true;
                    const bool takes=g.Board().At(m.to)||(g.Board().KindAt(m.from)==Chess::Pawn&&m.to==g.Board().EnPassant());
                    const float x=SquareX(m.to),z=SquareZ(m.to);
                    if(takes)AddSpot(mesh,x,z,0.48f*Cell(),m.to==at?red:gold,0.40f*Cell());
                    else AddSpot(mesh,x,z,(m.to==at?0.26f:0.13f)*Cell(),m.to==at?gold:pale);
                }
                if(at>=0&&at!=state.chosen&&state.cursor>=0&&state.hover<0)AddFrame(mesh,at,gold,0.005f);
                else if(at>=0&&state.chosen<0&&g.Board().ColourAt(at)==g.ColourOf(Chess::You)&&!g.MovesFrom(at).empty())AddFrame(mesh,at,pale,0.005f);
            }
            return std::move(mesh.out);
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
                state.afterword=T(L"いま、話し相手とチェスをしていたが、途中でやめてデスクトップに戻ってきたところ。そのことを話し相手にひと言（1〜2文）",
                                  L"You were playing chess with them, but they stopped midway and you have come back to the desktop. Say a word about it to them (one or two sentences)");
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
            // The first game's white by lot; then it goes back and forth.
            const int white=state.nextWhite>=0?state.nextWhite:static_cast<int>(device()%2);
            state.nextWhite=1-white;
            state.table.NewGame(device(),std::max(LeastCare,Care-LessPerBond*state.her.bond-LessPerLoss*state.lost),white==Chess::You?Chess::White:Chess::Black,ThinkSeconds);
            state.hover=state.cursor=state.chosen=state.promoting=-1;
            state.confirmQuit=false;
            state.said.clear();
            state.nextAt=GetTickCount64()+HerPlayWait;
            state.seen={};
            state.words.waiting=false;
            state.words.text.clear();
            state.afterword.clear();
            state.afterwordLine.clear();
            state.plannedFor=state.reachedFor=-1;
            state.movesSeen=-1;
            Prefetch();
        }

        // After a move of yours: the next waits a moment.
        void Moved()
        {
            state.hover=-1;
            state.chosen=state.promoting=-1;
            const ULONGLONG now=GetTickCount64();
            state.nextAt=now+(state.table.Now()==Phase::GameOver?(state.renderer?EndWait3D:EndWait):HerPlayWait);
            Redraw();
        }

        // A square pointed at and clicked (or Enter): the man chosen, or where it goes.
        void Choose(int square)
        {
            auto& t=state.table;
            if(!YourMoveNow()||square<0)return;
            if(state.chosen>=0&&IsChosenTarget(square))
            {
                const auto moves=t.MovesFrom(state.chosen);
                std::vector<Chess::Move> there;
                for(const auto& m:moves)if(m.to==square)there.push_back(m);
                if(there.size()>1){state.promoting=square;Redraw();return;}
                t.Play(there[0]);
                Moved();
                return;
            }
            if(square==state.chosen)state.chosen=-1;
            else if(t.Board().ColourAt(square)==t.ColourOf(Chess::You)&&!t.MovesFrom(square).empty())
            {
                state.chosen=square;
                PlayMahjongSound(MahjongSound::CardTake);
            }
            else state.chosen=-1;
            Redraw();
        }

        void Promote(int kind)
        {
            if(state.promoting<0||state.chosen<0)return;
            const Chess::Move m{state.chosen,state.promoting,kind};
            state.promoting=-1;
            if(state.table.CanPlay(m))
            {
                state.table.Play(m);
                Moved();
            }
            Redraw();
        }

        void Continue()
        {
            auto& t=state.table;
            if(t.Now()==Phase::GameOver)
            {
                state.lost=t.Winner()==Chess::Her?state.lost+1:0;
                NewGame();
            }
            Redraw();
        }

        // In 2D, a man's sound as soon as it moves (at the 3D table, as it lands).
        void Listen()
        {
            if(state.renderer)return;
            auto& t=state.table;
            if(t.Moves()>state.heardMoves&&state.heardMoves>=0)
            {
                PlayMahjongSound(MahjongSound::CardPlace);
                if(t.Last().captured)PlayMahjongSound(MahjongSound::CardFlip);
            }
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
            state.words.told+=Fmt(L"  {0} s, weight {1}: {2}\n",{std::to_wstring(static_cast<int>(state.clock)),std::to_wstring(weight),seen});
            if(state.her.onEvent&&!theirs.empty())state.her.onEvent(seen,weight,Pick(theirs));
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
        void Say(const wchar_t* name,int weight=2,double chance=1)
        {
            if(!state.lines.Roll(name,chance,state.sparkRandom))return;
            GameLine line;
            if(state.lines.Pick(name,state.words.recent,state.sparkRandom,line))SayLine(line,weight);
        }

        // What is new in the game since the last tick, for words: its start, a check, a big piece
        // taken, a promotion, the end.
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
            if(Flying())return;   // the men are still on their way
            const std::wstring name=HerName();
            if(!seen.started)
            {
                seen.started=true;
                const bool first=t.ColourOf(Chess::Her)==Chess::White;
                Happened(6,Fmt(T(L"これから話し相手とチェスの対局が始まる。あなたは{0}で、{1}。",L"A game of chess with them is about to begin. You have {0} and {1}. "),
                               {ColourOf(Chess::Her),first?T(L"先に指す",L"move first"):T(L"後から指す",L"move second")}),
                         T(L"わくわくする気持ち",L"how you look forward to it"),
                         L"始まり",
                         Fmt(T(L"{0}と、このパソコンの持ち主のチェスの対局が始まる。",L"A game of chess between {0} and the owner of this computer is about to begin."),{name}),
                         {T(L"がんばれー！",L"Good luck, you two!"),T(L"どっちが勝つかな？",L"Who's going to win?")});
            }
            if(t.Moves()!=seen.moves)
            {
                seen.moves=t.Moves();
                const auto& m=t.Last();
                const bool over=t.Now()==Phase::GameOver;
                if(!over&&m.who==Chess::You&&m.move.promote)
                    Happened(4,T(L"チェスで、話し相手のポーンが最後の列まで進んで成った。",L"At chess, their pawn has reached the last rank and been promoted."),
                             T(L"あわてる気持ち",L"how alarmed you are"),
                             L"相手の昇格",
                             Fmt(T(L"チェスで、持ち主のポーンが昇格した（{0}との対局）。",L"At chess, the owner's pawn was promoted (against {0})."),{name}),
                             {T(L"おお、昇格だ！",L"Ooh, a promotion!")});
                else if(!over&&m.who==Chess::Her&&m.move.promote)
                    Happened(4,T(L"チェスで、あなたのポーンが最後の列まで進んで成った。",L"At chess, your pawn has reached the last rank and been promoted."),
                             T(L"うれしい気持ち",L"how pleased you are"),
                             L"自分の昇格",
                             Fmt(T(L"チェスで、{0}のポーンが昇格した。",L"At chess, {0}'s pawn was promoted."),{name}),
                             {T(L"おお、昇格だ！",L"Ooh, a promotion!")});
                else if(!over&&m.who==Chess::You&&m.check)
                    Happened(4,T(L"チェスで、話し相手にチェックをかけられた。",L"At chess, they have put your king in check."),
                             T(L"あわてる気持ち",L"how alarmed you are"),
                             L"チェックされた",
                             Fmt(T(L"チェスで、持ち主がチェックをかけた（{0}との対局）。",L"At chess, the owner gave check (against {0})."),{name}),
                             {T(L"チェックだ！",L"Check!")});
                else if(!over&&m.who==Chess::Her&&m.check)
                    Happened(3,T(L"チェスで、あなたが話し相手にチェックをかけた。",L"At chess, you have put their king in check."),
                             T(L"得意な気持ち",L"how pleased you are with it"),
                             L"チェック",
                             Fmt(T(L"チェスで、{0}がチェックをかけた。",L"At chess, {0} gave check."),{name}),
                             {T(L"チェックだ！",L"Check!")});
                else if(!over&&m.who==Chess::You&&m.captured>=Chess::Rook)
                    Happened(5,T(L"チェスで、話し相手にあなたの大きな駒を取られた。",L"At chess, they have taken one of your big pieces."),
                             T(L"しまったという気持ち",L"that you wish you had seen it coming"),
                             L"取られた",
                             Fmt(T(L"チェスで、持ち主が{0}の大きな駒を取った。",L"At chess, the owner took a big piece of {0}'s."),{name}),
                             {T(L"おっ、大きい！",L"Ooh, that's a big one!"),T(L"いい手だね！",L"Nice move!")});
                else if(!over&&m.who==Chess::Her&&m.captured>=Chess::Rook)
                    Happened(4,T(L"チェスで、あなたが話し相手の大きな駒を取った。",L"At chess, you have taken one of their big pieces."),
                             T(L"うれしい気持ち",L"how pleased you are"),
                             L"取った",
                             Fmt(T(L"チェスで、{0}が持ち主の大きな駒を取った。",L"At chess, {0} took a big piece of the owner's."),{name}),
                             {Fmt(T(L"{0}、大きいのを取った！",L"{0} took a big one!"),{name})});
            }
            const bool over=t.Now()==Phase::GameOver;
            if(over&&!seen.over)
            {
                const bool won=t.Winner()==Chess::Her,tie=t.Winner()<0;
                const std::wstring how=EndingWords();
                state.afterword=Fmt(T(L"いま、話し相手とチェスを最後まで指して、デスクトップに戻ってきたところ。結果は{0}（{1}）。その対局の感想を、話し相手にひと言（1〜2文）",
                                      L"You have just played a game of chess to its end with them and come back to the desktop. {0} ({1}). Tell them what you thought of the game (one or two sentences)"),
                                    {won?T(L"あなたの勝ち",L"You won"):tie?T(L"引き分け",L"A draw"):T(L"あなたの負け",L"You lost"),how});
                state.afterwordLine=won?T(L"チェス、楽しかったね！またやろう。",L"That was fun! Let's play again sometime."):T(L"くやしいなあ…次は勝つからね！",L"So close... I'll win next time!");
                if(won)Happened(9,T(L"チェスの対局が終わって、あなたの勝ち（",L"The game of chess is over, and you won (")+how+T(L"）。",L")."),
                                T(L"勝ってうれしい気持ち",L"how glad you are to have won"),
                                L"勝った",
                                Fmt(T(L"{0}と持ち主のチェスの対局が終わり、{0}が勝った。",L"The chess game between {0} and the owner is over: {0} won."),{name}),
                                {T(L"おつかれさま！いい勝負だったね。",L"Well played, both of you!"),Fmt(T(L"{0}の勝ちだ！",L"{0} wins!"),{name})});
                else if(tie)Happened(9,T(L"チェスの対局が終わって、引き分け（",L"The game of chess is over: a draw (")+how+T(L"）。",L")."),
                                     T(L"引き分けの気持ち",L"how you feel about a draw"),
                                     L"引き分け",
                                     Fmt(T(L"{0}と持ち主のチェスの対局が引き分けで終わった。",L"The chess game between {0} and the owner ended in a draw."),{name}),
                                     {T(L"おつかれさま！いい勝負だったね。",L"Well played, both of you!")});
                else Happened(9,T(L"チェスの対局が終わって、あなたの負け（",L"The game of chess is over, and you lost (")+how+T(L"）。",L")."),
                              T(L"負けて悔しい気持ち",L"how sorry you are to have lost"),
                              L"負けた",
                              Fmt(T(L"{0}と持ち主のチェスの対局が終わり、持ち主が勝った。",L"The chess game between {0} and the owner is over: the owner won."),{name}),
                              {T(L"おつかれさま！いい勝負だったね。",L"Well played, both of you!"),T(L"おめでとう！",L"Congratulations!")});
            }
            seen.over=over;
        }

        void Tick()
        {
            auto& t=state.table;
            const ULONGLONG now=GetTickCount64();
            if(Flying())state.nextAt=std::max(state.nextAt,now+120);   // play waits for the men
            if(!state.said.empty()&&now-state.saidAt>SaidShown){state.said.clear();Redraw();}
            // Her turn coming: she starts to read at once; at the 3D table once she is seated,
            // time for her hand to go out to her man, and now and then to stop and think first.
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
                if(t.Now()==Phase::GameOver)state.nextAt=now+(state.renderer?EndWait3D:EndWait);
                else state.nextAt=now+150;
            }
            if(state.autoPlay&&t.Now()==Phase::YourMove&&YourMoveNow())   // (diagnostics)
            {
                t.PlayForYou(0.05);
                Moved();
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

        // A man drawn flat (the 2D board, the panels, the promotion's choice): its sign, filled
        // in its colour with an edge of the other.
        void DrawMan(Graphics& g,int kind,int colour,float cx,float cy,float size)
        {
            static constexpr const wchar_t* Signs[7]={L"",L"♟",L"♞",L"♝",L"♜",L"♛",L"♚"};
            static FontFamily symbols(L"Segoe UI Symbol");
            GraphicsPath path;
            StringFormat format;
            format.SetAlignment(StringAlignmentCenter);
            format.SetLineAlignment(StringAlignmentCenter);
            path.AddString(Signs[kind],1,symbols.IsAvailable()?&symbols:Family(),FontStyleRegular,size,RectF(cx-size,cy-size,2*size,2*size),&format);
            Pen edge(colour==Chess::White?Color(255,50,40,30):Color(255,210,204,190),std::max(1.0f,size*0.035f));
            SolidBrush fill(colour==Chess::White?Color(255,240,232,212):Color(255,28,28,34));
            g.DrawPath(&edge,&path);
            g.FillPath(&fill,&path);
        }

        void PaintEnd(Graphics& g,float cw,float ch,float s)
        {
            auto& t=state.table;
            // At the side, the board in full view (the king lying there is the point).
            const bool beside=state.renderer!=nullptr;
            const float w=std::min(cw-40*s,beside?360*s:560*s),h=std::min(ch-40*s,beside?210*s:250*s);
            const RectF box(beside?14*s:(cw-w)/2,beside?std::max(120*s,ch*0.30f):(ch-h)/2,w,h);
            FillRound(g,box,14*s,Color(245,40,22,22),Color(255,200,170,90),2*s);
            const Color gold(255,240,206,120),white(255,240,240,236),soft(255,200,190,186);
            float y=box.Y+18*s;
            const float big=EndingWords().size()>8?20*s:30*s;
            Text(g,EndingWords(),RectF(box.X,y,w,44*s),big,gold,true,StringAlignmentCenter,StringAlignmentCenter,true);
            y+=58*s;
            if(t.Winner()==Chess::Her)Runs(g,{{HerName()},{T(L"の勝ち",L" wins")}},RectF(box.X,y,w,48*s),30*s,white);
            else Runs(g,{{t.Winner()==Chess::You?T(L"あなたの勝ち！",L"You win!"):T(L"引き分け",L"A draw")}},RectF(box.X,y,w,48*s),30*s,white);
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
            g.Clear(hud?Color(0,0,0,0):Color(255,44,34,28));
            const float s=Scale(),W=static_cast<float>(cw),H=static_cast<float>(ch),mid=W/2;
            auto& t=state.table;
            const Color white(255,240,240,236),soft(255,206,196,186),gold(255,240,206,120),panel(185,40,18,16),edge(220,170,120,100);
            auto panelLine=[&](int who,float x,float y,float width){
                const int colour=t.ColourOf(who);
                const bool turn=t.Now()==(who==Chess::You?Phase::YourMove:Phase::HerTurn);
                DrawMan(g,Chess::King,colour,x+11*s,y+14*s,22*s);
                std::wstring line=colour==Chess::White?T(L"白",L"White"):T(L"黒",L"Black");
                const int ahead=(Worth(colour)-Worth(colour^1))/100;
                if(ahead>0)line+=L"  +"+std::to_wstring(ahead);
                if(turn)line+=T(L"　（番）",L"  (to move)");
                Text(g,line,RectF(x+28*s,y,width-28*s,28*s),18*s,turn?gold:soft);
            };

            // Her: name, colour and how she stands (at the top left over the 3D table; with her
            // face when there is none).
            const float nameX=hud?28*s:112*s,nameY=hud?20*s:14*s;
            if(hud)FillRound(g,RectF(14*s,12*s,260*s,92*s),10*s,panel,edge,1.2f*s);
            else
            {
                const RectF faceBox(16*s,14*s,84*s,84*s);
                FillRound(g,faceBox,10*s,Color(255,60,44,36),Color(255,170,140,110),1.5f*s);
                if(state.face)g.DrawImage(state.face.get(),faceBox.X+3*s,faceBox.Y+3*s,faceBox.Width-6*s,faceBox.Height-6*s);
                else Text(g,HerName().substr(0,1),faceBox,40*s,white,true,StringAlignmentCenter,StringAlignmentCenter);
            }
            Text(g,HerName(),RectF(nameX,nameY,230*s,28*s),20*s,white,true);
            panelLine(Chess::Her,nameX,nameY+30*s,230*s);
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

            // The game so far (at the top right).
            {
                const RectF info(W-254*s,12*s,240*s,64*s);
                FillRound(g,info,10*s,Color(hud?215:200,40,18,16),edge,1.2f*s);
                Text(g,T(L"チェス",L"Chess"),RectF(info.X+12*s,info.Y+6*s,100*s,26*s),18*s,white,true);
                Text(g,Fmt(T(L"{0}手目",L"Move {0}"),{std::to_wstring(t.Moves()/2+1)}),RectF(info.X+90*s,info.Y+8*s,138*s,24*s),15*s,soft,false,StringAlignmentFar);
                if(!t.Last().name.empty())
                    Text(g,Fmt(T(L"最後の手：{0}",L"Last move: {0}"),{t.Last().name}),RectF(info.X+12*s,info.Y+34*s,220*s,24*s),15*s,soft);
            }

            if(!hud)
            {
                // The board drawn flat, when there is no 3D table.
                const float c=Cell2D*s,x0=mid-4*c,y0=std::max(120*s,(H-8*c)/2-10*s);
                FillRound(g,RectF(x0-14*s,y0-14*s,8*c+28*s,8*c+28*s),8*s,Color(255,70,40,22),Color(255,30,18,10),2*s);
                const auto chosen=ChosenMoves();
                const int at=Pointed();
                for(int sq=0;sq<Squares;++sq)
                {
                    const float x=x0+ColumnOf(sq)*c,y=y0+RowOf(sq)*c;
                    state.cellBoxes[static_cast<size_t>(sq)]=RectF(x,y,c,c);
                    const bool isDark=(Chess::FileOf(sq)+Chess::RankOf(sq))%2==0;
                    Color fill=isDark?Color(255,124,82,50):Color(255,226,198,152);
                    if(t.Last().move.Valid()&&(sq==t.Last().move.from||sq==t.Last().move.to))fill=isDark?Color(255,120,132,50):Color(255,190,204,100);
                    if(t.InCheck()&&sq==t.Board().KingOf(t.Board().ToMove()))fill=Color(255,200,60,48);
                    SolidBrush brush(fill);
                    g.FillRectangle(&brush,x,y,c,c);
                }
                for(int sq=0;sq<Squares;++sq)
                {
                    const RectF& b=state.cellBoxes[static_cast<size_t>(sq)];
                    if(t.Board().KindAt(sq))DrawMan(g,t.Board().KindAt(sq),t.Board().ColourAt(sq),b.X+c/2,b.Y+c/2,c*0.78f);
                    if(sq==state.chosen||(sq==at&&state.hover<0&&YourMoveNow()))
                    {
                        Pen frame(gold,2.5f*s);
                        g.DrawRectangle(&frame,b.X+2*s,b.Y+2*s,c-4*s,c-4*s);
                    }
                    if(std::any_of(chosen.begin(),chosen.end(),[&](const Chess::Move& m){return m.to==sq;}))
                    {
                        const float r=sq==at?c*0.22f:c*0.11f;
                        SolidBrush mark(sq==at?gold:Color(210,240,220,140));
                        g.FillEllipse(&mark,b.X+c/2-r,b.Y+c/2-r,2*r,2*r);
                    }
                }
                for(int i=0;i<8;++i)
                {
                    const int bottom=SquareAtCell(i,7),left=SquareAtCell(0,i);
                    Text(g,std::wstring(1,static_cast<wchar_t>(L'a'+Chess::FileOf(bottom))),RectF(x0+i*c,y0+8*c-1*s,c,14*s),11*s,soft,false,StringAlignmentCenter,StringAlignmentCenter);
                    Text(g,std::wstring(1,static_cast<wchar_t>(L'1'+Chess::RankOf(left))),RectF(x0-14*s,y0+i*c,14*s,c),11*s,soft,false,StringAlignmentCenter,StringAlignmentCenter);
                }
            }

            // You: colour and how you stand.
            {
                const float boxY=H-104*s,boxW=210*s;
                FillRound(g,RectF(8*s,boxY,boxW,92*s),10*s,panel,edge,1.2f*s);
                Text(g,T(L"あなた",L"You"),RectF(22*s,boxY+8*s,boxW,26*s),18*s,white,true);
                panelLine(Chess::You,22*s,boxY+42*s,boxW-24*s);
            }

            // What to do now, early in the game; a check said aloud.
            std::wstring prompt;
            if(YourMoveNow()&&t.InCheck())prompt=T(L"チェック！ キングを守ってください",L"Check! Your king must be saved");
            else if(YourMoveNow()&&t.Moves()<4)
                prompt=state.chosen<0?T(L"動かす駒をクリックしてください（矢印キーとEnterでも）",L"Click a man to move (or the arrow keys and Enter)"):
                                      T(L"金色の印のマスをクリックすると動きます",L"Click a square marked in gold to move there");
            if(!prompt.empty()&&state.promoting<0)
            {
                const float tw=TextWidth(g,prompt,17*s)+28*s,y=H-46*s;
                FillRound(g,RectF(mid-tw/2,y,tw,34*s),10*s,panel,edge,1.2f*s);
                Text(g,prompt,RectF(mid-tw/2,y,tw,34*s),17*s,gold,false,StringAlignmentCenter,StringAlignmentCenter);
            }
            if(state.promoting>=0)
            {
                // Which it becomes.
                static constexpr int Kinds[4]={Chess::Queen,Chess::Rook,Chess::Bishop,Chess::Knight};
                const float size=86*s,gap=12*s,total=4*size+3*gap+40*s;
                const RectF box(mid-total/2,H*0.5f-size/2-44*s,total,size+88*s);
                FillRound(g,box,14*s,Color(245,40,22,22),Color(255,200,170,90),2*s);
                Text(g,T(L"何に昇格しますか？（Q R B N）",L"Promote to? (Q R B N)"),RectF(box.X,box.Y+10*s,total,30*s),19*s,white,true,StringAlignmentCenter);
                for(int i=0;i<4;++i)
                {
                    const RectF button(box.X+20*s+i*(size+gap),box.Y+50*s,size,size);
                    state.promoteButtons[i]=button;
                    FillRound(g,button,10*s,Color(255,92,70,50),Color(255,200,170,90),1.5f*s);
                    DrawMan(g,Kinds[i],t.ColourOf(Chess::You),button.X+size/2,button.Y+size/2,size*0.72f);
                }
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
                Text(g,T(L"チェスをやめますか？",L"Stop playing?"),RectF(box.X,box.Y+22*s,box.Width,36*s),24*s,white,true,StringAlignmentCenter);
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

        // The square under (x, y). At the 3D table: where you could go, when that is on the
        // board there; else the man standing over the point (the nearest of them to you, since
        // tall men hide the squares behind); else the board's square itself.
        int SquareAt(float x,float y)
        {
            if(state.renderer)
            {
                if(!state.quadsSet)return -1;
                int board=-1;
                for(int s=0;s<Squares&&board<0;++s)
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
                    if(inside&&sign!=0)board=s;
                }
                if(board>=0&&state.chosen>=0&&IsChosenTarget(board))return board;
                int best=-1;
                float nearest=1e30f;
                const bool yours=YourMoveNow();
                for(const auto& m:state.manMarks)
                {
                    // On your move, only those you could choose or take (a tall man with no
                    // move does not stand in the way of the one behind it).
                    if(yours&&!IsChosenTarget(m.square)&&
                       (state.table.Board().ColourAt(m.square)!=state.table.ColourOf(Chess::You)||state.table.MovesFrom(m.square).empty()))continue;
                    // The distance from the point to the man's upright line on the screen.
                    const float ax=static_cast<float>(m.base.x),ay=static_cast<float>(m.base.y),bx=static_cast<float>(m.top.x),by=static_cast<float>(m.top.y);
                    const float vx=bx-ax,vy=by-ay,l=vx*vx+vy*vy;
                    const float u=l>0?std::clamp(((x-ax)*vx+(y-ay)*vy)/l,0.0f,1.0f):0;
                    const float px=ax+vx*u-x,py=ay+vy*u-y;
                    // Wider at the base than at the top.
                    const float width=m.width*(1-0.35f*u);
                    if(px*px+py*py<=width*width&&m.toEye<nearest){nearest=m.toEye;best=m.square;}
                }
                return best>=0?best:board;
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
            if(state.promoting>=0)
            {
                if(key==VK_ESCAPE){state.promoting=-1;Redraw();}
                else if(key=='Q'||key==VK_RETURN)Promote(Chess::Queen);
                else if(key=='R')Promote(Chess::Rook);
                else if(key=='B')Promote(Chess::Bishop);
                else if(key=='N')Promote(Chess::Knight);
                return;
            }
            if(key==VK_ESCAPE)
            {
                if(state.chosen>=0)state.chosen=-1;
                else state.confirmQuit=true;
                Redraw();
                return;
            }
            if(!YourMoveNow())return;
            if(key==VK_LEFT||key==VK_RIGHT||key==VK_UP||key==VK_DOWN)
            {
                // Over the board's squares; to start, your king.
                if(state.cursor<0)state.cursor=state.chosen>=0?state.chosen:t.Board().KingOf(t.ColourOf(Chess::You));
                else
                {
                    const int column=std::clamp(ColumnOf(state.cursor)+(key==VK_LEFT?-1:key==VK_RIGHT?1:0),0,7);
                    const int row=std::clamp(RowOf(state.cursor)+(key==VK_UP?-1:key==VK_DOWN?1:0),0,7);
                    state.cursor=SquareAtCell(column,row);
                }
                state.hover=-1;
                Redraw();
            }
            else if((key==VK_RETURN||key==VK_SPACE)&&state.cursor>=0)Choose(state.cursor);
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
                if(at!=state.hover){state.hover=at;state.cursor=-1;Redraw();}
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
                if(state.promoting>=0)
                {
                    static constexpr int Kinds[4]={Chess::Queen,Chess::Rook,Chess::Bishop,Chess::Knight};
                    bool picked=false;
                    for(int i=0;i<4&&!picked;++i)
                        if(state.promoteButtons[i].Contains(x,y)){Promote(Kinds[i]);picked=true;}
                    if(!picked){state.promoting=-1;Redraw();}
                    return 0;
                }
                Choose(SquareAt(x,y));
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

    bool ChessWindowOpen(){return state.window!=nullptr;}
    HWND ChessWindowHandle(){return state.window;}
    bool ChessWindowRect(RECT& r)
    {
        return state.window&&IsWindowVisible(state.window)&&!IsIconic(state.window)&&GetWindowRect(state.window,&r);
    }

    namespace
    {
        // Diagnostics: moves played for both (yours as she would play them), quickly.
        void PlaySimply(int moves)
        {
            auto& t=state.table;
            for(int i=0;i<moves&&t.Now()!=Phase::GameOver;++i)
            {
                if(t.Now()==Phase::HerTurn)t.Step();
                else t.PlayForYou(0.02);
            }
            state.nextAt=0;
            state.hover=state.cursor=state.chosen=-1;
        }
    }

    void ChessLines(const std::wstring& answer)
    {
        state.prefetching=false;
        if(!state.window)return;
        state.lines.Answered(answer);
        Prefetch();
    }

    bool ChessAfterword(std::wstring& situation,std::wstring& fallback)
    {
        if(state.afterword.empty())return false;
        situation=std::move(state.afterword);
        fallback=std::move(state.afterwordLine);
        state.afterword.clear();
        state.afterwordLine.clear();
        return true;
    }

    void ChessHeard(const std::wstring& line,bool done)
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

    void PlayChessMoves(int moves,float yaw,bool goOn)
    {
        if(!state.window)return;
        PlaySimply(moves);
        state.camera.yaw+=yaw;
        state.autoPlay=goOn;
        Redraw();
    }

    bool SetChessPosition(const std::wstring& fen)
    {
        std::string narrow;
        for(wchar_t c:fen)narrow+=static_cast<char>(c);
        if(!state.window||!state.table.FromFen(narrow))return false;
        state.movesSeen=-1;
        state.plannedFor=state.reachedFor=-1;
        state.nextAt=0;
        Redraw();
        return true;
    }

    void SaveChessFrames(const std::wstring& folder,double every)
    {
        state.framesDir=folder;
        state.frameEvery=every;
    }

    void TiltChess(float pitch,float distance,float up)
    {
        state.camera.pitch=pitch;
        state.camera.distance=distance;
        state.camera.y=up;
    }

    void PointChess(int square,int chosen)
    {
        state.chosen=chosen;
        state.hover=square;
        Redraw();
    }

    namespace
    {
        void Sparkle(const std::array<float,3>& at,float scale)
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

        // A matrix as MascotRenderer::PieceDraw wants it.
        void StoreRows(DirectX::FXMMATRIX m,float (&out)[12])
        {
            DirectX::XMFLOAT4X4 k;
            DirectX::XMStoreFloat4x4(&k,m);
            for(int row=0;row<3;++row)
                for(int column=0;column<4;++column)out[row*4+column]=k.m[column][row];
        }

        // A turn by a rotation vector (x, z; y left out): how a man or its neck leans.
        DirectX::XMMATRIX Turned(float x,float z)
        {
            using namespace DirectX;
            const float angle=std::sqrt(x*x+z*z);
            if(angle<1e-5f)return XMMatrixIdentity();
            return XMMatrixRotationAxis(XMVectorSet(x/angle,0,z/angle,0),angle);
        }

        // Each man as it stands now: on its way, resting and swaying a little; the one chosen
        // lifted and glowing, the one pointed at perking up, one that could be taken flinching,
        // a king in check shaking his head. Those taken hang their heads.
        void DrawMen(MascotRenderer& r,float step,DirectX::FXMVECTOR eye)
        {
            using namespace DirectX;
            auto& t=state.table;
            const float S=ModelScale(),C=Cell();
            const int at=Pointed();
            const bool yours=YourMoveNow();
            const int checked=t.InCheck()&&!Flying()?t.Board().KingOf(t.Board().ToMove()):-1;
            const float time=static_cast<float>(state.clock);
            const bool targetOfChosen=state.chosen>=0&&at>=0&&IsChosenTarget(at);
            std::vector<MascotRenderer::PieceDraw> draws;
            state.manMarks.clear();
            state.auraMotes.clear();
            for(auto& f:state.figures)
            {
                while(f.keys.size()>=2&&state.clock>=f.keys.back().at)
                {
                    f.home=f.keys.back().pose;
                    f.home.yaw=FacingOf(f.colour);
                    f.keys.clear();
                }
                Pose p=PoseOf(f);
                if(p.scale<=0.01f)continue;
                // Eased toward what it is doing now.
                const bool busy=!f.keys.empty();
                const bool chosen=f.square>=0&&f.square==state.chosen&&!busy;
                const bool pointed=yours&&!busy&&f.square>=0&&f.square==at&&state.chosen<0&&f.colour==t.ColourOf(Chess::You)&&!t.MovesFrom(f.square).empty();
                const bool flinch=yours&&!busy&&targetOfChosen&&f.square==at&&f.colour!=t.ColourOf(Chess::You);
                const float rate=1-std::exp(-step/0.09f);
                f.perk+=((chosen?1.0f:pointed?0.45f:0.0f)-f.perk)*rate;
                f.flinch+=((flinch?1.0f:0.0f)-f.flinch)*rate;
                const float check=f.square>=0&&f.square==checked?1.0f:0.0f;
                f.alarm+=(check-f.alarm)*rate;
                f.droop+=((f.square<0&&!busy?1.0f:0.0f)-f.droop)*(1-std::exp(-step/0.5f));
                // Its aura: comes up when it is pointed at or chosen, sinks away after.
                const bool yourMan=f.square>=0&&f.colour==t.ColourOf(Chess::You);
                const bool hovered=yours&&!busy&&yourMan&&f.square==at&&!t.MovesFrom(f.square).empty();
                f.aura+=((chosen?1.0f:flinch?0.9f:hovered?0.75f:0.0f)-f.aura)*(1-std::exp(-step/0.14f));

                // Swaying, and now and then looking round.
                const float sway=0.028f+0.012f*std::sin(f.phase*3);
                float nx=p.neck[0]+sway*std::sin(time*0.9f+f.phase),nz=p.neck[1]+sway*std::sin(time*0.7f+2.1f*f.phase),twist=p.neck[2];
                const float cycle=std::fmod(time+f.look,23.0f);
                if(cycle<2.6f&&f.square>=0)
                {
                    const float glance=std::sin(cycle/2.6f*XM_PI);
                    twist+=glance*glance*(std::sin(f.phase*5)>0?1.0f:-1.0f)*0.62f;
                }
                // Chosen or pointed at: up a little, bobbing, and looking up to you.
                if(f.perk>0.002f)
                {
                    const float bob=0.5f+0.5f*std::sin(time*5.5f+f.phase);
                    p.y+=f.perk*(0.30f+0.10f*bob)*C;
                    nx-=f.perk*(0.10f+0.05f*bob);
                }
                if(f.flinch>0.002f)
                {
                    // Away from the man that comes for it, trembling.
                    float ax=0,az=0;
                    if(state.chosen>=0)
                    {
                        ax=f.home.x-SquareX(state.chosen);
                        az=f.home.z-SquareZ(state.chosen);
                        const float l=std::sqrt(ax*ax+az*az);
                        if(l>0){ax/=l;az/=l;}
                    }
                    nx+=f.flinch*az*0.34f;
                    nz-=f.flinch*ax*0.34f;
                    p.tilt[0]+=f.flinch*az*0.07f;
                    p.tilt[1]-=f.flinch*ax*0.07f;
                    nz+=f.flinch*0.025f*std::sin(time*38);
                }
                if(f.alarm>0.002f)
                {
                    twist+=f.alarm*0.7f*std::sin(time*7);
                    nx+=f.alarm*0.05f*std::sin(time*13);
                    nz+=f.alarm*0.09f*std::sin(time*9);
                }
                if(f.droop>0.002f)
                {
                    const float away=f.colour==t.ColourOf(Chess::You)?1.0f:-1.0f;
                    nx+=f.droop*away*0.46f;
                }
                const auto& shape=state.shapes[f.colour][f.kind];
                const float scale=S*p.scale;
                // The neck turned about its own joint, in the man's own axes.
                const XMVECTOR axis=XMVector3TransformNormal(XMVectorSet(nx,0,nz,0),XMMatrixRotationY(-p.yaw));
                const XMMATRIX bend=XMMatrixTranslation(0,-shape.neckY,0)*XMMatrixRotationY(twist)*Turned(XMVectorGetX(axis),XMVectorGetZ(axis))*XMMatrixTranslation(0,shape.neckY,0);
                const XMMATRIX body=XMMatrixScaling(scale,scale,scale)*XMMatrixRotationY(p.yaw)*Turned(p.tilt[0],p.tilt[1])*XMMatrixTranslation(p.x,p.y,p.z);
                MascotRenderer::PieceDraw man;
                man.indexStart=shape.indexStart;
                man.indexCount=shape.indexCount;
                man.baseVertex=shape.baseVertex;
                StoreRows(body,man.world);
                StoreRows(bend,man.neck);
                if(chosen||f.perk>0.01f){man.tint[0]=0.16f*f.perk;man.tint[1]=0.11f*f.perk;man.tint[2]=0.02f*f.perk;}
                if(f.alarm>0.01f){const float pulse=0.5f+0.5f*std::sin(time*7);man.tint[0]+=f.alarm*(0.10f+0.16f*pulse);man.tint[1]+=f.alarm*0.01f;}
                man.clear=f.colour==Chess::White;
                man.tint[3]=shape.height;
                draws.push_back(man);
                // Its shadow on what is under it, softer the higher it is.
                const float ground=GroundAt(p.x,p.z),height=std::max(0.0f,p.y-ground);
                const float radius=shape.radius*scale*(1.9f+0.6f*std::min(1.0f,height/(2*C)));
                MascotRenderer::PieceDraw blob;
                blob.indexStart=state.blobStart;
                blob.indexCount=state.blobCount;
                blob.blob=true;
                StoreRows(XMMatrixScaling(radius,1,radius)*XMMatrixTranslation(p.x-0.10f*radius,ground+0.0019f*state.upper,p.z-0.06f*radius),blob.world);
                blob.tint[3]=(f.colour==Chess::White?0.42f:0.85f)*(1-0.7f*std::min(1.0f,height/(2*C)));
                if(f.colour==Chess::White)
                {
                    // Under a crystal man, a pool of the light that comes up through it.
                    blob.tint[0]=0.62f;blob.tint[1]=0.80f;blob.tint[2]=1.0f;
                }
                draws.push_back(blob);
                if(f.aura>0.01f&&f.square>=0)
                {
                    // Gold or ice by its side; red on one about to be taken.
                    const bool taken=f.colour!=t.ColourOf(Chess::You);
                    const float colour[3]={taken?1.0f:f.colour==Chess::White?0.42f:1.0f,taken?0.22f:f.colour==Chess::White?0.74f:0.70f,taken?0.12f:f.colour==Chess::White?1.0f:0.26f};
                    const float mild=!taken&&f.colour==Chess::White?0.62f:1.0f;   // (crystal is bright already)
                    const float across=shape.radius*S,tall=shape.height*S*1.12f;
                    const float spin=time*(taken?3.2f:2.0f)+f.phase;
                    auto glow=[&](UINT start,UINT count,FXMMATRIX where,float strength){
                        MascotRenderer::PieceDraw d;
                        d.indexStart=start;
                        d.indexCount=count;
                        d.glow=true;
                        StoreRows(where,d.world);
                        d.tint[0]=colour[0];d.tint[1]=colour[1];d.tint[2]=colour[2];
                        d.tint[3]=strength;
                        draws.push_back(d);
                    };
                    glow(state.ringStart,state.ringCount,XMMatrixScaling(1.42f*across*(0.6f+0.4f*f.aura),1,1.42f*across*(0.6f+0.4f*f.aura))*XMMatrixRotationY(-spin*0.7f)*XMMatrixTranslation(f.home.x,ground+0.0024f*state.upper,f.home.z),mild*f.aura);
                    glow(state.sheathStart,state.sheathCount,XMMatrixScaling(1.12f*across,tall*f.aura,1.12f*across)*XMMatrixRotationY(spin)*XMMatrixTranslation(f.home.x,ground,f.home.z),0.85f*mild*f.aura);
                    // Motes going up, winding round it.
                    constexpr int Motes=14;
                    for(int k=0;k<Motes;++k)
                    {
                        const float u=std::fmod(time*0.42f+k*0.618f,1.0f),turn=spin*1.3f+k*2.4f+u*5,round=across*(1.05f+0.28f*u);
                        MascotRenderer::Particle m;
                        m.position={f.home.x+std::cos(turn)*round,ground+tall*1.1f*u*f.aura,f.home.z+std::sin(turn)*round};
                        m.size=(0.012f+0.008f*std::sin(k*1.7f))*state.upper*(1-0.5f*u);
                        const float fade=f.aura*std::min(1.0f,u*6)*(1-u);
                        m.color={colour[0]*0.5f+0.5f,colour[1]*0.5f+0.5f,colour[2]*0.5f+0.5f,fade};
                        m.angle=turn;
                        state.auraMotes.push_back(m);
                    }
                }
                // Where it stands on the screen, for pointing at it.
                if(f.square>=0)
                {
                    State::ManMark mark;
                    mark.square=f.square;
                    mark.base=r.Project({f.home.x,f.home.y,f.home.z});
                    mark.top=r.Project({f.home.x,f.home.y+shape.height*S,f.home.z});
                    const POINT side=r.Project({f.home.x+shape.radius*S,f.home.y,f.home.z});
                    mark.width=static_cast<float>(std::abs(side.x-mark.base.x))*1.08f;
                    const float ex=XMVectorGetX(eye)-f.home.x,ey=XMVectorGetY(eye)-f.home.y,ez=XMVectorGetZ(eye)-f.home.z;
                    mark.toEye=ex*ex+ey*ey+ez*ez;
                    state.manMarks.push_back(mark);
                }
            }
            // The clear ones far first, each seen through those before it.
            const float ex=XMVectorGetX(eye),ey=XMVectorGetY(eye),ez=XMVectorGetZ(eye);
            std::sort(draws.begin(),draws.end(),[&](const MascotRenderer::PieceDraw& a,const MascotRenderer::PieceDraw& b){
                auto apart=[&](const MascotRenderer::PieceDraw& d){
                    const float x=d.world[3]-ex,y=d.world[7]-ey,z=d.world[11]-ez;
                    return x*x+y*y+z*z;
                };
                return apart(a)>apart(b);
            });
            r.SetPieces(std::move(draws));
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

    }

    void TickChessWindow(MascotModel& model,double dt)
    {
        if(!state.window||!state.renderer)return;
        auto& r=*state.renderer;
        using namespace DirectX;
        const float D=state.upper,top=Top();
        dt=std::min(dt,0.1);
        auto& t=state.table;
        // What changed on the board since the last frame: its men set on their way (a new game's
        // come down as she sits down).
        if(t.Moves()!=state.movesSeen&&(t.Moves()>0||state.entrance==5))
        {
            const bool fresh=t.Moves()==0||state.movesSeen<0||t.Moves()<state.movesSeen;
            if(fresh)
            {
                state.dues.clear();
                state.moveUntil=state.clock;
                state.endingShown=false;
                PlaceFigures(t.Moves()==0);
            }
            else if(t.Moves()==state.movesSeen+1)StartMove();
            else
            {
                state.dues.clear();
                state.moveUntil=state.clock;
                PlaceFigures(false);
            }
            state.movesSeen=t.Moves();
        }
        if(t.Now()==Phase::GameOver&&!state.endingShown&&state.movesSeen==t.Moves())
        {
            state.endingShown=true;
            StartEnding();
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
        for(size_t i=0;i<state.dues.size();)
            if(state.dues[i].first<=state.clock)
            {
                const auto call=std::move(state.dues[i].second);
                state.dues.erase(state.dues.begin()+static_cast<std::ptrdiff_t>(i));
                call();
            }
            else ++i;
        if(auto scene=Scene();scene!=state.scene||state.scene.empty()){state.scene=std::move(scene);r.SetGeometry(BuildTable());}
        // Her hand goes out to the man she will move (tried again next frame while a gesture
        // of hers ends).
        if(t.Now()==Phase::HerTurn&&state.entrance==5&&!Flying()&&state.plannedFor==t.Moves()&&state.reachedFor!=t.Moves()&&
           GetTickCount64()+ReachLead>=state.nextAt&&t.Thought())
        {
            const Chess::Move k=t.HerNext();
            if(!k.Valid())state.reachedFor=t.Moves();
            else
            {
                auto spot=SquareSpot(k.from);
                spot[1]+=state.shapes[0][t.Board().KindAt(k.from)].height*ModelScale()*0.8f;
                if(ReachFor(model,spot))state.reachedFor=t.Moves();
            }
        }
        if(state.thinkFor>0&&state.entrance==5){model.EmoteSeated(L"ponder",state.thinkFor);state.thinkFor=0;}
        {
            const bool over=t.Now()==Phase::GameOver;
            if(over&&!state.seenOver&&t.Winner()>=0)
            {
                std::uniform_real_distribution<double> any(0,1);
                state.endGesture=t.Winner()==Chess::Her?(any(state.sparkRandom)<0.5?L"cheeks":L"clap"):L"oops";
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
            shown.insert(shown.end(),state.auraMotes.begin(),state.auraMotes.end());
            ShowRoomGlows(shown,state.roomGlows,state.clock);
            r.SetParticles(shown);
        }
        const float hip=0.12f*state.hips;
        const XMMATRIX world=XMMatrixTranslation(-model.RootOffset(),-hip,0)*XMMatrixRotationX(-lean)*XMMatrixTranslation(0,hip,0)*XMMatrixRotationY(state.herAngle)*XMMatrixTranslation(state.herX,lift,state.entrance==0?seatZ:state.herZ);
        r.SetModelTransform(world);
        SetTableSolids(model,world);
        // From your side of the table, above her, looking down at the table between you.
        const auto& c=state.camera;
        const XMVECTOR target=XMVectorSet(c.x*D,top+(0.52f+c.y+0.7f*standing)*D,(c.z-0.92f)*D,0);
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
        DrawMen(r,step,eye);
        // The board's squares as they are seen, for pointing and clicking.
        {
            const float cs=Cell()/2,y=BoardTop();
            for(int s=0;s<Squares;++s)
            {
                const float x=SquareX(s),z=SquareZ(s);
                const std::array<float,3> corners[4]={{x-cs,y,z-cs},{x+cs,y,z-cs},{x+cs,y,z+cs},{x-cs,y,z+cs}};
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
        if(!state.framesDir.empty()&&state.clock>=state.nextFrameAt&&Flying())   // (diagnostics)
        {
            state.nextFrameAt=state.clock+state.frameEvery;
            r.SaveLastFrame(state.framesDir+L"\\f"+std::to_wstring(state.frameIndex++)+L".png");
        }
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

    std::wstring DescribeChess()
    {
        if(!state.window)return L"";
        auto& t=state.table;
        int taken=0;
        for(const auto& f:state.figures)taken+=f.square<0;
        std::wstring out=Fmt(L"chess: entrance {0}, phase {1}, moves {2}, you {3}, last {4}, men {5} (taken {6}), ending {7}, depth {8}, score {9}, on the way until {10}, clock {11}, chosen {12}, hover {13}\n",
                             {std::to_wstring(state.entrance),std::to_wstring(static_cast<int>(t.Now())),std::to_wstring(t.Moves()),t.ColourOf(Chess::You)==Chess::White?L"white":L"black",
                              t.Last().name,std::to_wstring(state.figures.size()),std::to_wstring(taken),std::to_wstring(static_cast<int>(t.HowEnded())),
                              std::to_wstring(t.LastThought().depth),std::to_wstring(t.LastThought().score),std::to_wstring(state.moveUntil),std::to_wstring(state.clock),std::to_wstring(state.chosen),std::to_wstring(state.hover)});
        return out+(state.words.told.empty()?std::wstring():L"chess words:\n"+state.words.told);
    }

    bool SaveChessView(const std::wstring& path)
    {
        return state.renderer&&state.renderer->SaveLastFrame(path);
    }

    bool SaveChessShot(const std::wstring& path,int moves,std::uint32_t seed)
    {
        GdiplusStartupInput input;
        if(!state.gdiplus)GdiplusStartup(&state.gdiplus,&input,nullptr);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc=DefWindowProcW;
        wc.hInstance=GetModuleHandleW(nullptr);
        wc.lpszClassName=L"MikuMikuDesktopPerchChessShot";
        RegisterClassExW(&wc);
        const int dpi=static_cast<int>(GetDpiForSystem());
        state.window=CreateWindowExW(0,wc.lpszClassName,L"",WS_OVERLAPPEDWINDOW,0,0,MulDiv(static_cast<int>(BaseWidth)+16,dpi,96),MulDiv(static_cast<int>(BaseHeight)+39,dpi,96),
                                     nullptr,nullptr,wc.hInstance,nullptr);
        state.her=ChessOpponent{L"ミク"};
        state.table.NewGame(seed,Care,seed%2?Chess::Black:Chess::White,0.05);
        PlaySimply(moves);
        // Your move next, when it is: a man chosen, where it can go shown.
        if(state.table.Now()==Phase::YourMove)
            for(int s=0;s<Squares;++s)
                if(state.table.Board().ColourAt(s)==state.table.ColourOf(Chess::You)&&state.table.MovesFrom(s).size()>=2)
                {
                    state.chosen=s;
                    state.hover=state.table.MovesFrom(s)[0].to;
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

    void ShowChessWindow(HWND owner,const ChessOpponent& her,std::function<void()> onEnd)
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
        state.figures.clear();
        state.nextWhite=-1;
        state.quadsSet=false;
        state.lines.Load(L"CHESSLINES",L"chess",her.ownLinesFile,her.linesFile,her.styleKey,English(),
                         {L"チェス",L"chess",L"",L""});
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
        const std::wstring title=std::wstring(T(L"チェス",L"Chess"))+L" - "+HerName();
        state.window=CreateWindowExW(0,ClassName,title.c_str(),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                                     work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,
                                     nullptr,nullptr,instance,nullptr);
        if(CreateView())PlaceView();
        SetTimer(state.window,TimerId,40,nullptr);
        ShowWindow(state.window,SW_SHOWNORMAL);
        SetForegroundWindow(state.window);
    }
}
