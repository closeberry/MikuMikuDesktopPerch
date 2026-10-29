#define NOMINMAX
#include "MetroidWindow.hpp"
#include "Metroid.hpp"
#include "Log.hpp"
#include "MascotModel.hpp"
#include "MascotRenderer.hpp"
#include "Text.hpp"
#include "TextureLoader.hpp"
#include <objidl.h>
#include <gdiplus.h>
#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <unordered_set>
#include <vector>
#include <xinput.h>
#pragma comment(lib,"gdiplus.lib")
#pragma comment(lib,"xinput.lib")

namespace MikuMikuDesktopPerch
{
    namespace
    {
        using namespace Gdiplus;
        constexpr wchar_t ClassName[]=L"MikuMikuDesktopPerchRuins";
        constexpr wchar_t ViewClassName[]=L"MikuMikuDesktopPerchRuinsView";
        constexpr UINT_PTR TimerId=1;

        // The geometry shader's patterns, by the colour's alpha (GeometryPS): plain, floor stones,
        // wall bricks (each with the area's look added in steps of 0.02), a thing's own stone.
        constexpr float Plain=1,Stones=0.75f,Bricks=0.5f,Stone=0.06f;
        // The rock's faces: its front this many tiles toward the camera from her, its back (where
        // the wall behind her is) this many away.
        constexpr float Front=1.5f,Back=1.5f;
        // Rock drawn round a room, beyond its own edge (seen past it as the camera comes near).
        constexpr int Margin=10;
        // The camera shows this many tiles from top to bottom, looking down this much.
        constexpr float ViewTiles=12,Pitch=0.14f,Fov=0.6f;
        // She is turned this far toward the camera from her side (her face shows as she runs).
        constexpr float Toward=0.3f;

        struct State
        {
            HWND window=nullptr,view=nullptr;
            ULONG_PTR gdiplus=0;
            MetroidHero her;
            std::function<void()> onEnd;
            Metroid::Map map;
            std::unique_ptr<Metroid::Game> game;
            std::unique_ptr<MascotRenderer> renderer;
            std::unique_ptr<Bitmap> canvas;
            LARGE_INTEGER last{};
            bool paused=false,asking=false;
            Metroid::Input keys;
            // A gamepad (XInput): the one in use (-1: none found yet), its buttons last time, and
            // when to look for one again while there is none.
            int pad=-1;
            WORD padButtons=0;
            ULONGLONG padLookAt=0;
            Metroid::Input padHeld;   // what it holds this frame
            // Diagnostics: keys held in turn instead (PlayMetroidForTest), and how long the one
            // now has left.
            std::vector<std::pair<Metroid::Input,double>> script;
            size_t scriptAt=0;
            double scriptLeft=0;
            bool scripted=false;
            int frames=0,jumps=0,landings=0;
            float steepest=0;   // the fastest landing (tiles/s)
            // World units: a tile's side (her height over 2.5), where she is drawn turned.
            float T=8;
            int builtArea=-1;
            float drawnAngle=0;
            Activity gait=Activity::Idle;
            float cameraX=0,cameraY=0;
            bool cameraPlaced=false;
            std::wstring hudShown;
        } state;

        float Scale(){return state.window?static_cast<float>(GetDpiForWindow(state.window))/96.0f:1;}
        std::wstring HerName(){return state.her.name.empty()?std::wstring(T(L"彼女",L"Her")):state.her.name;}
        bool Playing(){return state.game&&!state.paused&&!state.asking;}

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

        void RoundRect(GraphicsPath& path,RectF r,float radius)
        {
            const float d=radius*2;
            path.AddArc(r.X,r.Y,d,d,180,90);
            path.AddArc(r.X+r.Width-d,r.Y,d,d,270,90);
            path.AddArc(r.X+r.Width-d,r.Y+r.Height-d,d,d,0,90);
            path.AddArc(r.X,r.Y+r.Height-d,d,d,90,90);
            path.CloseFigure();
        }

        std::wstring Lang(const std::array<std::wstring,2>& names){return names[English()?1:0];}

        const Metroid::Room* HereRoom()
        {
            const int r=state.game?state.game->Room():-1;
            return r>=0?&state.map.Rooms()[static_cast<size_t>(r)]:nullptr;
        }

        // ---- What is drawn over the picture -------------------------------------------------

