#define NOMINMAX
#include "AirHockeyWindow.hpp"
#include "AirHockey.hpp"
#include "Log.hpp"
#include "MascotModel.hpp"
#include "MascotRenderer.hpp"
#include "Text.hpp"
#include "TextureLoader.hpp"
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>
#pragma comment(lib,"gdiplus.lib")

namespace MikuMikuDesktopPerch
{
    namespace
    {
        using namespace Gdiplus;
        using AirHockey::Vec;
        constexpr wchar_t ClassName[]=L"MikuMikuDesktopPerchAirHockey";
        constexpr wchar_t ViewClassName[]=L"MikuMikuDesktopPerchAirHockeyView";
        constexpr UINT_PTR TimerId=1;
        constexpr int HerHand=1;   // she holds her mallet in her right hand

        // Where the table is in the 2D window: its middle, and pixels per table unit (your end down).
        struct Layout
        {
            float cx=0,cy=0,ppu=1,s=1;
            PointF At(Vec p) const {return PointF(cx+p.x*ppu,cy-p.y*ppu);}
            Vec Table(float x,float y) const {return {(x-cx)/ppu,(cy-y)/ppu};}
        };

        struct State
        {
            HWND window=nullptr,view=nullptr;
            ULONG_PTR gdiplus=0;
            AirHockeyOpponent her;
            std::function<void()> onEnd;
            std::unique_ptr<AirHockey::Game> game;
            std::unique_ptr<AirHockey::Brain> brain,autoplay;   // hers; yours in a test
            std::unique_ptr<Bitmap> canvas,table;   // the frame drawn, and the table under it (drawn once per size)
            Layout layout;
            LARGE_INTEGER last{};
            bool paused=false,asking=false,overTold=false;
            bool held=false;   // the cursor is kept over your half (ClipCursor)
            ULONGLONG speedShownUntil=0;   // GetTickCount64: until then, the speed just chosen is shown
            int frames=0;   // drawn, for the snapshot's report
            Vec target{};

            // The 3D table (none: the 2D one). World units: the table's top at y = top, its middle
            // over (0, 0); table x along world x, table y (toward her) along world z, k world
            // units to a table unit. She stands at z = herZ facing -z.
            std::unique_ptr<MascotRenderer> renderer;
            float upper=10,hips=10,k=10,top=8,herZ=12;
            float herX=0;   // she moves a little to the side with her mallet (world units)
            bool sliding=false;   // on her way there
            // Where her hand can take her mallet (table units): across, and the nearest to the middle.
            float reachLeft=-0.5f,reachRight=0.5f,reachNear=0;
            // The wrist's target over where the grip is to be, learned as she holds (model units).
            std::array<float,3> holdOffset{};
            double holding=0;          // seconds she has held it (her hand goes out first)
            // Where she means her mallet to go (the game's player for her, kept where it was
            // until that moves enough), and where her hand is taking it now as an arm would
            // (speeding up and slowing down; still when it is there). Table units.
            Vec goal{},intent{},intentVelocity{};
            // Diagnostics: how much her grip shakes (its moves' changes from frame to frame).
            std::array<std::array<float,3>,2> gripBefore{};
            double shake=0;
            int shakeFrames=0,stillFrames=0;
            // The camera: round where it looks (yaw, pitch in radians), how far (her upper body's
            // lengths), and where it looks from the default.
            struct Camera {float yaw=0,pitch=0.62f,distance=5.6f,x=0,y=0,z=0;} camera;
            POINT dragFrom{};
            int dragging=0;            // 0, or the button held: 1 turns, 2 moves
            std::wstring hudShown;     // what the picture over the table last showed
            DirectX::XMFLOAT3 eye{},look{};   // the camera as last set
        } state;

        std::uint32_t Seed()
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            return static_cast<std::uint32_t>(now.QuadPart)^0x9E3779B9u;
        }

        std::wstring HerName(){return state.her.name.empty()?std::wstring(T(L"相手",L"Her")):state.her.name;}

        // The puck's top speed chosen (1-4, AirHockey::PuckSpeeds), in words.
        std::wstring SpeedName(int level)
        {
            switch(level)
            {
            case 1:return T(L"ゆっくり",L"slow");
            case 2:return T(L"ふつう",L"normal");
            case 3:return T(L"はやい",L"fast");
            default:return T(L"とてもはやい",L"very fast");
            }
        }
        int SpeedLevel(const AirHockey::Game& game)
        {
            int best=1;
            for(int i=0;i<4;++i)if(std::abs(AirHockey::PuckSpeeds[i]-game.Rule().puckSpeed)<std::abs(AirHockey::PuckSpeeds[best-1]-game.Rule().puckSpeed))best=i+1;
            return best;
        }

        const FontFamily* Family()
        {
            static FontFamily gothic(L"Yu Gothic UI");
            return gothic.IsAvailable()?&gothic:FontFamily::GenericSansSerif();
        }

        void Text(Graphics& g,const std::wstring& text,const RectF& box,float size,Color color,bool bold=false,
                  StringAlignment across=StringAlignmentCenter,StringAlignment down=StringAlignmentCenter)
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

        void Disc(Graphics& g,PointF at,float radius,const Brush& brush)
        {
            g.FillEllipse(&brush,at.X-radius,at.Y-radius,radius*2,radius*2);
        }

        // The table in a w x h picture (s: the screen's scale): its size, and where it is.
        Layout Place(int w,int h,float s,const AirHockey::Rules& rules)
        {
            Layout l;
            l.s=s;
            const float top=58*s,margin=18*s,rim=0.07f;
            l.ppu=std::min((w-2*margin)/(rules.width+2*rim),(h-top-margin)/(rules.length+2*rim));
            l.cx=w/2.0f;
            l.cy=top+(h-top-margin)/2.0f;
            return l;
        }

        // What does not move: the room's floor, the table's rim and white top, its lines, the
        // goals' slots and the air holes.
        void DrawTable(Graphics& g,int w,int h,const Layout& l,const AirHockey::Rules& rules)
        {
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.Clear(Color(255,38,44,58));
            const float W=rules.width/2,L=rules.length/2,rim=0.07f*l.ppu,u=l.ppu;
            const PointF corner=l.At({-W,L});
            const RectF top(corner.X,corner.Y,rules.width*u,rules.length*u);
            {
                GraphicsPath frame;
                RoundRect(frame,RectF(top.X-rim,top.Y-rim,top.Width+2*rim,top.Height+2*rim),rim*1.2f);
                SolidBrush wood(Color(255,58,92,150));
                g.FillPath(&wood,&frame);
                Pen edge(Color(255,120,160,214),1.5f*l.s);
                g.DrawPath(&edge,&frame);
            }
            LinearGradientBrush surface(top,Color(255,236,243,250),Color(255,214,228,242),LinearGradientModeVertical);
            g.FillRectangle(&surface,top);
            // The air holes, faint.
            SolidBrush hole(Color(60,90,120,160));
            const float step=0.05f;
            for(float y=-L+step/2;y<L;y+=step)
                for(float x=-W+step/2;x<W;x+=step)Disc(g,l.At({x,y}),std::max(0.8f,0.004f*u),hole);
            // The centre line and circle, the goals' creases.
            Pen red(Color(200,214,70,84),std::max(2.0f,0.012f*u)),blue(Color(170,70,110,200),std::max(1.5f,0.008f*u));
            g.DrawLine(&red,l.At({-W,0}),l.At({W,0}));
            const float circle=0.16f*u;
            const PointF mid=l.At({0,0});
            g.DrawEllipse(&blue,mid.X-circle,mid.Y-circle,circle*2,circle*2);
            for(float s:{-1.0f,1.0f})
            {
                const PointF g0=l.At({0,s*L});
                const float crease=rules.goal*0.75f*u;
                g.DrawArc(&blue,g0.X-crease,g0.Y-crease,crease*2,crease*2,s>0?0.0f:180.0f,180.0f);
                // The goal: a dark slot in the rim.
                SolidBrush slot(Color(255,18,20,28));
                const PointF a=l.At({-rules.goal/2,s*L});
                g.FillRectangle(&slot,a.X,s>0?a.Y-rim:a.Y,rules.goal*u,rim);
            }
        }

