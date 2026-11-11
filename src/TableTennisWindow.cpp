#define NOMINMAX
#include "TableTennisWindow.hpp"
#include "TableTennis.hpp"
#include "Log.hpp"
#include "MascotModel.hpp"
#include "MascotRenderer.hpp"
#include "Text.hpp"
#include "BubbleText.hpp"
#include "TextureLoader.hpp"
#include "GameLines.hpp"
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <random>
#include <vector>
#pragma comment(lib,"gdiplus.lib")
#pragma comment(lib,"winmm.lib")

namespace MikuMikuDesktopPerch
{
    namespace
    {
        using namespace Gdiplus;
        using TableTennis::Vec;
        using TableTennis::Vec3;
        using TableTennis::Stroke;
        using TableTennis::Phase;
        using TableTennis::Reason;
        using TableTennis::You;
        using TableTennis::Her;
        constexpr wchar_t ClassName[]=L"MikuMikuDesktopPerchTableTennis";
        constexpr wchar_t ViewClassName[]=L"MikuMikuDesktopPerchTableTennisView";
        constexpr UINT_PTR TimerId=1;
        constexpr int HerHand=1;   // she holds her racket in her right hand
        // A racket (metres): the blade's radius, its handle, and how far the blade's middle is
        // from the middle of the hand that holds it.
        constexpr float BladeRadius=0.085f,HandleLength=0.10f,BladeOut=0.05f+BladeRadius;
        constexpr float SwingShown=0.30f;   // seconds a swing is seen

        struct State
        {
            HWND window=nullptr,view=nullptr;
            ULONG_PTR gdiplus=0;
            TableTennisOpponent her;
            std::function<void()> onEnd;
            std::unique_ptr<TableTennis::Game> game;
            std::unique_ptr<TableTennis::Brain> brain,autoplay;   // hers; yours in a test
            LARGE_INTEGER last{};
            bool paused=false,asking=false;
            RectF askButtons[2];   // asking: where "stop" and "go on" are drawn (window pixels), for a click
            bool held=false;       // the cursor is kept in the window (ClipCursor)
            ULONGLONG levelShownUntil=0,handShownUntil=0;   // GetTickCount64: until then, what was just chosen is shown
            int frames=0;          // drawn, for the snapshot's report
            // The ball's trail: where it was over the last moments (how fast, and whose stroke: 0
            // yours, 1 hers, 2 a smash), by the clock of what is shown.
            struct TrailPoint {Vec3 p{};float speed=0;int kind=0;double at=0;};
            std::vector<TrailPoint> trail;
            // The sparks thrown off where a smash is struck.
            struct Spark {Vec3 p{},v{};float life=0,age=0,size=0,turn=0;};
            std::vector<Spark> sparks;
            double shown=0;
            Vec target{};          // where the mouse asks your racket to be
            Vec herGoal{};         // where she means her racket to go
            float yourHeight=0.18f;   // your racket's height over the table (it goes with the ball)
            float yourShift=0;        // your racket taken across to the ball as it swings (metres, eased)
            float inBody=0;           // diagnostics: the most her racket arm was left in her body (leg lengths)

            // The 3D table. World units: the table's top at y = top, its middle over (0, 0); x
            // across, the table's length along world z (toward her), k world units to a metre.
            // She stands at z = herZ facing -z.
            std::unique_ptr<MascotRenderer> renderer;
            float upper=10,hips=10,k=10,top=8,herZ=12;
            float herX=0;          // she steps to the side with her racket (world units)
            bool sliding=false;
            float slideVelocity=0;
            float herHeight=0.2f;  // her racket's height over the table (metres), eased
            // The wrist's target from where the blade's middle is to be, learned as she holds
            // (model units).
            std::array<float,3> holdOffset{};
            std::array<float,3> otherOffset{};   // the same for the other hold (forehand and backhand each have one)
            bool backhand=false;   // she takes the ball on her backhand (it comes to her left)
            double chosenFor=-1;   // the ball that was chosen for (Brain::HitAt)
            float herElbow=0;      // her racket arm's elbow across the room now (world x)
            std::array<float,6> body{};   // her body with the stroke, eased (MascotModel::HoldBody)
            std::array<float,6> bodySpeed{};   // how fast each of those is changing (they follow as springs: no sudden starts)
            float heightSpeed=0;   // the same for her racket's height
            std::array<float,3> handTrim{};   // her wrist's target corrected by how far her blade is still off (model units)
            float trim=0;          // her step corrected by how far her blade is still off across (world units)
            float aimX=0;          // where her blade was last to be across (world x, her stroke's reach back included)
            double strokeAt=-1;    // when she means to meet the ball she is playing (the game's clock)
            // Her serve as it is seen: the ball on her other hand's palm, then thrown up to where the
            // game has it to be struck.
            // Diagnostics: how upright her racket stands (its way's upward part) and how squarely its
            // face is to you (its palm's part toward you), summed over the frames a ball is coming, by forehand and backhand.
            std::array<double,2> uprightSum{},squareSum{},slopeSum{};   // (slope: her wrist above her elbow, metres)
            std::array<int,2> poseFrames{};
            // Diagnostics: how much her blade shakes up and down while she waits (its height's
            // change of change from frame to frame, metres), and the most in one frame.
            std::array<float,2> heightBefore{};
            double shake=0;
            float shakeMost=0;
            int shakeFrames=0,waited=0;
            bool ballOnHand=false;
            Vec3 tossFrom{};       // where the ball left her palm
            Vec3 ballShown{};
            float stance=0,stanceSpeed=0;   // her feet apart (world units each), eased
            // Her feet on the floor (world x and z, each less where it stands at rest in her
            // model): planted while her body moves over them, lifted and set down again in a
            // quick step when she has gone too far from one.
            struct Foot {float x=0,z=0,fromX=0,fromZ=0,toX=0,toZ=0,t=1;bool placed=false;};
            std::array<Foot,2> feet{};
            int steps=0;   // diagnostics
            float depthSpeed=0;    // how fast she is moving toward or away from the table (herZ)
            Stroke strokeKind=Stroke::Drive;
            bool within=true;      // her blade was where it was to be when her swing began
            int strikes=0,whiffs=0;      // diagnostics: her strokes, and her swings that missed for want of her hand
            double holding=0;      // seconds she has held it
            bool gripped=false;    // her racket as last posed: the blade's middle and the handle's way (world)
            DirectX::XMFLOAT3 herBlade{},herAlong{},herPalm{};
            // Diagnostics: how far her blade was from where the game has her racket, at her strokes.
            std::array<double,2> strokeAcross{};   // (how far to +x of her racket her blade was at her strokes: forehand, backhand)
            std::array<int,2> strokeSide{};
            double strokeOff=0,strokeAlong=0;   // (along: how far her blade was still behind, away from the net)
            int strokes=0;
            struct Camera {float yaw=0,pitch=0.44f,distance=5.5f,x=0,y=0,z=0;} camera;
            POINT dragFrom{};
            int dragging=0;            // 0, or 1 turns, 2 moves
            std::wstring hudShown;     // what the picture over the table last showed
            // Her words in the bubble beside her (a line asked for ahead, or one of her own), as
            // at mahjong.
            struct Words
            {
                std::wstring text;
                ULONGLONG since=0,until=0;
                int weight=0;
                std::vector<std::wstring> recent;
                bool voiced=false;   // the words are said by a voice file of their own
                std::wstring told;   // diagnostics: what happened and what she said, for the snapshot's report
            } words;
            GameLines lines;
            GameVoice voice;
            bool prefetching=false;
            std::mt19937 wordsRandom{12345};
            bool started=false,overSeen=false,cheered=false,deuceSaid=false;
            std::array<bool,2> matchPoint{};
            double faceUntil=0,overAt=0;   // the game's clock: her face back, the game's end
            std::wstring afterword,afterwordLine;   // TableTennisAfterword
            // Her head on the screen and beside it to the right (her bubble), as last drawn; x < 0: not known.
            POINT headScreen{-1,-1},besideHead{-1,-1};
            DirectX::XMFLOAT3 eye{},look{};   // the camera as last set
        } state;

        std::uint32_t Seed()
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            return static_cast<std::uint32_t>(now.QuadPart)^0x9E3779B9u;
        }

        // Her words come out a character at a time, as on the desktop without a voice.
        double CharSeconds(){return English()?0.045:0.12;}
        std::wstring ShownWords()
        {
            const auto& w=state.words;
            if(w.text.empty())return {};
            const size_t n=w.voiced?static_cast<size_t>(state.voice.Revealed()):static_cast<size_t>((GetTickCount64()-w.since)/1000.0/CharSeconds())+1;
            return w.text.substr(0,std::min(n,w.text.size()));
        }

        std::wstring HerName(){return state.her.name.empty()?std::wstring(T(L"相手",L"Her")):state.her.name;}

        std::wstring LevelName(int level)
        {
            switch(level)
            {
            case 1:return T(L"やさしい",L"easy");
            case 3:return T(L"つよい",L"strong");
            default:return T(L"ふつう",L"normal");
            }
        }
        std::wstring HandName(){return state.her.leftHanded?T(L"左利き",L"left-handed"):T(L"右利き",L"right-handed");}

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

        TableTennis::Game& game(){return *state.game;}
        bool Playing(){return state.game&&state.game->Now()!=Phase::Over&&!state.paused&&!state.asking;}
        float Scale(){return state.window?static_cast<float>(GetDpiForWindow(state.window))/96.0f:1;}

        // ---- The sounds: the ball on a racket and on the table, made here ----------------

        std::vector<BYTE> Tone(float hertz,float seconds,float noise,float loud)
        {
            constexpr int rate=22050;
            const int n=static_cast<int>(rate*seconds);
            std::vector<BYTE> wav(44+n*2);
            auto put32=[&](int at,std::uint32_t v){memcpy(wav.data()+at,&v,4);};
            auto put16=[&](int at,std::uint16_t v){memcpy(wav.data()+at,&v,2);};
            memcpy(wav.data(),"RIFF",4);put32(4,36+n*2);memcpy(wav.data()+8,"WAVEfmt ",8);put32(16,16);put16(20,1);put16(22,1);
            put32(24,rate);put32(28,rate*2);put16(32,2);put16(34,16);memcpy(wav.data()+36,"data",4);put32(40,n*2);
            std::uint32_t r=0x1234567u;
            for(int i=0;i<n;++i)
            {
                const float t=static_cast<float>(i)/rate,fade=std::exp(-t/(seconds*0.22f))*std::min(1.0f,t*rate/20.0f);
                r^=r<<13;r^=r>>17;r^=r<<5;
                const float hiss=static_cast<float>(r%2001)/1000.0f-1;
                const float v=(std::sin(DirectX::XM_2PI*hertz*t)*(1-noise)+hiss*noise)*fade*loud;
                const std::int16_t sample=static_cast<std::int16_t>(std::clamp(v,-1.0f,1.0f)*32000);
                memcpy(wav.data()+44+i*2,&sample,2);
            }
            return wav;
        }

        // A hard knock, as on wood: a click, then a few quick-dying tones of a struck block.
        std::vector<BYTE> Knock(float hertz,float loud)
        {
            constexpr int rate=44100;
            const int n=rate*9/100;
            std::vector<BYTE> wav(44+n*2);
            auto put32=[&](int at,std::uint32_t v){memcpy(wav.data()+at,&v,4);};
            auto put16=[&](int at,std::uint16_t v){memcpy(wav.data()+at,&v,2);};
            memcpy(wav.data(),"RIFF",4);put32(4,36+n*2);memcpy(wav.data()+8,"WAVEfmt ",8);put32(16,16);put16(20,1);put16(22,1);
            put32(24,rate);put32(28,rate*2);put16(32,2);put16(34,16);memcpy(wav.data()+36,"data",4);put32(40,n*2);
            std::uint32_t r=0x2468ACEu;
            for(int i=0;i<n;++i)
            {
                const float t=static_cast<float>(i)/rate;
                r^=r<<13;r^=r>>17;r^=r<<5;
                const float hiss=static_cast<float>(r%2001)/1000.0f-1;
                float v=0.55f*hiss*std::exp(-t/0.0011f);
                v+=1.00f*std::sin(DirectX::XM_2PI*hertz*t)*std::exp(-t/0.010f);
                v+=0.55f*std::sin(DirectX::XM_2PI*hertz*2.76f*t)*std::exp(-t/0.005f);
                v+=0.30f*std::sin(DirectX::XM_2PI*hertz*5.40f*t)*std::exp(-t/0.0025f);
                const std::int16_t sample=static_cast<std::int16_t>(std::clamp(v*loud*0.6f,-1.0f,1.0f)*32000);
                memcpy(wav.data()+44+i*2,&sample,2);
            }
            return wav;
        }

        enum class Sound {Racket,Smash,Table,Net};
        void Play(Sound sound)
        {
            if(state.autoplay)return;   // a diagnostic run is silent
            // On the table a hard wooden knock; on a racket higher and softer (a smash louder).
            static const std::vector<BYTE> sounds[4]={Knock(1500,0.42f),Knock(1380,0.70f),Knock(1050,0.95f),Tone(180,0.12f,0.7f,0.3f)};
            PlaySoundW(reinterpret_cast<LPCWSTR>(sounds[static_cast<int>(sound)].data()),nullptr,SND_MEMORY|SND_ASYNC|SND_NODEFAULT);
        }

        // ---- The 3D table ----------------------------------------------------------------

        // The geometry shader's patterns, by the colour's alpha (GeometryPS): wood (as the mahjong
        // room's), wallpaper and metal, lit; 1 is plain and unlit, 0.25 glows.
        constexpr float Wood=0.10f,Paper=0.08f,Metal=0.04f,Glow=0.25f;

        using DirectX::XMFLOAT3;
        using DirectX::XMVECTOR;