        // The area and the room top left, the keys at the foot, and the question when leaving.
        void DrawHud(Graphics& g,int w,int h,float s)
        {
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            if(const auto* room=HereRoom())
            {
                const auto* area=state.map.AreaOf(room->area);
                const std::wstring place=(area?Lang(area->name)+L"  ":L"")+Lang(room->name);
                SolidBrush back(Color(130,10,12,18));
                g.FillRectangle(&back,RectF(0,0,static_cast<REAL>(w),30*s));
                Text(g,place,RectF(12*s,5*s,w-24*s,22*s),15*s,Color(235,236,226,206));
            }
            const std::wstring help=state.pad>=0?T(L"左スティック・十字 走る　A ジャンプ（長押しで高く）　START やめる",
                                                   L"Left stick, D-pad run   A jump (hold for higher)   START leave"):
                                                 T(L"←→ 走る　Z ジャンプ（長押しで高く）　Esc やめる",
                                                   L"←→ run   Z jump (hold for higher)   Esc leave");
            Text(g,help,RectF(12*s,h-26*s,w-24*s,22*s),13*s,Color(200,226,222,210));
            if(state.asking||state.paused)
            {
                const std::wstring ask=state.asking?(state.pad>=0?T(L"遺跡を出ますか？（Y：出る　B：続ける）",L"Leave the ruins? (Y: leave  B: go on)"):
                                                                  T(L"遺跡を出ますか？（Y：出る　N：続ける）",L"Leave the ruins? (Y: leave  N: go on)")):
                                                    T(L"一時停止中（クリックかキーで続ける）",L"Paused (click or press a key to go on)");
                const RectF box(w/2.0f-230*s,h/2.0f-34*s,460*s,68*s);
                GraphicsPath path;
                RoundRect(path,box,12*s);
                SolidBrush fill(Color(215,16,18,26));
                g.FillPath(&fill,&path);
                Pen edge(Color(200,200,180,130),1.5f*s);
                g.DrawPath(&edge,&path);
                Text(g,ask,box,17*s,Color(255,240,236,220),true,StringAlignmentCenter,StringAlignmentCenter);
            }
        }

        // The 2D picture (no 3D): the room she is in, fitted to the window, and her as a figure.
        void DrawRoom2D(Graphics& g,int w,int h)
        {
            g.Clear(Color(255,22,24,30));
            const auto* room=HereRoom();
            if(!room)return;
            const float s=Scale(),top=30*s,bottom=30*s;
            const float px=std::min(w/static_cast<float>(room->width),(h-top-bottom)/static_cast<float>(room->height));
            const float ox=(w-px*room->width)/2,oy=top+(h-top-bottom-px*room->height)/2;
            SolidBrush rock(Color(255,92,96,84)),rockTop(Color(255,128,134,112)),ledge(Color(255,150,118,80)),air(Color(255,40,42,50));
            for(int ty=0;ty<room->height;++ty)
                for(int tx=0;tx<room->width;++tx)
                {
                    const char c=room->rows[static_cast<size_t>(ty)][static_cast<size_t>(tx)];
                    const RectF cell(ox+tx*px,oy+ty*px,px+0.5f,px+0.5f);
                    if(c=='#')
                    {
                        g.FillRectangle(&rock,cell);
                        if(state.map.At(room->x+tx,room->y+ty-1)!='#')g.FillRectangle(&rockTop,RectF(cell.X,cell.Y,cell.Width,px*0.2f));
                    }
                    else
                    {
                        g.FillRectangle(&air,cell);
                        if(c=='=')g.FillRectangle(&ledge,RectF(cell.X,cell.Y,cell.Width,px*0.25f));
                    }
                }
            const auto& b=state.game->Hero();
            const auto& m=state.game->Rule();
            const RectF body(ox+(b.x-room->x-m.halfWidth)*px,oy+(b.y-room->y-m.height)*px,2*m.halfWidth*px,m.height*px);
            GraphicsPath path;
            RoundRect(path,body,body.Width*0.45f);
            SolidBrush pink(Color(255,236,140,170));
            g.FillPath(&pink,&path);
            SolidBrush eye(Color(255,40,30,40));
            g.FillEllipse(&eye,body.X+body.Width*(b.facing>0?0.62f:0.18f),body.Y+body.Height*0.14f,body.Width*0.2f,body.Width*0.2f);
            Text(g,HerName(),RectF(body.X-60*s,body.Y-22*s,body.Width+120*s,20*s),12*s,Color(255,250,240,245),false,StringAlignmentCenter);
        }

        // ---- The 3D rooms -----------------------------------------------------------------

