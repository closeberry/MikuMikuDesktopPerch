#define NOMINMAX
#include "BlockTowerWindow.hpp"
#include "ShownKey.hpp"
#include "BlockTower.hpp"
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
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <random>
#pragma comment(lib,"gdiplus.lib")

namespace MikuMikuDesktopPerch
{
    namespace
    {
        using namespace Gdiplus;
        using BlockTower::Phase;
        constexpr wchar_t ClassName[]=L"MikuMikuDesktopPerchBlockTower";
        constexpr wchar_t ViewClassName[]=L"MikuMikuDesktopPerchBlockTowerView";
        constexpr UINT_PTR TimerId=1;
        // Sizes in logical pixels (at 96 dpi, at the size the window opens).
        constexpr float BaseWidth=1180,BaseHeight=840;
        // How carefully she plays (BlockTower::Game::NewGame): a little less as you grow closer,
        // and after games you lost.
        constexpr double Care=0.85,LessPerBond=0.03,LessPerLoss=0.08,LeastCare=0.4;
        // Her turn: she thinks at least this long (s) before her magic goes out.
        constexpr double ThinkLeast=1.6;
        constexpr double EndShownAfter=2.6;   // the end's panel after the tower fell (s)
        constexpr float KeyPull=3.0f;         // cm a second with W and S held
        constexpr float Shaky=0.30f;          // cm off: the tower sways enough to speak of

        enum class Act {PutBack,Slot,Again};
        struct Button {Act act;int value;std::wstring label;RectF box;bool on=true;};

        struct State
        {
            HWND window=nullptr;
            ULONG_PTR gdiplus=0;
            BlockTowerOpponent her;
            std::function<void()> onEnd;
            BlockTower::Game game;
            int lost=0;                // games you lost in a row
            std::array<int,2> wins{};  // games won this time, yours and hers
            int first=BlockTower::You; // who began the game
            bool gameOverTold=false;
            bool confirmQuit=false;
            bool help=false;
            RectF helpBox;
            RectF quitButtons[2];
            std::vector<Button> buttons;
            int hover=-1;              // the button under the mouse
            int hoverBlock=-1;         // the block under the mouse
            int hoverSlot=-1;          // the slot under the mouse (laying a block)
            std::wstring why;          // why the block under the mouse may not be taken
            // A drag drawing your block out: where it began (pixels) and where the block was wanted then.
            bool pulling=false;
            POINT pullFrom{};
            float pullStart=0;
            std::wstring said;         // shown in the big bubble a moment
            ULONGLONG saidAt=0;
            // Her turn: when her thinking began.
            double thinkingSince=-1;
            int turnSeen=-1;           // the turn whose words were spoken
            bool shakyTold=false,heightTold[2]{};
            bool pullTold=false;
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
                bool voiced=false;
                std::wstring told;
            } words;
            GameLines lines;
            GameVoice voice;
            bool prefetching=false;
            POINT headScreen{-1,-1},besideHead{-1,-1};
            std::wstring afterword,afterwordLine;
            struct Seen {bool started=false,over=false;} seen;
            std::unique_ptr<Bitmap> canvas;
            bool overlayDue=true;
            std::uint64_t shown=0;
            HWND view=nullptr;
            std::unique_ptr<MascotRenderer> renderer;
            int entrance=0;
            float herX=0,herZ=0,herAngle=0,entranceTime=0,chairZ=0;
            struct Camera {float yaw=0,pitch=0.36f,distance=2.2f,x=0,y=0,z=0;} camera;
            float yawGoal=0;
            POINT dragFrom{};
            int dragging=0;
            float upper=10,hips=10;
            float floorY=-5;
            // The camera as last set (for rays from the mouse).
            DirectX::XMFLOAT3 eye{},target{};
            double clock=0,knockAt=0;
            struct Spark {std::array<float,3> at,velocity;float age,life,size,angle,spin;std::array<float,4> color;bool glow;};
            std::vector<Spark> sparks;
            std::vector<MascotRenderer::Particle> roomGlows;
            std::mt19937 sparkRandom{12345};
            double sparkDue=0;
            std::wstring endGesture;
            double endGestureUntil=0;
            bool endReacted=false;
            double reachedAt=-10;
            Expression face=Expression::Neutral;
            bool tableBuilt=false;
            bool frozen=false;         // (diagnostics) the physics held still
            float towerTop=0;          // the tower's height the camera follows (world units, eased)
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

        void Redraw(){state.overlayDue=true;}

        const FontFamily* Family()
        {
            static FontFamily gothic(L"Yu Gothic UI");
            return gothic.IsAvailable()?&gothic:FontFamily::GenericSansSerif();
        }

        void Text(Graphics& g,const std::wstring& text,const RectF& box,float size,Color color,bool bold=false,
                  StringAlignment across=StringAlignmentNear,StringAlignment down=StringAlignmentNear)
        {
            Font font(Family(),size,bold?FontStyleBold:FontStyleRegular,UnitPixel);
            SolidBrush paint(color);
            StringFormat format;
            format.SetAlignment(across);
            format.SetLineAlignment(down);
            format.SetFormatFlags(StringFormatFlagsNoWrap);
            g.DrawString(text.c_str(),static_cast<INT>(text.size()),&font,box,&format,&paint);
        }

        float TextWidth(Graphics& g,const std::wstring& text,float size,bool bold=false)
        {
            Font font(Family(),size,bold?FontStyleBold:FontStyleRegular,UnitPixel);
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

        bool YourTurn(){return state.game.ToMove()==BlockTower::You&&state.entrance==5;}
        bool EndShown(){return state.game.Now()==Phase::Fallen&&state.game.PhaseSeconds()>=EndShownAfter;}

        LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam);

        LRESULT CALLBACK ViewProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            // The camera as at the other tables: the right button turns it, the middle one (or
            // Shift and the right) moves it, the wheel comes nearer; a right double-click or F1
            // puts it back. The left button is the window's (a block, a button).
            auto& c=state.camera;
            if(message==WM_LBUTTONDOWN||message==WM_LBUTTONDBLCLK||message==WM_LBUTTONUP)
            {
                SetFocus(state.window);
                return Proc(state.window,message==WM_LBUTTONDBLCLK?WM_LBUTTONDOWN:message,wParam,lParam);
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
                        state.yawGoal=c.yaw;
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
                c.distance=std::clamp(c.distance*std::pow(0.9f,GET_WHEEL_DELTA_WPARAM(wParam)/120.0f),0.6f,4.5f);
                return 0;
            case WM_RBUTTONDBLCLK:
                c={};
                state.yawGoal=0;
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
                Log(L"blocktower: resize: %s",error.c_str());
            Redraw();
        }

        void ReleaseView()
        {
            if(state.renderer){state.renderer->WaitIdle();state.renderer.reset();Log(L"blocktower: 3D view released");}
            state.view=nullptr;
        }

        float FloorY(){return state.floorY;}

        // ---- The table and the tower in the world ----
        // The tower's physics is in centimetres over the table's middle (BlockTower.hpp); here a
        // block is BlockTower::BlockShare of her upper body long.