        // Plain shapes for the renderer's geometry: wood and the others patterned along x, z or y
        // (grain 0, 1, 2), grainScale apart as the shader draws them for 1.
        struct Mesh
        {
            std::vector<MascotRenderer::GeometryVertex> out;
            using V=std::array<float,3>;
            int grain=0;
            float grainScale=1;
            void Point(const V& q,const V& n,const float (&col)[4])
            {
                MascotRenderer::GeometryVertex v{{q[0],q[1],q[2]},{n[0],n[1],n[2]},{col[0],col[1],col[2],col[3]}};
                if(col[3]==Wood||col[3]==Paper||col[3]==Metal)
                {
                    const float along=q[grain==0?0:grain==1?2:1],up=q[grain==2?0:1],across=q[grain==1?0:2];
                    v.wall[0]=along/grainScale;v.wall[1]=up/grainScale;v.wall[3]=across/grainScale;
                }
                out.push_back(v);
            }
            void Quad(const V& a,const V& b,const V& c,const V& d,const V& n,const float (&col)[4])
            {
                const V* p[6]={&a,&b,&c,&a,&c,&d};
                for(const V* q:p)Point(*q,n,col);
            }
            // A triangle seen from the side its normal points to, whichever way its corners go round.
            void Tri(const V& a,V b,V c,const V& n,const float (&col)[4])
            {
                const V u{b[0]-a[0],b[1]-a[1],b[2]-a[2]},w{c[0]-a[0],c[1]-a[1],c[2]-a[2]};
                const V x{u[1]*w[2]-u[2]*w[1],u[2]*w[0]-u[0]*w[2],u[0]*w[1]-u[1]*w[0]};
                if(x[0]*n[0]+x[1]*n[1]+x[2]*n[2]>0)std::swap(b,c);
                Point(a,n,col);Point(b,n,col);Point(c,n,col);
            }
            void Face(const V& a,const V& b,const V& c,const V& d,const V& n,const float (&col)[4])
            {
                Tri(a,b,c,n,col);
                Tri(a,c,d,n,col);
            }
            void Box(float x0,float y0,float z0,float x1,float y1,float z1,const float (&c)[4])
            {
                Quad({x0,y1,z0},{x1,y1,z0},{x1,y1,z1},{x0,y1,z1},{0,1,0},c);
                Quad({x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},{0,0,-1},c);
                Quad({x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1},{0,0,1},c);
                Quad({x0,y0,z0},{x0,y0,z1},{x0,y1,z1},{x0,y1,z0},{-1,0,0},c);
                Quad({x1,y0,z0},{x1,y0,z1},{x1,y1,z1},{x1,y1,z0},{1,0,0},c);
            }
            // A flat ring lying at height y, from radius r0 to r1 round (x, z).
            void Ring(float x,float z,float y,float r0,float r1,const float (&c)[4],int sides=32)
            {
                for(int i=0;i<sides;++i)
                {
                    const float b0=DirectX::XM_2PI*i/sides,b1=DirectX::XM_2PI*(i+1)/sides;
                    Face({x+r0*std::cos(b0),y,z+r0*std::sin(b0)},{x+r0*std::cos(b1),y,z+r0*std::sin(b1)},
                         {x+r1*std::cos(b1),y,z+r1*std::sin(b1)},{x+r1*std::cos(b0),y,z+r1*std::sin(b0)},{0,1,0},c);
                }
            }
            // A box round middle, its sides along the unit vectors a, b, c (half lengths ha, hb, hc).
            void Turned(const V& m,const V& a,float ha,const V& b,float hb,const V& c,float hc,const float (&col)[4])
            {
                auto at=[&](float i,float j,float l){return V{m[0]+a[0]*ha*i+b[0]*hb*j+c[0]*hc*l,m[1]+a[1]*ha*i+b[1]*hb*j+c[1]*hc*l,m[2]+a[2]*ha*i+b[2]*hb*j+c[2]*hc*l};};
                const V na{-a[0],-a[1],-a[2]},nb{-b[0],-b[1],-b[2]},nc{-c[0],-c[1],-c[2]};
                Face(at(1,-1,-1),at(1,1,-1),at(1,1,1),at(1,-1,1),a,col);
                Face(at(-1,-1,-1),at(-1,1,-1),at(-1,1,1),at(-1,-1,1),na,col);
                Face(at(-1,1,-1),at(1,1,-1),at(1,1,1),at(-1,1,1),b,col);
                Face(at(-1,-1,-1),at(1,-1,-1),at(1,-1,1),at(-1,-1,1),nb,col);
                Face(at(-1,-1,1),at(1,-1,1),at(1,1,1),at(-1,1,1),c,col);
                Face(at(-1,-1,-1),at(1,-1,-1),at(1,1,-1),at(-1,1,-1),nc,col);
            }
            // A racket: its blade round middle (facing the unit vector face: that side's rubber
            // front, the other back), the handle going the unit vector handle (square to face).
            void Racket(const V& middle,const V& handle,const V& face,float scale,const float (&front)[4],const float (&back)[4])
            {
                const float wood[4]={0.80f,0.62f,0.40f,1},grip[4]={0.62f,0.44f,0.26f,1};
                const V side{face[1]*handle[2]-face[2]*handle[1],face[2]*handle[0]-face[0]*handle[2],face[0]*handle[1]-face[1]*handle[0]};
                const float R=BladeRadius*scale,thick=0.006f*scale;
                const V opposite{-face[0],-face[1],-face[2]};
                constexpr int sides=20;
                auto rim=[&](int i,float out){
                    // A little longer than wide, as a blade is.
                    const float t=DirectX::XM_2PI*i/sides,ch=std::cos(t)*R*1.06f,sh=std::sin(t)*R*0.94f;
                    return V{middle[0]+handle[0]*ch+side[0]*sh+face[0]*out,middle[1]+handle[1]*ch+side[1]*sh+face[1]*out,middle[2]+handle[2]*ch+side[2]*sh+face[2]*out};
                };
                const V f{middle[0]+face[0]*thick,middle[1]+face[1]*thick,middle[2]+face[2]*thick};
                const V b{middle[0]-face[0]*thick,middle[1]-face[1]*thick,middle[2]-face[2]*thick};
                for(int i=0;i<sides;++i)
                {
                    Tri(f,rim(i,thick),rim(i+1,thick),face,front);
                    Tri(b,rim(i,-thick),rim(i+1,-thick),opposite,back);
                    const float t=DirectX::XM_2PI*(i+0.5f)/sides;
                    const V out{handle[0]*std::cos(t)+side[0]*std::sin(t),handle[1]*std::cos(t)+side[1]*std::sin(t),handle[2]*std::cos(t)+side[2]*std::sin(t)};
                    Face(rim(i,-thick),rim(i+1,-thick),rim(i+1,thick),rim(i,thick),out,wood);
                }
                const float from=R*0.98f,length=HandleLength*scale;
                const V h{middle[0]+handle[0]*(from+length/2),middle[1]+handle[1]*(from+length/2),middle[2]+handle[2]*(from+length/2)};
                Turned(h,handle,length/2,side,0.015f*scale,face,0.011f*scale,grip);
            }
            // A ball, shaded as lit from above (its colour is drawn as it is).
            void Sphere(const V& m,float r,const float (&col)[4])
            {
                constexpr int rows=6,around=12;
                auto at=[&](int i,int j){
                    const float a=DirectX::XM_PI*i/rows,b=DirectX::XM_2PI*j/around;
                    return V{std::sin(a)*std::cos(b),std::cos(a),std::sin(a)*std::sin(b)};
                };
                auto put=[&](const V& n){
                    const float lit=0.70f+0.30f*std::max(0.0f,n[0]*0.25f+n[1]*0.93f-n[2]*0.25f);
                    const float c[4]={col[0]*lit,col[1]*lit,col[2]*lit,col[3]};
                    Point({m[0]+n[0]*r,m[1]+n[1]*r,m[2]+n[2]*r},n,c);
                };
                for(int i=0;i<rows;++i)
                    for(int j=0;j<around;++j)
                    {
                        const V a=at(i,j),b=at(i,j+1),c=at(i+1,j+1),d=at(i+1,j);
                        // Seen from outside, whichever way the corners go round (as Tri).
                        auto three=[&](const V& p,const V& q,const V& s){
                            const V u{q[0]-p[0],q[1]-p[1],q[2]-p[2]},w{s[0]-p[0],s[1]-p[1],s[2]-p[2]};
                            const V x{u[1]*w[2]-u[2]*w[1],u[2]*w[0]-u[0]*w[2],u[0]*w[1]-u[1]*w[0]};
                            const bool turn=x[0]*(p[0]+q[0]+s[0])+x[1]*(p[1]+q[1]+s[1])+x[2]*(p[2]+q[2]+s[2])>0;
                            put(p);put(turn?s:q);put(turn?q:s);
                        };
                        three(a,b,c);
                        three(a,c,d);
                    }
            }
        };

        constexpr float TopThick=0.03f;   // the table's top (metres)

        // What does not move: the room, the table and its lines, the net.
        std::vector<MascotRenderer::GeometryVertex> BuildRoom()
        {
            const auto& rules=state.game->Rule();
            const float k=state.k,top=state.top,W=rules.width/2*k,L=rules.length/2*k;
            const float D=state.upper;
            Mesh m;
            using V=Mesh::V;
            // The room, big enough for the camera to turn in: a floor of warm planks with a rug
            // under the table; walls papered above a panelled wooden wainscot, with a rail, a
            // skirting board and a moulding at the ceiling; behind her two bright windows, and
            // framed pictures on the side walls.
            const float rx=7*D,rz=8*D,ceiling=state.hips+2.2f*D;
            const float planks[4]={0.55f,0.38f,0.24f,Wood},panel[4]={0.42f,0.27f,0.16f,Wood},trim[4]={0.30f,0.19f,0.11f,Wood};
            const float paper[4]={0.90f,0.86f,0.78f,Paper};
            const float rug[4]={0.20f,0.30f,0.26f,1},rugEdge[4]={0.80f,0.64f,0.36f,1},rugInner[4]={0.16f,0.25f,0.22f,1};
            const float glass[4]={0.86f,0.92f,1.0f,Glow},sash[4]={0.92f,0.91f,0.88f,1};
            m.grain=0;m.grainScale=1;
            m.Quad({-rx,0,rz},{rx,0,rz},{rx,0,-rz},{-rx,0,-rz},{0,1,0},planks);
            {
                const float ux=W+1.6f*D,uz=L+1.5f*D,y=0.004f*D;
                m.Quad({-ux,y,uz},{ux,y,uz},{ux,y,-uz},{-ux,y,-uz},{0,1,0},rug);
                const float b=0.18f*D;
                m.Quad({-ux+b,2*y,uz-b},{ux-b,2*y,uz-b},{ux-b,2*y,-uz+b},{-ux+b,2*y,-uz+b},{0,1,0},rugEdge);
                const float c=0.26f*D;
                m.Quad({-ux+c,3*y,uz-c},{ux-c,3*y,uz-c},{ux-c,3*y,-uz+c},{-ux+c,3*y,-uz+c},{0,1,0},rugInner);
            }
            const float wain=0.85f*D,proud=0.04f*D;
            auto wall=[&](const V& a,const V& b,const V& n){
                const bool alongX=std::abs(a[0]-b[0])>std::abs(a[2]-b[2]);
                m.grain=alongX?0:1;
                m.grainScale=0.35f*D;   // the wallpaper's pattern
                m.Quad({a[0],wain,a[2]},{b[0],wain,b[2]},{b[0],ceiling,b[2]},{a[0],ceiling,a[2]},n,paper);
                m.grainScale=0.6f;
                const V in{n[0]*proud,0,n[2]*proud};
                m.Quad({a[0]+in[0],0,a[2]+in[2]},{b[0]+in[0],0,b[2]+in[2]},{b[0]+in[0],wain,b[2]+in[2]},{a[0]+in[0],wain,a[2]+in[2]},n,panel);
                const float lo[3]={std::min(a[0],b[0]),0,std::min(a[2],b[2])},hi[3]={std::max(a[0],b[0]),0,std::max(a[2],b[2])};
                const float ox=n[0]*proud*1.8f,oz=n[2]*proud*1.8f;
                auto band=[&](float y0,float y1){m.Box(lo[0]+std::min(0.0f,ox),y0,lo[2]+std::min(0.0f,oz),hi[0]+std::max(0.0f,ox),y1,hi[2]+std::max(0.0f,oz),trim);};
                band(wain-0.02f*D,wain+0.05f*D);           // the rail
                band(0,0.14f*D);                           // the skirting board
                band(ceiling-0.14f*D,ceiling);             // the moulding
                // The wainscot's panels: upright strips between them.
                const float length=alongX?hi[0]-lo[0]:hi[2]-lo[2],strip=0.06f*D;
                const int panels=std::max(1,static_cast<int>(length/(1.1f*D)));
                m.grain=2;
                for(int i=1;i<panels;++i)
                {
                    const float s=(alongX?lo[0]:lo[2])+length*i/panels;
                    if(alongX)m.Box(s-strip/2,0.14f*D,lo[2]+std::min(0.0f,oz),s+strip/2,wain-0.02f*D,hi[2]+std::max(0.0f,oz),trim);
                    else m.Box(lo[0]+std::min(0.0f,ox),0.14f*D,s-strip/2,hi[0]+std::max(0.0f,ox),wain-0.02f*D,s+strip/2,trim);
                }
            };
            wall({-rx,0,rz},{rx,0,rz},{0,0,-1});
            wall({rx,0,-rz},{-rx,0,-rz},{0,0,1});
            wall({-rx,0,-rz},{-rx,0,rz},{1,0,0});
            wall({rx,0,rz},{rx,0,-rz},{-1,0,0});
            // Behind her, to either side: windows bright with the day.
            for(float side:{-1.0f,1.0f})
            {
                const float cx=side*3.6f*D,sx=1.1f*D,y0=wain+0.35f*D,y1=ceiling-0.45f*D,z=rz-0.03f*D,bar=0.06f*D,edge=0.12f*D;
                m.Quad({cx-sx,y0,z},{cx+sx,y0,z},{cx+sx,y1,z},{cx-sx,y1,z},{0,0,-1},glass);
                const float zf=z-0.05f*D;
                m.Box(cx-sx-edge,y0-edge,zf,cx-sx,y1+edge,z,sash);
                m.Box(cx+sx,y0-edge,zf,cx+sx+edge,y1+edge,z,sash);
                m.Box(cx-sx,y1,zf,cx+sx,y1+edge,z,sash);
                m.Box(cx-sx,y0-edge,zf,cx+sx,y0,z,sash);
                m.Box(cx-bar/2,y0,zf,cx+bar/2,y1,z,sash);
                for(float t:{1.0f/3,2.0f/3})m.Box(cx-sx,y0+(y1-y0)*t-bar/2,zf,cx+sx,y0+(y1-y0)*t+bar/2,z,sash);
                m.grain=0;m.grainScale=0.6f;
                m.Box(cx-sx-1.6f*edge,y0-edge-0.06f*D,z-0.22f*D,cx+sx+1.6f*edge,y0-edge,z,trim);   // the sill
            }
            // On the side walls: framed pictures.
            {
                const float tints[4][2][4]={{{0.36f,0.58f,0.80f,1},{0.40f,0.62f,0.38f,1}},{{0.92f,0.66f,0.42f,1},{0.52f,0.36f,0.46f,1}},
                                            {{0.62f,0.78f,0.86f,1},{0.84f,0.86f,0.90f,1}},{{0.86f,0.48f,0.36f,1},{0.30f,0.34f,0.52f,1}}};
                const float mount[4]={0.95f,0.94f,0.90f,1};
                int n=0;
                for(float side:{-1.0f,1.0f})
                    for(float cz:{-2.5f*D,2.5f*D})
                    {
                        const float x=side*(rx-0.03f*D),nx=-side,hw=0.75f*D,y0=wain+0.55f*D,y1=y0+1.1f*D,f=0.10f*D,m0=0.12f*D;
                        const auto& tint=tints[n++%4];
                        m.grain=1;m.grainScale=0.4f;
                        const float fx0=std::min(x,x+nx*0.06f*D),fx1=std::max(x,x+nx*0.06f*D);
                        m.Box(fx0,y0-f,cz-hw-f,fx1,y1+f,cz+hw+f,trim);
                        const float px=x+nx*0.065f*D;
                        m.Face({px,y0,cz-hw},{px,y0,cz+hw},{px,y1,cz+hw},{px,y1,cz-hw},{nx,0,0},mount);
                        const float mid=y0+m0+(y1-y0-2*m0)*0.45f,pxi=px+nx*0.004f*D;
                        m.Face({pxi,mid,cz-hw+m0},{pxi,mid,cz+hw-m0},{pxi,y1-m0,cz+hw-m0},{pxi,y1-m0,cz-hw+m0},{nx,0,0},tint[0]);
                        m.Face({pxi,y0+m0,cz-hw+m0},{pxi,y0+m0,cz+hw-m0},{pxi,mid,cz+hw-m0},{pxi,mid,cz-hw+m0},{nx,0,0},tint[1]);
                    }
            }
            m.grain=0;m.grainScale=1;
            // The table: a blue top with white lines round its edge and down its middle, a dark
            // frame under it, metal legs.
            const float blue[4]={0.10f,0.30f,0.56f,1},side[4]={0.06f,0.18f,0.36f,1},white[4]={0.95f,0.96f,0.98f,1};
            const float under[4]={0.10f,0.11f,0.13f,1},legs[4]={0.30f,0.32f,0.36f,Metal};
            const float thick=TopThick*k,line=0.02f*k,y=top+0.0015f*k;
            m.Box(-W,top-thick,-L,W,top,L,side);
            m.Quad({-W,top,-L},{W,top,-L},{W,top,L},{-W,top,L},{0,1,0},blue);
            m.Box(-W,top,-L,W,y,-L+line,white);
            m.Box(-W,top,L-line,W,y,L,white);
            m.Box(-W,top,-L,-W+line,y,L,white);
            m.Box(W-line,top,-L,W,y,L,white);
            m.Box(-0.004f*k,top,-L,0.004f*k,y,L,white);
            const float in=0.25f*k,rail=0.05f*k;
            m.Box(-W+in,top-thick-rail,-L+in,W-in,top-thick,L-in,under);
            for(float x:{-W+in,W-in-rail})
                for(float z:{-L+in,L-in-rail,-rail/2})m.Box(x,0,z,x+rail,top-thick-rail,z+rail,legs);
            // The net: its posts, a white band along its top, and its strings (seen through).
            const float high=rules.net*k,out=0.1525f*k,string=0.0025f*k;
            const float post[4]={0.16f,0.17f,0.19f,1},cord[4]={0.14f,0.17f,0.20f,1};
            for(float s:{-1.0f,1.0f})m.Box(s*(W+out)-0.012f*k,top-thick,-0.012f*k,s*(W+out)+0.012f*k,top+high,0.012f*k,post);
            m.Box(-W-out,top+high-0.015f*k,-0.003f*k,W+out,top+high,0.003f*k,white);
            for(float x=-W-out+0.03f*k;x<W+out;x+=0.03f*k)
            {
                m.Quad({x-string,top,0},{x+string,top,0},{x+string,top+high,0},{x-string,top+high,0},{0,0,-1},cord);
                m.Quad({x-string,top,0},{x+string,top,0},{x+string,top+high,0},{x-string,top+high,0},{0,0,1},cord);
            }
            for(float h=0.03f*k;h<high-0.02f*k;h+=0.03f*k)
            {
                m.Quad({-W-out,top+h-string,0},{W+out,top+h-string,0},{W+out,top+h+string,0},{-W-out,top+h+string,0},{0,0,-1},cord);
                m.Quad({-W-out,top+h-string,0},{W+out,top+h-string,0},{W+out,top+h+string,0},{-W-out,top+h+string,0},{0,0,1},cord);
            }
            return std::move(m.out);
        }