        struct Mesh
        {
            std::vector<MascotRenderer::GeometryVertex> out;
            using V=std::array<float,3>;
            float local=1;   // a thing's own pattern: world units to its units
            void Quad(const V& a,const V& b,const V& c,const V& d,const V& n,const float (&col)[4])
            {
                const V* p[6]={&a,&b,&c,&a,&c,&d};
                for(const V* q:p)
                {
                    MascotRenderer::GeometryVertex v{{(*q)[0],(*q)[1],(*q)[2]},{n[0],n[1],n[2]},{col[0],col[1],col[2],col[3]}};
                    if(col[3]<0.24f){v.wall[0]=(*q)[0]*local;v.wall[1]=(*q)[1]*local;v.wall[3]=(*q)[2]*local;}
                    out.push_back(v);
                }
            }
            void Box(float x0,float y0,float z0,float x1,float y1,float z1,const float (&c)[4])
            {
                Quad({x0,y1,z0},{x1,y1,z0},{x1,y1,z1},{x0,y1,z1},{0,1,0},c);
                Quad({x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},{0,0,-1},c);
                Quad({x0,y0,z0},{x0,y0,z1},{x0,y1,z1},{x0,y1,z0},{-1,0,0},c);
                Quad({x1,y0,z0},{x1,y0,z1},{x1,y1,z1},{x1,y1,z0},{1,0,0},c);
                Quad({x0,y0,z0},{x1,y0,z0},{x1,y0,z1},{x0,y0,z1},{0,-1,0},c);
            }
        };

        // An area's rooms (and the rock round them): each rock tile's face toward the camera,
        // its top where nothing stands on it (the floor's stones), its sides and underside where
        // open; behind each open tile the wall at the back; the ledges as slabs of stone.
        std::vector<MascotRenderer::GeometryVertex> BuildArea(int area)
        {
            // The looks of the walls and floors, as the dungeon's (GeometryPS's themes).
            struct Look {float floor[3],wall[3],ceiling[3];};
            static constexpr Look Looks[]={
                {{0.50f,0.46f,0.58f},{0.54f,0.38f,0.33f},{0.85f,0.78f,0.80f}},
                {{0.50f,0.50f,0.44f},{0.52f,0.52f,0.46f},{0.72f,0.74f,0.66f}},
                {{0.70f,0.82f,0.94f},{0.52f,0.66f,0.82f},{0.82f,0.90f,1.00f}},
                {{0.34f,0.28f,0.26f},{0.32f,0.26f,0.24f},{0.42f,0.32f,0.30f}},
                {{0.44f,0.38f,0.56f},{0.34f,0.28f,0.46f},{0.52f,0.44f,0.62f}}};
            const auto* a=state.map.AreaOf(area);
            const int look=std::clamp(a?a->look:0,0,static_cast<int>(std::size(Looks))-1);
            const Look& L=Looks[look];
            const float floorPattern=Stones+0.02f*look,wallPattern=Bricks+0.02f*look;
            const float floor[4]={L.floor[0],L.floor[1],L.floor[2],floorPattern};
            const float wall[4]={L.wall[0],L.wall[1],L.wall[2],wallPattern};
            const float ceiling[4]={L.ceiling[0]*0.6f,L.ceiling[1]*0.6f,L.ceiling[2]*0.6f,floorPattern};
            // The wall behind her: darker, so the rock in front of it stands out.
            const float back[4]={L.wall[0]*0.55f,L.wall[1]*0.55f,L.wall[2]*0.6f,wallPattern};
            const float slab[4]={0.58f,0.55f,0.50f,Stone};
            const float T=state.T,zf=-Front*T,zb=Back*T;
            Mesh m;
            m.local=10/T;
            const auto& map=state.map;
            auto rock=[&](int tx,int ty){return map.At(tx,ty)=='#';};
            std::unordered_set<long long> done;   // tiles outside every room, drawn once
            const auto& rooms=map.Rooms();
            for(size_t i=0;i<rooms.size();++i)
            {
                const auto& r=rooms[i];
                if(r.area!=area)continue;
                for(int ty=r.y-Margin;ty<r.y+r.height+Margin;++ty)
                    for(int tx=r.x-Margin;tx<r.x+r.width+Margin;++tx)
                    {
                        const int owner=map.RoomAt(tx,ty);
                        if(owner>=0&&owner!=static_cast<int>(i))continue;   // its own room draws it (or it is in another area)
                        if(owner<0&&!done.insert((static_cast<long long>(tx)<<32)^static_cast<unsigned>(ty)).second)continue;
                        const float x0=tx*T,x1=(tx+1)*T,y1=-ty*T,y0=-(ty+1)*T;
                        const char c=map.At(tx,ty);
                        if(c=='#')
                        {
                            m.Quad({x0,y0,zf},{x1,y0,zf},{x1,y1,zf},{x0,y1,zf},{0,0,-1},wall);
                            if(!rock(tx,ty-1))m.Quad({x0,y1,zf},{x1,y1,zf},{x1,y1,zb},{x0,y1,zb},{0,1,0},floor);
                            if(!rock(tx,ty+1))m.Quad({x0,y0,zf},{x1,y0,zf},{x1,y0,zb},{x0,y0,zb},{0,-1,0},ceiling);
                            if(!rock(tx-1,ty))m.Quad({x0,y0,zf},{x0,y0,zb},{x0,y1,zb},{x0,y1,zf},{-1,0,0},wall);
                            if(!rock(tx+1,ty))m.Quad({x1,y0,zf},{x1,y0,zb},{x1,y1,zb},{x1,y1,zf},{1,0,0},wall);
                        }
                        else
                        {
                            m.Quad({x0,y0,zb},{x1,y0,zb},{x1,y1,zb},{x0,y1,zb},{0,0,-1},back);
                            if(c=='=')
                            {
                                // A slab, a little apart from the next one, resting on the back wall.
                                const float gap=tx>0&&map.At(tx-1,ty)=='='?0.0f:0.04f*T,gap1=map.At(tx+1,ty)=='='?0.0f:0.04f*T;
                                m.Box(x0+gap,y1-0.24f*T,zf*0.75f,x1-gap1,y1,zb,slab);
                            }
                        }
                    }
            }
            Log(L"ruins: area %d built (%zu vertices)",area,m.out.size());
            return std::move(m.out);
        }