        void DrawMallet(Graphics& g,PointF at,float radius,Color body,Color ring)
        {
            SolidBrush shadow(Color(60,0,0,0));
            Disc(g,PointF(at.X+radius*0.12f,at.Y+radius*0.16f),radius,shadow);
            SolidBrush outer(ring),inner(body);
            Disc(g,at,radius,outer);
            Disc(g,at,radius*0.78f,inner);
            SolidBrush knob(ring);
            Disc(g,at,radius*0.42f,knob);
            SolidBrush shine(Color(110,255,255,255));
            Disc(g,PointF(at.X-radius*0.15f,at.Y-radius*0.15f),radius*0.16f,shine);
        }

        // The words over the table: the score, and a banner in the middle (at height mid) for
        // what is going on.
        void DrawHud(Graphics& g,int w,float mid,float s,const AirHockey::Game& game,bool paused,bool asking,bool live,bool speedChosen)
        {
            using namespace AirHockey;
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAlias);
            const auto& rules=game.Rule();
            // The score: hers on the left, yours on the right.
            const Color white(255,240,242,248),soft(255,170,184,208),gold(255,246,210,120);
            if(state.renderer)
            {
                GraphicsPath path;
                RoundRect(path,RectF(8*s,4*s,static_cast<float>(w)-16*s,52*s),10*s);
                SolidBrush back(Color(150,14,20,34));
                g.FillPath(&back,&path);
            }
            Text(g,HerName(),RectF(16*s,8*s,w/2.0f-60*s,24*s),15*s,soft,false,StringAlignmentNear);
            Text(g,T(L"あなた",L"You"),RectF(w/2.0f+44*s,8*s,w/2.0f-60*s,24*s),15*s,soft,false,StringAlignmentFar);
            if(live)Text(g,T(L"パック：",L"Puck: ")+SpeedName(SpeedLevel(game))+T(L"（1〜4キー）",L" (keys 1-4)"),
                         RectF(w/2.0f+44*s,30*s,w/2.0f-60*s,22*s),12*s,soft,false,StringAlignmentFar);
            Text(g,std::to_wstring(game.Score(Her))+L"  -  "+std::to_wstring(game.Score(You)),RectF(0,4*s,static_cast<float>(w),46*s),30*s,white,true);
            Text(g,T(L"先に",L"First to ")+std::to_wstring(rules.toWin)+T(L"点",L""),RectF(16*s,30*s,w/2.0f-60*s,22*s),12*s,soft,false,StringAlignmentNear);