        XMFLOAT3 World(Vec3 p){return {p.x*state.k,state.top+p.z*state.k,p.y*state.k};}

        // Your racket's hand: 1 on the right of the table, -1 on the left.
        float YourHand(){return state.her.leftHanded?-1.0f:1.0f;}

        // What moves: the ball and its shadow, where it will come down on your half, the two rackets.
        std::vector<MascotRenderer::GeometryVertex> BuildPieces()
        {
            const auto& g=game();
            const auto& rules=g.Rule();
            const float k=state.k,top=state.top;
            Mesh m;
            using V=Mesh::V;
            TableTennis::Ball ball=g.TheBall();
            if(state.ballOnHand)ball.p=state.ballShown;   // (her serve: on her palm, then thrown up)
            const bool over=std::abs(ball.p.x)<=rules.width/2&&std::abs(ball.p.y)<=rules.length/2&&ball.p.z>=0;
            {
                // Its shadow, straight under it: on the table, or on the floor.
                const float shade[4]={0.03f,0.12f,0.26f,1},floorShade[4]={0.10f,0.16f,0.14f,1};
                const float r=rules.ball*k*(1.1f+std::max(0.0f,ball.p.z)*0.8f);
                if(over)m.Ring(ball.p.x*k,ball.p.y*k,top+0.003f*k,0,r,shade,16);
                else if(ball.p.z>-rules.height+rules.ball*1.5f)m.Ring(ball.p.x*k,ball.p.y*k,0.02f*state.upper,0,r,floorShade,16);
            }
            Vec mark;
            if(g.Mark(mark)&&g.LastHitter()==Her&&g.Stage()<2)
            {
                const float ring[4]={0.62f,0.86f,1.0f,1};
                m.Ring(mark.x*k,mark.y*k,top+0.0025f*k,0.045f*k,0.062f*k,ring,24);
            }
            {
                const XMFLOAT3 p=World(ball.p);
                const float orange[4]={1.0f,0.66f,0.18f,1};
                m.Sphere({p.x,p.y,p.z},rules.ball*k,orange);
            }
            const float red[4]={0.82f,0.14f,0.16f,1},black[4]={0.10f,0.10f,0.12f,1};
            {
                // Yours: forehand on your racket hand's side of the table, turned over to the
                // backhand on the other; a swing takes it forward (a drive up and closed, a cut
                // down and open, a smash far and down).
                using namespace DirectX;
                const Vec at=g.Racket(You);
                const float hand=YourHand(),fore=std::clamp((at.x*hand+0.06f)/0.12f,0.0f,1.0f),f=fore*fore*(3-2*fore);
                const float lean=XMConvertToRadians(-35+85*f);
                XMVECTOR handle=XMVector3Normalize(XMVectorSet(-hand*std::sin(lean),-std::cos(lean),-0.3f,0));
                const XMVECTOR ahead=XMVector3Normalize(XMVectorSubtract(XMVectorSet(0,0,1,0),XMVectorScale(handle,XMVectorGetZ(handle))));
                // Turned over about the handle between forehand and backhand.
                XMVECTOR face=XMVector3Transform(ahead,XMMatrixRotationAxis(handle,(1-f)*XM_PI));
                Vec3 middle{at.x+state.yourShift,at.y,state.yourHeight};
                const float age=g.SwingAge(You);
                if(age<SwingShown)
                {
                    // A swing turns the forearm about the elbow (behind and below the racket, on
                    // the racket hand's side): drawn back first, then swept through the ball and
                    // on across the body (a backhand: out from the body), and back. A drive
                    // rises and closes over the ball, a cut comes down under it with the face
                    // open, a smash starts high and comes down hard.
                    const Stroke stroke=g.SwingStroke(You);
                    const float u=age/SwingShown;
                    const float back=stroke==Stroke::Smash?-50.0f:stroke==Stroke::Cut?-25.0f:-38.0f;
                    const float through=stroke==Stroke::Smash?95.0f:stroke==Stroke::Cut?50.0f:80.0f;
                    auto ease=[](float t){t=std::clamp(t,0.0f,1.0f);return t*t*(3-2*t);};
                    // Degrees, and how far through the stroke (0 drawn back .. 1 followed through).
                    const float degrees=u<0.18f?back*ease(u/0.18f):u<0.58f?back+(through-back)*ease((u-0.18f)/0.40f):through*(1-ease((u-0.58f)/0.42f));
                    const float along=(degrees-back)/(through-back);
                    const float side=hand*(2*f-1);   // the forehand sweeps across the body, the backhand away from it
                    const float sweep=-XMConvertToRadians(degrees)*side;
                    // The racket from the elbow, at rest: ahead of it and a little up.
                    const XMVECTOR arm=XMVectorSet(-0.06f*side,0.10f,0.30f,0);
                    const XMMATRIX turn=XMMatrixRotationY(sweep);
                    const XMVECTOR moved=XMVectorSubtract(XMVector3TransformNormal(arm,turn),arm);
                    const float rise=stroke==Stroke::Smash?0.22f-0.34f*along:stroke==Stroke::Cut?0.10f-0.20f*along:-0.06f+0.20f*along;
                    const float shown=u<0.18f?ease(u/0.18f):u<0.58f?1.0f:1-ease((u-0.58f)/0.42f);
                    middle.x+=XMVectorGetX(moved);
                    middle.y+=XMVectorGetZ(moved);
                    middle.z+=XMVectorGetY(moved)+rise*shown;
                    const float tip=XMConvertToRadians(stroke==Stroke::Smash?40.0f:stroke==Stroke::Cut?-50.0f:28.0f)*shown*(0.3f+0.7f*along);
                    const XMMATRIX whole=XMMatrixMultiply(XMMatrixRotationX(tip),turn);
                    handle=XMVector3TransformNormal(handle,whole);
                    face=XMVector3TransformNormal(face,whole);
                }
                const XMFLOAT3 p=World(middle);
                // The forehand's rubber is red, the backhand's black: seen from behind, the other one.
                m.Racket({p.x,p.y,p.z},{XMVectorGetX(handle),XMVectorGetY(handle),XMVectorGetZ(handle)},
                         {XMVectorGetX(face),XMVectorGetY(face),XMVectorGetZ(face)},k,red,black);
            }
            if(state.gripped)
            {
                // Hers, as her hand holds it: the handle in her fist, the red rubber toward you.
                using namespace DirectX;
                const XMVECTOR along=XMVector3Normalize(XMLoadFloat3(&state.herAlong));
                // Its face is her palm's (the forehand's rubber red, on the palm's side).
                const XMVECTOR flat=XMLoadFloat3(&state.herPalm);
                XMVECTOR face=XMVectorSubtract(flat,XMVectorScale(along,XMVectorGetX(XMVector3Dot(flat,along))));
                if(XMVectorGetX(XMVector3Length(face))<0.1f)face=XMVectorSet(0,0,-1,0);
                face=XMVector3Normalize(face);
                const XMVECTOR handle=XMVectorNegate(along);
                // (On her backhand the other rubber is toward you.)
                m.Racket({state.herBlade.x,state.herBlade.y,state.herBlade.z},{XMVectorGetX(handle),XMVectorGetY(handle),XMVectorGetZ(handle)},
                         {XMVectorGetX(face),XMVectorGetY(face),XMVectorGetZ(face)},k,red,black);
            }
            return std::move(m.out);
        }

        constexpr double TrailSeconds=0.40;