        // ---- The game ----------------------------------------------------------------------

        // Where to stand in a room (diagnostics): the first open tile over rock along its middle row
        // of columns, with room for her above.
        bool StandIn(const Metroid::Room& r,float& x,float& y)
        {
            for(int k=0;k<r.width;++k)
            {
                const int tx=r.x+r.width/2+((k&1)?-(k+1)/2:k/2);
                for(int ty=r.y+3;ty<r.y+r.height;++ty)
                    if(state.map.At(tx,ty)=='#'&&state.map.At(tx,ty-1)!='#'&&state.map.At(tx,ty-2)!='#'&&state.map.At(tx,ty-3)!='#')
                    {
                        x=tx+0.5f;y=static_cast<float>(ty);
                        return true;
                    }
            }
            return false;
        }

        // The keys held now: the keyboard's and the gamepad's together, or the test's.
        Metroid::Input Held(double dt)
        {
            if(!state.scripted)
            {
                const auto &k=state.keys,&p=state.padHeld;
                return {k.left||p.left,k.right||p.right,k.up||p.up,k.down||p.down,k.jump||p.jump};
            }
            while(state.scriptAt<state.script.size()&&state.scriptLeft<=0)
            {
                if(++state.scriptAt<state.script.size())state.scriptLeft=state.script[state.scriptAt].second;
            }
            if(state.scriptAt>=state.script.size())return {};
            state.scriptLeft-=dt;
            return state.script[state.scriptAt].first;
        }

        void StepGame(double dt)
        {
            if(!Playing())return;
            state.game->Step(dt,Held(dt));
        }

        void Finish()
        {
            if(!state.window)return;
            KillTimer(state.window,TimerId);
            if(MascotModel* model=state.her.model)
            {
                model->SetCarryMotion(0,0,0,0);
                model->SetFacingWalk(false);
                model->SetAutoGestures(true);
            }
            auto onEnd=std::move(state.onEnd);
            DestroyWindow(state.window);
            if(onEnd)onEnd();
        }

        void ClearKeys(){state.keys={};}