            auto banner=[&](const std::wstring& big,const std::wstring& note){
                const float bw=std::min(static_cast<float>(w)-40*s,420*s),bh=note.empty()?64*s:96*s;
                const RectF box(w/2.0f-bw/2,mid-bh/2,bw,bh);
                GraphicsPath path;
                RoundRect(path,box,12*s);
                SolidBrush back(Color(215,16,22,36));
                g.FillPath(&back,&path);
                Text(g,big,RectF(box.X,box.Y+8*s,box.Width,note.empty()?box.Height-16*s:48*s),26*s,gold,true);
                if(!note.empty())Text(g,note,RectF(box.X,box.Y+56*s,box.Width,30*s),14*s,white);
            };
            if(game.Now()==Phase::Over)
            {
                std::wstring note=std::to_wstring(game.Score(Her))+L" - "+std::to_wstring(game.Score(You));
                note+=T(L"　いちばん長い打ち合い ",L"   longest rally ")+std::to_wstring(game.LongestRally());
                if(live)note+=T(L"　Enterでもう一度・Escで閉じる",L"   Enter: again, Esc: close");
                banner(game.Winner()==You?T(L"あなたの勝ち！",L"You win!"):HerName()+T(L"の勝ち",L" wins"),note);
            }
            else if(asking)banner(T(L"やめますか？",L"Stop playing?"),T(L"Y：やめる　N：続ける",L"Y: stop   N: go on"));
            else if(speedChosen)banner(T(L"パックの速さ：",L"Puck speed: ")+SpeedName(SpeedLevel(game)),L"");
            else if(paused)banner(T(L"一時停止",L"Paused"),live?T(L"Spaceで再開",L"Space to go on"):L"");
            else if(game.Now()==Phase::Serve&&game.GoalServe()&&game.ServeLeft()>0.3)
                banner(game.LastScorer()==You?T(L"ゴール！",L"Goal!"):HerName()+T(L"のゴール",L" scores"),L"");
        }

        // The moving things on the 2D table, and the words over it.
        void DrawPlay(Graphics& g,int w,const Layout& l,const AirHockey::Game& game,bool paused,bool asking,bool live,bool speedChosen=false)
        {
            using namespace AirHockey;
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            const auto& rules=game.Rule();
            const float u=l.ppu;
            DrawMallet(g,l.At(game.Mallet(Her)),rules.mallet[Her]*u,Color(255,236,96,134),Color(255,190,50,92));
            {
                const PointF p=l.At(game.Puck());
                const float r=rules.puck*u;
                SolidBrush shadow(Color(70,0,0,0)),body(Color(255,30,32,40)),rim(Color(255,224,180,60));
                Disc(g,PointF(p.X+r*0.15f,p.Y+r*0.2f),r,shadow);
                Disc(g,p,r,rim);
                Disc(g,p,r*0.8f,body);
            }
            DrawMallet(g,l.At(game.Mallet(You)),rules.mallet[You]*u,Color(255,90,170,236),Color(255,40,110,190));
            DrawHud(g,w,l.cy,l.s,game,paused,asking,live,speedChosen);
        }

        float Scale(){return state.window?static_cast<float>(GetDpiForWindow(state.window))/96.0f:1;}

        // ---- The 3D table ----------------------------------------------------------------

        // Plain shapes for the renderer's geometry (no wood grain, no texture).
        struct Mesh
        {
            std::vector<MascotRenderer::GeometryVertex> out;
            using V=std::array<float,3>;
            void Quad(const V& a,const V& b,const V& c,const V& d,const V& n,const float (&col)[4])
            {
                const V* p[6]={&a,&b,&c,&a,&c,&d};
                for(const V* q:p)
                {
                    MascotRenderer::GeometryVertex v{{(*q)[0],(*q)[1],(*q)[2]},{n[0],n[1],n[2]},{col[0],col[1],col[2],col[3]}};
                    out.push_back(v);
                }
            }
            void Box(float x0,float y0,float z0,float x1,float y1,float z1,const float (&c)[4])
            {
                Quad({x0,y1,z0},{x1,y1,z0},{x1,y1,z1},{x0,y1,z1},{0,1,0},c);
                Quad({x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},{0,0,-1},c);
                Quad({x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1},{0,0,1},c);
                Quad({x0,y0,z0},{x0,y0,z1},{x0,y1,z1},{x0,y1,z0},{-1,0,0},c);
                Quad({x1,y0,z0},{x1,y0,z1},{x1,y1,z1},{x1,y1,z0},{1,0,0},c);
            }
            // An upright cylinder (its top and side) round (x, z).
            void Cylinder(float x,float z,float y0,float y1,float r,const float (&c)[4],int sides=24)
            {
                for(int i=0;i<sides;++i)
                {
                    const float a0=DirectX::XM_2PI*i/sides,a1=DirectX::XM_2PI*(i+1)/sides,am=(a0+a1)/2;
                    const V p0{x+r*std::cos(a0),y1,z+r*std::sin(a0)},p1{x+r*std::cos(a1),y1,z+r*std::sin(a1)};
                    Quad({x,y1,z},p1,p0,{x,y1,z},{0,1,0},c);
                    Quad({p0[0],y0,p0[2]},{p1[0],y0,p1[2]},p1,p0,{std::cos(am),0,std::sin(am)},c);
                }
            }
            // A flat ring on the table's top (its lines), from radius r0 to r1 round (x, z), the
            // part of it between angles a0 and a1.
            void Ring(float x,float z,float y,float r0,float r1,float a0,float a1,const float (&c)[4],int sides=48)
            {
                for(int i=0;i<sides;++i)
                {
                    const float b0=a0+(a1-a0)*i/sides,b1=a0+(a1-a0)*(i+1)/sides;
                    Quad({x+r0*std::cos(b0),y,z+r0*std::sin(b0)},{x+r0*std::cos(b1),y,z+r0*std::sin(b1)},
                         {x+r1*std::cos(b1),y,z+r1*std::sin(b1)},{x+r1*std::cos(b0),y,z+r1*std::sin(b0)},{0,1,0},c);
                }
            }
        };

        using DirectX::XMFLOAT3;
        AirHockey::Game& game(){return *state.game;}

        // The table's rim (table units round the top) and its height above the top.
        constexpr float Rim=0.07f,RimHigh=0.035f;

        // What does not move: the room, the table (its legs, its rim with the goals' slots, its
        // white top with the lines).
        std::vector<MascotRenderer::GeometryVertex> BuildRoom()
        {
            const auto& rules=state.game->Rule();
            const float k=state.k,top=state.top,W=rules.width/2*k,L=rules.length/2*k,rim=Rim*k,high=RimHigh*k,mouth=rules.goal/2*k;
            const float D=state.upper;
            Mesh m;
            const float floor[4]={0.50f,0.42f,0.34f,1},wall[4]={0.80f,0.82f,0.86f,1},skirting[4]={0.42f,0.36f,0.30f,1};
            const float white[4]={0.93f,0.95f,0.98f,1},frame[4]={0.20f,0.33f,0.56f,1},dark[4]={0.06f,0.07f,0.09f,1},legs[4]={0.16f,0.17f,0.20f,1};
            const float red[4]={0.85f,0.25f,0.30f,1},blue[4]={0.32f,0.46f,0.80f,1};
            // The room: a floor, walls round, big enough for the camera to turn in.
            const float rx=7*D,rz=8*D,ceiling=state.hips+2.2f*D;
            m.Quad({-rx,0,rz},{rx,0,rz},{rx,0,-rz},{-rx,0,-rz},{0,1,0},floor);
            m.Quad({-rx,0,rz},{-rx,ceiling,rz},{rx,ceiling,rz},{rx,0,rz},{0,0,-1},wall);
            m.Quad({-rx,0,-rz},{rx,0,-rz},{rx,ceiling,-rz},{-rx,ceiling,-rz},{0,0,1},wall);
            m.Quad({-rx,0,-rz},{-rx,ceiling,-rz},{-rx,ceiling,rz},{-rx,0,rz},{1,0,0},wall);
            m.Quad({rx,0,-rz},{rx,0,rz},{rx,ceiling,rz},{rx,ceiling,-rz},{-1,0,0},wall);
            m.Box(-rx,0,rz-0.05f*D,rx,0.25f*D,rz,skirting);
            m.Box(-rx,0,-rz,rx,0.25f*D,-rz+0.05f*D,skirting);
            // The table: a body under the top, legs to the floor.
            const float body=top-0.22f*D;
            m.Box(-W-rim,body,-L-rim,W+rim,top-0.01f*k,L+rim,frame);
            const float leg=0.10f*D;
            for(float x:{-W-rim+0.03f*D,W+rim-0.03f*D-leg})
                for(float z:{-L-rim+0.03f*D,L+rim-0.03f*D-leg})m.Box(x,0,z,x+leg,body,z+leg,legs);
            // The top, and the rim round it (cut at the goals' mouths, a dark slot below each).
            m.Box(-W,top-0.01f*k,-L,W,top,L,white);
            m.Box(-W-rim,top-0.01f*k,-L,-W,top+high,L,frame);
            m.Box(W,top-0.01f*k,-L,W+rim,top+high,L,frame);
            for(float s:{-1.0f,1.0f})
            {
                const float z0=s>0?L:-L-rim,z1=s>0?L+rim:-L;
                m.Box(-W-rim,top-0.01f*k,z0,-mouth,top+high,z1,frame);
                m.Box(mouth,top-0.01f*k,z0,W+rim,top+high,z1,frame);
                m.Box(-mouth,top-0.05f*k,z0,mouth,top-0.004f*k,z1,dark);
            }
            // The lines: the middle one, the middle circle, the goals' creases.
            const float y=top+0.0015f*k,line=0.006f*k;
            m.Box(-W,top,-line,W,y,line,red);
            m.Ring(0,0,y,0.16f*k-line,0.16f*k+line,0,DirectX::XM_2PI,blue);
            const float crease=rules.goal*0.75f*k;
            m.Ring(0,L,y,crease-line,crease+line,DirectX::XM_PI,DirectX::XM_2PI,blue);
            m.Ring(0,-L,y,crease-line,crease+line,0,DirectX::XM_PI,blue);
            return std::move(m.out);
        }

        XMFLOAT3 World(Vec p,float above=0){return {p.x*state.k,state.top+above,p.y*state.k};}

        // The mallet's height (table units): its base, and the knob on it up to its top.
        float MalletBase(int side){return 0.35f*state.game->Rule().mallet[side];}
        float MalletTop(int side){return 2.4f*state.game->Rule().mallet[side];}
        // Where her grip's middle goes over her mallet (table units above the top): the knob's top.
        float GripHeight(){return MalletTop(AirHockey::Her);}

        // What moves: the puck and the two mallets.
        std::vector<MascotRenderer::GeometryVertex> BuildPieces()
        {
            using namespace AirHockey;
            const auto& rules=state.game->Rule();
            const float k=state.k,top=state.top;
            Mesh m;
            const float puck[4]={0.10f,0.10f,0.12f,1},puckRim[4]={0.86f,0.68f,0.22f,1};
            const float colors[2][2][4]={{{0.24f,0.56f,0.90f,1},{0.13f,0.36f,0.70f,1}},{{0.92f,0.36f,0.52f,1},{0.72f,0.18f,0.34f,1}}};
            {
                const XMFLOAT3 p=World(game().Puck());
                const float r=rules.puck*k,h=0.25f*rules.puck*k;
                m.Cylinder(p.x,p.z,top,top+h,r,puckRim);
                m.Cylinder(p.x,p.z,top+h,top+h*1.02f,r*0.8f,puck);
            }
            for(int side:{You,Her})
            {
                const XMFLOAT3 p=World(game().Mallet(side));
                const float R=rules.mallet[side]*k;
                m.Cylinder(p.x,p.z,top,top+MalletBase(side)*k,R,colors[side][0]);
                m.Cylinder(p.x,p.z,top+MalletBase(side)*k,top+MalletBase(side)*k*1.15f,R*0.78f,colors[side][1]);
                m.Cylinder(p.x,p.z,top+MalletBase(side)*k,top+MalletTop(side)*k,R*0.42f,colors[side][1],16);
                m.Cylinder(p.x,p.z,top+MalletTop(side)*k,top+MalletTop(side)*k*1.04f,R*0.5f,colors[side][0],16);
            }
            return std::move(m.out);
        }

        // Her model's transform: standing at her end, facing you, her walks' sideways shift taken out.
        DirectX::XMMATRIX HerWorld(MascotModel& model){return DirectX::XMMatrixTranslation(state.herX-model.RootOffset(),0,state.herZ);}

        // The camera: from behind your end, a little above, looking down the table at her.
        void PlaceCamera()
        {
            using namespace DirectX;
            const auto& c=state.camera;
            const float D=state.upper;
            const XMVECTOR target=XMVectorSet(c.x*D,state.top+(0.35f+c.y)*D,(0.30f+c.z)*D,0);
            const XMVECTOR toEye=XMVectorSet(std::sin(c.yaw)*std::cos(c.pitch),std::sin(c.pitch),-std::cos(c.yaw)*std::cos(c.pitch),0);
            const XMVECTOR eye=XMVectorAdd(target,XMVectorScale(toEye,c.distance*D));
            XMStoreFloat3(&state.eye,eye);
            XMStoreFloat3(&state.look,target);
            state.renderer->SetCamera(eye,target,XMConvertToRadians(42.0f));
        }

        // The point on the table's top under a pixel of the view (false: it looks above it).
        bool TableAt(float px,float py,Vec& at)
        {
            using namespace DirectX;
            RECT client{};
            GetClientRect(state.view,&client);
            const float w=static_cast<float>(std::max(1L,client.right)),h=static_cast<float>(std::max(1L,client.bottom));
            const XMVECTOR eye=XMLoadFloat3(&state.eye),forward=XMVector3Normalize(XMVectorSubtract(XMLoadFloat3(&state.look),eye));
            const XMVECTOR right=XMVector3Normalize(XMVector3Cross(XMVectorSet(0,1,0,0),forward)),up=XMVector3Cross(forward,right);
            const float t=std::tan(XMConvertToRadians(42.0f)/2);
            const XMVECTOR ray=XMVectorAdd(forward,XMVectorAdd(XMVectorScale(right,(2*px/w-1)*t*w/h),XMVectorScale(up,(1-2*py/h)*t)));
            const float dy=XMVectorGetY(ray);
            if(dy>=-1e-4f)return false;
            const float along=(state.top-XMVectorGetY(eye))/dy;
            const XMVECTOR hit=XMVectorAdd(eye,XMVectorScale(ray,along));
            at={XMVectorGetX(hit)/state.k,XMVectorGetZ(hit)/state.k};
            return true;
        }

        // The table as solids in her way (MascotModel::SetSolids): its body up to the top, in her
        // model's space (she is only moved, not turned). The low rim is left out: her forearm
        // passes over it as she reaches.
        void SetTableSolids(MascotModel& model)
        {
            const auto& rules=game().Rule();
            const float k=state.k,W=rules.width/2*k,L=rules.length/2*k,rim=Rim*k,high=state.top+RimHigh*k,body=state.top-0.22f*state.upper;
            const float dx=model.RootOffset()-state.herX,dz=-state.herZ;
            auto box=[&](float x0,float y0,float z0,float x1,float y1,float z1){
                MascotModel::Solid s;
                s.middle={(x0+x1)/2+dx,(y0+y1)/2,(z0+z1)/2+dz};
                s.half={(x1-x0)/2,(y1-y0)/2,(z1-z0)/2};
                return s;
            };
            (void)high;
            model.SetSolids({box(-W-rim,body,-L-rim,W+rim,state.top,L+rim)});
        }

        // Where her hand is to be for the mallet at p (model space): the grip's middle over the
        // knob, the wrist where that puts it (holdOffset, learned).
        std::array<float,3> ModelPoint(MascotModel& model,const XMFLOAT3& p)
        {
            return {p.x+model.RootOffset()-state.herX,p.y,p.z-state.herZ};
        }

        // Her measures and reach, once, as the 3D table opens: the table made to her size; her
        // hand held out ahead, to her left and to her right at the knob's height to find how far
        // her mallet can go (her physics then let go).
        void MeasureHer(MascotModel& model)
        {
            std::array<float,3> lo{},hi{},legLo{},legHi{},head{};
            if(model.BoneBounds(lo,hi)&&model.LegBounds(legLo,legHi)&&model.HeadPosition(head)&&head[1]>legHi[1])
            {
                state.hips=std::max(legHi[1]-lo[1],0.1f);
                state.upper=head[1]-legHi[1];
            }
            const auto& rules=game().Rule();
            const float D=state.upper;
            // As a full-size table to her (a little bigger than a table-tennis table: some 2.7 m
            // long and 1.5 m wide to a person 1.6 m tall): wide enough for two to stand at one end.
            state.k=2.0f*D;
            state.herX=0;
            state.top=0.9f*state.hips;
            // Back from her end by her thighs' depth, and a little more (her front above them
            // leans over the table).
            state.herZ=(rules.length/2+Rim)*state.k+0.34f*D;
            model.SetActivity(Activity::Idle);
            model.SetAutoGestures(false);
            auto reach=[&](Vec toward){
                std::array<float,3> wrist=ModelPoint(model,World(toward,GripHeight()*state.k+0.06f*D));
                std::array<float,3> middle{},along{};
                for(int i=0;i<45;++i){model.Hold(HerHand,wrist,true);model.Update(1/30.0);}
                if(!model.Grip(HerHand,middle,along))return toward;
                return Vec{(middle[0]-model.RootOffset()+state.herX)/state.k,(middle[2]+state.herZ)/state.k};
            };
            const float L=rules.length/2;
            const Vec ahead=reach({-0.08f,0}),left=reach({-rules.width/2,L*0.55f}),right=reach({rules.width/2,L*0.55f});
            state.reachNear=std::clamp(ahead.y,0.0f,L*0.8f);
            state.reachLeft=std::max(-rules.width/2,left.x);
            state.reachRight=std::min(rules.width/2,right.x);
            for(int i=0;i<15;++i){model.Hold(HerHand,{},false);model.Update(1/30.0);}
            model.ResetPhysics();
            Log(L"air hockey: hips %.1f, upper body %.1f, table %.1f long at %.1f; her mallet reaches %.2f..%.2f across, to %.2f",
                state.hips,state.upper,rules.length*state.k,state.top,state.reachLeft,state.reachRight,state.reachNear);
        }

        bool Playing(){return state.game&&state.game->Now()!=AirHockey::Phase::Over&&!state.paused&&!state.asking;}

        LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam);

        LRESULT CALLBACK ViewProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            // The camera: drag with the right button to turn it around the table, with the
            // middle one (or Shift and the right) to move it, the wheel to come nearer; a right
            // double-click or F1 puts it back. The left button never moves it.
            auto& c=state.camera;
            switch(message)
            {
            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
                SetFocus(state.window);
                return Proc(state.window,WM_LBUTTONDOWN,wParam,lParam);
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
                SetFocus(state.window);
                state.dragging=message==WM_MBUTTONDOWN||(wParam&MK_SHIFT)?2:1;
                state.dragFrom={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
                SetCapture(hwnd);
                return 0;
            case WM_MOUSEMOVE:
                if(state.dragging)
                {
                    const float dx=static_cast<float>(GET_X_LPARAM(lParam)-state.dragFrom.x),dy=static_cast<float>(GET_Y_LPARAM(lParam)-state.dragFrom.y);
                    state.dragFrom={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
                    const float k=1/Scale();
                    if(state.dragging==1)
                    {
                        c.yaw-=dx*k*0.008f;
                        c.pitch=std::clamp(c.pitch+dy*k*0.006f,0.05f,1.5f);
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
                c.distance=std::clamp(c.distance*std::pow(0.9f,GET_WHEEL_DELTA_WPARAM(wParam)/120.0f),0.8f,6.0f);   // inside the room
                return 0;
            case WM_RBUTTONDBLCLK:
                c={};
                return 0;
            case WM_SETCURSOR:
                // Over the table while playing, the mallet is the cursor.
                if(LOWORD(lParam)==HTCLIENT&&Playing()&&!state.dragging){SetCursor(nullptr);return TRUE;}
                break;
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
                Log(L"air hockey: resize: %s",error.c_str());
            state.hudShown.clear();   // the picture over the table, at the new size
        }

        void ReleaseView()
        {
            if(state.renderer){state.renderer->WaitIdle();state.renderer.reset();Log(L"air hockey: 3D view released");}
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
            renderer->SetBackground(0.80f,0.82f,0.86f,1);
            std::wstring error;
            if(!renderer->Initialize(state.view,w,h,state.her.light?1:state.her.msaa,false,error)||
               !renderer->LoadModel(*state.her.model,state.her.toonDir,state.her.light?512:state.her.maxTexture,error))
            {
                Log(L"air hockey: no 3D view (%s)",error.c_str());
                renderer->WaitIdle();
                DestroyWindow(state.view);
                state.view=nullptr;
                return false;
            }
            renderer->SetLight(20,45);
            MeasureHer(*state.her.model);
            state.renderer=std::move(renderer);
            state.renderer->SetGeometry(BuildRoom());
            state.camera={};
            state.dragging=0;
            state.holding=0;
            state.holdOffset={0,0.04f*state.upper,0.08f*state.upper};
            state.goal=state.intent=state.game->Mallet(AirHockey::Her);
            state.intentVelocity={};
            state.sliding=false;
            state.hudShown.clear();
            Log(L"air hockey: 3D view %ux%u on %s",w,h,state.renderer->AdapterName().c_str());
            return true;
        }

        void Paint(HDC dc)
        {
            RECT client{};
            GetClientRect(state.window,&client);
            const int w=std::max(1L,client.right),h=std::max(1L,client.bottom);
            const float s=Scale();
            const auto& rules=state.game->Rule();
            if(state.renderer)return;   // the 3D view covers the window (its words drawn over it)
            if(!state.canvas||static_cast<int>(state.canvas->GetWidth())!=w||static_cast<int>(state.canvas->GetHeight())!=h)
            {
                state.canvas=std::make_unique<Bitmap>(w,h,PixelFormat32bppPARGB);
                state.table=std::make_unique<Bitmap>(w,h,PixelFormat32bppPARGB);
                state.layout=Place(w,h,s,rules);
                Graphics t(state.table.get());
                DrawTable(t,w,h,state.layout,rules);
            }
            Graphics g(state.canvas.get());
            g.SetCompositingMode(CompositingModeSourceCopy);
            g.DrawImage(state.table.get(),0,0);
            g.SetCompositingMode(CompositingModeSourceOver);
            DrawPlay(g,w,state.layout,*state.game,state.paused,state.asking,true,GetTickCount64()<state.speedShownUntil);
            Graphics screen(dc);
            screen.DrawImage(state.canvas.get(),0,0);
            ++state.frames;
        }

        // The words over the 3D table, drawn again only when they change.
        void UpdateOverlay()
        {
            RECT client{};
            GetClientRect(state.window,&client);
            const int w=std::max(1L,client.right),h=std::max(1L,client.bottom);
            const auto& g=game();
            const bool speedChosen=GetTickCount64()<state.speedShownUntil;
            const bool goal=g.Now()==AirHockey::Phase::Serve&&g.GoalServe()&&g.ServeLeft()>0.3;
            const std::wstring shown=std::to_wstring(w)+L"x"+std::to_wstring(h)+L" "+std::to_wstring(g.Score(0))+L"-"+std::to_wstring(g.Score(1))+
                                     L" "+std::to_wstring(static_cast<int>(g.Now()))+(state.paused?L"p":L"")+(state.asking?L"a":L"")+(speedChosen?L"s":L"")+
                                     (goal?L"g":L"")+std::to_wstring(SpeedLevel(g));
            if(shown==state.hudShown)return;
            state.hudShown=shown;
            Bitmap canvas(w,h,PixelFormat32bppPARGB);
            {
                Graphics gr(&canvas);
                gr.Clear(Color(0,0,0,0));
                DrawHud(gr,w,h*0.42f,Scale(),g,state.paused,state.asking,true,speedChosen);
            }
            // Laid over the table: premultiplied BGRA to RGBA.
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

        void NewGame()
        {
            state.game=std::make_unique<AirHockey::Game>();
            state.game->SetPuckSpeed(AirHockey::PuckSpeeds[std::clamp(state.her.puckSpeed,1,4)-1]);
            state.speedShownUntil=0;
            AirHockey::Style manner;
            if(state.renderer)manner.reach=std::clamp(1-state.reachNear/(state.game->Rule().length/2),0.2f,1.0f);
            state.brain=std::make_unique<AirHockey::Brain>(AirHockey::Her,manner,Seed());
            state.paused=state.asking=state.overTold=false;
            state.target=state.game->Mallet(AirHockey::You);
            state.goal=state.intent=state.game->Mallet(AirHockey::Her);
            state.intentVelocity={};
            state.hudShown.clear();
            QueryPerformanceCounter(&state.last);
        }

        // While you play, the cursor stays over your half (where your mallet's middle can go, or
        // round it as the camera sees it), so the mallet follows it at once however far the mouse
        // moves. Let go of whenever play stops (paused, asked, over), the camera is dragged, or
        // the window is left, resized or closed.
        bool CursorArea(RECT& area)
        {
            if(!state.window||!state.game)return false;
            const auto& rules=state.game->Rule();
            const float R=rules.mallet[AirHockey::You],W=rules.width/2-R,L=rules.length/2-R;
            POINT lo{},hi{};
            if(state.renderer)
            {
                LONG l=LONG_MAX,t=LONG_MAX,r=LONG_MIN,b=LONG_MIN;
                for(Vec corner:{Vec{-W,-R},Vec{W,-R},Vec{-W,-L},Vec{W,-L}})
                {
                    const XMFLOAT3 p=World(corner);
                    const POINT q=state.renderer->Project({p.x,p.y,p.z});
                    l=std::min(l,q.x);t=std::min(t,q.y);r=std::max(r,q.x);b=std::max(b,q.y);
                }
                RECT client{};
                GetClientRect(state.window,&client);
                lo={std::clamp(l,0L,client.right-1),std::clamp(t,0L,client.bottom-1)};
                hi={std::clamp(r,0L,client.right-1),std::clamp(b,0L,client.bottom-1)};
            }
            else
            {
                if(!state.canvas)return false;
                const PointF a=state.layout.At({-W,-R}),b=state.layout.At({W,-L});
                lo={static_cast<LONG>(std::ceil(a.X)),static_cast<LONG>(std::ceil(a.Y))};
                hi={static_cast<LONG>(std::floor(b.X)),static_cast<LONG>(std::floor(b.Y))};
            }
            ClientToScreen(state.window,&lo);
            ClientToScreen(state.window,&hi);
            area={lo.x,lo.y,hi.x+1,hi.y+1};
            return true;
        }
        void HoldCursor(bool on)
        {
            RECT area{};
            if(on&&CursorArea(area))
            {
                RECT now{};
                if(!state.held||!GetClipCursor(&now)||!EqualRect(&now,&area))ClipCursor(&area);
                state.held=true;
            }
            else if(state.held)
            {
                ClipCursor(nullptr);
                state.held=false;
            }
        }

        void Finish()
        {
            if(!state.window)return;
            HoldCursor(false);
            KillTimer(state.window,TimerId);
            if(MascotModel* model=state.her.model)
            {
                model->Hold(HerHand,{},false);
                model->SetSolids({});
                model->SetAutoGestures(true);
            }
            auto onEnd=std::move(state.onEnd);
            DestroyWindow(state.window);
            if(onEnd)onEnd();
        }

        // The game on by dt: your mallet toward the mouse, hers as she means to move it (where
        // her hand can take it).
        void StepGame(double dt)
        {
            if(GetForegroundWindow()!=state.window&&Playing()&&!state.autoplay)state.paused=true;   // away from the window: it waits
            HoldCursor(Playing()&&!state.autoplay&&!state.dragging&&GetForegroundWindow()==state.window&&!IsIconic(state.window));
            if(!Playing()&&state.game->Now()!=AirHockey::Phase::Over)return;
            // Your mallet goes where the mouse is over the table (kept on your half by the game).
            POINT cursor;
            if(state.autoplay)state.target=state.autoplay->Think(*state.game,dt);
            else if(!state.dragging&&GetCursorPos(&cursor)&&Playing())
            {
                if(state.renderer)
                {
                    Vec at;
                    if(ScreenToClient(state.view,&cursor)&&TableAt(static_cast<float>(cursor.x),static_cast<float>(cursor.y),at))state.target=at;
                }
                else if(ScreenToClient(state.window,&cursor))state.target=state.layout.Table(static_cast<float>(cursor.x),static_cast<float>(cursor.y));
            }
            Vec herTarget=state.brain->Think(*state.game,dt);
            if(state.renderer)
            {
                herTarget.x=std::clamp(herTarget.x,state.reachLeft,state.reachRight);
                herTarget.y=std::max(herTarget.y,state.reachNear);
                // Her hand stays where it is until she means it to go somewhere else (no
                // twitching after each small change of mind).
                if(std::hypot(herTarget.x-state.goal.x,herTarget.y-state.goal.y)>0.035f)state.goal=herTarget;
            }
            state.game->Step(dt,state.target,herTarget);
            state.game->TakeEvents();
            if(state.game->Now()==AirHockey::Phase::Over&&!state.overTold)
            {
                state.overTold=true;
                if(state.her.onGameOver)state.her.onGameOver();
            }
        }

        void Tick()
        {
            LARGE_INTEGER now,rate;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&rate);
            const double dt=std::min(0.05,static_cast<double>(now.QuadPart-state.last.QuadPart)/rate.QuadPart);
            state.last=now;
            if(state.renderer)return;   // the 3D table goes on with her frames (TickAirHockeyWindow)
            StepGame(dt);
            InvalidateRect(state.window,nullptr,FALSE);
        }

        void ChooseSpeed(int level)
        {
            // The puck's top speed, from now on (and kept for the next time).
            state.her.puckSpeed=level;
            state.game->SetPuckSpeed(AirHockey::PuckSpeeds[level-1]);
            state.speedShownUntil=GetTickCount64()+1200;
            if(state.her.onPuckSpeed)state.her.onPuckSpeed(level);
            InvalidateRect(state.window,nullptr,FALSE);
        }

        LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            switch(message)
            {
            case WM_TIMER:
                if(wParam==TimerId&&state.game)Tick();
                return 0;
            case WM_SETCURSOR:
                // Over the table while playing, the mallet is the cursor.
                if(LOWORD(lParam)==HTCLIENT&&Playing()){SetCursor(nullptr);return TRUE;}
                break;
            case WM_KEYDOWN:
                if(state.asking)
                {
                    if(wParam=='Y'){Finish();return 0;}
                    if(wParam=='N'||wParam==VK_ESCAPE){state.asking=false;QueryPerformanceCounter(&state.last);}
                    return 0;
                }
                if(wParam==VK_ESCAPE)
                {
                    if(state.game->Now()==AirHockey::Phase::Over)Finish();
                    else state.asking=true;
                }
                else if(wParam==VK_SPACE&&state.game->Now()!=AirHockey::Phase::Over)
                {
                    state.paused=!state.paused;
                    QueryPerformanceCounter(&state.last);
                }
                else if(wParam==VK_RETURN&&state.game->Now()==AirHockey::Phase::Over)NewGame();
                else if(wParam==VK_F1)state.camera={};   // the camera back where it was
                else if(wParam>='1'&&wParam<='4')ChooseSpeed(static_cast<int>(wParam-'1')+1);
                else if(wParam>=VK_NUMPAD1&&wParam<=VK_NUMPAD4)ChooseSpeed(static_cast<int>(wParam-VK_NUMPAD1)+1);
                return 0;
            case WM_LBUTTONDOWN:
                if(state.paused&&!state.asking){state.paused=false;QueryPerformanceCounter(&state.last);}
                return 0;
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
            case WM_ACTIVATE:
                if(LOWORD(wParam)==WA_INACTIVE)HoldCursor(false);
                break;
            case WM_MOVE:
                HoldCursor(false);   // held again where it now is, at the next tick
                return 0;
            case WM_SIZE:
                HoldCursor(false);
                state.canvas.reset();
                PlaceView();
                InvalidateRect(hwnd,nullptr,FALSE);
                return 0;
            case WM_CLOSE:
                Finish();
                return 0;
            case WM_DESTROY:
                HoldCursor(false);
                state.window=nullptr;
                ReleaseView();
                state.canvas.reset();
                state.table.reset();
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

    void ShowAirHockeyWindow(HWND owner,const AirHockeyOpponent& her,std::function<void()> onEnd)
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
        NewGame();

        // In 3D wide (the table seen from behind your end, her across it); in 2D upright, as
        // the table is long: most of the screen's height either way.
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&info);
        const RECT work=info.rcWork;
        const int dpi=static_cast<int>(GetDpiForSystem());
        const bool wide=her.model!=nullptr;
        const int h=std::min(static_cast<int>(work.bottom-work.top)*9/10,MulDiv(wide?820:900,dpi,96));
        const int w=std::min(static_cast<int>(work.right-work.left)*9/10,wide?h*4/3:h*2/3);
        const std::wstring title=std::wstring(T(L"エアホッケー",L"Air hockey"))+L" - "+HerName();
        state.window=CreateWindowExW(0,ClassName,title.c_str(),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                                     work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,
                                     nullptr,nullptr,instance,nullptr);
        if(CreateView())
        {
            PlaceView();
            NewGame();   // her reach now known
        }
        else if(wide)
        {
            // No 3D: the 2D table, upright.
            const int w2=std::min(static_cast<int>(work.right-work.left)*9/10,h*2/3);
            MoveWindow(state.window,work.left+(work.right-work.left-w2)/2,work.top+(work.bottom-work.top-h)/2,w2,h,FALSE);
        }
        SetTimer(state.window,TimerId,10,nullptr);
        ShowWindow(state.window,SW_SHOWNORMAL);
        SetForegroundWindow(state.window);
    }

    bool AirHockeyWindowOpen(){return state.window!=nullptr;}
    HWND AirHockeyWindowHandle(){return state.window;}
    bool AirHockeyWindowRect(RECT& r)
    {
        return state.window&&IsWindowVisible(state.window)&&!IsIconic(state.window)&&GetWindowRect(state.window,&r);
    }

    void TickAirHockeyWindow(MascotModel& model,double dt)
    {
        if(!state.window||!state.renderer||!state.game)return;
        using namespace DirectX;
        dt=std::min(dt,0.1);
        auto& r=*state.renderer;
        const auto& g=game();
        // Her hand on her mallet: the wrist's target where the grip comes over the knob (where
        // the mallet will be next, as it goes), the offset from the wrist learned from where the
        // grip came; the mallet kept within her hand's reach of where her hand is.
        const float D=state.upper;
        state.holding+=dt;
        // Her mallet is in her hand: it goes where her hand is (the grip's middle, as posed now).
        std::array<float,3> middle{},along{};
        const bool gripped=model.Grip(HerHand,middle,along);
        if(gripped&&state.holding>0.6)
            game().HoldMallet(AirHockey::Her,{(middle[0]-model.RootOffset()+state.herX)/state.k,(middle[2]+state.herZ)/state.k});
        StepGame(dt);
        // Her hand toward where she means the mallet to go, as an arm takes it there.
        {
            const auto& rules=g.Rule();
            const float a=rules.herAcceleration,most=rules.malletSpeed[AirHockey::Her],step=static_cast<float>(dt);
            const Vec to{state.goal.x-state.intent.x,state.goal.y-state.intent.y};
            const float gap=std::hypot(to.x,to.y);
            Vec& v=state.intentVelocity;
            if(gap<0.004f){v={};state.intent=state.goal;}
            else
            {
                const float speed=std::min(most,std::sqrt(2*a*gap));
                Vec change{to.x/gap*speed-v.x,to.y/gap*speed-v.y};
                const float c=std::hypot(change.x,change.y),limit=a*step;
                if(c>limit){change.x*=limit/c;change.y*=limit/c;}
                v.x+=change.x;v.y+=change.y;
                state.intent.x+=v.x*step;state.intent.y+=v.y*step;
            }
        }
        // She moves a little toward her mallet's side (a step's worth), slowly, and only when it
        // is well off to that side.
        {
            const float want=std::clamp(state.goal.x*state.k*0.45f,-0.3f*D,0.3f*D),most=static_cast<float>(0.9*D*dt);
            if(std::abs(want-state.herX)>0.08f*D||state.sliding)
            {
                state.sliding=std::abs(want-state.herX)>0.01f*D;
                state.herX+=std::clamp(want-state.herX,-most,most);
            }
        }
        const std::array<float,3> grip=ModelPoint(model,World(state.intent,GripHeight()*state.k));
        const bool still=std::hypot(state.intentVelocity.x,state.intentVelocity.y)<0.02f&&!state.sliding;
        if(gripped&&state.holding>0.6)
        {
            // Diagnostics: how much the hand shakes while it is to stay where it is (its moves'
            // changes from frame to frame, on frames when she means it to be still).
            state.stillFrames=still?state.stillFrames+1:0;
            if(state.holding>1.5&&state.stillFrames>=3)
            {
                float jerk=0;
                for(int a=0;a<3;++a){const float d=middle[a]-2*state.gripBefore[0][a]+state.gripBefore[1][a];jerk+=d*d;}
                state.shake+=std::sqrt(jerk)/state.upper;
                ++state.shakeFrames;
            }
            state.gripBefore[1]=state.gripBefore[0];
            state.gripBefore[0]=middle;
            // Learned only while her hand is still there (a moving hand lags behind its target and
            // would teach it wrong) and when it is off by more than a little: across and along
            // freely, up and down only a little (the stops lift a hand that comes too near the
            // table, and the wrist must not chase it down). Slowly, about half a second.
            const float off=std::sqrt((grip[0]-middle[0])*(grip[0]-middle[0])+(grip[2]-middle[2])*(grip[2]-middle[2]));
            if(still&&off>0.02f*D)
            {
                const float learn=static_cast<float>(1.0-std::exp(-dt/0.5));
                for(int a:{0,2})state.holdOffset[a]=std::clamp(state.holdOffset[a]+(grip[a]-middle[a])*learn,-0.35f*D,0.35f*D);
                state.holdOffset[1]=std::clamp(state.holdOffset[1]+(grip[1]-middle[1])*learn,0.0f,0.15f*D);
            }
        }
        model.Hold(HerHand,{grip[0]+state.holdOffset[0],grip[1]+state.holdOffset[1],grip[2]+state.holdOffset[2]},true);
        SetTableSolids(model);

        r.SetMovingGeometry(BuildPieces());
        r.SetModelTransform(HerWorld(model));
        PlaceCamera();
        UpdateOverlay();
        r.Render(model,state.her.edgeScale);
        ++state.frames;
    }

    void PlayAirHockeyForTest(){state.autoplay=std::make_unique<AirHockey::Brain>(AirHockey::You,AirHockey::Style{0.25f,0.12f,0.6f,1.0f},7);}

    bool SaveAirHockeyView(const std::wstring& path)
    {
        if(state.renderer)return state.renderer->SaveLastFrame(path);
        return state.canvas&&SavePng(*state.canvas,path);
    }

    void SetAirHockeyCamera(float pitch,float distance,float yaw)
    {
        state.camera.pitch=pitch;
        state.camera.distance=distance;
        state.camera.yaw=yaw;
    }

    std::wstring DescribeAirHockey()
    {
        if(!state.game)return L"";
        const auto& g=*state.game;
        RECT area{},client{};
        CursorArea(area);
        if(state.window){GetClientRect(state.window,&client);MapWindowPoints(state.window,nullptr,reinterpret_cast<POINT*>(&client),2);}
        std::wstring text;
        wchar_t line[260];
        swprintf_s(line,L"airhockey: cursor area %ld,%ld-%ld,%ld in the window's %ld,%ld-%ld,%ld\n",area.left,area.top,area.right,area.bottom,client.left,client.top,client.right,client.bottom);
        text+=line;
        if(state.renderer)
        {
            const Vec hers=g.Mallet(AirHockey::Her);
            std::array<float,3> middle{},along{};
            float off=-1,high=0;
            if(state.her.model&&state.her.model->Grip(HerHand,middle,along))
            {
                off=std::hypot((middle[0]-state.her.model->RootOffset()+state.herX)/state.k-hers.x,(middle[2]+state.herZ)/state.k-hers.y);
                high=(middle[1]-state.top)/state.k-GripHeight();
            }
            swprintf_s(line,L"airhockey: 3D, table %.2f per unit, top %.2f, she stands at %.2f; reach %.2f..%.2f to %.2f; her grip %.3f from her mallet, %.3f above where it goes (table units); shake %.4f\n",
                       state.k,state.top,state.herZ,state.reachLeft,state.reachRight,state.reachNear,off,high,state.shakeFrames?state.shake/state.shakeFrames:0.0);
            text+=line;
        }
        swprintf_s(line,L"airhockey: her %d you %d, %.1f s, %s, rally %d (longest %d), frames %d, paused %d\n",g.Score(AirHockey::Her),g.Score(AirHockey::You),
                   g.Clock(),g.Now()==AirHockey::Phase::Over?L"over":g.Now()==AirHockey::Phase::Serve?L"serve":L"play",g.Rally(),g.LongestRally(),state.frames,state.paused?1:0);
        text+=line;
        return text;
    }

    std::wstring RunAirHockeySim(int games,std::uint32_t seed,int speed)
    {
        using namespace AirHockey;
        std::wstring lines;
        int wrong=0,goals=0,restarts=0,longest=0,wins[2]={};
        double seconds=0;
        float fastest=0;
        for(int i=0;i<games;++i)
        {
            Game game;
            if(speed>=1&&speed<=4)game.SetPuckSpeed(PuckSpeeds[speed-1]);
            game.NewGame(i%2);
            // Yours as a person plays it (a little slower to see, the mouse quick to move).
            Brain you(You,Style{0.25f,0.12f,0.6f,1.0f},seed*7919u+i*2+1),her(Her,Style{},seed*104729u+i*2+2);
            const double dt=1/120.0;
            while(game.Now()!=Phase::Over&&game.Clock()<900)
            {
                const Vec a=you.Think(game,dt),b=her.Think(game,dt);
                game.Step(dt,a,b);
                game.TakeEvents();
            }
            const bool ended=game.Now()==Phase::Over;
            const int bad=game.OffTable()+game.Overlaps()+(ended?0:1);
            wrong+=bad;
            if(ended)++wins[game.Winner()];
            goals+=game.Score(You)+game.Score(Her);
            restarts+=game.Restarts();
            longest=std::max(longest,game.LongestRally());
            fastest=std::max(fastest,game.FastestPuck());
            seconds+=game.Clock();
            wchar_t line[200];
            swprintf_s(line,L"game %d: her %d you %d, %.0f s, longest rally %d, fastest %.2f, restarts %d, off table %d, overlaps %d%s\n",
                       i+1,game.Score(Her),game.Score(You),game.Clock(),game.LongestRally(),game.FastestPuck(),game.Restarts(),
                       game.OffTable(),game.Overlaps(),ended?L"":L", NOT ENDED");
            lines+=line;
        }
        wchar_t head[240];
        swprintf_s(head,L"%d wrong\n%d games: her %d wins, you %d; %.1f goals and %.0f s a game, %.2f restarts a game, longest rally %d, fastest puck %.2f\n",
                   wrong,games,wins[Her],wins[You],games?static_cast<double>(goals)/games:0.0,games?seconds/games:0.0,
                   games?static_cast<double>(restarts)/games:0.0,longest,fastest);
        return head+lines;
    }

    bool SaveAirHockeyShot(const std::wstring& path,double seconds,std::uint32_t seed)
    {
        using namespace AirHockey;
        ULONG_PTR token=0;
        GdiplusStartupInput input;
        if(!state.gdiplus&&GdiplusStartup(&token,&input,nullptr)!=Ok)return false;
        bool saved=false;
        {
            Game game;
            Brain you(You,Style{0.25f,0.12f,0.6f,1.0f},seed*7919u+1),her(Her,Style{},seed*104729u+2);
            const double dt=1/60.0;
            for(double t=0;t<seconds&&game.Now()!=Phase::Over;t+=dt)
            {
                const Vec a=you.Think(game,dt),b=her.Think(game,dt);
                game.Step(dt,a,b);
                game.TakeEvents();
            }
            const int w=600,h=900;
            Bitmap image(w,h,PixelFormat32bppPARGB);
            Graphics g(&image);
            const Layout l=Place(w,h,1,game.Rule());
            DrawTable(g,w,h,l,game.Rule());
            DrawPlay(g,w,l,game,false,false,false);
            saved=SavePng(image,path);
        }
        if(token)GdiplusShutdown(token);
        return saved;
    }
}