        // A strip of light for the renderer's glowing pass (soft across: bright along its
        // middle), from a to b, half width w across the view.
        void LightStrip(std::vector<MascotRenderer::GeometryVertex>& out,XMFLOAT3 a,XMFLOAT3 b,float w,const float (&ca)[3],const float (&cb)[3])
        {
            using namespace DirectX;
            const XMVECTOR A=XMLoadFloat3(&a),B=XMLoadFloat3(&b);
            const XMVECTOR toEye=XMVectorSubtract(XMLoadFloat3(&state.eye),XMVectorScale(XMVectorAdd(A,B),0.5f));
            XMVECTOR side=XMVector3Cross(XMVectorSubtract(B,A),toEye);
            if(XMVectorGetX(XMVector3LengthSq(side))<1e-10f)return;
            side=XMVectorScale(XMVector3Normalize(side),w);
            auto v=[&](FXMVECTOR p,float u,const float (&c)[3]){
                MascotRenderer::GeometryVertex g{};
                XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(g.position),p);
                g.normal[1]=1;
                g.color[0]=c[0];g.color[1]=c[1];g.color[2]=c[2];g.color[3]=Glow;
                g.uv[0]=u;g.uv[2]=-9;
                out.push_back(g);
            };
            const XMVECTOR a0=XMVectorSubtract(A,side),a1=XMVectorAdd(A,side),b0=XMVectorSubtract(B,side),b1=XMVectorAdd(B,side);
            v(a0,0,ca);v(a1,1,ca);v(b1,1,cb);
            v(a0,0,ca);v(b1,1,cb);v(b0,0,cb);
        }

        // The ball's trail: behind it over the last moments, fading, as bright as the ball was
        // fast there (a slow cut leaves hardly any, a smash a wide golden one). Yours pale
        // blue, hers pink.
        std::vector<MascotRenderer::GeometryVertex> BuildGlow()
        {
            std::vector<MascotRenderer::GeometryVertex> out;
            const auto& trail=state.trail;
            const float k=state.k;
            for(size_t i=1;i<trail.size();++i)
            {
                const auto& a=trail[i-1];
                const auto& b=trail[i];
                const float fade=1-static_cast<float>((state.shown-(a.at+b.at)/2)/TrailSeconds);
                const float fast=std::clamp(((a.speed+b.speed)/2-2.0f)/7.0f,0.0f,1.0f);
                const float bright=fast*std::clamp(fade,0.0f,1.0f)*(b.kind==2?1.0f:0.8f);
                if(bright<0.02f)continue;
                static const float tints[3][3]={{0.50f,0.80f,1.0f},{1.0f,0.55f,0.78f},{1.0f,0.82f,0.32f}};
                const float* t=tints[b.kind];
                const float wide[3]={t[0]*bright*0.7f,t[1]*bright*0.7f,t[2]*bright*0.7f};
                const float core[3]={(0.5f+0.5f*t[0])*bright,(0.5f+0.5f*t[1])*bright,(0.5f+0.5f*t[2])*bright};
                const float w=(0.012f+0.030f*fade)*(b.kind==2?1.6f:1.0f)*k;
                LightStrip(out,World(a.p),World(b.p),w,wide,wide);
                LightStrip(out,World(a.p),World(b.p),w*0.35f,core,core);
            }
            return out;
        }

        // The sparkle of a smash: glitter drifting off its trail, and the sparks thrown off
        // where it was struck.
        std::vector<MascotRenderer::Particle> BuildSparks()
        {
            std::vector<MascotRenderer::Particle> out;
            const float k=state.k,t=static_cast<float>(state.shown);
            std::uniform_real_distribution<float> u(-1,1);
            for(const auto& at:state.trail)
            {
                if(at.kind!=2)continue;
                const float age=static_cast<float>((state.shown-at.at)/TrailSeconds);
                if(age<0||age>1)continue;
                const std::uint32_t seed=static_cast<std::uint32_t>(at.at*1000);
                for(int n=0;n<3;++n)
                {
                    std::mt19937 r(seed*2654435761u+n*40503u);
                    const float spread=0.015f+0.10f*age;
                    MascotRenderer::Particle q;
                    const XMFLOAT3 p=World({at.p.x+u(r)*spread,at.p.y+u(r)*spread,at.p.z+u(r)*spread-age*age*0.10f});
                    q.position={p.x,p.y,p.z};
                    const float twinkle=0.5f+0.5f*std::sin(t*22+seed*1.3f+n*2.1f);
                    q.size=(0.010f+0.016f*twinkle)*k*(1-0.5f*age);
                    q.color={1.0f,0.92f,0.55f+0.45f*twinkle,(1-age)*(0.6f+0.4f*twinkle)};
                    q.glow=true;
                    q.angle=t*3+seed;
                    out.push_back(q);
                }
            }
            for(const auto& sp:state.sparks)
            {
                const float age=sp.age/sp.life;
                MascotRenderer::Particle q;
                const XMFLOAT3 p=World(sp.p);
                q.position={p.x,p.y,p.z};
                const float twinkle=0.6f+0.4f*std::sin(t*30+sp.turn*7);
                q.size=sp.size*k*(1-0.6f*age)*(0.7f+0.5f*twinkle);
                q.color={1.0f,0.95f,0.70f+0.30f*twinkle,(1-age*age)*twinkle};
                q.angle=sp.turn+t*4;
                q.nearer=true;
                out.push_back(q);
            }
            return out;
        }

        // A smash struck at p: stars thrown off every way, mostly on the way the ball goes.
        void Sparkle(Vec3 p,Vec3 way)
        {
            std::uniform_real_distribution<float> u(-1,1);
            const float fast=std::max(0.001f,std::sqrt(way.x*way.x+way.y*way.y+way.z*way.z));
            for(int i=0;i<34;++i)
            {
                State::Spark sp;
                sp.p=p;
                const float speed=0.5f+1.4f*std::abs(u(state.wordsRandom));
                sp.v={u(state.wordsRandom)*speed+way.x/fast*0.8f,u(state.wordsRandom)*speed+way.y/fast*0.8f,u(state.wordsRandom)*speed*0.8f+0.4f};
                sp.life=0.28f+0.30f*std::abs(u(state.wordsRandom));
                sp.size=0.014f+0.022f*std::abs(u(state.wordsRandom));
                sp.turn=u(state.wordsRandom)*3;
                state.sparks.push_back(sp);
            }
        }

        // Her model's transform: standing at her end, facing you, her walks' sideways shift taken out.
        DirectX::XMMATRIX HerWorld(MascotModel& model){return DirectX::XMMatrixTranslation(state.herX-model.RootOffset(),0,state.herZ);}

        // The camera: from behind your end, a little above, looking down the table at her.
        void PlaceCamera()
        {
            using namespace DirectX;
            const auto& c=state.camera;
            const float D=state.upper;
            const XMVECTOR target=XMVectorSet(c.x*D,state.top+(0.30f+c.y)*D,(-0.40f+c.z)*D,0);
            const XMVECTOR toEye=XMVectorSet(std::sin(c.yaw)*std::cos(c.pitch),std::sin(c.pitch),-std::cos(c.yaw)*std::cos(c.pitch),0);
            const XMVECTOR eye=XMVectorAdd(target,XMVectorScale(toEye,c.distance*D));
            XMStoreFloat3(&state.eye,eye);
            XMStoreFloat3(&state.look,target);
            state.renderer->SetCamera(eye,target,XMConvertToRadians(42.0f));
        }

        // The point under a pixel of the view at the height a racket is held over the table
        // (false: it looks above it).
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
            const float along=(state.top+0.18f*state.k-XMVectorGetY(eye))/dy;
            const XMVECTOR hit=XMVectorAdd(eye,XMVectorScale(ray,along));
            at={XMVectorGetX(hit)/state.k,XMVectorGetZ(hit)/state.k};
            return true;
        }

        // The table as a solid in her way (MascotModel::SetSolids): its top, in her model's space
        // (she is only moved, not turned).
        void SetTableSolids(MascotModel& model)
        {
            const auto& rules=game().Rule();
            const float k=state.k,W=rules.width/2*k,L=rules.length/2*k;
            const float dx=model.RootOffset()-state.herX,dz=-state.herZ;
            MascotModel::Solid s;
            s.middle={dx,state.top-(TopThick+0.05f)*k/2,dz};
            s.half={W,(TopThick+0.05f)*k/2,L};
            model.SetSolids({s});
        }

        std::array<float,3> ModelPoint(MascotModel& model,const XMFLOAT3& p)
        {
            return {p.x+model.RootOffset()-state.herX,p.y,p.z-state.herZ};
        }

        // Her measures, once, as the 3D table opens: the table made to her size, and where she stands.
        void MeasureHer(MascotModel& model)
        {
            std::array<float,3> lo{},hi{},legLo{},legHi{},head{};
            if(model.StandardBoneBounds(lo,hi)&&model.LegBounds(legLo,legHi)&&model.HeadPosition(head)&&head[1]>legHi[1])
            {
                state.hips=std::max(legHi[1]-lo[1],0.1f);
                state.upper=head[1]-legHi[1];
            }
            const auto& rules=game().Rule();
            const float D=state.upper;
            // A full-size table to her (2.74 m long to a person 1.6 m tall), its top a little
            // under her hips.
            state.k=D/0.75f;
            state.herX=0;
            state.top=0.88f*state.hips;
            // Behind her end, where the ball comes up to her hand.
            state.herZ=(rules.length/2+0.95f)*state.k;
            model.SetActivity(Activity::Idle);
            model.SetAutoGestures(false);
            // A gesture or clip still playing would hold an arm where her hand's offset from the
            // racket is first learned: let it go first, all the way.
            model.EndGestures();
            model.StopMotion();
            for(int i=0;i<15||(i<90&&model.MotionPlaying());++i)model.Update(1/30.0);
            model.ClearArmStops();
            model.ResetPhysics();
            Log(L"table tennis: hips %.1f, upper body %.1f, table %.1f long at %.1f, she stands at %.1f",
                state.hips,state.upper,rules.length*state.k,state.top,state.herZ);
        }

        // ---- Her words and the watchers' (docs/TableTennis.md, しゃべる・反応) ----

        constexpr ULONGLONG WordsHold=3000;   // a lighter word does not cut in on a weightier one this soon

        const std::wstring& Pick(const std::vector<std::wstring>& lines)
        {
            return lines[state.wordsRandom()%lines.size()];
        }

        // Her line in the bubble beside her, for as long as it takes to read (or, with a voice file
        // of its own, as long as the voice plays).
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
            w.weight=weight;
            w.since=now;
            w.told+=L"  she says: "+line.text+(line.voice.empty()?L"":L" ["+line.voice+L"]")+L"\n";
            const ULONGLONG reading=static_cast<ULONGLONG>(CharSeconds()*1000*line.text.size())+2400+90*static_cast<ULONGLONG>(line.text.size());
            w.until=now+(w.voiced?std::max(reading,static_cast<ULONGLONG>(state.voice.Seconds()*1000)+1800):reading);
            if(!line.text.empty()){w.recent.push_back(line.text);if(w.recent.size()>4)w.recent.erase(w.recent.begin());}
        }

        // The next scene not yet in her own words, asked of the LLM (when there is one left).
        void Prefetch()
        {
            if(state.prefetching||!state.her.prefetch)return;
            const std::wstring ask=state.lines.NextAsk();
            if(!ask.empty())state.prefetching=state.her.prefetch(ask);
        }

        // Something happened: she speaks of it at once (a line of the scene, chance of the
        // time), and those watching hear of it. seen: what happened, told as to one watching;
        // scene: the scene of her lines (null: she says nothing); theirs: the watchers'.
        void Happened(int weight,const wchar_t* scene,double chance,const std::wstring& seen,const std::vector<std::wstring>& theirs)
        {
            const auto& g=*state.game;
            state.words.told+=Fmt(L"  {0} s, weight {1}: {2}\n",{std::to_wstring(static_cast<int>(g.Clock())),std::to_wstring(weight),seen});
            if(state.her.onEvent&&!theirs.empty())
                state.her.onEvent(seen+Fmt(T(L"（点数は{0}が{1}点、持ち主が{2}点、{3}点先取）",L" ({0} has {1}, the owner {2}; first to {3})"),
                                           {HerName(),std::to_wstring(g.Score(Her)),std::to_wstring(g.Score(You)),std::to_wstring(g.Rule().toWin)}),weight,Pick(theirs));
            if(!scene)return;
            auto& w=state.words;
            if(!w.text.empty()&&GetTickCount64()-w.since<WordsHold&&weight<w.weight)return;
            if(!state.lines.Roll(scene,chance,state.wordsRandom))return;
            GameLine line;
            if(state.lines.Pick(scene,w.recent,state.wordsRandom,line))SayLine(line,weight);
        }

        // Her face for a moment, after a point: pleased, or a little put out.
        void Face(Expression e,double seconds)
        {
            if(!state.her.model)return;
            state.her.model->SetExpression(e,false);
            state.faceUntil=state.game->Clock()+seconds;
        }

        // What happened in the game since the last step, for the sounds, her words and her
        // reactions: its start, a point (how it was won), a match point, its end.
        void Notice(const std::vector<TableTennis::Event>& events)
        {
            using TableTennis::Event;
            const auto& g=*state.game;
            auto& w=state.words;
            if(!w.text.empty()&&GetTickCount64()>w.until)w.text.clear();
            if(state.faceUntil>0&&g.Clock()>state.faceUntil){state.faceUntil=0;if(state.her.model)state.her.model->SetExpression(Expression::Neutral,false);}
            const std::wstring name=HerName();
            // The start: once her lines asked for ahead are there (a few seconds at most).
            if(!state.started&&(!state.prefetching||g.Clock()>4))
            {
                state.started=true;
                Happened(7,L"始まり",1,
                         Fmt(T(L"{0}と、このパソコンの持ち主の卓球の試合が始まる。",L"A table tennis game between {0} and the owner of this computer is about to begin."),{name}),
                         {T(L"がんばれー！",L"Good luck, you two!"),T(L"どっちが勝つかな？",L"Who's going to win?")});
            }
            for(const Event& e:events)
            {
                if(e.kind==Event::Hit)
                {
                    Play(e.stroke==Stroke::Smash?Sound::Smash:Sound::Racket);
                    if(e.stroke==Stroke::Smash)Sparkle(g.TheBall().p,g.TheBall().v);
                    if(e.side==Her&&state.gripped)
                    {
                        // Diagnostics: her blade against where the game has her racket.
                        const Vec at=g.Racket(Her);
                        state.strokeOff+=std::hypot(state.herBlade.x/state.k-at.x,state.herBlade.z/state.k-at.y);
                        state.strokeAlong+=state.herBlade.z/state.k-at.y;
                        state.strokeAcross[state.backhand?1:0]+=state.herBlade.x/state.k-at.x;
                        ++state.strokeSide[state.backhand?1:0];
                        ++state.strokes;
                    }
                }
                else if(e.kind==Event::Bounce){if(e.strength>0.8f)Play(Sound::Table);}
                else if(e.kind==Event::Net)Play(Sound::Net);
                if(e.kind!=Event::Point)continue;
                const bool hers=e.side==Her,winner=e.reason==Reason::Missed,smash=winner&&e.stroke==Stroke::Smash,longRally=e.rally>=10;
                Face(hers?Expression::Smile:Expression::Troubled,2.2);
                if(g.Winner()>=0)continue;   // the end is spoken of below
                const std::wstring how=e.reason==Reason::Net?T(L"ネットにかけて",L"into the net"):e.reason==Reason::Out?T(L"アウトにして",L"off the table"):L"";
                if(hers&&winner)
                    Happened(smash||longRally?7:5,smash?L"スマッシュを決めた":longRally?L"長い打ち合いで決めた":L"決めた",smash||longRally?1:0.55,
                             Fmt(smash?T(L"卓球で、{0}がスマッシュを決めて1点取った。",L"In the table tennis game, {0} won a point with a smash."):
                                       T(L"卓球で、{0}が打ち勝って1点取った。",L"In the table tennis game, {0} won a point."),{name}),
                             {Fmt(T(L"{0}、ナイス！",L"Nice one, {0}!"),{name}),T(L"おお、決まった！",L"Ooh, that's in!")});
                else if(hers)
                    Happened(4,L"相手のミス",0.5,
                             Fmt(T(L"卓球で、持ち主が{1}、{0}に1点入った。",L"In the table tennis game, the owner hit {1}: a point for {0}."),{name,how}),
                             {T(L"ドンマイ！",L"Never mind!"),T(L"おしい！",L"So close!")});
                else if(winner)
                    Happened(smash||longRally?7:5,smash?L"スマッシュを決められた":longRally?L"長い打ち合いで決められた":L"決められた",smash||longRally?1:0.55,
                             smash?T(L"卓球で、持ち主がスマッシュを決めて1点取った。",L"In the table tennis game, the owner won a point with a smash."):
                                   T(L"卓球で、持ち主が打ち勝って1点取った。",L"In the table tennis game, the owner won a point."),
                             {T(L"おお、決まった！",L"Oh, that's in!"),T(L"ナイスショット！",L"Nice shot!")});
                else
                    Happened(4,L"自分のミス",0.5,
                             Fmt(T(L"卓球で、{0}が{1}、持ち主に1点入った。",L"In the table tennis game, {0} hit {1}: a point for the owner."),{name,how}),
                             {Fmt(T(L"{0}、ドンマイ！",L"Never mind, {0}!"),{name}),T(L"あー、おしい！",L"Aw, so close!")});
                // Deuce; one more to win.
                const int toWin=g.Rule().toWin;
                if(g.Score(Her)==g.Score(You)&&g.Score(Her)>=toWin-1)
                {
                    state.matchPoint={};
                    if(!state.deuceSaid)
                    {
                        state.deuceSaid=true;
                        Happened(6,L"デュース",1,T(L"卓球で、10対10のデュースになった。ここからは2点差がつくまで続く。",L"The table tennis game is at deuce, 10 all: from here it goes on until one leads by two."),
                                 {T(L"デュースだ！",L"Deuce!"),T(L"手に汗にぎるね！",L"This is tense!")});
                    }
                }
                for(int side:{Her,You})
                    if(g.Score(side)>=toWin-1&&g.Score(side)>g.Score(1-side)&&!state.matchPoint[side])
                    {
                        state.matchPoint[side]=true;
                        if(side==Her)Happened(6,L"あと1点で勝ち",1,
                                              Fmt(T(L"卓球で、{0}があと1点で勝つ。",L"In the table tennis game, {0} needs one more point to win."),{name}),{T(L"マッチポイントだ！",L"Match point!")});
                        else Happened(6,L"あと1点で負け",1,
                                      T(L"卓球で、持ち主があと1点で勝つ。",L"In the table tennis game, the owner needs one more point to win."),{T(L"マッチポイント！",L"Match point!")});
                    }
            }
            if(g.Winner()>=0&&!state.overSeen)
            {
                state.overSeen=true;
                const bool won=g.Winner()==Her;
                const std::wstring score=Fmt(T(L"あなた{0}点、相手{1}点",L"you {0}, they {1}"),{std::to_wstring(g.Score(Her)),std::to_wstring(g.Score(You))});
                if(won)Happened(9,L"勝った",1,
                                Fmt(T(L"{0}と持ち主の卓球の試合が終わり、{0}が勝った。",L"The table tennis game between {0} and the owner is over: {0} won."),{name}),
                                {T(L"おつかれさま！いい勝負だったね。",L"Well played, both of you!"),Fmt(T(L"{0}の勝ちだ！",L"{0} wins!"),{name})});
                else Happened(9,L"負けた",1,
                              Fmt(T(L"{0}と持ち主の卓球の試合が終わり、持ち主が勝った。",L"The table tennis game between {0} and the owner is over: the owner won."),{name}),
                              {T(L"おつかれさま！いい勝負だったね。",L"Well played, both of you!"),T(L"勝った！おめでとう！",L"You won! Congratulations!")});
                state.afterword=Fmt(T(L"いま、話し相手と卓球を最後までして、デスクトップに戻ってきたところ。結果は{0}（{1}、いちばん長い打ち合いは{2}回）。その感想を、話し相手にひと言（1〜2文）",
                                      L"You have just played table tennis to the end with them and come back to the desktop. {0} ({1}; the longest rally was {2} strokes). Tell them what you thought of it (one or two sentences)"),
                                    {won?T(L"あなたの勝ち",L"You won"):T(L"あなたの負け",L"You lost"),score,std::to_wstring(g.LongestRally())});
                state.afterwordLine=won?T(L"卓球、楽しかったね！",L"Table tennis was fun!"):T(L"くやしいなあ、次は勝つよ！",L"So close... I'll win next time!");
                state.overAt=g.Clock();
                state.cheered=false;
            }
        }

        // ---- The words over the table ----------------------------------------------------

        void DrawHud(Graphics& g,int w,int h,float s)
        {
            const auto& t=game();
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAlias);
            const Color white(255,240,242,248),soft(255,232,238,250),gold(255,246,210,120);
            const float W=static_cast<float>(w),H=static_cast<float>(h);
            {
                GraphicsPath path;
                RoundRect(path,RectF(8*s,4*s,W-16*s,52*s),10*s);
                SolidBrush back(Color(185,14,20,34));
                g.FillPath(&back,&path);
            }
            // The score: hers on the left, yours on the right; a dot by whoever serves.
            const bool live=t.Now()!=Phase::Over;
            const std::wstring dot=L"● ";
            Text(g,(live&&t.Server()==Her?dot:L"")+HerName(),RectF(16*s,8*s,W/2-60*s,24*s),15*s,soft,false,StringAlignmentNear);
            Text(g,(live&&t.Server()==You?dot:L"")+std::wstring(T(L"あなた",L"You")),RectF(W/2+44*s,8*s,W/2-60*s,24*s),15*s,soft,false,StringAlignmentFar);
            Text(g,Fmt(T(L"相手の強さ：{0}（1〜3キー）　{1}（Hキー）",L"Opponent: {0} (keys 1-3)   {1} (H)"),{LevelName(state.her.level),HandName()}),
                 RectF(W/2+44*s,30*s,W/2-60*s,22*s),12*s,soft,false,StringAlignmentFar);
            Text(g,std::to_wstring(t.Score(Her))+L"  -  "+std::to_wstring(t.Score(You)),RectF(0,4*s,W,46*s),30*s,white,true);
            Text(g,Fmt(T(L"{0}点先取（10対10からは2点差）",L"First to {0} (two clear from 10 all)"),{std::to_wstring(t.Rule().toWin)}),
                 RectF(16*s,30*s,W/2-60*s,22*s),12*s,soft,false,StringAlignmentNear);
            // How to play, along the bottom.
            {
                const std::wstring keys=T(L"左クリック：打つ　右クリック：カット　Space・戻るボタン：スマッシュ（高い球に）　P：一時停止　Esc：やめる",
                                          L"Left click: strike   Right click: cut   Space / back button: smash (a high ball)   P: pause   Esc: stop");
                const float bh=26*s;
                SolidBrush back(Color(120,14,20,34));
                g.FillRectangle(&back,RectF(0,H-bh,W,bh));
                Text(g,keys,RectF(0,H-bh,W,bh),12.5f*s,soft);
            }
            // Her words: a bubble just right of her head, as wide as they need, its tail toward her.
            const float mid=H*0.36f;
            if(!state.words.text.empty())
            {
                const std::wstring& said=state.words.text;
                const float most=std::max(160*s,W*0.36f);
                const WordsBubble bubble(g,said,s,most);
                const float bw=bubble.Width(),bh=bubble.Height();
                const bool beside=state.besideHead.x>=0;
                const float x=std::clamp(beside?static_cast<float>(state.besideHead.x):W/2+70*s,14*s,W-14*s-bw);
                const float headY=beside?static_cast<float>(state.headScreen.y):70*s+bh/2;
                const float y=std::clamp(headY-bh*0.6f,64*s,std::max(64*s,mid-bh));
                const float ty=std::clamp(headY,y+12*s,y+bh-12*s);
                bubble.Draw(g,x,y,ty,ShownWords());
            }
            auto banner=[&](const std::wstring& big,const std::wstring& note,float at){
                const float bw=std::min(W-40*s,440*s),bh=note.empty()?64*s:96*s;
                const RectF box(W/2-bw/2,at-bh/2,bw,bh);
                GraphicsPath path;
                RoundRect(path,box,12*s);
                SolidBrush back(Color(215,16,22,36));
                g.FillPath(&back,&path);
                Text(g,big,RectF(box.X,box.Y+8*s,box.Width,note.empty()?box.Height-16*s:48*s),26*s,gold,true);
                if(!note.empty())Text(g,note,RectF(box.X,box.Y+56*s,box.Width,30*s),14*s,white);
            };
            const ULONGLONG now=GetTickCount64();
            if(t.Now()==Phase::Over)
            {
                std::wstring note=std::to_wstring(t.Score(Her))+L" - "+std::to_wstring(t.Score(You));
                note+=T(L"　いちばん長い打ち合い ",L"   longest rally ")+std::to_wstring(t.LongestRally());
                note+=T(L"　Enterでもう一度・Escで閉じる",L"   Enter: again, Esc: close");
                banner(t.Winner()==You?T(L"あなたの勝ち！",L"You win!"):HerName()+T(L"の勝ち",L" wins"),note,mid);
            }
            else if(state.asking)
            {
                // Two buttons under the question, for the mouse too (Y and N as before).
                const float bw=std::min(W-40*s,420*s),bh=120*s;
                const RectF box(W/2-bw/2,mid-bh/2,bw,bh);
                GraphicsPath path;
                RoundRect(path,box,12*s);
                SolidBrush back(Color(215,16,22,36));
                g.FillPath(&back,&path);
                Text(g,T(L"やめますか？",L"Stop playing?"),RectF(box.X,box.Y+8*s,box.Width,48*s),26*s,gold,true);
                const float gap=16*s,width=(box.Width-3*gap)/2;
                const std::wstring labels[2]={T(L"やめる（Y）",L"Stop (Y)"),T(L"続ける（N）",L"Go on (N)")};
                for(int i=0;i<2;++i)
                {
                    const RectF button(box.X+gap+i*(width+gap),box.Y+62*s,width,44*s);
                    state.askButtons[i]=button;
                    GraphicsPath shape;
                    RoundRect(shape,button,10*s);
                    SolidBrush face(i==0?Color(235,150,46,52):Color(235,46,92,150));
                    g.FillPath(&face,&shape);
                    Text(g,labels[i],RectF(button.X,button.Y+10*s,button.Width,26*s),17*s,white,true);
                }
            }
            else if(now<state.levelShownUntil)banner(T(L"相手の強さ：",L"Opponent: ")+LevelName(state.her.level),L"",mid);
            else if(now<state.handShownUntil)banner(HandName(),T(L"ラケットを持つ手の反対側ではバックハンドになります",L"On the other side of the table the racket turns to the backhand"),mid);
            else if(state.paused)banner(T(L"一時停止",L"Paused"),T(L"Pかクリックで再開",L"P or a click to go on"),mid);
            else if(t.Now()==Phase::Point&&t.Winner()<0)
            {
                // The point just played: how it ended, and whose it is.
                const bool yours=t.LastWinner()==You;
                const std::wstring why=t.LastReason()==Reason::Net?T(L"ネット",L"Net"):t.LastReason()==Reason::Out?T(L"アウト",L"Out"):
                                       t.LastStroke()==Stroke::Smash?T(L"スマッシュ！",L"Smash!"):T(L"ポイント",L"Point");
                banner(why,yours?T(L"あなたに1点",L"A point for you"):Fmt(T(L"{0}に1点",L"A point for {0}"),{HerName()}),H*0.26f);
            }
            else if(t.Now()==Phase::Serve&&t.Server()==You&&t.ServeLeft()<=0)
            {
                const float bw=std::min(W-40*s,460*s),bh=34*s;
                const RectF box(W/2-bw/2,H-26*s-bh-10*s,bw,bh);
                GraphicsPath path;
                RoundRect(path,box,10*s);
                SolidBrush back(Color(200,16,22,36));
                g.FillPath(&back,&path);
                Text(g,T(L"あなたのサーブ：クリックで打つ（右クリックでカットサーブ）",L"Your serve: click to serve (right click: a cut serve)"),box,15*s,gold,true);
            }
        }

        // The words over the 3D table, drawn again only when they change.
        void UpdateOverlay()
        {
            RECT client{};
            GetClientRect(state.window,&client);
            const int w=std::max(1L,client.right),h=std::max(1L,client.bottom);
            const auto& g=game();
            const ULONGLONG now=GetTickCount64();
            const std::wstring shown=std::to_wstring(w)+L"x"+std::to_wstring(h)+L" "+std::to_wstring(g.Score(0))+L"-"+std::to_wstring(g.Score(1))+
                                     L" "+std::to_wstring(static_cast<int>(g.Now()))+std::to_wstring(g.Server())+(g.ServeLeft()<=0?L"r":L"")+
                                     (state.paused?L"p":L"")+(state.asking?L"a":L"")+(now<state.levelShownUntil?L"l":L"")+(now<state.handShownUntil?L"h":L"")+
                                     std::to_wstring(state.her.level)+(state.her.leftHanded?L"L":L"R")+L"|"+ShownWords()+L"|"+
                                     std::to_wstring(state.besideHead.x/12)+L","+std::to_wstring(state.headScreen.y/12);
            if(shown==state.hudShown)return;
            state.hudShown=shown;
            Bitmap canvas(w,h,PixelFormat32bppPARGB);
            {
                Graphics gr(&canvas);
                gr.Clear(Color(0,0,0,0));
                DrawHud(gr,w,h,Scale());
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

        // ---- The window ------------------------------------------------------------------

        void SetLevel()
        {
            const TableTennis::Style manner=TableTennis::LevelStyle(state.her.level);
            state.brain=std::make_unique<TableTennis::Brain>(Her,manner,Seed());
            state.game->SetRacketSpeed(Her,manner.speed);
        }

        void NewGame()
        {
            state.game=std::make_unique<TableTennis::Game>(TableTennis::Rules{},Seed());
            state.game->NewGame(static_cast<int>(Seed()>>4&1));
            state.trail.clear();
            state.sparks.clear();
            state.her.level=std::clamp(state.her.level,1,3);
            SetLevel();
            if(state.autoplay)state.game->SetRacketSpeed(You,8.0f);
            state.paused=state.asking=false;
            state.started=state.overSeen=state.cheered=state.deuceSaid=false;
            state.matchPoint={};
            state.faceUntil=0;
            state.levelShownUntil=state.handShownUntil=0;
            // The last game's word is not left over in this one.
            state.words.text.clear();
            state.afterword.clear();
            state.afterwordLine.clear();
            state.target=state.game->Racket(You);
            state.hudShown.clear();
            QueryPerformanceCounter(&state.last);
        }

        // While you play, the cursor stays in the window, so the racket follows it however far
        // the mouse moves. Let go of whenever play stops (paused, asked, over), the camera is
        // dragged, or the window is left, resized or closed.
        void HoldCursor(bool on)
        {
            RECT area{};
            if(on&&state.window&&GetClientRect(state.window,&area))
            {
                MapWindowPoints(state.window,nullptr,reinterpret_cast<POINT*>(&area),2);
                InflateRect(&area,-2,-2);
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
            state.voice.Stop();
            // Stopped before the game's end: a word on that, back on the desktop.
            if(state.afterword.empty()&&state.game&&state.game->Winner()<0&&state.game->Clock()>3&&state.renderer)
            {
                const auto& g=*state.game;
                state.afterword=Fmt(T(L"いま、話し相手と卓球をしていたが、途中でやめてデスクトップに戻ってきたところ（あなた{0}点、相手{1}点だった）。そのことを話し相手にひと言（1〜2文）",
                                      L"You were playing table tennis with them, but they stopped before the end and you have come back to the desktop (you had {0}, they had {1}). Say a word about it to them (one or two sentences)"),
                                    {std::to_wstring(g.Score(Her)),std::to_wstring(g.Score(You))});
                state.afterwordLine=T(L"また今度、続きしようね。",L"Let's play again another time.");
            }
            if(MascotModel* model=state.her.model)
            {
                model->Hold(HerHand,{},false);
                model->HoldBody(HerHand,0,0,0,0,0);
                model->HoldFree(HerHand,false,{},{});
                model->SetExpression(Expression::Neutral,false);
                model->SetSolids({});
                model->SetAutoGestures(true);
            }
            auto onEnd=std::move(state.onEnd);
            DestroyWindow(state.window);
            if(onEnd)onEnd();
        }

        // Your swing: a drive, a cut or a smash (a serve while you hold the ball).
        void YourSwing(Stroke stroke)
        {
            if(!Playing()||state.autoplay)return;
            game().Swing(You,stroke);
        }

        // The game on by dt: your racket toward the mouse, hers where she means to meet the ball.
        void StepGame(double dt)
        {
            const bool front=GetForegroundWindow()==state.window;
            if(!front&&Playing()&&!state.autoplay)state.paused=true;   // away from the window: it waits
            HoldCursor(Playing()&&!state.autoplay&&!state.dragging&&front&&!IsIconic(state.window));
            if(!Playing()&&state.game->Now()!=Phase::Over)return;
            POINT cursor;
            if(state.autoplay)state.target=state.autoplay->Think(*state.game,dt);
            else if(!state.dragging&&GetCursorPos(&cursor)&&Playing())
            {
                Vec at;
                if(ScreenToClient(state.view,&cursor)&&TableAt(static_cast<float>(cursor.x),static_cast<float>(cursor.y),at))state.target=at;
            }
            const Vec herTarget=state.brain->Think(*state.game,dt);
            state.herGoal=herTarget;
            state.game->Step(dt,state.target,herTarget);
            Notice(state.game->TakeEvents());
        }

        void ChooseLevel(int level)
        {
            state.her.level=level;
            SetLevel();
            state.levelShownUntil=GetTickCount64()+1200;
            state.handShownUntil=0;
            if(state.her.onLevel)state.her.onLevel(level);
        }

        void ChangeHand()
        {
            state.her.leftHanded=!state.her.leftHanded;
            state.handShownUntil=GetTickCount64()+1800;
            state.levelShownUntil=0;
            if(state.her.onHand)state.her.onHand(state.her.leftHanded);
        }

        LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam);

        LRESULT CALLBACK ViewProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            // The mouse's left, right and back buttons play (the window's); the camera: drag
            // with the middle button to turn it around the table (with Shift, to move it), the
            // wheel to come nearer; a middle double-click or F1 puts it back.
            auto& c=state.camera;
            switch(message)
            {
            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
                SetFocus(state.window);
                return Proc(state.window,WM_LBUTTONDOWN,wParam,lParam);
            case WM_RBUTTONDOWN:
            case WM_RBUTTONDBLCLK:
                SetFocus(state.window);
                return Proc(state.window,WM_RBUTTONDOWN,wParam,lParam);
            case WM_XBUTTONDOWN:
            case WM_XBUTTONDBLCLK:
                SetFocus(state.window);
                return Proc(state.window,WM_XBUTTONDOWN,wParam,lParam);
            case WM_MBUTTONDOWN:
                SetFocus(state.window);
                state.dragging=(wParam&MK_SHIFT)?2:1;
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
            case WM_MBUTTONDBLCLK:
                c={};
                return 0;
            case WM_SETCURSOR:
                // While playing, the racket is the cursor.
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
                Log(L"table tennis: resize: %s",error.c_str());
            state.hudShown.clear();   // the picture over the table, at the new size
        }

        void ReleaseView()
        {
            if(state.renderer){state.renderer->WaitIdle();state.renderer.reset();Log(L"table tennis: 3D view released");}
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
                Log(L"table tennis: no 3D view (%s)",error.c_str());
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
            state.gripped=false;
            state.herX=0;
            state.slideVelocity=0;
            state.sliding=false;
            state.herHeight=0.2f;
            // The wrist behind and over where the blade's middle is to be, to begin with.
            // The wrist under where the blade's middle is to be, to begin with (the racket stands up from her fist).
            state.holdOffset={0,-0.18f*state.upper,0.04f*state.upper};
            state.strokeOff=0;
            state.strokes=0;
            state.inBody=0;
            state.otherOffset={0,-0.18f*state.upper,0.04f*state.upper};
            state.backhand=false;
            state.body={};
            state.bodySpeed={};
            state.heightSpeed=0;
            state.trim=0;
            state.handTrim={};
            state.strokeAt=-1;
            state.ballOnHand=false;
            state.stance=state.stanceSpeed=0;
            state.feet={};
            state.steps=0;
            state.depthSpeed=0;
            state.shake=0;state.shakeMost=0;state.shakeFrames=state.waited=0;
            state.uprightSum=state.squareSum=state.slopeSum={};
            state.poseFrames={};
            state.hudShown.clear();
            Log(L"table tennis: 3D view %ux%u on %s",w,h,state.renderer->AdapterName().c_str());
            return true;
        }

        LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            switch(message)
            {
            case WM_TIMER:
                return 0;
            case WM_SETCURSOR:
                if(LOWORD(lParam)==HTCLIENT&&Playing()){SetCursor(nullptr);return TRUE;}
                break;
            case WM_KEYDOWN:
                if(!state.game)return 0;
                if(state.asking)
                {
                    if(wParam=='Y'){Finish();return 0;}
                    if(wParam=='N'||wParam==VK_ESCAPE)state.asking=false;
                    return 0;
                }
                if(wParam==VK_ESCAPE)
                {
                    if(state.game->Now()==Phase::Over)Finish();
                    else state.asking=true;
                }
                else if(wParam==VK_SPACE)
                {
                    if(!(lParam&(1<<30)))YourSwing(Stroke::Smash);   // not a held key's repeats
                }
                else if(wParam=='P'&&state.game->Now()!=Phase::Over)state.paused=!state.paused;
                else if(wParam==VK_RETURN&&state.game->Now()==Phase::Over)NewGame();
                else if(wParam==VK_F1)state.camera={};   // the camera back where it was
                else if(wParam>='1'&&wParam<='3')ChooseLevel(static_cast<int>(wParam-'1')+1);
                else if(wParam>=VK_NUMPAD1&&wParam<=VK_NUMPAD3)ChooseLevel(static_cast<int>(wParam-VK_NUMPAD1)+1);
                else if(wParam=='H')ChangeHand();
                return 0;
            case WM_LBUTTONDOWN:
                if(!state.game)return 0;
                if(state.asking)
                {
                    const float x=static_cast<float>(GET_X_LPARAM(lParam)),y=static_cast<float>(GET_Y_LPARAM(lParam));
                    if(state.askButtons[0].Contains(x,y)){Finish();return 0;}
                    if(state.askButtons[1].Contains(x,y))state.asking=false;
                    return 0;
                }
                if(state.paused){state.paused=false;return 0;}
                YourSwing(Stroke::Drive);
                return 0;
            case WM_RBUTTONDOWN:
                if(state.game)YourSwing(Stroke::Cut);
                return 0;
            case WM_XBUTTONDOWN:
                // The back button (the forward one too, for a mouse held in the other hand).
                if(state.game)YourSwing(Stroke::Smash);
                return TRUE;
            case WM_CONTEXTMENU:
                return 0;
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                BeginPaint(hwnd,&ps);   // the 3D view covers the window
                EndPaint(hwnd,&ps);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_ACTIVATE:
                if(LOWORD(wParam)==WA_INACTIVE)HoldCursor(false);
                break;
            case WM_MOVE:
                HoldCursor(false);   // held again where it now is, at the next frame
                return 0;
            case WM_SIZE:
                HoldCursor(false);
                PlaceView();
                return 0;
            case WM_CLOSE:
                Finish();
                return 0;
            case WM_DESTROY:
                HoldCursor(false);
                state.window=nullptr;
                ReleaseView();
                return 0;
            }
            return DefWindowProcW(hwnd,message,wParam,lParam);
        }
    }

    void ShowTableTennisWindow(HWND owner,const TableTennisOpponent& her,std::function<void()> onEnd)
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
        state.lines.Load(L"TABLETENNISLINES",L"tabletennis",her.ownLinesFile,her.linesFile,her.styleKey,English(),
                         {L"卓球",L"table tennis",L"",L""});
        state.voice.Stop();
        state.prefetching=false;
        state.words={};
        state.words.told=state.lines.Report();
        if(!her.linesFile.empty())ShowLinesProblem(owner,state.lines);   // (none kept: a diagnostic run, which must not stop)
        state.frames=0;
        NewGame();
        Prefetch();

        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&info);
        const RECT work=info.rcWork;
        const int dpi=static_cast<int>(GetDpiForSystem());
        const int h=std::min(static_cast<int>(work.bottom-work.top)*9/10,MulDiv(820,dpi,96));
        const int w=std::min(static_cast<int>(work.right-work.left)*9/10,h*4/3);
        const std::wstring title=std::wstring(T(L"卓球",L"Table tennis"))+L" - "+HerName();
        state.window=CreateWindowExW(0,ClassName,title.c_str(),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                                     work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,
                                     nullptr,nullptr,instance,nullptr);
        if(!CreateView())
        {
            // 3D only: without it there is no table to play at.
            if(!her.linesFile.empty())
                MessageBoxW(owner,T(L"3D表示を作れなかったため、卓球を開けません。",L"Table tennis could not open: the 3D view could not be made."),
                            T(L"卓球",L"Table tennis"),MB_OK|MB_ICONINFORMATION);
            Finish();
            return;
        }
        PlaceView();
        NewGame();
        SetTimer(state.window,TimerId,100,nullptr);
        ShowWindow(state.window,SW_SHOWNORMAL);
        SetForegroundWindow(state.window);
    }

    bool TableTennisWindowOpen(){return state.window!=nullptr;}
    HWND TableTennisWindowHandle(){return state.window;}
    bool TableTennisWindowRect(RECT& r)
    {
        return state.window&&IsWindowVisible(state.window)&&!IsIconic(state.window)&&GetWindowRect(state.window,&r);
    }

    void TickTableTennisWindow(MascotModel& model,double dt)
    {
        if(!state.window||!state.renderer||!state.game)return;
        using namespace DirectX;
        dt=std::min(dt,0.1);
        auto& r=*state.renderer;
        const float D=state.upper,k=state.k;
        state.holding+=dt;
        StepGame(dt);
        if(!state.window||!state.renderer)return;
        const auto& g=game();
        const TableTennis::Ball& ball=g.TheBall();
        const bool live=Playing()||g.Now()==Phase::Over;
        // Your racket's height goes with the ball as it comes (and with the ball you hold up to serve).
        {
            float want=0.18f;
            if(g.Now()==Phase::Serve&&g.Server()==You)want=ball.p.z-0.05f;
            else if(g.Now()==Phase::Rally&&g.LastHitter()==Her&&ball.p.y<0.5f)want=std::clamp(ball.p.z,0.04f,0.75f);
            if(live)state.yourHeight+=(want-state.yourHeight)*static_cast<float>(1-std::exp(-dt/0.05));
        }
        // The ball's trail and a smash's sparks, on by dt.
        if(live)
        {
            state.shown+=dt;
            if(g.Now()==Phase::Serve)state.trail.clear();
            else
            {
                const int kind=g.LastStroke()==Stroke::Smash?2:g.LastHitter()==Her?1:0;
                state.trail.push_back({ball.p,std::sqrt(ball.v.x*ball.v.x+ball.v.y*ball.v.y+ball.v.z*ball.v.z),kind,state.shown});
            }
            while(!state.trail.empty()&&state.shown-state.trail.front().at>TrailSeconds)state.trail.erase(state.trail.begin());
            const float step=static_cast<float>(dt);
            for(auto& sp:state.sparks)
            {
                sp.age+=step;
                sp.p.x+=sp.v.x*step;sp.p.y+=sp.v.y*step;sp.p.z+=sp.v.z*step;
                sp.v.z-=2.5f*step;
            }
            std::erase_if(state.sparks,[](const State::Spark& sp){return sp.age>=sp.life;});
        }
        // Your swinging racket goes across to the ball by itself (as the game counts it).
        {
            float want=0;
            const float age=g.SwingAge(You);
            if(age<0.16f&&g.Now()==Phase::Rally&&g.LastHitter()==Her)
            {
                const float most=g.Rule().assist[You];
                want=std::clamp(ball.p.x-g.Racket(You).x,-most,most);
            }
            else if(age<SwingShown)want=g.SwingShift(You);
            if(live)state.yourShift+=(want-state.yourShift)*static_cast<float>(1-std::exp(-dt/0.035));
        }
        const Vec hers=g.Racket(Her);
        // Forehand or backhand, by which side of her racket arm's elbow the ball comes: where
        // she will meet it against where that elbow is when she sees where it goes. To its left
        // (toward +x as she faces you) the backhand, the racket before her middle; to its right
        // the forehand, out at her racket side. Chosen once for each ball and kept
        // while she goes to it; between balls she is back in the forehand's ready stance.
        if(live)
        {
            const bool before=state.backhand;
            if(state.brain->Comes()&&g.Now()==Phase::Rally&&g.LastHitter()==You)
            {
                if(state.brain->HitAt()!=state.chosenFor)
                {
                    state.chosenFor=state.brain->HitAt();
                    // (Her left of her racket arm's elbow as it is now: nearer her body than
                    // that, a forehand would have her arm folded up against her.)
                    state.backhand=state.brain->Spot().x*k>state.herElbow-0.12f*k;   // (and a little outside it: before her body is the backhand's)
                    state.words.told+=Fmt(L"  she takes it on the {0}: the ball {1} m to the left of her elbow\n",{state.backhand?L"backhand":L"forehand",
                                          std::to_wstring((state.brain->Spot().x*k-state.herElbow)/k)});
                }
            }
            else if(g.SwingAge(Her)>0.40f)state.backhand=false;
        }
        // She steps to where that puts her (she sets off with where she means her racket to go,
        // not with the racket), easing into the step and out of it.
        if(live)
        {
            // (The forehand well out to her racket side: her elbow by her body, her forearm out to the racket.)
            // She stands beside where she will meet the ball, not behind it: the racket out at her
        // side and a little before her (so her forearm points out, and the racket's face to
        // you), nearer the table for a short ball and further back for a deep one. Never so
        // near that her legs are in the table.
        if(live)
        {
            // (The forehand beside her, hardly before her; the backhand before her.)
            const float to=std::max((state.herGoal.y+(state.backhand?0.45f:0.30f))*k,(g.Rule().length/2+0.38f)*k);
            constexpr float W=14.0f;
            const int steps=std::max(1,static_cast<int>(std::ceil(dt*240)));
            const float h=static_cast<float>(dt)/steps;
            for(int i=0;i<steps;++i)
            {
                state.depthSpeed+=(W*W*(to-state.herZ)-2*W*state.depthSpeed)*h;
                state.depthSpeed=std::clamp(state.depthSpeed,-2.0f*D,2.0f*D);
                state.herZ+=state.depthSpeed*h;
            }
        }
        // Her blade still short of where it is to be across the table (a ball before her body
            // that her arm cannot get round to): she moves herself the rest of the way.
            if(state.gripped&&state.holding>1.5&&g.Now()==Phase::Rally&&g.LastHitter()==You&&state.brain->Comes())
                state.trim=std::clamp(state.trim+(state.aimX-state.herBlade.x)*static_cast<float>(dt)*5.0f,-0.45f*D,0.45f*D);
            else state.trim*=static_cast<float>(std::exp(-dt/0.6));
            const float want=std::clamp(state.herGoal.x*k+(state.backhand?-0.14f:state.brain->Comes()?0.72f:0.30f)*D+state.trim,-1.3f*D,1.3f*D);
            if(std::abs(want-state.herX)>0.05f*D||state.sliding)
            {
                constexpr float W=16.0f;
                float& v=state.slideVelocity;
                const int steps=std::max(1,static_cast<int>(std::ceil(dt*240)));
                const float h=static_cast<float>(dt)/steps;
                for(int i=0;i<steps;++i)
                {
                    v+=(W*W*(want-state.herX)-2*W*v)*h;
                    v=std::clamp(v,-2.6f*D,2.6f*D);
                    state.herX+=v*h;
                }
                state.sliding=std::abs(want-state.herX)>0.01f*D||std::abs(v)>0.02f*D;
                if(!state.sliding)v=0;
            }
        }
        // Her racket as her hand has it now: the blade out from her fist.
        {
            std::array<float,3> middle{},along{},palm{};
            // (As her fist has it: held as a hand is shaken, the blade up past her thumb, its face her palm's.)
            std::array<float,3> wrist{},elbow{};
            state.gripped=model.Fist(HerHand,middle,along,palm,&wrist,&elbow)&&state.holding>0.5;
            state.herElbow=state.gripped?elbow[0]+state.herX-model.RootOffset():state.herX-0.25f*D;
            state.herPalm={palm[0],palm[1],palm[2]};
            {
                const bool waiting=state.gripped&&state.holding>3&&!state.brain->Comes()&&g.SwingAge(Her)>1.0f&&!state.sliding&&!(g.Now()==Phase::Serve&&g.Server()==Her)&&g.Clock()-state.strokeAt>1.2;
                state.waited=waiting?state.waited+1:0;
                const float high=state.herBlade.y/k;
                if(state.waited>=3)
                {
                    const float jerk=std::abs(high-2*state.heightBefore[0]+state.heightBefore[1]);
                    state.shake+=jerk;
                    state.shakeMost=std::max(state.shakeMost,jerk);
                    ++state.shakeFrames;
                }
                state.heightBefore[1]=state.heightBefore[0];
                state.heightBefore[0]=high;
            }
            if(state.gripped&&state.brain->Comes()&&g.Now()==Phase::Rally&&g.LastHitter()==You)
            {
                const int which=state.backhand?1:0;
                state.uprightSum[which]+=along[1];
                state.squareSum[which]+=-palm[2];
                state.slopeSum[which]+=(wrist[1]-elbow[1])/k;
                ++state.poseFrames[which];
            }
            if(state.gripped)
            {
                // Where her wrist is from the blade's middle, as she holds it now (it changes as
                // her forearm turns): what the wrist is sent by, eased.
                // (Slowly, and not for a small change: followed closely, it answered her hand's own
                // small moves and set the hand trembling up and down as she waited.)
                // While she only waits it is left as it is.
                const bool waiting=!state.brain->Comes()&&g.SwingAge(Her)>0.8f&&!(g.Now()==Phase::Serve&&g.Server()==Her);
                const float out=BladeOut*k,follow=waiting?0.0f:static_cast<float>(1-std::exp(-dt/0.12));
                for(int a=0;a<3;++a)
                {
                    const float from=std::clamp(wrist[a]-(middle[a]+along[a]*out),-0.5f*D,0.5f*D),off=from-state.holdOffset[a];
                    if(std::abs(off)>0.008f*D)state.holdOffset[a]+=(off-(off>0?0.008f:-0.008f)*D)*follow;
                }
            }
            if(state.gripped)
            {
                const float out=BladeOut*k,dx=state.herX-model.RootOffset();
                state.herBlade={middle[0]+along[0]*out+dx,middle[1]+along[1]*out,middle[2]+along[2]*out+state.herZ};
                state.herAlong={along[0],along[1],along[2]};
            }
        }
        // Her hand takes the blade's middle to where the game has her racket, at the height she
        // means to meet the ball; a swing thrusts it on toward the net.
        if(live)
        {
            // (As a spring that just does not overshoot: it sets off and arrives gently.)
            constexpr float W=14.0f;
            const int steps=std::max(1,static_cast<int>(std::ceil(dt*240)));
            const float h=static_cast<float>(dt)/steps,to=std::clamp(state.brain->Height(),0.06f,0.60f);
            for(int i=0;i<steps;++i)
            {
                state.heightSpeed+=(W*W*(to-state.herHeight)-2*W*state.heightSpeed)*h;
                state.herHeight+=state.heightSpeed*h;
            }
        }
        Vec3 blade{hers.x,hers.y,state.herHeight};
        // The ball she is going to, and how soon. Her stroke is timed by when she means to
        // meet it, not by when the game's swing begins: drawn back over the third of a second
        // before, then through, begun early enough (her arm and body are eased, and follow a
        // little late) for the racket to come through as the ball arrives, and back after.
        const bool coming=state.brain->Comes()&&g.Now()==Phase::Rally&&g.LastHitter()==You;
        if(coming)
        {
            state.strokeAt=state.brain->HitAt();
            state.strokeKind=state.brain->Planned();
        }
        else if(g.Now()==Phase::Serve&&g.Server()==Her&&state.brain->ServeAt()>0)
        {
            // Her serve: her arm swings through as the ball comes down to it.
            state.strokeAt=state.brain->ServeAt();
            state.strokeKind=state.brain->Planned();
        }
        constexpr float HerSwing=0.67f,Lead=0.035f,Begins=0.25f;   // seconds: the stroke from its start to back at rest; begun this long early
        const float timed=state.strokeAt>0?static_cast<float>(g.Clock()-state.strokeAt)+Lead:-9.0f;
        const bool swinging=timed>=-Begins&&timed<HerSwing-Begins;
        const float age=timed+Begins;
        const float ready=timed<-Begins?std::clamp((timed+0.58f)/0.33f,0.0f,1.0f):0.0f,drawn=ready*ready*(3-2*ready);
        const bool striking=swinging&&timed>-0.10f;   // (from just before the ball is met)
        {
            // Her arm's stroke: the racket drawn back as the ball comes (out to her side and
            // away from the net; up, for a smash), then through the ball and on (forward and
            // across her; up with a drive, down under a cut, down hard with a smash), and
            // back. It passes where the game has her racket as the ball is met.
            const Stroke stroke=swinging?state.strokeKind:state.brain->Planned();
            const bool smash=stroke==Stroke::Smash,cut=stroke==Stroke::Cut;
            const float side=state.backhand?-1.0f:1.0f;   // (her racket side is toward -x)
            // (As far as your racket goes round its elbow: some 0.25 m back and 0.45 m through.)
            const Vec3 back{-side*(cut?0.05f:0.11f),smash?0.28f:cut?0.12f:0.22f,smash?0.26f:cut?0.12f:-0.07f};
            const Vec3 through{side*(smash?0.50f:cut?0.30f:0.54f),smash?-0.52f:cut?-0.32f:-0.42f,smash?-0.20f:cut?-0.14f:0.28f};   // (across: her arm comes round late, so it is sent on early)

            auto ease=[](float t){t=std::clamp(t,0.0f,1.0f);return t*t*(3-2*t);};
            float bx=0,by=0,bz=0;
            if(swinging)
            {
                // From drawn back to exactly where the game has her racket, a little before
                // the ball is met, and kept there until it has been (her eased arm follows a
                // little late: so it is on the ball as the ball is struck); then on through,
                // and back.
                const float in=1-ease((timed+Begins)/0.15f),on=ease((timed-0.04f)/0.14f)*(1-ease((timed-0.20f)/0.22f));
                bx=back.x*in+through.x*on;by=back.y*in+through.y*on;bz=back.z*in+through.z*on;
            }
            else{bx=back.x*drawn;by=back.y*drawn;bz=back.z*drawn;}
            blade.x+=bx;blade.y+=by;blade.z+=bz;
        }
        // Her whole body plays the stroke (MascotModel::HoldBody; lengths in her leg's): knees
        // bent and ready, lower for a low ball; turned back as the ball comes (to her racket
        // side on the forehand, the other way on the backhand) and through as she strikes, the
        // hips going with the shoulders; up out of the knees with a drive, down with a cut; a
        // smash from tall, wound far back, to low and far forward, the whole body thrown into
        // it. She goes forward to a short ball and back for a deep one.
        {
            const float Leg=state.hips,way=state.backhand?-1.0f:1.0f;   // (positive: toward her right)
            auto ease=[](float t){t=std::clamp(t,0.0f,1.0f);return t*t*(3-2*t);};
            const Stroke stroke=swinging?state.strokeKind:state.brain->Planned();
            const bool smash=stroke==Stroke::Smash,cut=stroke==Stroke::Cut;
            const float back=XMConvertToRadians(smash?38.0f:cut?16.0f:30.0f)*way,through=-XMConvertToRadians(smash?46.0f:cut?20.0f:36.0f)*way;
            const float low=std::clamp((0.26f-state.herHeight)/0.20f,0.0f,1.0f);
            // (Lower on the forehand: her shoulder comes down toward the ball's height, so her forearm lies level.)
            float down=(0.05f+0.05f*low+(coming&&!state.backhand?0.06f:0.0f))*Leg,forward=0,turn=0,lean=XMConvertToRadians(-5.0f);   // (more upright than one leaning over a table: her shoulders stay over her feet)
            if(swinging)
            {
                // (Her body turns through as the ball is met and after, not before: turned sooner, it carried her arm on ahead of the ball.)
                const float t=ease((timed+0.03f)/0.22f),home=1-ease((timed-0.20f)/0.22f);
                turn=(back+(through-back)*t)*home;
                if(smash)
                {
                    down+=(-0.05f+0.16f*t)*Leg*home;
                    lean+=XMConvertToRadians(18.0f)*t*home;
                    forward+=0.13f*Leg*t*home;
                }
                else if(cut)down+=0.05f*Leg*t*home;
                else
                {
                    down+=(0.03f-0.07f*t)*Leg*home;
                    forward+=0.05f*Leg*t*home;
                }
            }
            else
            {
                turn=back*drawn;
                down+=(smash?-0.05f:0.03f)*Leg*drawn;
            }
            // On the forehand her elbow is in by her body and her forearm points out to the
            // racket; on the backhand the elbow is out and the forearm comes in across her.
            // Between balls, half way: the forearm forward, the racket edge on (as a hand held out to be shaken).
            const float want[6]={std::max(down,0.0f),forward,turn,lean,turn*0.45f,state.backhand?0.0f:coming||swinging?1.0f:0.55f};
            // Each follows as a spring that just does not overshoot (quicker in the stroke): it
            // gathers speed and loses it, with no sudden start or stop.
            if(live)
            {
                const int steps=std::max(1,static_cast<int>(std::ceil(dt*240)));
                const float h=static_cast<float>(dt)/steps;
                for(int i=0;i<6;++i)
                {
                    const float W=i==5?9.0f:swinging?26.0f:13.0f;
                    for(int n=0;n<steps;++n)
                    {
                        state.bodySpeed[i]+=(W*W*(want[i]-state.body[i])-2*W*state.bodySpeed[i])*h;
                        state.body[i]+=state.bodySpeed[i]*h;
                    }
                }
                state.body[5]=std::clamp(state.body[5],0.0f,1.0f);
            }
            if(g.Now()!=Phase::Over)model.HoldBody(HerHand,state.body[0],state.body[1],state.body[2],state.body[3],state.body[4],state.body[5]);
            else model.HoldBody(HerHand,0,0,0,0,0);
        }
        // Her feet apart and her other hand: held before her for balance; at her serve, under
        // the ball (on her palm, low before her other side), which she throws up to where it is struck.
        {
            const float Leg=state.hips;
            if(live)
            {
                constexpr float W=7.0f;
                const int steps=std::max(1,static_cast<int>(std::ceil(dt*240)));
                const float h=static_cast<float>(dt)/steps,to=g.Now()==Phase::Over?0.0f:0.15f*Leg;
                for(int i=0;i<steps;++i)
                {
                    state.stanceSpeed+=(W*W*(to-state.stance)-2*W*state.stanceSpeed)*h;
                    state.stance+=state.stanceSpeed*h;
                }
            }
            const bool serving=g.Now()==Phase::Serve&&g.Server()==Her&&state.holding>1.0;
            state.ballOnHand=false;
            std::array<float,3> under{};
            if(serving)
            {
                // The throw: over the 0.45 s before she serves, the ball from her palm up (and a
                // little over) to where the game holds it; her hand goes up after it and comes down.
                const double at=state.brain->ServeAt();
                // (It leaves her palm, goes up over where it is to be struck and comes down to it just
                // as she strikes: it is never left hanging there.)
                const float q=at>0?std::clamp(static_cast<float>((g.Clock()-(at-0.55))/0.55),0.0f,1.0f):0.0f;
                const Vec3 from{ball.p.x+0.24f,ball.p.y+0.02f,ball.p.z-0.20f+0.12f*std::sin(XM_PI*std::min(q*2.2f,1.0f))};
                under=ModelPoint(model,World({from.x,from.y,from.z-0.05f}));
                std::array<float,3> palm{};
                Vec3 held=from;
                if(model.PalmPosition(1-HerHand,palm))
                    held={(palm[0]-model.RootOffset()+state.herX)/k,(palm[2]+state.herZ)/k,(palm[1]-state.top)/k+g.Rule().ball};
                if(q<=0)state.tossFrom=held;
                const Vec3 start=state.tossFrom;
                state.ballShown={start.x+(ball.p.x-start.x)*q,start.y+(ball.p.y-start.y)*q,start.z+(ball.p.z-start.z)*q+0.32f*4*q*(1-q)};
                state.ballOnHand=true;
            }
            // Her feet: side steps. Each foot belongs a stance's width to its side of her; while
            // she moves, the foot on the side she is going to is lifted and set down ahead
            // first, then the other is brought after it (never both off the floor), in quick
            // short steps; standing, they are still.
            std::array<std::array<float,3>,2> feet{};
            {
                const float D2=state.upper;
                for(int f=0;f<2;++f)
                {
                    auto& foot=state.feet[f];
                    const float homeX=state.herX+(f==0?1.0f:-1.0f)*state.stance,homeZ=state.herZ;
                    if(!foot.placed){foot.x=homeX;foot.z=homeZ;foot.placed=true;foot.t=1;}
                }
                const float goingX=state.slideVelocity,goingZ=state.depthSpeed;
                for(int n=0;n<2&&live;++n)
                {
                    // (The foot on the side she is going to has the first say.)
                    const int f=(goingX>=0)==(n==0)?0:1;
                    auto& foot=state.feet[f];
                    const auto& other=state.feet[1-f];
                    const float homeX=state.herX+(f==0?1.0f:-1.0f)*state.stance,homeZ=state.herZ;
                    if(foot.t<1)
                    {
                        const float stride=std::hypot(foot.toX-foot.fromX,foot.toZ-foot.fromZ);
                        foot.t=std::min(1.0f,foot.t+static_cast<float>(dt)/(0.13f+0.10f*std::min(stride/(0.5f*D2),1.0f)));
                        // (Its landing place goes on with her while it is in the air.)
                        foot.toX=homeX+goingX*0.10f;foot.toZ=homeZ+goingZ*0.10f;
                        const float e=foot.t*foot.t*(3-2*foot.t);
                        foot.x=foot.fromX+(foot.toX-foot.fromX)*e;
                        foot.z=foot.fromZ+(foot.toZ-foot.fromZ)*e;
                    }
                    // (Only when it must: her body goes over her planted feet as far as her legs let it,
                    // further across than toward or away from the table, before a foot is moved.)
                    else if(other.t>=1&&std::hypot((homeX-foot.x)/0.30f,(homeZ-foot.z)/0.22f)>D2)
                    {
                        foot.fromX=foot.x;foot.fromZ=foot.z;
                        foot.toX=homeX+goingX*0.10f;foot.toZ=homeZ+goingZ*0.10f;
                        foot.t=0;
                        ++state.steps;
                    }
                }
                for(int f=0;f<2;++f)
                {
                    const auto& foot=state.feet[f];
                    // (No further from under her than a leg can go.)
                    const float most=0.55f*D2;
                    const float lift=foot.t<1?std::sin(XM_PI*foot.t)*0.07f*Leg:0.0f;
                    feet[f]={std::clamp(foot.x-state.herX,-most,most),lift,std::clamp(foot.z-state.herZ,-most,most)};
                }
            }
            if(g.Now()!=Phase::Over)model.HoldFree(HerHand,serving,under,feet);
            else model.HoldFree(HerHand,false,{},{});
        }
        // Not across her body while she is still on her way to the side: no further toward her
        // left than her middle (her arm would have to go through her).
        XMFLOAT3 goal=World(blade);
        state.aimX=goal.x;
        goal.x=std::min(goal.x,state.herX+0.30f*D);
        const std::array<float,3> point=ModelPoint(model,goal);
        const Vec speed=g.RacketVelocity(Her);
        const bool still=std::hypot(speed.x,speed.y)<0.05f&&!state.sliding&&!swinging;
        // Her swing strikes only when her hand has the racket where the game has it: out of her
        // reach, or not there yet, she swings through the air.
        if(state.gripped&&state.holding>1.5)
        {
            // (As it was when the swing began: the swing itself carries the racket fast, ahead of her hand.)
            // (Within a racket and a half across of where it was sent, or of where the game has her
            // racket: drawn back, it is a little aside of the ball on purpose.)
            if(!striking)state.within=std::min(std::abs(state.herBlade.x-goal.x),std::abs(state.herBlade.x-hers.x*k))/k<0.25f&&std::abs(state.herBlade.z-goal.z)/k<0.60f;
            game().SetAble(Her,state.within);
            // Diagnostics: where her blade was from where it was to be, at a swing that missed for it.
            if(g.Missed(Her)!=state.whiffs)
            {
                state.whiffs=g.Missed(Her);
                wchar_t line[200];
                swprintf_s(line,L"  her swing missed: blade %.2f across, %.2f up, %.2f along from its place (m); %s, she %.2f from where she was going\n",
                           (state.herBlade.x-goal.x)/k,(state.herBlade.y-goal.y)/k,(state.herBlade.z-goal.z)/k,state.backhand?L"backhand":L"forehand",
                           (state.herX-std::clamp(state.herGoal.x*k+(state.backhand?-0.14f:state.brain->Comes()?0.72f:0.30f)*D+state.trim,-1.3f*D,1.3f*D))/k);
                state.words.told+=line;
            }
        }
        else game().SetAble(Her,true);
        if(g.Now()!=Phase::Over)
        {
            // What her blade is still off by (her arm's own limits and its way of bending leave it
            // a little aside of where it was sent) is added to where her wrist is sent, built up
            // while she waits for the ball and let go after: so the blade is on its place when she swings.
            if(state.gripped&&state.holding>1.5&&timed<0.04f&&g.Now()==Phase::Rally&&g.LastHitter()==You&&state.brain->Comes())
            {
                const std::array<float,3> now=ModelPoint(model,state.herBlade);
                const float gain=static_cast<float>(dt)*7.0f,most=0.30f*D;
                for(int a=0;a<3;++a)state.handTrim[a]=std::clamp(state.handTrim[a]+(point[a]-now[a])*gain,-most,most);
            }
            else if(!striking)for(float& t:state.handTrim)t*=static_cast<float>(std::exp(-dt/1.2));
            // (Style 3: her whole arm kept out of her body, by her own model's surface.)
            model.Hold(HerHand,{point[0]+state.holdOffset[0]+state.handTrim[0],point[1]+state.holdOffset[1]+state.handTrim[1],point[2]+state.holdOffset[2]+state.handTrim[2]},true,3);
            if(state.holding>1)state.inBody=std::max(state.inBody,model.HoldInBody(HerHand));
        }
        else
        {
            // The game over: her racket put down, then claps when she won, "oops" when not.
            model.Hold(HerHand,{},false);
            state.holding=0;
            state.gripped=false;
            if(!state.cheered&&g.Clock()>state.overAt+0.6&&model.EmoteSeated(g.Winner()==Her?L"clap":L"oops",3.2))state.cheered=true;
        }
        SetTableSolids(model);

        r.SetMovingGeometry(BuildPieces());
        r.SetGlowGeometry(BuildGlow());
        r.SetParticles(BuildSparks());
        r.SetModelTransform(HerWorld(model));
        PlaceCamera();
        // Her head as it is seen, and beside it to the right, for her bubble.
        if(std::array<float,3> head{};model.HeadPosition(head))
        {
            const XMVECTOR at=XMVector3Transform(XMVectorSet(head[0],head[1],head[2],1),HerWorld(model));
            const XMVECTOR right=XMVector3Normalize(XMVector3Cross(XMVectorSet(0,1,0,0),XMVectorSubtract(XMLoadFloat3(&state.look),XMLoadFloat3(&state.eye))));
            const XMVECTOR beside=XMVectorAdd(at,XMVectorScale(right,0.3f*D));
            state.headScreen=r.Project({XMVectorGetX(at),XMVectorGetY(at),XMVectorGetZ(at)});
            state.besideHead=r.Project({XMVectorGetX(beside),XMVectorGetY(beside),XMVectorGetZ(beside)});
        }
        // Her eyes: while she talks, on you, behind your end; while she plays, on the ball.
        {
            const bool talking=!state.words.text.empty()&&g.Now()!=Phase::Rally;
            const XMFLOAT3 at=talking||g.Now()==Phase::Over?World({0,-g.Rule().length/2-0.6f,0.45f}):World(ball.p);
            XMVECTOR det;
            const XMVECTOR there=XMVector3Transform(XMLoadFloat3(&at),XMMatrixInverse(&det,HerWorld(model)));
            model.LookToward({XMVectorGetX(there),XMVectorGetY(there),XMVectorGetZ(there)},true);
        }
        // Her mouth follows a line's voice file while it plays.
        state.voice.Tick(dt);
        if(state.voice.Playing())model.SetMouth(state.voice.Mouth(),true);
        else model.SetMouth({},false);   // also a mouth left open by talk before the game
        UpdateOverlay();
        r.Render(model,state.her.edgeScale);
        ++state.frames;
    }

    void TableTennisLines(const std::wstring& answer)
    {
        state.prefetching=false;
        if(!state.window)return;
        state.lines.Answered(answer);
        Prefetch();
    }

    bool TableTennisAfterword(std::wstring& situation,std::wstring& fallback)
    {
        if(state.afterword.empty())return false;
        situation=std::move(state.afterword);
        fallback=std::move(state.afterwordLine);
        state.afterword.clear();
        state.afterwordLine.clear();
        return true;
    }

    void PlayTableTennisForTest(float across)
    {
        // Swinging as the mouse does (Game::Swing), a little slower to see than she is; or
        // every stroke sent to one place across the table.
        TableTennis::Style manner{0.28f,8.0f,0.2f,0.08f,0.4f,0.4f,across>=5};
        manner.across=across;
        state.autoplay=std::make_unique<TableTennis::Brain>(You,manner,7);
        if(state.game)state.game->SetRacketSpeed(You,8.0f);
    }

    bool SaveTableTennisView(const std::wstring& path)
    {
        return state.renderer&&state.renderer->SaveLastFrame(path);
    }

    void SetTableTennisCamera(float pitch,float distance,float yaw)
    {
        state.camera.pitch=pitch;
        state.camera.distance=distance;
        state.camera.yaw=yaw;
    }

    std::wstring DescribeTableTennis()
    {
        if(!state.game)return L"";
        const auto& g=*state.game;
        std::wstring text;
        wchar_t line[320];
        swprintf_s(line,L"tabletennis: 3D %d, %.2f world units a metre, top %.2f, she stands at %.2f (%.2f to the side); hold offset %.2f %.2f %.2f (upper bodies)\n",
                   state.renderer?1:0,state.k,state.top,state.herZ,state.herX,state.holdOffset[0]/state.upper,state.holdOffset[1]/state.upper,state.holdOffset[2]/state.upper);
        text+=line;
        swprintf_s(line,L"tabletennis: her blade %.3f m from her racket at her strokes (%.3f behind it; to +x of it %.3f on the forehand, %.3f on the backhand; %d strokes; %d swings missed for want of her hand); her racket arm left in her body %.3f at most (leg lengths)\n",
                   state.strokes?state.strokeOff/state.strokes:0.0,state.strokes?state.strokeAlong/state.strokes:0.0,state.strokeSide[0]?state.strokeAcross[0]/state.strokeSide[0]:0.0,state.strokeSide[1]?state.strokeAcross[1]/state.strokeSide[1]:0.0,state.strokes,g.Missed(Her),state.inBody);
        text+=line;
        swprintf_s(line,L"tabletennis: her racket upright %.2f and its palm toward you %.2f on the forehand (%d frames); %.2f and %.2f on the backhand (%d frames)\n",
                   state.poseFrames[0]?state.uprightSum[0]/state.poseFrames[0]:0.0,state.poseFrames[0]?state.squareSum[0]/state.poseFrames[0]:0.0,state.poseFrames[0],
                   state.poseFrames[1]?state.uprightSum[1]/state.poseFrames[1]:0.0,state.poseFrames[1]?state.squareSum[1]/state.poseFrames[1]:0.0,state.poseFrames[1]);
        text+=line;
        swprintf_s(line,L"tabletennis: her wrist is %.3f m above her elbow on the forehand, %.3f on the backhand (as a ball comes)\n",
                   state.poseFrames[0]?state.slopeSum[0]/state.poseFrames[0]:0.0,state.poseFrames[1]?state.slopeSum[1]/state.poseFrames[1]:0.0);
        text+=line;
        swprintf_s(line,L"tabletennis: her feet took %d steps\n",state.steps);
        text+=line;
        swprintf_s(line,L"tabletennis: her blade's height shakes %.2f mm a frame while she waits, %.2f at most (%d frames)\n",
                   state.shakeFrames?state.shake/state.shakeFrames*1000:0.0,state.shakeMost*1000,state.shakeFrames);
        text+=line;
        const auto& b=g.TheBall();
        swprintf_s(line,L"tabletennis: her %d you %d, %.1f s, %s, server %s, rally %d (longest %d), strokes %d, ball %.2f %.2f %.2f, frames %d, paused %d, level %d, wrong %d\n",
                   g.Score(Her),g.Score(You),g.Clock(),g.Now()==Phase::Over?L"over":g.Now()==Phase::Serve?L"serve":g.Now()==Phase::Rally?L"rally":L"point",
                   g.Server()==Her?L"her":L"you",g.Rally(),g.LongestRally(),g.Hits(),b.p.x,b.p.y,b.p.z,state.frames,state.paused?1:0,state.her.level,g.Wrong());
        text+=line;
        if(!state.words.told.empty())text+=L"tabletennis words:\n"+state.words.told;
        return text;
    }
}