        // The gamepad (XInput, the first one connected; looked for every 2 s while there is
        // none): the stick or the D-pad runs, A jumps; START asks whether to leave, and then Y
        // leaves, B or START goes on. Only while the window is in front (XInput reads the pad
        // whatever has the focus). The window may be gone after (Y).
        void PollPad()
        {
            state.padHeld={};
            const ULONGLONG now=GetTickCount64();
            if(state.pad<0)
            {
                if(now<state.padLookAt)return;
                state.padLookAt=now+2000;
                for(DWORD i=0;i<XUSER_MAX_COUNT&&state.pad<0;++i)
                    if(XINPUT_STATE x{};XInputGetState(i,&x)==ERROR_SUCCESS){state.pad=static_cast<int>(i);state.padButtons=x.Gamepad.wButtons;}
                if(state.pad<0)return;
                Log(L"ruins: gamepad %d",state.pad);
            }
            XINPUT_STATE x{};
            if(XInputGetState(static_cast<DWORD>(state.pad),&x)!=ERROR_SUCCESS)
            {
                state.pad=-1;
                state.padButtons=0;
                state.padLookAt=0;
                return;
            }
            const WORD held=x.Gamepad.wButtons,pressed=held&~state.padButtons;
            state.padButtons=held;
            if(GetForegroundWindow()!=state.window)return;
            if(state.asking)
            {
                if(pressed&XINPUT_GAMEPAD_Y){Finish();return;}
                if(pressed&(XINPUT_GAMEPAD_B|XINPUT_GAMEPAD_START)){state.asking=false;QueryPerformanceCounter(&state.last);}
                return;
            }
            if(state.paused&&pressed){state.paused=false;QueryPerformanceCounter(&state.last);}
            if(pressed&XINPUT_GAMEPAD_START){state.asking=true;ClearKeys();return;}
            const float dead=0.45f*32767;
            const float lx=x.Gamepad.sThumbLX,ly=x.Gamepad.sThumbLY;
            auto& in=state.padHeld;
            in.left=(held&XINPUT_GAMEPAD_DPAD_LEFT)||lx<-dead;
            in.right=(held&XINPUT_GAMEPAD_DPAD_RIGHT)||lx>dead;
            in.up=(held&XINPUT_GAMEPAD_DPAD_UP)||ly>dead;
            in.down=(held&XINPUT_GAMEPAD_DPAD_DOWN)||ly<-dead;
            in.jump=(held&XINPUT_GAMEPAD_A)!=0;
        }

        // The words over the 3D picture, drawn again only when they change.
        void UpdateOverlay()
        {
            RECT client{};
            GetClientRect(state.window,&client);
            const int w=std::max(1L,client.right),h=std::max(1L,client.bottom);
            const std::wstring shown=std::to_wstring(w)+L"x"+std::to_wstring(h)+L" "+std::to_wstring(state.game->Room())+
                                     (state.paused?L"p":L"")+(state.asking?L"a":L"")+(state.pad>=0?L"g":L"");
            if(shown==state.hudShown)return;
            state.hudShown=shown;
            Bitmap canvas(w,h,PixelFormat32bppPARGB);
            {
                Graphics g(&canvas);
                g.Clear(Color(0,0,0,0));
                DrawHud(g,w,h,Scale());
            }
            ImageRgba8 picture;
            picture.levels.resize(1);
            auto& level=picture.levels[0];
            level.width=static_cast<unsigned>(w);level.height=static_cast<unsigned>(h);
            level.pixels.resize(static_cast<size_t>(w)*h*4);
            BitmapData bits{};
            Rect whole(0,0,w,h);
            if(canvas.LockBits(&whole,ImageLockModeRead,PixelFormat32bppPARGB,&bits)==Ok)
            {
                for(int y=0;y<h;++y)
                {
                    const BYTE* from=static_cast<const BYTE*>(bits.Scan0)+static_cast<size_t>(y)*bits.Stride;
                    BYTE* to=level.pixels.data()+static_cast<size_t>(y)*w*4;
                    for(int x=0;x<w;++x,from+=4,to+=4){to[0]=from[2];to[1]=from[1];to[2]=from[0];to[3]=from[3];}
                }
                canvas.UnlockBits(&bits);
            }
            state.renderer->SetOverlay(picture);
        }

        void Paint(HDC dc)
        {
            if(state.renderer)return;   // the 3D view covers the window
            RECT client{};
            GetClientRect(state.window,&client);
            const int w=std::max(1L,client.right),h=std::max(1L,client.bottom);
            if(!state.canvas||static_cast<int>(state.canvas->GetWidth())!=w||static_cast<int>(state.canvas->GetHeight())!=h)
                state.canvas=std::make_unique<Bitmap>(w,h,PixelFormat32bppPARGB);
            Graphics g(state.canvas.get());
            DrawRoom2D(g,w,h);
            DrawHud(g,w,h,Scale());
            Graphics screen(dc);
            screen.DrawImage(state.canvas.get(),0,0);
            ++state.frames;
        }

        void Tick()
        {
            LARGE_INTEGER now,rate;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&rate);
            const double dt=std::min(0.05,static_cast<double>(now.QuadPart-state.last.QuadPart)/rate.QuadPart);
            state.last=now;
            if(state.renderer)return;   // the 3D view goes on with her frames (TickMetroidWindow)
            PollPad();
            if(!state.window)return;
            StepGame(dt);
            InvalidateRect(state.window,nullptr,FALSE);
        }

        LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam);

        LRESULT CALLBACK ViewProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            switch(message)
            {
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
                SetFocus(state.window);
                return Proc(state.window,WM_LBUTTONDOWN,wParam,lParam);
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
                Log(L"ruins: resize: %s",error.c_str());
            state.hudShown.clear();
        }

        void ReleaseView()
        {
            if(state.renderer){state.renderer->WaitIdle();state.renderer.reset();Log(L"ruins: 3D view released");}
            state.view=nullptr;
        }

        bool CreateView()
        {
            if(!state.her.model)return false;
            const HINSTANCE instance=GetModuleHandleW(nullptr);
            static bool registered=false;
            if(!registered)
            {
                WNDCLASSEXW wc{sizeof(wc)};
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
            renderer->SetBackground(0.04f,0.04f,0.06f,1);
            if(state.her.light)renderer->SetResolutionScale(0.75f);
            std::wstring error;
            if(!renderer->Initialize(state.view,w,h,state.her.light?1:state.her.msaa,false,error)||
               !renderer->LoadModel(*state.her.model,state.her.toonDir,state.her.light?512:state.her.maxTexture,error))
            {
                Log(L"ruins: no 3D view (%s)",error.c_str());
                renderer->WaitIdle();
                DestroyWindow(state.view);
                state.view=nullptr;
                return false;
            }
            renderer->SetLight(-25,40);
            state.renderer=std::move(renderer);
            // A tile is her height over 2.5 (she clears a 2-tile step with a plain jump).
            const auto lo=state.her.model->BoundsMin(),hi=state.her.model->BoundsMax();
            state.T=std::max(0.5f,(hi[1]-lo[1])/2.5f);
            state.builtArea=-1;
            state.cameraPlaced=false;
            state.hudShown.clear();
            Log(L"ruins: 3D view %ux%u on %s, a tile %.2f",w,h,state.renderer->AdapterName().c_str(),state.T);
            return true;
        }

        void SetKey(WPARAM key,bool down)
        {
            auto& k=state.keys;
            switch(key)
            {
            case VK_LEFT:case 'A':k.left=down;break;
            case VK_RIGHT:case 'D':k.right=down;break;
            case VK_UP:case 'W':k.up=down;break;
            case VK_DOWN:case 'S':k.down=down;break;
            case 'Z':case VK_SPACE:case 'K':k.jump=down;break;
            }
        }

        LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            switch(message)
            {
            case WM_TIMER:
                if(wParam==TimerId&&state.game)Tick();
                return 0;
            case WM_KEYDOWN:
                if(state.asking)
                {
                    if(wParam=='Y'){Finish();return 0;}
                    if(wParam=='N'||wParam==VK_ESCAPE){state.asking=false;QueryPerformanceCounter(&state.last);}
                    return 0;
                }
                if(state.paused){state.paused=false;QueryPerformanceCounter(&state.last);}
                if(wParam==VK_ESCAPE){state.asking=true;ClearKeys();return 0;}
                if(!(lParam&(1<<30)))SetKey(wParam,true);   // not the key's repeats
                return 0;
            case WM_KEYUP:
                SetKey(wParam,false);
                return 0;
            case WM_LBUTTONDOWN:
                if(state.paused&&!state.asking){state.paused=false;QueryPerformanceCounter(&state.last);}
                return 0;
            case WM_ACTIVATE:
                // Away from the window, the ruins wait (unless keys are played for a test).
                if(LOWORD(wParam)==WA_INACTIVE){ClearKeys();if(!state.scripted&&!state.asking)state.paused=true;}
                break;
            case WM_KILLFOCUS:
                ClearKeys();
                break;
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC dc=BeginPaint(hwnd,&ps);
                if(state.game)Paint(dc);
                EndPaint(hwnd,&ps);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_SIZE:
                state.canvas.reset();
                PlaceView();
                InvalidateRect(hwnd,nullptr,FALSE);
                return 0;
            case WM_CLOSE:
                Finish();
                return 0;
            case WM_DESTROY:
                state.window=nullptr;
                ReleaseView();
                state.canvas.reset();
                return 0;
            }
            return DefWindowProcW(hwnd,message,wParam,lParam);
        }

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

    void ShowMetroidWindow(HWND owner,const MetroidHero& her,std::function<void()> onEnd)
    {
        if(state.window)
        {
            ShowWindow(state.window,SW_SHOWNORMAL);
            SetForegroundWindow(state.window);
            return;
        }
        std::wstring error;
        if(!state.map.LoadBuiltIn(error))
        {
            Log(L"ruins: %s",error.c_str());
            if(onEnd)onEnd();
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
        state.game=std::make_unique<Metroid::Game>(state.map);
        state.paused=state.asking=false;
        state.keys=state.padHeld={};
        state.pad=-1;
        state.padButtons=0;
        state.padLookAt=0;
        state.script.clear();
        state.scripted=false;
        state.frames=state.jumps=state.landings=0;
        state.steepest=0;
        state.gait=Activity::Idle;
        state.drawnAngle=-DirectX::XM_PIDIV2+Toward;

        // Wide, as the rooms are: most of the screen.
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&info);
        const RECT work=info.rcWork;
        const int dpi=static_cast<int>(GetDpiForSystem());
        const int h=std::min(static_cast<int>(work.bottom-work.top)*9/10,MulDiv(760,dpi,96));
        const int w=std::min(static_cast<int>(work.right-work.left)*9/10,h*16/9);
        const std::wstring title=std::wstring(T(L"遺跡の探検",L"The ruins"))+L" - "+HerName();
        state.window=CreateWindowExW(0,ClassName,title.c_str(),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                                     work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,
                                     nullptr,nullptr,instance,nullptr);
        if(CreateView())
        {
            PlaceView();
            her.model->SetAutoGestures(false);
            her.model->EndGestures();
        }
        QueryPerformanceCounter(&state.last);
        SetTimer(state.window,TimerId,10,nullptr);
        ShowWindow(state.window,SW_SHOWNORMAL);
        SetForegroundWindow(state.window);
    }

    bool MetroidWindowOpen(){return state.window!=nullptr;}
    HWND MetroidWindowHandle(){return state.window;}
    bool MetroidWindowRect(RECT& r)
    {
        return state.window&&IsWindowVisible(state.window)&&!IsIconic(state.window)&&GetWindowRect(state.window,&r);
    }

    void TickMetroidWindow(MascotModel& model,double dt)
    {
        if(!state.window||!state.renderer||!state.game)return;
        using namespace DirectX;
        dt=std::min(dt,0.1);
        PollPad();
        if(!state.window||!state.renderer)return;   // left with the gamepad
        auto& r=*state.renderer;
        StepGame(dt);
        const auto& g=*state.game;
        const auto& b=g.Hero();
        const float T=state.T;
        const auto events=state.game->TakeEvents();
        if(events.jumped)++state.jumps;
        if(events.landed>0){++state.landings;state.steepest=std::max(state.steepest,events.landed);}
        if(const auto* room=HereRoom();room&&room->area!=state.builtArea)
        {
            state.builtArea=room->area;
            r.SetGeometry(BuildArea(room->area));
        }

        // Her motion: running on the ground (her steps timed to how fast she goes), in the air
        // (the pose from how fast she rises or falls), standing; a landing dips her knees.
        model.SetFacingWalk(true,1,g.Rule().run*T);
        const Activity want=!b.ground?Activity::Air:std::abs(b.vx)>0.6f?Activity::Run:Activity::Idle;
        if(want!=state.gait)
        {
            model.SetActivity(want,want==Activity::Run?1:0);
            state.gait=want;
        }
        if(events.landed>0)model.Land(events.landed*T*0.35f);
        // Falling with her, the hair and the skirt float a little (no more than weightless).
        model.SetCarryMotion(0,-b.vy*T,0,b.ground?0.0f:-0.8f*std::min(g.Rule().gravity*T,98.0f));
        // Turned the way she goes, a little toward the camera; turning round, she passes facing it.
        const float angle=b.facing>0?-XM_PIDIV2+Toward:XM_PIDIV2-Toward;
        state.drawnAngle+=(angle-state.drawnAngle)*static_cast<float>(1-std::exp(-dt/0.06));
        const XMMATRIX world=XMMatrixTranslation(-model.RootOffset(),0,0)*XMMatrixRotationY(state.drawnAngle)*XMMatrixTranslation(b.x*T,-b.y*T,0);
        r.SetModelTransform(world);

        // The camera: from the side, a little above, ahead of her the way she faces; it eases
        // after her (slower up and down), and stays within the room (unless it is smaller).
        RECT client{};
        GetClientRect(state.window,&client);
        const float aspect=static_cast<float>(std::max(1L,client.right))/static_cast<float>(std::max(1L,client.bottom));
        const float halfH=ViewTiles/2,halfW=halfH*aspect;
        float wantX=b.x+b.facing*1.5f,wantY=b.y-2.2f;
        if(const auto* room=HereRoom())
        {
            const float x0=room->x+halfW,x1=room->x+room->width-halfW,y0=room->y+halfH,y1=room->y+room->height-halfH;
            wantX=x0<x1?std::clamp(wantX,x0,x1):room->x+room->width/2.0f;
            wantY=y0<y1?std::clamp(wantY,y0,y1):room->y+room->height/2.0f;
        }
        if(!state.cameraPlaced){state.cameraX=wantX;state.cameraY=wantY;state.cameraPlaced=true;}
        state.cameraX+=(wantX-state.cameraX)*static_cast<float>(1-std::exp(-dt/0.22));
        state.cameraY+=(wantY-state.cameraY)*static_cast<float>(1-std::exp(-dt/0.32));
        const float distance=halfH*T/std::tan(Fov/2);
        const XMVECTOR target=XMVectorSet(state.cameraX*T,-state.cameraY*T,0,0);
        const XMVECTOR eye=XMVectorAdd(target,XMVectorSet(0,distance*std::sin(Pitch),-distance*std::cos(Pitch),0));
        r.SetCamera(eye,target,Fov);

        UpdateOverlay();
        r.Render(model,state.her.edgeScale);
        ++state.frames;
    }

    void PlayMetroidForTest(const std::wstring& keys,int room)
    {
        state.script.clear();
        size_t at=0;
        while(at<keys.size())
        {
            while(at<keys.size()&&keys[at]==L' ')++at;
            Metroid::Input in;
            bool any=false;
            for(;at<keys.size()&&iswalpha(keys[at]);++at)
            {
                any=true;
                switch(towupper(keys[at]))
                {
                case L'L':in.left=true;break;
                case L'R':in.right=true;break;
                case L'U':in.up=true;break;
                case L'D':in.down=true;break;
                case L'Z':in.jump=true;break;
                default:break;   // W: none held
                }
            }
            const size_t from=at;
            while(at<keys.size()&&(iswdigit(keys[at])||keys[at]==L'.'))++at;
            if(!any)break;
            state.script.push_back({in,from<at?_wtof(keys.substr(from,at-from).c_str()):0.5});
        }
        state.scriptAt=0;
        state.scriptLeft=state.script.empty()?0:state.script[0].second;
        state.scripted=true;
        state.paused=false;
        if(room>0&&state.game&&room<=static_cast<int>(state.map.Rooms().size()))
        {
            float x=0,y=0;
            if(StandIn(state.map.Rooms()[static_cast<size_t>(room-1)],x,y))state.game->Place(x,y);
            state.cameraPlaced=false;
        }
    }

    bool SaveMetroidView(const std::wstring& path)
    {
        if(state.renderer)return state.renderer->SaveLastFrame(path);
        return state.canvas&&SavePng(*state.canvas,path);
    }

    std::wstring DescribeMetroid()
    {
        if(!state.game)return L"";
        const auto& g=*state.game;
        const auto& b=g.Hero();
        std::wstring text;
        wchar_t line[400];
        const auto* room=HereRoom();
        int visited=0;
        for(bool v:g.Visited())visited+=v?1:0;
        swprintf_s(line,L"metroid: %s, room %d (%s), at %.2f,%.2f, speed %.2f,%.2f, %s, facing %d, motion %d, clock %.1f\n",
                   state.renderer?L"3D":L"2D",g.Room()+1,room?room->name[0].c_str():L"-",b.x,b.y,b.vx,b.vy,b.ground?L"on the ground":L"in the air",
                   b.facing,static_cast<int>(state.gait),g.Clock());
        text+=line;
        swprintf_s(line,L"metroid: tile %.2f, jumps %d, landings %d (fastest %.1f tiles/s), rooms visited %d of %zu, frames %d, keys %s\n",
                   state.T,state.jumps,state.landings,state.steepest,visited,state.map.Rooms().size(),state.frames,
                   state.scripted?(state.scriptAt>=state.script.size()?L"played":L"playing"):L"keyboard");
        text+=line;
        return text;
    }

    std::wstring CheckMetroidMap()
    {
        Metroid::Map map;
        std::wstring error;
        if(!map.LoadBuiltIn(error))return L"1 wrong\n"+error+L"\n";
        const std::wstring problems=map.Check();
        const size_t wrong=static_cast<size_t>(std::count(problems.begin(),problems.end(),L'\n'));
        std::wstring text=std::to_wstring(wrong)+L" wrong\n"+problems;
        for(const auto& a:map.Areas())
        {
            int rooms=0,screens=0;
            for(const auto& r:map.Rooms())if(r.area==a.number){++rooms;screens+=r.width/Metroid::ScreenWidth*(r.height/Metroid::ScreenHeight);}
            text+=L"area "+std::to_wstring(a.number)+L" "+a.name[0]+L": "+std::to_wstring(rooms)+L" rooms, "+std::to_wstring(screens)+L" screens\n";
        }
        return text;
    }
}