        float Top(){return 0.12f*state.hips+0.30f*state.upper;}
        float FieldZ(){return -1.15f*state.upper;}
        float Cm(){return BlockTower::BlockShare*state.upper/BlockTower::Length;}
        std::array<float,3> ToWorld(const std::array<float,3>& p){const float k=Cm();return {p[0]*k,Top()+p[1]*k,FieldZ()+p[2]*k};}
        std::array<float,3> ToCm(const DirectX::XMFLOAT3& p){const float k=Cm();return {p.x/k,(p.y-Top())/k,(p.z-FieldZ())/k};}

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
                Log(L"blocktower: no 3D view (%s)",error.c_str());
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
                model.SettlePose();
                model.Update(1/30.0);
                model.ResetPhysics();
            }
            Log(L"blocktower: 3D view %ux%u on %s (hips %.1f, upper body %.1f, floor %.1f)",w,h,renderer->AdapterName().c_str(),state.hips,state.upper,state.floorY);
            {
                std::vector<MascotRenderer::GeometryVertex> room;
                BuildWesternRoom(room,state.roomGlows,{state.upper,state.hips,state.floorY,Top(),FieldZ()});
                if(!renderer->SetRoomGeometry(room))Log(L"blocktower: no room");
            }
            state.renderer=std::move(renderer);
            state.overlayDue=true;
            state.entrance=0;
            state.camera={};
            state.yawGoal=0;
            state.dragging=0;
            state.sparks.clear();
            state.endGesture.clear();
            state.tableBuilt=false;
            state.towerTop=0;
            return true;
        }

        constexpr float Wood=0.10f;

        struct Mesh
        {
            using V=std::array<float,3>;
            std::vector<MascotRenderer::GeometryVertex> out;
            int grain=0;
            float grainScale=1;
            void Quad(const V& a,const V& b,const V& c,const V& d,const V& n,const float (&col)[4])
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
            const float D=state.upper,top=Top();
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

        // The table: a dark polished top on its frame and legs.
        std::vector<MascotRenderer::GeometryVertex> BuildTable()
        {
            Mesh m;
            const float D=state.upper,floor=FloorY();
            const float frame[4]={0.20f,0.09f,0.05f,Wood},skirt[4]={0.15f,0.07f,0.04f,Wood},board[4]={0.30f,0.15f,0.08f,Wood};
            const TableShape t=Table3D();
            m.grainScale=0.35f;
            m.grain=0;
            m.Box(t.x0,t.top-0.05f*D,t.zFar,t.x1,t.top,t.zNear,board);
            m.Box(t.x0-t.rw,t.frameBottom,t.zFar-t.rw,t.x1+t.rw,t.frameTop,t.zFar,frame);
            m.Box(t.x0-t.rw,t.frameBottom,t.zNear,t.x1+t.rw,t.frameTop,t.zNear+t.rw,frame);
            m.grain=1;
            m.Box(t.x0-t.rw,t.frameBottom,t.zFar,t.x0,t.frameTop,t.zNear,frame);
            m.Box(t.x1,t.frameBottom,t.zFar,t.x1+t.rw,t.frameTop,t.zNear,frame);
            m.grain=2;
            for(float lx:{t.x0-t.rw+0.02f*D,t.x1+t.rw-0.02f*D-t.leg})
                for(float lz:{t.zFar-t.rw+0.02f*D,t.zNear+t.rw-0.02f*D-t.leg})
                    m.Box(lx,floor,lz,lx+t.leg,t.frameBottom,lz+t.leg,skirt);
            return std::move(m.out);
        }

        // ---- The blocks ----

        using V3=std::array<float,3>;

        V3 Rotate(const std::array<float,4>& q,const V3& v)
        {
            using namespace DirectX;
            const XMVECTOR r=XMVector3Rotate(XMVectorSet(v[0],v[1],v[2],0),XMVectorSet(q[0],q[1],q[2],q[3]));
            return {XMVectorGetX(r),XMVectorGetY(r),XMVectorGetZ(r)};
        }

        // A block in the world: a box with its edges bevelled, light wood with the grain along it
        // (each block its own, from its number), a tint (0: none) and how much brighter.
        void AddBlock(Mesh& m,const BlockTower::Block& k,int number,float bright=0,const float* tint=nullptr,float grow=1)
        {
            const float s=Cm();
            const float hx=k.size[0]/2*grow,hy=k.size[1]/2*grow,hz=k.size[2]/2*grow,bevel=0.10f;
            const V3 at=ToWorld(k.at);
            const float shift=static_cast<float>((number*37)%17)*0.7f,lift=static_cast<float>((number*13)%7)*0.012f;
            const float tone=0.92f+0.08f*static_cast<float>((number*29)%11)/10.0f;
            float col[4]={0.83f*tone,0.66f*tone,0.44f*tone,Wood},edge[4]={0.72f*tone,0.55f*tone,0.36f*tone,Wood};
            for(float* c:{col,edge})
            {
                for(int i=0;i<3;++i)
                {
                    if(tint)c[i]=c[i]*(1-tint[3])+tint[i]*tint[3];
                    c[i]*=1+bright;
                }
            }
            // A local point (cm, the block's own axes) to the world, and its grain coordinates.
            auto place=[&](float x,float y,float z){
                const V3 r=Rotate(k.turn,{x,y,z});
                return V3{at[0]+r[0]*s,at[1]+r[1]*s,at[2]+r[2]*s};
            };
            auto normal=[&](float x,float y,float z){return Rotate(k.turn,{x,y,z});};
            auto quad=[&](const V3 (&p)[4],const V3& n,const float (&c)[4]){
                const V3 w[4]={place(p[0][0],p[0][1],p[0][2]),place(p[1][0],p[1][1],p[1][2]),place(p[2][0],p[2][1],p[2][2]),place(p[3][0],p[3][1],p[3][2])};
                const V3 nn=normal(n[0],n[1],n[2]);
                for(int i:{0,1,2,0,2,3})
                {
                    MascotRenderer::GeometryVertex v{{w[i][0],w[i][1],w[i][2]},{nn[0],nn[1],nn[2]},{c[0],c[1],c[2],c[3]}};
                    const V3& q=p[i];
                    v.wall[0]=(q[0]+hx)*1.1f+shift;
                    v.wall[1]=0.12f+lift+(q[1]+hy)/(2*hy)*0.33f;
                    v.wall[3]=(q[2]+hz)/(2*hz)*0.9f;
                    m.out.push_back(v);
                }
            };
            const float ax=hx-bevel,ay=hy-bevel,az=hz-bevel;
            // The six faces, inset by the bevel.
            quad({{-ax,hy,-az},{-ax,hy,az},{ax,hy,az},{ax,hy,-az}},{0,1,0},col);
            quad({{-ax,-hy,-az},{ax,-hy,-az},{ax,-hy,az},{-ax,-hy,az}},{0,-1,0},col);
            quad({{hx,-ay,-az},{hx,ay,-az},{hx,ay,az},{hx,-ay,az}},{1,0,0},edge);
            quad({{-hx,-ay,-az},{-hx,-ay,az},{-hx,ay,az},{-hx,ay,-az}},{-1,0,0},edge);
            quad({{-ax,-ay,hz},{ax,-ay,hz},{ax,ay,hz},{-ax,ay,hz}},{0,0,1},col);
            quad({{-ax,-ay,-hz},{-ax,ay,-hz},{ax,ay,-hz},{ax,-ay,-hz}},{0,0,-1},col);
            // The bevels along the edges.
            const float r=0.7071f;
            for(int sy:{-1,1})
                for(int sz:{-1,1})
                {
                    const float y0=sy*ay,y1=sy*hy,z0=sz*hz,z1=sz*az;
                    V3 p[4]={{-ax,y0,z0},{ax,y0,z0},{ax,y1,z1},{-ax,y1,z1}};
                    if(sy*sz>0)std::swap(p[1],p[3]);
                    quad(p,{0,sy*r,sz*r},edge);
                }
            for(int sx:{-1,1})
                for(int sy:{-1,1})
                {
                    const float x0=sx*hx,x1=sx*ax,y0=sy*ay,y1=sy*hy;
                    V3 p[4]={{x0,y0,-az},{x0,y0,az},{x1,y1,az},{x1,y1,-az}};
                    if(sx*sy<0)std::swap(p[1],p[3]);
                    quad(p,{sx*r,sy*r,0},edge);
                }
            for(int sx:{-1,1})
                for(int sz:{-1,1})
                {
                    const float x0=sx*hx,x1=sx*ax,z0=sz*az,z1=sz*hz;
                    V3 p[4]={{x0,-ay,z0},{x0,ay,z0},{x1,ay,z1},{x1,-ay,z1}};
                    if(sx*sz>0)std::swap(p[1],p[3]);
                    quad(p,{sx*r,0,sz*r},edge);
                }
            // The corners.
            for(int sx:{-1,1})
                for(int sy:{-1,1})
                    for(int sz:{-1,1})
                    {
                        V3 a{sx*hx,sy*ay,sz*az},b{sx*ax,sy*hy,sz*az},c{sx*ax,sy*ay,sz*hz};
                        if(sx*sy*sz<0)std::swap(b,c);
                        quad({a,b,c,c},{sx*0.577f,sy*0.577f,sz*0.577f},edge);
                    }
        }

        // A glowing outline of a block's box (additive): its twelve edges as soft strips.
        void AddGlowBox(std::vector<MascotRenderer::GeometryVertex>& out,const BlockTower::Block& k,const std::array<float,3>& color,float width,float grow=1.06f)
        {
            const float s=Cm();
            const V3 at=ToWorld(k.at);
            const float hx=k.size[0]/2*grow,hy=k.size[1]/2*grow,hz=k.size[2]/2*grow;
            auto place=[&](const V3& p){const V3 r=Rotate(k.turn,p);return V3{at[0]+r[0]*s,at[1]+r[1]*s,at[2]+r[2]*s};};
            const V3 eye{state.eye.x,state.eye.y,state.eye.z};
            auto strip=[&](const V3& a,const V3& b){
                const V3 pa=place(a),pb=place(b);
                const V3 d{pb[0]-pa[0],pb[1]-pa[1],pb[2]-pa[2]},v{eye[0]-pa[0],eye[1]-pa[1],eye[2]-pa[2]};
                V3 n{d[1]*v[2]-d[2]*v[1],d[2]*v[0]-d[0]*v[2],d[0]*v[1]-d[1]*v[0]};
                const float l=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
                if(l<1e-6f)return;
                for(float& x:n)x*=width*s/l;
                const V3 q[4]={{pa[0]-n[0],pa[1]-n[1],pa[2]-n[2]},{pa[0]+n[0],pa[1]+n[1],pa[2]+n[2]},{pb[0]+n[0],pb[1]+n[1],pb[2]+n[2]},{pb[0]-n[0],pb[1]-n[1],pb[2]-n[2]}};
                const float u[4]={0,1,1,0};
                for(int i:{0,1,2,0,2,3})
                {
                    MascotRenderer::GeometryVertex g{{q[i][0],q[i][1],q[i][2]},{0,1,0},{color[0],color[1],color[2],0.25f}};
                    g.uv[0]=u[i];g.uv[1]=0;g.uv[2]=-9;
                    out.push_back(g);
                }
            };
            for(int a=0;a<3;++a)
                for(int s1:{-1,1})
                    for(int s2:{-1,1})
                    {
                        V3 p{0,0,0},q{0,0,0};
                        const float h[3]={hx,hy,hz};
                        const int b=(a+1)%3,c=(a+2)%3;
                        p[a]=-h[a];q[a]=h[a];
                        p[b]=q[b]=s1*h[b];
                        p[c]=q[c]=s2*h[c];
                        strip(p,q);
                    }
        }

        // A magic circle (additive) round a point, facing along axis: two rings and marks between.
        void AddCircle(std::vector<MascotRenderer::GeometryVertex>& out,const V3& middle,const V3& axis,float radius,const std::array<float,3>& color,float spin)
        {
            V3 u{axis[1]*0-axis[2]*1,axis[2]*0-axis[0]*0,axis[0]*1-axis[1]*0};   // axis x up
            float l=std::sqrt(u[0]*u[0]+u[1]*u[1]+u[2]*u[2]);
            if(l<1e-4f){u={1,0,0};l=1;}
            for(float& x:u)x/=l;
            const V3 v{axis[1]*u[2]-axis[2]*u[1],axis[2]*u[0]-axis[0]*u[2],axis[0]*u[1]-axis[1]*u[0]};
            auto at=[&](float a,float r){return V3{middle[0]+(u[0]*std::cos(a)+v[0]*std::sin(a))*r,middle[1]+(u[1]*std::cos(a)+v[1]*std::sin(a))*r,middle[2]+(u[2]*std::cos(a)+v[2]*std::sin(a))*r};};
            auto ring=[&](float r0,float r1,float bright,int steps,float from,float span){
                for(int i=0;i<steps;++i)
                {
                    const float a0=from+span*i/steps,a1=from+span*(i+1)/steps;
                    const V3 q[4]={at(a0,r0),at(a0,r1),at(a1,r1),at(a1,r0)};
                    const float uu[4]={0,1,1,0};
                    for(int k:{0,1,2,0,2,3})
                    {
                        MascotRenderer::GeometryVertex g{{q[k][0],q[k][1],q[k][2]},{axis[0],axis[1],axis[2]},{color[0]*bright,color[1]*bright,color[2]*bright,0.25f}};
                        g.uv[0]=uu[k];g.uv[2]=-9;
                        out.push_back(g);
                    }
                }
            };
            constexpr float Full=DirectX::XM_2PI;
            ring(radius*0.90f,radius*1.04f,1.0f,40,spin,Full);
            ring(radius*0.62f,radius*0.70f,0.8f,32,-spin*1.6f,Full);
            for(int i=0;i<8;++i)ring(radius*0.72f,radius*0.88f,0.7f,2,spin*0.5f+Full*i/8,Full/40);
        }

        // ---- Her words and the watchers' ----

        constexpr ULONGLONG WordsHold=3500;
        constexpr ULONGLONG AnswerWait=9000;

        void SayLine(const GameLine& line,int weight)
        {
            auto& w=state.words;
            const ULONGLONG now=GetTickCount64();
            state.voice.Stop();
            std::wstring error;
            w.voiced=!line.voice.empty()&&state.voice.Play(line,error);
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

        std::wstring Standing()
        {
            return Fmt(T(L"（{0}段、{1}手目。持ち主{2}勝、{3}{4}勝）",L" ({0} levels, turn {1}; the owner has won {2}, {3} {4})"),
                       {std::to_wstring(state.game.Height()),std::to_wstring(state.game.Turns()+1),std::to_wstring(state.wins[BlockTower::You]),HerName(),std::to_wstring(state.wins[BlockTower::Her])});
        }

        // Something happened: she speaks of it (scene: the scene of her lines; null: she says
        // nothing), and those watching hear of it (seen, theirs).
        void Happened(int weight,const wchar_t* scene,const std::wstring& seen,const std::vector<std::wstring>& theirs,double chance=1)
        {
            state.words.told+=Fmt(L"  {0} s, weight {1}: {2}\n",{std::to_wstring(static_cast<int>(state.clock)),std::to_wstring(weight),seen});
            if(state.her.onEvent&&!theirs.empty())state.her.onEvent(seen+Standing(),weight,Pick(theirs));
            if(!scene)return;
            auto& w=state.words;
            const ULONGLONG now=GetTickCount64();
            if((w.waiting||now-w.since<WordsHold)&&weight<w.weight)return;
            if(!state.lines.Roll(scene,chance,state.sparkRandom))return;
            GameLine line;
            if(state.lines.Pick(scene,w.recent,state.sparkRandom,line))SayLine(line,weight);
        }

        void Prefetch()
        {
            if(state.prefetching||!state.her.prefetch)return;
            const std::wstring ask=state.lines.NextAsk();
            if(!ask.empty())state.prefetching=state.her.prefetch(ask);
        }

        // A line of the scene, now and then (chance), unless she has just spoken.
        void Say(const wchar_t* name,int weight,double chance)
        {
            const ULONGLONG now=GetTickCount64();
            if(state.words.waiting||(now-state.words.since<WordsHold&&weight<=state.words.weight))return;
            if(!state.lines.Roll(name,chance,state.sparkRandom))return;
            GameLine line;
            if(state.lines.Pick(name,state.words.recent,state.sparkRandom,line))SayLine(line,weight);
        }

        void Finish()
        {
            if(!state.window)return;
            KillTimer(state.window,TimerId);
            state.voice.Stop();
            if(state.her.model)state.her.model->SetSolids({});
            if(state.her.model&&state.face!=Expression::Neutral)state.her.model->SetExpression(Expression::Neutral,false);
            state.face=Expression::Neutral;
            if(state.afterword.empty()&&state.seen.started&&state.game.Now()!=Phase::Fallen)
            {
                state.afterword=Fmt(T(L"いま、話し相手とブロックタワー（積んだ木のブロックを魔法で1本ずつ抜いて上に積み、崩した人が負け）をしていたが、途中でやめてデスクトップに戻ってきたところ（塔は{0}段だった）。そのことを話し相手にひと言（1〜2文）",
                                      L"You were playing the block tower (taking wooden blocks out of a tower by magic one by one and laying them on top; whoever brings it down loses) with them, but they stopped midway and you have come back to the desktop (the tower stood {0} levels high). Say a word about it to them (one or two sentences)"),
                                    {std::to_wstring(state.game.Height())});
                state.afterwordLine=T(L"また今度、続きやろうね。",L"Let's build it again another time.");
            }
            auto onEnd=std::move(state.onEnd);
            DestroyWindow(state.window);
            if(onEnd)onEnd();
        }

        void NewGame()
        {
            state.gameOverTold=false;
            std::random_device device;
            state.game.NewGame(device(),state.first,std::max(LeastCare,Care-LessPerBond*state.her.bond-LessPerLoss*state.lost));
            state.hover=-1;
            state.hoverBlock=state.hoverSlot=-1;
            state.pulling=false;
            state.confirmQuit=false;
            state.help=false;
            state.said.clear();
            state.thinkingSince=-1;
            state.turnSeen=-1;
            state.shakyTold=state.pullTold=false;
            state.heightTold[0]=state.heightTold[1]=false;
            state.seen={};
            state.words.waiting=false;
            state.words.text.clear();
            state.afterword.clear();
            state.afterwordLine.clear();
            state.endReacted=false;
            state.endGesture.clear();
            Prefetch();
        }

        // ---- Rays from the mouse ----

        // The ray under a point of the window, in the tower's centimetres.
        bool RayAt(int x,int y,std::array<float,3>& from,std::array<float,3>& to)
        {
            using namespace DirectX;
            if(!state.renderer||!state.view)return false;
            RECT client{};
            GetClientRect(state.view,&client);
            if(client.right<1||client.bottom<1)return false;
            const XMMATRIX view=XMMatrixLookAtLH(XMLoadFloat3(&state.eye),XMLoadFloat3(&state.target),XMVectorSet(0,1,0,0));
            const XMMATRIX projection=XMMatrixPerspectiveFovLH(XMConvertToRadians(42.0f),static_cast<float>(client.right)/client.bottom,0.1f,1000.0f);
            const XMMATRIX back=XMMatrixInverse(nullptr,view*projection);
            const float nx=2.0f*x/client.right-1,ny=1-2.0f*y/client.bottom;
            XMFLOAT3 a,b;
            XMStoreFloat3(&a,XMVector3TransformCoord(XMVectorSet(nx,ny,0,1),back));
            XMStoreFloat3(&b,XMVector3TransformCoord(XMVectorSet(nx,ny,1,1),back));
            from=ToCm(a);
            to=ToCm(b);
            return true;
        }

        // The slot (of those free) whose place the ray passes through, nearest first; -1: none.
        int SlotAt(const std::array<float,3>& from,const std::array<float,3>& to)
        {
            int best=-1;
            float nearest=2;
            for(int s:state.game.FreeSlots())
            {
                const BlockTower::Block k=state.game.SlotBlock(s);
                // Its box lies along x or z (no turn but the level's): a box hit test, a little larger.
                const bool alongZ=k.level%2==0;
                const float h[3]={(alongZ?k.size[2]:k.size[0])/2+0.3f,k.size[1]/2+0.4f,(alongZ?k.size[0]:k.size[2])/2+0.3f};
                float t0=0,t1=1;
                bool hit=true;
                for(int a=0;a<3&&hit;++a)
                {
                    const float d=to[a]-from[a],lo=k.at[a]-h[a]-from[a],hi=k.at[a]+h[a]-from[a];
                    if(std::abs(d)<1e-6f){if(lo>0||hi<0)hit=false;continue;}
                    float u=lo/d,v=hi/d;
                    if(u>v)std::swap(u,v);
                    t0=std::max(t0,u);t1=std::min(t1,v);
                    if(t0>t1)hit=false;
                }
                if(hit&&t0<nearest){nearest=t0;best=s;}
            }
            return best;
        }

        POINT Screen(const std::array<float,3>& cm)
        {
            return state.renderer?state.renderer->Project(ToWorld(cm)):POINT{0,0};
        }

        // How the mouse moving by (dx, dy) pixels moves the block chosen along its length (cm).
        float AlongFor(float dx,float dy)
        {
            auto& g=state.game;
            const int b=g.Active();
            if(b<0)return 0;
            const auto axis=g.Axis(b);
            const auto& k=g.BlockAt(b);
            const POINT p0=Screen(k.at);
            const POINT p1=Screen({k.at[0]+axis[0],k.at[1]+axis[1],k.at[2]+axis[2]});
            const POINT pu=Screen({k.at[0],k.at[1]+1,k.at[2]});
            const float ax=static_cast<float>(p1.x-p0.x),ay=static_cast<float>(p1.y-p0.y);
            const float a2=ax*ax+ay*ay,unit=std::max(1.0f,std::hypot(static_cast<float>(pu.x-p0.x),static_cast<float>(pu.y-p0.y)));
            if(a2>=0.16f*unit*unit)return (dx*ax+dy*ay)/a2;
            // Its length points at the camera (or away): down the window brings it nearer.
            const float toEye=(state.eye.x-ToWorld(k.at)[0])*axis[0]+(state.eye.y-ToWorld(k.at)[1])*axis[1]+(state.eye.z-ToWorld(k.at)[2])*axis[2];
            return dy/unit*(toEye>0?1.0f:-1.0f);
        }

        // ---- What you do ----

        // The free slots as they lie on the window from left to right.
        std::vector<int> SlotsLeftToRight()
        {
            std::vector<std::pair<LONG,int>> xs;
            for(int s:state.game.FreeSlots())xs.push_back({Screen(state.game.SlotBlock(s).at).x,s});
            std::sort(xs.begin(),xs.end());
            std::vector<int> out;
            for(const auto& [x,s]:xs)out.push_back(s);
            return out;
        }

        void Do(Act act,int value=0)
        {
            auto& g=state.game;
            if(act==Act::Again){if(EndShown())NewGame();Redraw();return;}
            if(!YourTurn())return;
            if(act==Act::PutBack){g.PutBack();state.pulling=false;}
            else if(act==Act::Slot&&g.Hovering())g.Place(value);
            Redraw();
        }

        void ClickAt(int x,int y)
        {
            auto& g=state.game;
            if(!YourTurn())return;
            std::array<float,3> from{},to{};
            if(!RayAt(x,y,from,to))return;
            if(g.Now()==Phase::Place)
            {
                if(g.Hovering())
                    if(const int s=SlotAt(from,to);s>=0)g.Place(s);
                return;
            }
            if(g.Now()!=Phase::Choose&&g.Now()!=Phase::Pull)return;
            const int b=g.Pick(from,to);
            if(b<0)return;
            if(b==g.Active()&&g.Now()==Phase::Pull)
            {
                // The block being drawn out: on with it.
                state.pulling=true;
                state.pullFrom={x,y};
                state.pullStart=g.Wanted();
                SetCapture(state.window);
                return;
            }
            if(!g.Legal(b))
            {
                state.said=g.WhyNot(b);
                if(state.said.empty())state.said=T(L"いまはそのブロックを選べません",L"Not that one now");
                state.saidAt=GetTickCount64();
                Redraw();
                return;
            }
            if(g.Choose(b))
            {
                state.pulling=g.Active()==b;
                state.pullFrom={x,y};
                state.pullStart=0;
                if(state.pulling)SetCapture(state.window);
                Redraw();
            }
        }

        // ---- The game going on ----

        void UpdateFace()
        {
            auto& g=state.game;
            Expression e=Expression::Neutral;
            if(g.Now()==Phase::Fallen)e=g.PhaseSeconds()<0.3?Expression::Surprise:g.Loser()==BlockTower::Her?Expression::Sad:Expression::Smile;
            else if(g.Active()>=0&&g.Wobble()>Shaky)e=g.ToMove()==BlockTower::Her?Expression::Troubled:Expression::Surprise;
            else if(g.Active()>=0&&g.ToMove()==BlockTower::Her&&g.Resistance()>0.75f)e=Expression::Troubled;
            if(e==state.face)return;
            state.face=e;
            if(state.her.model&&state.renderer)state.her.model->SetExpression(e,false);
        }

        void NoticeWords()
        {
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
        }

        void Tick()
        {
            auto& g=state.game;
            const ULONGLONG now=GetTickCount64();
            const std::wstring name=HerName();
            if(!state.said.empty()&&now-state.saidAt>2400){state.said.clear();Redraw();}
            NoticeWords();
            UpdateFace();
            if(state.entrance<5)return;
            if(!state.seen.started)
            {
                state.seen.started=true;
                Happened(6,L"始まり",Fmt(T(L"{0}とこのパソコンの持ち主のブロックタワーが始まる。",L"{0} and the owner of this computer are starting a game of the block tower."),{name}),
                         {T(L"がんばれー！",L"Good luck!"),T(L"崩さないでね！",L"Don't knock it over!")});
            }
            // A turn over: a word on how it went.
            if(g.Turns()!=state.turnSeen)
            {
                if(state.turnSeen>=0&&g.Now()!=Phase::Fallen)
                {
                    const bool hers=g.ToMove()==BlockTower::You;   // (she moved last)
                    const bool scary=g.TurnWobble()>Shaky||g.TurnResistance()>0.7f;
                    if(hers)Say(L"抜けた",scary?2:1,scary?0.7:0.12);
                    else if(scary)Say(L"相手が抜いた",2,0.6);
                    if(g.Height()>=24&&!state.heightTold[1]){state.heightTold[1]=state.heightTold[0]=true;Say(L"高くなった",3,0.8);}
                    else if(g.Height()>=21&&!state.heightTold[0]){state.heightTold[0]=true;Say(L"高くなった",3,0.8);}
                }
                state.turnSeen=g.Turns();
                state.shakyTold=state.pullTold=false;
                state.thinkingSince=-1;
            }
            // Her turn: she thinks (tries blocks in her head), then her magic goes out to the one chosen.
            if(g.ToMove()==BlockTower::Her&&g.Now()==Phase::Choose&&!g.HersRunning())
            {
                if(state.thinkingSince<0)
                {
                    g.Think();
                    state.thinkingSince=state.clock;
                    Say(L"考える",1,0.3);
                    if(state.her.model)state.her.model->EmoteSeated(L"ponder",ThinkLeast+0.6);
                }
                else if(g.Thought()&&state.clock-state.thinkingSince>=ThinkLeast)
                {
                    g.BeginHers();
                    Say(L"抜く",2,0.25);
                    state.reachedAt=-10;
                }
            }
            if(g.Now()==Phase::Pull&&g.ToMove()==BlockTower::You&&!state.pullTold){state.pullTold=true;Say(L"見守る",1,0.12);}
            if((g.Now()==Phase::Pull||g.Now()==Phase::Place)&&g.Wobble()>Shaky&&!state.shakyTold)
            {
                state.shakyTold=true;
                if(g.ToMove()==BlockTower::Her)Say(L"危ない",3,0.8);
                else Say(L"相手が危ない",3,0.7);
            }
            if(g.Now()==Phase::Fallen&&!state.seen.over)
            {
                state.seen.over=true;
                const int loser=g.Loser();
                ++state.wins[static_cast<size_t>(1-loser)];
                // How she goes easier next game comes from how this one ended.
                state.lost=loser==BlockTower::You?state.lost+1:0;
                state.first=1-state.first;
                const std::wstring height=std::to_wstring(g.Height()),turns=std::to_wstring(g.Turns()+1);
                if(loser==BlockTower::Her)
                {
                    Happened(9,L"崩した",Fmt(T(L"{0}が{1}手目に塔を崩して、持ち主の勝ち（{2}段）。",L"{0} brought the tower down on turn {1}: the owner wins ({2} levels)."),{name,turns,height}),
                             {T(L"あーあ、崩れちゃった！",L"Oh no, down it went!"),T(L"持ち主の勝ちだ！",L"The owner wins!")});
                    state.afterword=Fmt(T(L"いま、話し相手とブロックタワー（積んだ木のブロックを魔法で1本ずつ抜いて上に積み、崩した人が負け）をして、あなたが{0}段の塔を崩して負け、デスクトップに戻ってきたところ。その勝負の感想を、話し相手にひと言（1〜2文）",
                                          L"You have just played the block tower (taking wooden blocks out of a tower by magic and laying them on top; whoever brings it down loses) with them: you brought the {0}-level tower down and lost, and have come back to the desktop. Tell them what you thought of the game (one or two sentences)"),{height});
                    state.afterwordLine=T(L"崩しちゃった……次は負けないからね。",L"I knocked it over... I'll win next time.");
                }
                else
                {
                    Happened(9,L"相手が崩した",Fmt(T(L"持ち主が{1}手目に塔を崩して、{0}の勝ち（{2}段）。",L"The owner brought the tower down on turn {1}: {0} wins ({2} levels)."),{name,turns,height}),
                             {T(L"崩れたー！",L"It fell!"),Fmt(T(L"{0}の勝ちだ！",L"{0} wins!"),{name})});
                    state.afterword=Fmt(T(L"いま、話し相手とブロックタワー（積んだ木のブロックを魔法で1本ずつ抜いて上に積み、崩した人が負け）をして、話し相手が{0}段の塔を崩してあなたの勝ち、デスクトップに戻ってきたところ。その勝負の感想を、話し相手にひと言（1〜2文）",
                                          L"You have just played the block tower (taking wooden blocks out of a tower by magic and laying them on top; whoever brings it down loses) with them: they brought the {0}-level tower down and you won, and have come back to the desktop. Tell them what you thought of the game (one or two sentences)"),{height});
                    state.afterwordLine=T(L"ブロックタワー、ドキドキしたね！またやろう。",L"That was a thrill! Let's play again.");
                }
            }
            if(EndShown()&&!state.gameOverTold)
            {
                state.gameOverTold=true;
                if(state.her.onGameOver)state.her.onGameOver();
                Redraw();
            }
        }

        // ---- Drawing the 2D layer ----

        void PaintEnd(Graphics& g,float cw,float ch,float s)
        {
            auto& t=state.game;
            const float w=std::min(cw-40*s,560*s),h=std::min(ch-40*s,280*s);
            const RectF box((cw-w)/2,std::max(60*s,ch*0.16f),w,h);
            FillRound(g,box,14*s,Color(240,40,22,22),Color(255,200,170,90),2*s);
            float y=box.Y+22*s;
            const Color gold(255,240,206,120),white(255,240,240,236),soft(255,200,190,186);
            Text(g,T(L"塔が崩れた！",L"Down it came!"),RectF(box.X,y,w,44*s),32*s,gold,true,StringAlignmentCenter);
            y+=62*s;
            const bool youWin=t.Loser()==BlockTower::Her;
            Text(g,youWin?std::wstring(T(L"あなたの勝ち！",L"You win!")):Fmt(T(L"{0}の勝ち",L"{0} wins"),{HerName()}),RectF(box.X,y,w,40*s),28*s,youWin?gold:white,true,StringAlignmentCenter);
            y+=54*s;
            Text(g,Fmt(T(L"{0}段まで、{1}手目で崩れました",L"{0} levels high, down on turn {1}"),{std::to_wstring(t.Height()),std::to_wstring(t.Turns()+1)}),RectF(box.X,y,w,30*s),18*s,soft,false,StringAlignmentCenter);
            y+=32*s;
            Text(g,Fmt(T(L"あなた {0}勝　{1} {2}勝",L"You {0}   {1} {2}"),{std::to_wstring(state.wins[BlockTower::You]),HerName(),std::to_wstring(state.wins[BlockTower::Her])}),RectF(box.X,y,w,30*s),18*s,white,false,StringAlignmentCenter);
            Text(g,T(L"Enter：もう一度　Esc：閉じる",L"Enter: play again   Esc: close"),RectF(box.X,box.GetBottom()-40*s,w,30*s),16*s,soft,false,StringAlignmentCenter);
        }

        void PaintHelp(Graphics& g,float W,float H,float s)
        {
            SolidBrush shade(Color(150,0,0,0));
            g.FillRectangle(&shade,0.0f,0.0f,W,H);
            const float w=std::min(W-40*s,780*s),h=std::min(H-40*s,520*s);
            const RectF box((W-w)/2,(H-h)/2,w,h);
            FillRound(g,box,14*s,Color(250,40,22,22),Color(255,200,170,90),2*s);
            const Color gold(255,240,206,120),white(255,240,240,236),soft(255,200,190,186);
            Text(g,T(L"ブロックタワーの遊び方",L"How to play the block tower"),RectF(box.X,box.Y+14*s,w,36*s),24*s,gold,true,StringAlignmentCenter);
            struct Row {std::wstring name,about;};
            const Row rows[]={
                {T(L"進め方",L"Your turn"),T(L"ブロックを1本抜いて、一番上に積む。塔を崩した人の負け",L"take one block out and lay it on the top; whoever brings the tower down loses")},
                {T(L"選ぶ",L"Choosing"),T(L"ブロックをクリック。一番上の段と、そろっていない一番上の段の下の段からは抜けない",L"click a block; not from the top level, nor from the one under an unfinished top")},
                {T(L"抜く",L"Drawing out"),T(L"ドラッグで長さの向きに動かす（W・S でも）。固いブロックは上の段を引きずる",L"drag it along its length (or W and S); a tight one drags the level above")},
                {T(L"手ごたえ",L"Resistance"),T(L"上の重みがかかったブロックは固く、まわりを引きずる。固ければ戻して（B）別のブロックへ",L"a block carrying weight is tight and drags the others: put it back (B), try another")},
                {T(L"戻す",L"Putting back"),T(L"3分の1より出ていなければ戻せる（ほかのブロックをクリックしても）",L"while less than a third is out (clicking another block does too)")},
                {T(L"積む",L"Laying"),T(L"抜けたら上に浮かぶので、置く場所をクリック（1〜3キー：画面の左から）",L"once out it floats over the top: click where it goes (1-3: from the left)")},
                {T(L"ゆれ",L"Sway"),T(L"塔がどれだけ動いたか。ブロックが落ちたり倒れたりしたら崩れたことになる",L"how far the tower has moved; a block dropping or tipping over brings it down")},
                {T(L"カメラ",L"Camera"),T(L"右ドラッグで回す、中ドラッグで動かす、ホイールで寄る。← → でも回る",L"right drag turns, middle drag moves, the wheel comes near; left and right arrows turn too")}};
            float y=box.Y+64*s;
            for(const Row& r:rows)
            {
                Text(g,r.name,RectF(box.X+24*s,y,160*s,30*s),17*s,white,true);
                Text(g,r.about,RectF(box.X+170*s,y+1*s,w-184*s,30*s),15*s,soft);
                y+=48*s;
            }
            Text(g,T(L"Tab：閉じる",L"Tab: close"),RectF(box.X,box.GetBottom()-34*s,w,26*s),15*s,soft,false,StringAlignmentCenter);
        }

        // A bar: name, how full (0-1), coloured from green through yellow to red.
        void Gauge(Graphics& g,float x,float y,float w,float s,const std::wstring& name,float full)
        {
            full=std::clamp(full,0.0f,1.0f);
            Text(g,name,RectF(x,y,90*s,24*s),15*s,Color(255,236,226,210),true,StringAlignmentNear,StringAlignmentCenter);
            const RectF bar(x+92*s,y+5*s,w-92*s,14*s);
            FillRound(g,bar,7*s,Color(200,20,12,10),Color(220,170,120,100),1*s);
            if(full>0.01f)
            {
                const int r=static_cast<int>(std::min(1.0f,full*2)*230+20),gr=static_cast<int>(std::min(1.0f,(1-full)*2)*200+30);
                FillRound(g,RectF(bar.X+2*s,bar.Y+2*s,(bar.Width-4*s)*full,bar.Height-4*s),5*s,Color(240,static_cast<BYTE>(r),static_cast<BYTE>(gr),60));
            }
        }

        void LayButtons(Graphics& g,float W,float H,float s)
        {
            auto& t=state.game;
            std::vector<Button> b;
            if(YourTurn()&&t.Now()==Phase::Pull&&t.CanPutBack())b.push_back({Act::PutBack,0,T(L"戻す（B）",L"Put back (B)"),RectF(),true});
            if(YourTurn()&&t.Hovering())
            {
                const auto order=SlotsLeftToRight();
                const wchar_t* names[3]={T(L"左",L"Left"),T(L"真ん中",L"Middle"),T(L"右",L"Right")};
                for(size_t i=0;i<order.size();++i)
                {
                    const wchar_t* label=order.size()==3?names[i]:order.size()==2?names[i*2]:T(L"ここ",L"Here");
                    b.push_back({Act::Slot,order[i],Fmt(L"{0}（{1}）",{label,std::to_wstring(i+1)}),RectF(),true});
                }
            }
            float total=0;
            std::vector<float> widths;
            for(const auto& k:b)
            {
                const float bw=std::max(120*s,TextWidth(g,k.label,16*s,true)+26*s);
                widths.push_back(bw);
                total+=bw+10*s;
            }
            float x=W/2-(total-10*s)/2;
            const float y=H-74*s;
            for(size_t i=0;i<b.size();++i)
            {
                Button& k=b[i];
                k.box=RectF(x,y,widths[i],46*s);
                x+=widths[i]+10*s;
                const bool lit=k.on&&(static_cast<int>(i)==state.hover||(k.act==Act::Slot&&k.value==state.hoverSlot));
                const Color fill=lit?Color(250,250,236,200):Color(240,236,226,196);
                FillRound(g,k.box,10*s,fill,Color(255,90,60,30),1.5f*s);
                Text(g,k.label,k.box,16*s,Color(255,40,30,20),true,StringAlignmentCenter,StringAlignmentCenter);
            }
            state.buttons=std::move(b);
        }

        void Paint()
        {
            RECT client{};
            GetClientRect(state.window,&client);
            const int cw=std::max(1L,client.right),ch=std::max(1L,client.bottom);
            if(!state.canvas||static_cast<int>(state.canvas->GetWidth())!=cw||static_cast<int>(state.canvas->GetHeight())!=ch)
                state.canvas=std::make_unique<Bitmap>(cw,ch,PixelFormat32bppPARGB);
            Bitmap& canvas=*state.canvas;
            Graphics g(&canvas);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAlias);
            g.Clear(Color(0,0,0,0));
            const float s=Scale(),W=static_cast<float>(cw),H=static_cast<float>(ch),mid=W/2;
            auto& t=state.game;
            const Color white(255,240,240,236),soft(255,206,196,186),gold(255,240,206,120),panel(185,40,18,16),edge(220,170,120,100);
            // Her and you: the games won.
            FillRound(g,RectF(14*s,12*s,260*s,66*s),10*s,panel,edge,1.2f*s);
            Text(g,HerName(),RectF(28*s,18*s,240*s,28*s),20*s,white,true);
            Text(g,Fmt(T(L"{0} {1}勝　あなた {2}勝",L"{0} {1}   you {2}"),{HerName(),std::to_wstring(state.wins[BlockTower::Her]),std::to_wstring(state.wins[BlockTower::You])}),RectF(28*s,46*s,240*s,24*s),15*s,gold);
            // Her words: a bubble just right of her head.
            if(!state.words.text.empty())
            {
                const std::wstring& said=state.words.text;
                const float most=std::max(160*s,W*0.4f);
                const WordsBubble bubble(g,said,s,most);
                const float bw=bubble.Width(),bh=bubble.Height();
                const bool beside=state.besideHead.x>=0;
                const float x=beside?std::clamp(static_cast<float>(state.besideHead.x),14*s,W-14*s-bw):W-14*s-bw;
                const float y=beside?std::clamp(static_cast<float>(state.headScreen.y)-bh*0.6f,96*s,std::max(96*s,H*0.6f-bh)):96*s;
                const float ty=beside?std::clamp(static_cast<float>(state.headScreen.y),y+12*s,y+bh-12*s):y+std::min(bh*0.5f,30*s);
                const std::wstring shown=said.substr(0,std::min(said.size(),std::max<size_t>(1,static_cast<size_t>(state.words.shown))));
                bubble.Draw(g,x,y,ty,shown);
            }
            // The tower and the turn (at the top right), and how to play.
            {
                const RectF info(W-254*s,12*s,240*s,64*s);
                FillRound(g,info,10*s,Color(215,40,18,16),edge,1.2f*s);
                Text(g,Fmt(T(L"ブロックタワー  {0}段",L"Block tower  {0} levels"),{std::to_wstring(t.Height())}),RectF(info.X+12*s,info.Y+6*s,228*s,26*s),18*s,white,true);
                Text(g,Fmt(T(L"{0}手目",L"Turn {0}"),{std::to_wstring(t.Turns()+1)}),RectF(info.X+12*s,info.Y+34*s,224*s,24*s),15*s,soft);
                state.helpBox=RectF(W-254*s,82*s,240*s,30*s);
                FillRound(g,state.helpBox,8*s,Color(215,40,18,16),edge,1.2f*s);
                Text(g,T(L"遊び方（Tab）",L"How to play (Tab)"),state.helpBox,14*s,soft,false,StringAlignmentCenter,StringAlignmentCenter);
            }
            // Whose turn: under her name (at the top middle it hid her face).
            if(t.Now()!=Phase::Fallen&&state.entrance==5)
            {
                const std::wstring whose=YourTurn()?std::wstring(T(L"あなたの番",L"Your turn")):Fmt(T(L"{0}の番",L"{0}'s turn"),{HerName()});
                const RectF box(14*s,84*s,260*s,38*s);
                FillRound(g,box,12*s,YourTurn()?Color(225,40,90,120):Color(225,120,40,90),edge,1.2f*s);
                Text(g,whose,box,20*s,white,true,StringAlignmentCenter,StringAlignmentCenter);
            }
            // The pull: how hard the magic pulls, how far the tower has moved.
            const bool gauges=t.Active()>=0&&(t.Now()==Phase::Pull||t.Now()==Phase::Place);
            if(gauges)
            {
                const float gw=300*s,gx=mid-gw/2,gy=H-142*s;
                FillRound(g,RectF(gx-14*s,gy-8*s,gw+28*s,62*s),10*s,panel,edge,1.2f*s);
                Gauge(g,gx,gy,gw,s,T(L"手ごたえ",L"Resistance"),t.Now()==Phase::Pull?t.Resistance():0);
                Gauge(g,gx,gy+24*s,gw,s,T(L"ゆれ",L"Sway"),t.Wobble()/0.6f);
            }
            // What to do now.
            {
                std::wstring prompt;
                if(state.entrance<5||t.Now()==Phase::Fallen)prompt.clear();
                else if(!YourTurn())prompt=t.Now()==Phase::Settle?T(L"塔が落ち着くのを待っています…",L"Waiting for the tower to settle..."):
                                         t.Active()<0?Fmt(T(L"{0}が考えています…",L"{0} is thinking..."),{HerName()}):
                                         t.Now()==Phase::Pull?Fmt(T(L"{0}が魔法でブロックを抜いています",L"{0} is drawing a block out by magic"),{HerName()}):
                                         Fmt(T(L"{0}がブロックを積んでいます",L"{0} is laying the block on top"),{HerName()});
                else if(t.Now()==Phase::Choose)prompt=T(L"抜くブロックをクリックして、ドラッグで引き出します（Tabで遊び方）",L"Click a block and drag it out (Tab: how to play)");
                else if(t.Now()==Phase::Pull&&t.Resistance()>0.85f)prompt=t.CanPutBack()?T(L"固いブロック：上の重みがかかっていて、まわりを引きずります。B で戻せます",L"It's tight: the weight above is on it, and it drags the others. B puts it back"):
                                                    T(L"固いブロック：上の重みがかかっていて、まわりを引きずります",L"It's tight: the weight above is on it, and it drags the others");
                else if(t.Now()==Phase::Pull)prompt=t.CanPutBack()?T(L"ドラッグ（W・S）で引き出します。固ければ B で戻せます",L"Drag (W, S) to draw it out; B puts it back if it is tight"):
                                                    T(L"そのまま引き出します（もう戻せません）",L"Draw it on out (it can't go back now)");
                else if(t.Now()==Phase::Place)prompt=t.Hovering()?T(L"置く場所をクリックします（1〜3）",L"Click where it goes on the top (1-3)"):T(L"抜けました！",L"Out!");
                else if(t.Now()==Phase::Settle)prompt=T(L"塔が落ち着くのを待っています…",L"Waiting for the tower to settle...");
                if(!state.why.empty()&&YourTurn()&&t.Now()==Phase::Choose)prompt=state.why;
                if(!prompt.empty())
                {
                    const float tw=TextWidth(g,prompt,16*s)+24*s,y=H-(gauges?184:118)*s;
                    FillRound(g,RectF(mid-tw/2,y,tw,30*s),10*s,panel,edge,1.2f*s);
                    Text(g,prompt,RectF(mid-tw/2,y,tw,30*s),16*s,gold,false,StringAlignmentCenter,StringAlignmentCenter);
                }
            }
            LayButtons(g,W,H,s);
            if(!state.said.empty())
            {
                const float tw=TextWidth(g,state.said,22*s,true)+40*s;
                const RectF bubble(mid-tw/2,H*0.30f,tw,48*s);
                FillRound(g,bubble,14*s,Color(245,255,252,240),Color(255,230,120,60),2*s);
                Text(g,state.said,bubble,22*s,Color(255,200,40,40),true,StringAlignmentCenter,StringAlignmentCenter);
            }
            if(EndShown())PaintEnd(g,W,H,s);
            if(state.help)PaintHelp(g,W,H,s);
            if(state.confirmQuit)
            {
                SolidBrush shade(Color(160,0,0,0));
                g.FillRectangle(&shade,0.0f,0.0f,W,H);
                const RectF box(mid-220*s,ch/2.0f-70*s,440*s,140*s);
                FillRound(g,box,14*s,Color(250,40,22,22),Color(255,200,170,90),2*s);
                Text(g,T(L"ブロックタワーをやめますか？",L"Stop playing?"),RectF(box.X,box.Y+22*s,box.Width,36*s),24*s,white,true,StringAlignmentCenter);
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
            if(state.renderer)state.renderer->SetOverlay(picture);
        }

        // All the 2D layer shows.
        std::uint64_t Shown()
        {
            auto& t=state.game;
            ShownKey k;
            k(t.Now(),t.Turns(),t.Height(),t.Active(),t.ToMove(),t.CanPutBack(),t.Hovering(),EndShown(),YourTurn(),state.entrance==5);
            k(static_cast<int>(t.Resistance()*40),static_cast<int>(t.Wobble()*60),state.hover,state.hoverSlot,state.why,state.said,state.help,state.confirmQuit,state.wins[0],state.wins[1]);
            k(SlotsLeftToRight());
            k.Words(state.words.text,state.words.shown,static_cast<float>(state.besideHead.x),static_cast<float>(state.headScreen.y),2*Scale());
            return k.Value();
        }

        void DrawOverlay()
        {
            const std::uint64_t shown=Shown();
            if((!state.overlayDue&&shown==state.shown)||IsIconic(state.window))return;
            state.shown=shown;
            state.overlayDue=false;
            Paint();
        }

        int ButtonAt(float x,float y)
        {
            for(size_t i=0;i<state.buttons.size();++i)if(state.buttons[i].box.Contains(x,y))return static_cast<int>(i);
            return -1;
        }

        void OnKey(WPARAM key)
        {
            auto& t=state.game;
            if(key==VK_F1){state.camera={};state.yawGoal=0;return;}
            if(state.confirmQuit)
            {
                if(key=='Y')Finish();
                else if(key=='N'||key==VK_ESCAPE){state.confirmQuit=false;Redraw();}
                return;
            }
            if(key==VK_TAB){state.help=!state.help;Redraw();return;}
            if(state.help){if(key==VK_ESCAPE){state.help=false;Redraw();}return;}
            if(key==VK_LEFT||key=='A'){state.yawGoal+=0.5f;return;}
            if(key==VK_RIGHT||key=='D'){state.yawGoal-=0.5f;return;}
            if(t.Now()==Phase::Fallen)
            {
                if((key==VK_RETURN||key==VK_SPACE)&&EndShown())Do(Act::Again);
                else if(key==VK_ESCAPE)Finish();
                return;
            }
            if(key==VK_ESCAPE){state.confirmQuit=true;Redraw();return;}
            if(key=='B'){Do(Act::PutBack);return;}
            if(key>='1'&&key<='3'&&YourTurn()&&t.Hovering())
            {
                const auto order=SlotsLeftToRight();
                const size_t i=static_cast<size_t>(key-'1');
                if(i<order.size())Do(Act::Slot,order[i]);
            }
        }

        void Hover(int x,int y)
        {
            auto& g=state.game;
            int block=-1,slot=-1;
            std::wstring why;
            std::array<float,3> from{},to{};
            if(YourTurn()&&RayAt(x,y,from,to))
            {
                if(g.Now()==Phase::Place&&g.Hovering())slot=SlotAt(from,to);
                else if(g.Now()==Phase::Choose||g.Now()==Phase::Pull)
                {
                    block=g.Pick(from,to);
                    if(block>=0&&block!=g.Active()&&!g.Legal(block))why=g.WhyNot(block);
                }
            }
            if(block!=state.hoverBlock||slot!=state.hoverSlot||why!=state.why){state.hoverBlock=block;state.hoverSlot=slot;state.why=why;Redraw();}
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
                if(state.pulling)
                {
                    auto& g=state.game;
                    if(g.Now()!=Phase::Pull||!YourTurn()){state.pulling=false;ReleaseCapture();return 0;}
                    g.PullTo(state.pullStart+AlongFor(x-static_cast<float>(state.pullFrom.x),y-static_cast<float>(state.pullFrom.y)));
                    return 0;
                }
                Hover(GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam));
                return 0;
            }
            case WM_LBUTTONUP:
                if(state.pulling){state.pulling=false;ReleaseCapture();}
                return 0;
            case WM_LBUTTONDOWN:
            {
                const float x=static_cast<float>(GET_X_LPARAM(lParam)),y=static_cast<float>(GET_Y_LPARAM(lParam));
                if(state.confirmQuit)
                {
                    if(state.quitButtons[0].Contains(x,y))Finish();
                    else if(state.quitButtons[1].Contains(x,y)){state.confirmQuit=false;Redraw();}
                    return 0;
                }
                if(state.help){state.help=false;Redraw();return 0;}
                if(state.helpBox.Contains(x,y)){state.help=true;Redraw();return 0;}
                if(state.game.Now()==Phase::Fallen){if(EndShown())Do(Act::Again);return 0;}
                if(const int b=ButtonAt(x,y);b>=0&&state.buttons[static_cast<size_t>(b)].on)
                {
                    const Button k=state.buttons[static_cast<size_t>(b)];
                    Do(k.act,k.value);
                    return 0;
                }
                ClickAt(GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam));
                return 0;
            }
            case WM_CAPTURECHANGED:
                state.pulling=false;
                return 0;
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                BeginPaint(hwnd,&ps);
                state.overlayDue=true;
                EndPaint(hwnd,&ps);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_SIZE:
                PlaceView();
                return 0;
            case WM_CLOSE:
                Finish();
                return 0;
            case WM_DESTROY:
                StopMahjongSounds();
                state.window=nullptr;
                ReleaseView();
                state.canvas.reset();
                return 0;
            }
            return DefWindowProcW(hwnd,message,wParam,lParam);
        }
    }

    bool BlockTowerWindowOpen(){return state.window!=nullptr;}
    HWND BlockTowerWindowHandle(){return state.window;}
    bool BlockTowerWindowRect(RECT& r)
    {
        return state.window&&IsWindowVisible(state.window)&&!IsIconic(state.window)&&GetWindowRect(state.window,&r);
    }

    void BlockTowerLines(const std::wstring& answer)
    {
        state.prefetching=false;
        if(!state.window)return;
        state.lines.Answered(answer);
        Prefetch();
    }

    bool BlockTowerAfterword(std::wstring& situation,std::wstring& fallback)
    {
        if(state.afterword.empty())return false;
        situation=std::move(state.afterword);
        fallback=std::move(state.afterwordLine);
        state.afterword.clear();
        state.afterwordLine.clear();
        return true;
    }

    void BlockTowerHeard(const std::wstring& line,bool done)
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

    namespace
    {
        // Sparkles of magic from the block being moved (yours blue, hers pink), drifting up.
        void Emit(const std::array<float,3>& at,const std::array<float,4>& color,int count,float spread)
        {
            const float D=state.upper;
            std::uniform_real_distribution<float> any(0,1);
            auto& r=state.sparkRandom;
            for(int i=0;i<count;++i)
            {
                State::Spark k;
                const float a=any(r)*DirectX::XM_2PI;
                k.at={at[0]+(any(r)-0.5f)*spread,at[1]+(any(r)-0.5f)*spread*0.3f,at[2]+(any(r)-0.5f)*spread};
                k.velocity={std::cos(a)*0.05f*D*any(r),(0.04f+0.08f*any(r))*D,std::sin(a)*0.05f*D*any(r)};
                k.age=0;
                k.life=0.5f+0.6f*any(r);
                k.glow=i%3==0;
                k.size=(k.glow?0.022f:0.008f+0.008f*any(r))*D;
                k.angle=any(r)*DirectX::XM_2PI;
                k.spin=(any(r)-0.5f)*6;
                k.color=i%4==0?std::array<float,4>{1,1,1,1}:color;
                state.sparks.push_back(k);
            }
        }

        // Her arm out toward a point (the block her magic moves), as if casting.
        bool ReachFor(MascotModel& model,const std::array<float,3>& spot)
        {
            std::array<float,3> head{};
            if(!model.HeadPosition(head))return false;
            const float hx=head[0]-model.RootOffset()+state.herX,hz=head[2]+state.herZ;
            const std::array<float,3> from{spot[0]-hx,spot[1]+0.10f*state.hips-head[1],spot[2]-hz};
            return model.Reach(spot[0]>0.15f*state.upper?0:1,from,2.8,false,true);
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

        std::array<float,3> Tint(int who){return who==BlockTower::Her?std::array<float,3>{1.0f,0.45f,0.80f}:std::array<float,3>{0.40f,0.80f,1.0f};}
    }

    void TickBlockTowerWindow(MascotModel& model,double dt)
    {
        if(!state.window||!state.renderer)return;
        auto& r=*state.renderer;
        using namespace DirectX;
        const float D=state.upper,top=Top();
        dt=std::min(dt,0.1);
        auto& g=state.game;
        if(!state.tableBuilt){state.tableBuilt=true;r.SetGeometry(BuildTable());}
        // The entrance, as at the other tables: she walks in, turns to you, sits down and moves
        // in with her chair.
        const float seatZ=SeatZ(),seatHeight=-FloorY(),step=static_cast<float>(dt);
        const float standX=0,standZ=0.25f*D+SeatDepth(),pulled=standZ+0.25f*D+SeatFront(),startX=-1.8f*D,startZ=standZ;
        float lift=0,standing=1,lean=0;
        auto ease=[](float u){return u*u*(3-2*u);};
        if(state.entrance==0)
        {
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
        // Your block drawn out with the keys (W away from you, S toward you).
        if(YourTurn()&&g.Now()==Phase::Pull&&GetForegroundWindow()==state.window&&!state.pulling)
        {
            const bool away=(GetKeyState('W')&0x8000)||(GetKeyState(VK_UP)&0x8000),toward=(GetKeyState('S')&0x8000)||(GetKeyState(VK_DOWN)&0x8000);
            if(away!=toward)
            {
                const auto axis=g.Axis(g.Active());
                const auto at=ToWorld(g.BlockAt(g.Active()).at);
                const float toEye=(state.eye.x-at[0])*axis[0]+(state.eye.y-at[1])*axis[1]+(state.eye.z-at[2])*axis[2];
                const float way=(toEye>0?1.0f:-1.0f)*(toward?1.0f:-1.0f);
                g.PullTo(g.Wanted()+way*KeyPull*static_cast<float>(dt));
            }
        }
        // The physics (once she sits).
        if(state.entrance==5&&!state.frozen)g.Advance(dt);
        g.TakeKnocks();   // (no sound for them: the user found it better without)
        // Her arm out while her magic moves the block.
        const int active=g.Active();
        const bool moving=active>=0&&(g.Now()==Phase::Pull||g.Now()==Phase::Place);
        if(moving&&g.ToMove()==BlockTower::Her&&g.HersRunning()&&state.clock-state.reachedAt>2.4)
            if(ReachFor(model,ToWorld(g.BlockAt(active).at)))state.reachedAt=state.clock;
        // The end: "oops" when she brought it down, a clap (or her cheeks) when you did.
        if(g.Now()==Phase::Fallen&&!state.endReacted&&g.PhaseSeconds()>0.7)
        {
            state.endReacted=true;
            std::uniform_real_distribution<double> any(0,1);
            state.endGesture=g.Loser()==BlockTower::Her?L"oops":any(state.sparkRandom)<0.5?L"clap":L"cheeks";
            state.endGestureUntil=state.clock+1.5;
        }
        if(!state.endGesture.empty()&&state.entrance==5&&(model.EmoteSeated(state.endGesture,3.2)||state.clock>state.endGestureUntil))state.endGesture.clear();
        // The camera turned by the keys, eased.
        state.camera.yaw+=(state.yawGoal-state.camera.yaw)*static_cast<float>(1-std::exp(-dt/0.18));
        // What moves: her chair, the blocks, and the magic on the one being moved.
        std::vector<MascotRenderer::GeometryVertex> glow;
        {
            Mesh blocks;
            blocks.out=BuildChair(state.chairZ);
            const bool choosing=YourTurn()&&(g.Now()==Phase::Choose||g.Now()==Phase::Pull);
            for(int b=0;b<BlockTower::Blocks;++b)
            {
                float bright=0;
                if(b==active&&moving)bright=0.12f;
                else if(choosing&&b==state.hoverBlock)bright=g.Legal(b)?0.22f:-0.25f;
                AddBlock(blocks,g.BlockAt(b),b,bright);
            }
            r.SetMovingGeometry(std::move(blocks.out));
            if(choosing&&state.hoverBlock>=0&&state.hoverBlock!=active&&g.Legal(state.hoverBlock))
                AddGlowBox(glow,g.BlockAt(state.hoverBlock),{0.55f,0.45f,0.20f},0.05f);
            if(moving)
            {
                const auto color=Tint(g.ToMove());
                const auto& k=g.BlockAt(active);
                const float pulse=0.75f+0.25f*static_cast<float>(std::sin(state.clock*6));
                AddGlowBox(glow,k,{color[0]*0.7f*pulse,color[1]*0.7f*pulse,color[2]*0.7f*pulse},0.07f);
                const auto at=ToWorld(k.at);
                if(g.Now()==Phase::Pull)
                {
                    // The circle just past the end it comes out by, facing along it.
                    const auto axis=g.Axis(active);
                    const float way=g.Offset()>=0?1.0f:-1.0f,reach=(k.size[0]/2+0.6f)*Cm();
                    const std::array<float,3> c{at[0]+axis[0]*way*reach,at[1]+axis[1]*way*reach,at[2]+axis[2]*way*reach};
                    AddCircle(glow,c,{axis[0]*way,axis[1]*way,axis[2]*way},1.5f*Cm(),{color[0]*0.6f,color[1]*0.6f,color[2]*0.6f},static_cast<float>(state.clock*1.4));
                }
                else AddCircle(glow,{at[0],at[1]-(k.size[1]/2+0.3f)*Cm(),at[2]},{0,1,0},2.4f*Cm(),{color[0]*0.5f,color[1]*0.5f,color[2]*0.5f},static_cast<float>(state.clock*1.2));
                if(state.clock>=state.sparkDue)
                {
                    state.sparkDue=state.clock+0.05;
                    Emit(at,{color[0],color[1],color[2],1},2,k.size[0]*Cm());
                }
            }
            // Laying a block: where it may go.
            if(g.Hovering())
                for(int s:g.FreeSlots())
                {
                    const bool lit=s==state.hoverSlot&&YourTurn();
                    const float pulse=0.6f+0.4f*static_cast<float>(std::sin(state.clock*4+s));
                    const auto color=Tint(g.ToMove());
                    const float k=lit?1.2f:0.65f*pulse;
                    AddGlowBox(glow,g.SlotBlock(s),{color[0]*k,color[1]*k,color[2]*k},lit?0.11f:0.08f,1.0f);
                }
        }
        r.SetGlowGeometry(std::move(glow));
        {
            std::vector<MascotRenderer::Particle> shown;
            for(auto& k:state.sparks)
            {
                k.age+=step;
                for(int a=0;a<3;++a)k.at[a]+=k.velocity[a]*step;
                for(int a:{0,2})k.velocity[a]*=std::exp(-step*1.5f);
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
        // From your side of the table, a little above, looking at the tower (higher as it grows).
        const auto& c=state.camera;
        // (eased: the height steps up as a block starts a new level, and the view jerked with it)
        const float towerGoal=static_cast<float>(g.Height())*BlockTower::Thick*Cm();
        state.towerTop=state.towerTop<=0?towerGoal:state.towerTop+(towerGoal-state.towerTop)*static_cast<float>(1-std::exp(-dt/0.8));
        const float towerTop=state.towerTop;
        const XMVECTOR target=XMVectorSet(c.x*D,top+std::max(0.30f*D,0.55f*towerTop)+(c.y+0.6f*standing)*D,FieldZ()+c.z*D,0);
        const XMVECTOR toEye=XMVectorSet(std::sin(c.yaw)*std::cos(c.pitch),std::sin(c.pitch),-std::cos(c.yaw)*std::cos(c.pitch),0);
        const XMVECTOR eye=XMVectorAdd(target,XMVectorScale(toEye,c.distance*(1+0.22f*standing)*D));
        r.SetCamera(eye,target,XMConvertToRadians(42.0f));
        XMStoreFloat3(&state.eye,eye);
        XMStoreFloat3(&state.target,target);
        if(std::array<float,3> head{};model.HeadPosition(head))
        {
            const XMVECTOR at=XMVector3Transform(XMVectorSet(head[0],head[1],head[2],1),world);
            const XMVECTOR right=XMVector3Normalize(XMVector3Cross(XMVectorSet(0,1,0,0),XMVectorSubtract(target,eye)));
            const XMVECTOR beside=XMVectorAdd(at,XMVectorScale(right,0.26f*D));
            state.headScreen=r.Project({XMVectorGetX(at),XMVectorGetY(at),XMVectorGetZ(at)});
            state.besideHead=r.Project({XMVectorGetX(beside),XMVectorGetY(beside),XMVectorGetZ(beside)});
        }
        {
            // She looks at the block being moved, else at you.
            XMVECTOR det;
            const XMMATRIX back=XMMatrixInverse(&det,world);
            std::array<float,3> head{};
            const bool talking=!state.words.text.empty()&&model.HeadPosition(head);
            if(moving&&!talking)
            {
                const auto at=ToWorld(g.BlockAt(active).at);
                const XMVECTOR p=XMVector3Transform(XMVectorSet(at[0],at[1],at[2],1),back);
                model.LookToward({XMVectorGetX(p),XMVectorGetY(p),XMVectorGetZ(p)},true);
            }
            else
            {
                model.HeadPosition(head);
                const XMVECTOR you=XMVector3Transform(eye,back);
                model.LookToward({XMVectorGetX(you),head[1],XMVectorGetZ(you)},talking);
            }
        }
        {
            std::vector<MascotRenderer::PointLight> lights;
            MascotRenderer::PointLight lamp;
            lamp.position[0]=0;lamp.position[1]=top+2.3f*D;lamp.position[2]=FieldZ();
            lamp.radius=4.2f*D;
            lamp.color[0]=0.42f;lamp.color[1]=0.30f;lamp.color[2]=0.16f;
            lamp.onModel=false;
            lights.push_back(lamp);
            if(moving)
            {
                // The magic's light on the blocks round it.
                const auto at=ToWorld(g.BlockAt(active).at);
                const auto color=Tint(g.ToMove());
                MascotRenderer::PointLight magic;
                magic.position[0]=at[0];magic.position[1]=at[1];magic.position[2]=at[2];
                magic.radius=0.5f*D;
                for(size_t i=0;i<3;++i)magic.color[i]=color[i]*0.35f;
                magic.onModel=false;
                lights.push_back(magic);
            }
            r.SetLights(lights);
        }
        {
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

    std::wstring DescribeBlockTower()
    {
        if(!state.window)return L"";
        std::wstring out=Fmt(L"blocktower: entrance {0}, {1}, wins you {2} her {3}, face {4}, sparks {5}\n",
                             {std::to_wstring(state.entrance),state.game.Report(),std::to_wstring(state.wins[0]),std::to_wstring(state.wins[1]),
                              std::to_wstring(static_cast<int>(state.face)),std::to_wstring(state.sparks.size())});
        return out+(state.words.told.empty()?std::wstring():L"blocktower words:\n"+state.words.told);
    }

    bool SaveBlockTowerView(const std::wstring& path)
    {
        return state.renderer&&state.renderer->SaveLastFrame(path);
    }

    void PlayBlockTowerMoves(int turns,float yaw,int stage)
    {
        if(!state.window)return;
        auto& g=state.game;
        // A turn of hers going on is played out first.
        for(int i=0;i<240*30&&g.Now()!=Phase::Choose&&g.Now()!=Phase::Fallen;++i)g.Advance(1/240.0);
        for(int i=0;i<turns&&g.Now()==Phase::Choose;++i)g.PlayTurn();
        state.turnSeen=g.Turns();
        state.camera.yaw+=yaw;
        state.yawGoal=state.camera.yaw;
        if((stage==1||stage==2)&&g.Now()==Phase::Choose)
        {
            g.Think();
            while(!g.Thought())Sleep(10);
            g.BeginHers();
            for(int i=0;i<4000&&g.Now()==Phase::Pull&&std::abs(g.Offset())<(stage==1?BlockTower::Length*0.5f:100.0f);++i)g.Advance(1/240.0);
            if(stage==2)for(int i=0;i<480&&g.Now()==Phase::Place&&!g.Hovering();++i)g.Advance(1/240.0);
            state.frozen=true;
        }
        if(stage==5)
        {
            // Your pull through the mouse: her choice of block clicked where it shows, then dragged
            // a block's length and a half along it (the physics then follows).
            if(g.ToMove()!=BlockTower::You&&g.Now()==Phase::Choose)g.PlayTurn();
            g.Think();
            while(!g.Thought())Sleep(10);
            const int b=g.HerChoiceNow();
            const auto axis=g.Axis(b);
            const auto& k=g.BlockAt(b);
            // Its end nearer the camera (its middle may lie behind the level above).
            const auto mid=ToWorld(k.at);
            const float toEye=(state.eye.x-mid[0])*axis[0]+(state.eye.y-mid[1])*axis[1]+(state.eye.z-mid[2])*axis[2];
            const float way=toEye>0?1.0f:-1.0f,end=BlockTower::Length/2-0.4f;
            const POINT at=Screen({k.at[0]+axis[0]*way*end,k.at[1]+axis[1]*way*end,k.at[2]+axis[2]*way*end});
            Proc(state.window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(at.x,at.y));
            const float pull=way*(end+12);
            const POINT far1=Screen({k.at[0]+axis[0]*pull,k.at[1]+axis[1]*pull,k.at[2]+axis[2]*pull});
            Proc(state.window,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(far1.x,far1.y));
            state.words.told+=Fmt(L"  test: block {0} clicked at {1},{2}: active {3}, wanted {4}\n",{std::to_wstring(b),std::to_wstring(at.x),std::to_wstring(at.y),
                                  std::to_wstring(g.Active()),std::to_wstring(g.Wanted())});
            Proc(state.window,WM_LBUTTONUP,0,MAKELPARAM(far1.x,far1.y));
        }
        if(stage==3)state.help=true;
        if(stage==4){g.Shove();for(int i=0;i<720;++i)g.Advance(1/240.0);}
        Redraw();
    }

    void ShowBlockTowerWindow(HWND owner,const BlockTowerOpponent& her,std::function<void()> onEnd)
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
        state.lost=0;
        state.wins={};
        std::random_device device;
        state.first=device()%2?BlockTower::You:BlockTower::Her;
        state.lines.Load(L"BLOCKTOWERLINES",L"blocktower",her.ownLinesFile,her.linesFile,her.styleKey,English(),
                         {L"ブロックタワー（積んだ木のブロックを魔法で1本ずつ抜いて上に積むゲーム）",L"the block tower (a game of taking wooden blocks out of a tower by magic and laying them on top)",L"",L""});
        state.voice.Stop();
        state.prefetching=false;
        state.face=Expression::Neutral;
        state.words={};
        state.words.told=state.lines.Report();
        if(!her.linesFile.empty())ShowLinesProblem(owner,state.lines);
        NewGame();

        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&info);
        const RECT work=info.rcWork;
        const int dpi=static_cast<int>(GetDpiForSystem());
        const int w=std::min(static_cast<int>(work.right-work.left)*9/10,MulDiv(static_cast<int>(BaseWidth)+16,dpi,96));
        const int h=std::min(static_cast<int>(work.bottom-work.top)*9/10,MulDiv(static_cast<int>(BaseHeight)+39,dpi,96));
        const std::wstring title=std::wstring(T(L"ブロックタワー",L"Block tower"))+L" - "+HerName();
        state.window=CreateWindowExW(0,ClassName,title.c_str(),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                                     work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,
                                     nullptr,nullptr,instance,nullptr);
        if(!CreateView())
        {
            MessageBoxW(state.window,T(L"ブロックタワーは3D表示でしか遊べません（3D表示を作れませんでした）。",L"The block tower can only be played in 3D, and the 3D view could not be made."),title.c_str(),MB_ICONINFORMATION);
            auto end=std::move(state.onEnd);
            DestroyWindow(state.window);
            if(end)end();
            return;
        }
        PlaceView();
        SetTimer(state.window,TimerId,40,nullptr);
        ShowWindow(state.window,SW_SHOWNORMAL);
        SetForegroundWindow(state.window);
    }
}
