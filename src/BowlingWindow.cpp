#define NOMINMAX
#include "BowlingWindow.hpp"
#include "Bowling.hpp"
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
#include <deque>
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
        using Bowling::Vec3;
        using Bowling::Quat;
        using Bowling::Throw;
        using Bowling::You;
        using Bowling::Her;
        using DirectX::XMFLOAT3;
        using DirectX::XMVECTOR;
        constexpr wchar_t ClassName[]=L"MikuMikuDesktopPerchBowling";
        constexpr wchar_t ViewClassName[]=L"MikuMikuDesktopPerchBowlingView";
        constexpr UINT_PTR TimerId=1;
        constexpr int HerHand=1;              // she bowls right-handed
        constexpr float LanePitch=1.80f;      // between the lanes' middles (metres)
        constexpr float HeldZ=-0.55f;         // your ball held this far behind the foul line
        constexpr float HeldHeight=0.40f;
        constexpr float StanceZ=-2.85f;       // where she stands to begin her approach
        constexpr float WaitX=-1.10f,WaitZ=-1.2f;   // where she waits while you bowl
        constexpr float OverX=-0.30f,OverZ=-3.4f;   // where she stands once the game is over
        // Her approach (seconds from its start): four steps, the last a slide; the ball let go at ReleaseAt.
        constexpr float StepAt[4]={0.0f,0.40f,0.80f,1.20f},StepLength[4]={0.40f,0.40f,0.40f,0.62f};
        constexpr float ReleaseAt=1.78f,ApproachEnd=1.95f,Relaxed=3.8f;
        constexpr float ResultSeconds=1.9f,SweepSeconds=1.5f;
        constexpr float AddressSeconds=0.9f;
        // Her ball waits on the ball return to the right of your lane between her balls; she
        // fetches it from beside the return, facing it (FetchX metres to its left).
        constexpr float RackX=0.5f*1.80f,RackZ=-2.30f,FetchX=RackX-0.46f,FetchZ=RackZ+0.16f;   // (clear of the return's base, so her knees stay off it; the ball before her right shoulder, so the arm hangs clear of her body)
        constexpr float ReachSeconds=0.70f,GripSeconds=0.30f,LiftSeconds=1.05f;   // down to the ball, taking hold (its weight), up to her chest

        enum class Stage {Ready,Rolling,Result,Sweep,Over};
        // Her part: going to her place (with or without the ball), the ball held before her, the
        // approach (and what follows it until she stands easy).
        enum class Plan {Fetch,Lift,Walk,Address,Approach};

        float Ease(float t){t=std::clamp(t,0.0f,1.0f);return t*t*(3-2*t);}

        struct State
        {
            HWND window=nullptr,view=nullptr;
            ULONG_PTR gdiplus=0;
            BowlingOpponent her;
            std::function<void()> onEnd;
            Bowling::Game game;
            Bowling::Roll roll;
            std::array<Bowling::Pin,10> pins{};      // the rack as it stands before this ball
            std::array<Bowling::Pin,10> nextPins{};  // the rack the sweep leaves (for the next ball)
            std::array<Bowling::Pin,10> kept{};      // the pins standing after this ball
            bool rolled=false;     // the roll is this ball's (its pins are the ones shown)
            Stage stage=Stage::Ready;
            double stageTime=0;
            int thrower=You;
            Throw thrown{};
            double crashAt=-1;     // when to sound the crash (a moment after the ball meets the pins)
            bool crashed=false;
            bool autoplay=false;   // a diagnostic run: your balls thrown for you
            int testStage=0;
            std::mt19937 random{20261115};
            bool paused=false,asking=false;
            RectF askButtons[2];
            bool held=false;       // the cursor is kept in the window (ClipCursor)
            ULONGLONG levelShownUntil=0;
            int frames=0;
            double clock=0;
            // Which balls went into the gutter, for the board's marks.
            std::array<std::array<std::vector<bool>,10>,2> gutters{};

            // Your ball: where you stand (across, metres), its spin, the mouse's stroke.
            float standX=0,spin=0;
            bool pressing=false;
            float pressY=0,pull=0;     // where the button went down (client pixels); how far the ball is drawn back (metres)
            struct Sample {double at;float x,y;};
            std::deque<Sample> samples;
            std::wstring hint;
            ULONGLONG hintUntil=0;
            float lastSpeed=0;
            float cursorX=0;          // where the cursor was last frame (client pixels), to move your ball by
            bool cursorKnown=false;

            std::unique_ptr<MascotRenderer> renderer;
            float upper=10,hips=10,k=10;
            float herX=0,herZ=0,velX=0,velZ=0;
            std::array<float,3> shoulderGot{},elbowGot{};   // diagnostics: her ball arm as posed
            float leftOff=0,leftOffStand=0,rightMiss=0,leftBend=180,liftLeftOff=0,leftBendSum=0;int leftBendN=0;   // diagnostics: her left hand's farthest from her ball, walking it and standing with it
            float stoop=0;          // more bend for the ball while her straight arm is still short of it (0-1)
            std::array<float,3> reachFix{};   // added to where her wrist is sent for the ball: what it fell short by, learned
            bool picked=false;      // her ball taken up from the return (its jump to her hand eased out from then)
            XMFLOAT3 pickOffset{};   // where the ball was against where her hand holds it, as she took it
            bool walking=false;     // on the generated walk (a long way to go): the hold let go
            float yaw=0;            // her turn from facing down the lane (radians; she faces the way she walks)
            float approachFrom=0;  // her z as her approach began
            struct Foot {float x=0,z=0,fromX=0,fromZ=0,toX=0,toZ=0,t=1,lift=0;bool placed=false;};
            std::array<Foot,2> feet{};   // 0 her left foot, 1 her right (world; their places at rest less her middle)
            float stance=0;
            std::array<float,2> restAnkle{},restHip{};
            std::array<float,2> waist{};   // 上半身 at rest (height, z): where her upper body bends   // her legs at rest (model x): the feet are set from these
            float track=1;          // her ankles' spacing as a share of her hip joints' (eased: narrower on the approach)
            Plan plan=Plan::Walk;
            double planTime=0;
            Throw herThrow{};
            bool hasBall=false;
            bool released=false;
            double approachTime=-1;  // seconds into her approach (and after it); < 0: none going on
            std::array<float,3> shoulder{};   // her right shoulder at rest (model units), measured
            float armLength=1;
            // How far the hold's arm reaches (its upper arm and three quarters of its forearm),
            // and the turn the hold gives her shoulders toward her hand by itself, as it eases
            // it (both GeneratedMotion's): her own lean and turn are given less these.
            float reach=1,twist=0;
            float handSide=0;      // how far to her right of her middle her ball hangs (world units)
            float ballSide=0;      // her ball's side of her wrist at the address (model units), kept through the swing
            std::array<float,5> body{},bodySpeed{};
            std::array<float,3> lead{},leadSpeed{};
            std::array<float,3> wrist{},wristSpeed{};
            bool wristSet=false;
            XMFLOAT3 herBall{};
            XMFLOAT3 ballFrom{};
            int steps=0;
            float inBody=0;
            float releaseOff=0,releaseSide=0;
            std::array<float,3> wristGot{};   // diagnostics: where her wrist really was (model units)    // diagnostics: how far her drawn ball was from where the roll began (m)

            struct TrailPoint {Vec3 p{};double at=0;};
            std::vector<TrailPoint> trail;
            struct Spark {Vec3 p{},v{};float life=0,age=0,size=0,turn=0;std::array<float,3> colour{};};
            std::vector<Spark> sparks;

            struct Camera {float yaw=0,pitch=0,distance=1,x=0,y=0,z=0;} camera;
            XMFLOAT3 eye{},look{},eyeAt{},lookAt{};
            bool cameraPlaced=false;
            POINT dragFrom{};
            int dragging=0;
            std::wstring hudShown;
            std::wstring banner,bannerNote;
            ULONGLONG bannerUntil=0;
            std::array<int,2> inRow{};

            struct Words
            {
                std::wstring text;
                ULONGLONG since=0,until=0;
                int weight=0;
                std::vector<std::wstring> recent;
                bool voiced=false;
                std::wstring told;
            } words;
            GameLines lines;
            GameVoice voice;
            bool prefetching=false;
            std::mt19937 wordsRandom{12345};
            bool started=false,overSeen=false,cheered=false,lastFrameSaid=false;
            double faceUntil=0,overAt=0;
            std::wstring afterword,afterwordLine;
            POINT headScreen{-1,-1},besideHead{-1,-1};
            std::wstring swingTrace;   // diagnostics: her first approach, frame by frame
            bool headSeen=false;       // her head is in the view (her words beside it; else at the lower left)
        } state;

        std::uint32_t Seed()
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            return static_cast<std::uint32_t>(now.QuadPart)^0x9E3779B9u;
        }

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

        bool Playing(){return !state.game.Over()&&!state.paused&&!state.asking;}
        float Scale(){return state.window?static_cast<float>(GetDpiForWindow(state.window))/96.0f:1;}

        std::array<Bowling::Pin,10> Rack()
        {
            std::array<Bowling::Pin,10> pins{};
            for(int n=0;n<10;++n)
            {
                auto& p=pins[static_cast<size_t>(n)];
                p.p=Bowling::PinSpot(n);
                p.p.z=Bowling::PinCentre;
                p.q={};
                p.there=true;
            }
            return pins;
        }
        int Count(const std::array<Bowling::Pin,10>& pins)
        {
            int n=0;
            for(const auto& p:pins)n+=p.there?1:0;
            return n;
        }

        // ---- The sounds, made here: the ball rumbling down the lane, the pins' crash -------

        void WavHeader(std::vector<BYTE>& wav,int rate,int n)
        {
            auto put32=[&](int at,std::uint32_t v){memcpy(wav.data()+at,&v,4);};
            auto put16=[&](int at,std::uint16_t v){memcpy(wav.data()+at,&v,2);};
            memcpy(wav.data(),"RIFF",4);put32(4,36+n*2);memcpy(wav.data()+8,"WAVEfmt ",8);put32(16,16);put16(20,1);put16(22,1);
            put32(24,rate);put32(28,rate*2);put16(32,2);put16(34,16);memcpy(wav.data()+36,"data",4);put32(40,n*2);
        }

        // A ball on the boards: low noise, swelling and fading.
        std::vector<BYTE> Rumble(float seconds,float loud)
        {
            constexpr int rate=22050;
            const int n=static_cast<int>(rate*seconds);
            std::vector<BYTE> wav(44+n*2);
            WavHeader(wav,rate,n);
            std::uint32_t r=0x51ED27u;
            float a=0,b=0;
            for(int i=0;i<n;++i)
            {
                const float t=static_cast<float>(i)/rate;
                r^=r<<13;r^=r>>17;r^=r<<5;
                const float hiss=static_cast<float>(r%2001)/1000.0f-1;
                a+=(hiss-a)*0.035f;
                b+=(a-b)*0.08f;
                const float swell=std::min(1.0f,t/0.25f)*std::min(1.0f,(seconds-t)/0.6f)*(0.8f+0.2f*std::sin(t*23));
                const std::int16_t sample=static_cast<std::int16_t>(std::clamp(b*9*swell*loud,-1.0f,1.0f)*32000);
                memcpy(wav.data()+44+i*2,&sample,2);
            }
            return wav;
        }

        // The pins' crash: a thump, then hard wooden knocks, thick at first and thinning out.
        std::vector<BYTE> Crash(int knocks,float loud,std::uint32_t seed)
        {
            constexpr int rate=44100;
            const int n=rate*13/10;
            std::vector<float> mix(static_cast<size_t>(n));
            std::mt19937 random(seed);
            std::uniform_real_distribution<float> u(0,1);
            auto knock=[&](int at,float hertz,float gain){
                for(int i=0;i<rate/12&&at+i<n;++i)
                {
                    const float t=static_cast<float>(i)/rate;
                    float v=0.22f*(u(random)*2-1)*std::exp(-t/0.0012f);   // (a softer click: the pins sound low)
                    v+=std::sin(DirectX::XM_2PI*hertz*t)*std::exp(-t/0.012f);
                    v+=0.6f*std::sin(DirectX::XM_2PI*hertz*2.71f*t)*std::exp(-t/0.006f);
                    v+=0.12f*std::sin(DirectX::XM_2PI*hertz*5.2f*t)*std::exp(-t/0.003f);
                    mix[static_cast<size_t>(at+i)]+=v*gain;
                }
            };
            for(int i=0;i<rate/5;++i)
            {
                const float t=static_cast<float>(i)/rate;
                mix[static_cast<size_t>(i)]+=0.9f*std::sin(DirectX::XM_2PI*(70-90*t)*t)*std::exp(-t/0.07f);
            }
            for(int i=0;i<knocks;++i)
            {
                const float when=-0.28f*std::log(1-0.97f*u(random));
                knock(static_cast<int>(when*rate),300+380*u(random),(0.35f+0.45f*u(random))*std::exp(-when/0.7f));
            }
            float a=0;
            for(int i=0;i<n;++i)
            {
                const float t=static_cast<float>(i)/rate;
                a+=((u(random)*2-1)-a)*0.2f;
                mix[static_cast<size_t>(i)]+=a*0.5f*std::exp(-t/0.35f)*std::min(1.0f,t/0.01f);
            }
            std::vector<BYTE> wav(44+n*2);
            WavHeader(wav,rate,n);
            for(int i=0;i<n;++i)
            {
                const std::int16_t sample=static_cast<std::int16_t>(std::clamp(mix[static_cast<size_t>(i)]*loud*0.45f,-1.0f,1.0f)*32000);
                memcpy(wav.data()+44+i*2,&sample,2);
            }
            return wav;
        }

        enum class Sound {Roll,Light,Crash,Big,Gutter};
        void Play(Sound sound)
        {
            if(state.autoplay)return;   // a diagnostic run is silent
            static const std::vector<BYTE> sounds[5]={Rumble(3.4f,0.5f),Crash(5,0.65f,1),Crash(16,0.9f,2),Crash(34,1.0f,3),Rumble(2.0f,0.35f)};
            PlaySoundW(reinterpret_cast<LPCWSTR>(sounds[static_cast<int>(sound)].data()),nullptr,SND_MEMORY|SND_ASYNC|SND_NODEFAULT);
        }

        // ---- The alley -------------------------------------------------------------------

        // The geometry shader's looks, by the colour's alpha (GeometryPS): wood, wallpaper and
        // metal, lit; 1 plain and lit; 0.25 glows.
        constexpr float Wood=0.10f,Paper=0.08f,Metal=0.04f,Glow=0.25f;

        XMFLOAT3 World(Vec3 p){return {p.x*state.k,p.z*state.k,p.y*state.k};}

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
                Face({x0,y1,z0},{x1,y1,z0},{x1,y1,z1},{x0,y1,z1},{0,1,0},c);
                Face({x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},{0,0,-1},c);
                Face({x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1},{0,0,1},c);
                Face({x0,y0,z0},{x0,y0,z1},{x0,y1,z1},{x0,y1,z0},{-1,0,0},c);
                Face({x1,y0,z0},{x1,y0,z1},{x1,y1,z1},{x1,y1,z0},{1,0,0},c);
            }
            void Flat(float x0,float z0,float x1,float z1,float y,const float (&c)[4]){Face({x0,y,z0},{x1,y,z0},{x1,y,z1},{x0,y,z1},{0,1,0},c);}
            void Disc(float x,float z,float y,float r,const float (&c)[4],int sides=20)
            {
                for(int i=0;i<sides;++i)
                {
                    const float a=DirectX::XM_2PI*i/sides,b=DirectX::XM_2PI*(i+1)/sides;
                    Tri({x,y,z},{x+r*std::cos(a),y,z+r*std::sin(a)},{x+r*std::cos(b),y,z+r*std::sin(b)},{0,1,0},c);
                }
            }
            // A ball round m (world), radius r, turned by turn (game axes): finger holes and a
            // lighter swirl go round with it.
            void Ball(const V& m,float r,const float (&col)[4],const Quat& turn)
            {
                using namespace DirectX;
                const XMVECTOR q=XMVectorSet(turn.x,turn.y,turn.z,turn.w);
                constexpr int rows=10,around=20;
                auto local=[&](int i,int j){
                    const float a=XM_PI*i/rows,b=XM_2PI*j/around;
                    return V{std::sin(a)*std::cos(b),std::sin(a)*std::sin(b),std::cos(a)};
                };
                // Game axes to world: (x, y, z) -> (x, z, y).
                auto world=[&](const V& l){
                    const XMVECTOR g=XMVector3Rotate(XMVectorSet(l[0],l[1],l[2],0),q);
                    return V{XMVectorGetX(g),XMVectorGetZ(g),XMVectorGetY(g)};
                };
                auto colour=[&](const V& l,float (&c)[4]){
                    c[0]=col[0];c[1]=col[1];c[2]=col[2];c[3]=col[3];
                    static const V holes[3]={{0.0f,0.30f,0.954f},{-0.20f,-0.12f,0.972f},{0.20f,-0.12f,0.972f}};
                    for(const V& h:holes)if(l[0]*h[0]+l[1]*h[1]+l[2]*h[2]>0.988f){c[0]=c[1]=c[2]=0.04f;return;}
                    const float swirl=std::sin(l[0]*7+l[1]*3)*std::sin(l[2]*5-l[1]*4);
                    if(swirl>0.35f)for(int i=0;i<3;++i)c[i]=std::min(1.0f,c[i]*1.5f+0.10f);
                };
                for(int i=0;i<rows;++i)
                    for(int j=0;j<around;++j)
                    {
                        const V a=local(i,j),b=local(i,j+1),c=local(i+1,j+1),d=local(i+1,j);
                        const V* corners[2][3]={{&a,&b,&c},{&a,&c,&d}};
                        for(auto& corner:corners)
                        {
                            const V mid{(*corner[0])[0]+(*corner[1])[0]+(*corner[2])[0],(*corner[0])[1]+(*corner[1])[1]+(*corner[2])[1],(*corner[0])[2]+(*corner[1])[2]+(*corner[2])[2]};
                            float c[4];
                            colour({mid[0]/3,mid[1]/3,mid[2]/3},c);
                            const V w0=world(*corner[0]),w1=world(*corner[1]),w2=world(*corner[2]);
                            const V wm=world(mid);
                            auto p=[&](const V& w){return V{m[0]+w[0]*r,m[1]+w[1]*r,m[2]+w[2]*r};};
                            // (Each vertex its own normal: smooth.)
                            const V u{w1[0]-w0[0],w1[1]-w0[1],w1[2]-w0[2]},v{w2[0]-w0[0],w2[1]-w0[1],w2[2]-w0[2]};
                            const V x{u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};
                            const bool flip=x[0]*wm[0]+x[1]*wm[1]+x[2]*wm[2]>0;
                            Point(p(w0),w0,c);
                            Point(p(flip?w2:w1),flip?w2:w1,c);
                            Point(p(flip?w1:w2),flip?w1:w2,c);
                        }
                    }
            }
        };

        // The pin's shape, as Bowling.cpp has it (radius up its height, metres), with the two red
        // bands round its neck.
        constexpr std::array<std::array<float,2>,17> PinProfile{{
            {0.000f,0.0258f},{0.012f,0.0360f},{0.040f,0.0490f},{0.080f,0.0582f},{0.114f,0.0605f},{0.140f,0.0598f},
            {0.180f,0.0505f},{0.220f,0.0360f},{0.250f,0.0270f},{0.268f,0.0236f},{0.290f,0.0244f},{0.318f,0.0290f},
            {0.342f,0.0305f},{0.358f,0.0284f},{0.370f,0.0228f},{0.378f,0.0130f},{0.381f,0.0f}}};
        constexpr float BandA0=0.236f,BandA1=0.249f,BandB0=0.262f,BandB1=0.275f;

        // The profile with rows added at the bands' edges.
        const std::vector<std::array<float,2>>& DrawnProfile()
        {
            static const std::vector<std::array<float,2>> rows=[]{
                std::vector<std::array<float,2>> out(PinProfile.begin(),PinProfile.end());
                for(float h:{BandA0,BandA1,BandB0,BandB1})
                    for(size_t i=1;i<PinProfile.size();++i)
                        if(PinProfile[i][0]>h)
                        {
                            const float u=(h-PinProfile[i-1][0])/(PinProfile[i][0]-PinProfile[i-1][0]);
                            out.push_back({h,PinProfile[i-1][1]+(PinProfile[i][1]-PinProfile[i-1][1])*u});
                            break;
                        }
                std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){return a[0]<b[0];});
                return out;
            }();
            return rows;
        }

        // A pin at p (its middle of mass, game metres), turned by q, lifted by lift (metres).
        void AddPin(Mesh& m,const Vec3& p,const Quat& q,float lift)
        {
            using namespace DirectX;
            const float k=state.k;
            const XMVECTOR turn=XMVectorSet(q.x,q.y,q.z,q.w);
            const auto& rows=DrawnProfile();
            constexpr int around=14;
            const float white[4]={0.95f,0.94f,0.91f,1},red[4]={0.80f,0.08f,0.10f,1};
            auto at=[&](size_t i,int j,Mesh::V& normal){
                const float a=XM_2PI*j/around,r=rows[i][1],h=rows[i][0]-Bowling::PinCentre;
                const size_t i0=i==0?0:i-1,i1=std::min(rows.size()-1,i+1);
                const float dr=rows[i1][1]-rows[i0][1],dh=std::max(1e-4f,rows[i1][0]-rows[i0][0]),nl=std::hypot(dh,dr);
                const XMVECTOR g=XMVector3Rotate(XMVectorSet(r*std::cos(a),r*std::sin(a),h,0),turn);
                const XMVECTOR n=XMVector3Rotate(XMVectorSet(dh/nl*std::cos(a),dh/nl*std::sin(a),-dr/nl,0),turn);
                normal={XMVectorGetX(n),XMVectorGetZ(n),XMVectorGetY(n)};
                return Mesh::V{(p.x+XMVectorGetX(g))*k,(p.z+lift+XMVectorGetZ(g))*k,(p.y+XMVectorGetY(g))*k};
            };
            for(size_t i=0;i+1<rows.size();++i)
            {
                const float mid=(rows[i][0]+rows[i+1][0])/2;
                const bool band=(mid>BandA0&&mid<BandA1)||(mid>BandB0&&mid<BandB1);
                const auto& c=band?red:white;
                for(int j=0;j<around;++j)
                {
                    Mesh::V n00,n01,n10,n11;
                    const Mesh::V p00=at(i,j,n00),p01=at(i,j+1,n01),p10=at(i+1,j,n10),p11=at(i+1,j+1,n11);
                    // (Outward: the corners' order checked against the outward normal.)
                    const Mesh::V u{p01[0]-p00[0],p01[1]-p00[1],p01[2]-p00[2]},w{p11[0]-p00[0],p11[1]-p00[1],p11[2]-p00[2]};
                    const Mesh::V x{u[1]*w[2]-u[2]*w[1],u[2]*w[0]-u[0]*w[2],u[0]*w[1]-u[1]*w[0]};
                    const Mesh::V out{n00[0]+n11[0],n00[1]+n11[1],n00[2]+n11[2]};
                    const bool flip=x[0]*out[0]+x[1]*out[1]+x[2]*out[2]<0;
                    auto tri=[&](const Mesh::V& a,const Mesh::V& na,const Mesh::V& b,const Mesh::V& nb,const Mesh::V& d,const Mesh::V& nd){
                        m.Point(a,na,c);
                        if(flip){m.Point(d,nd,c);m.Point(b,nb,c);}
                        else{m.Point(b,nb,c);m.Point(d,nd,c);}
                    };
                    tri(p00,n00,p01,n01,p11,n11);
                    tri(p00,n00,p11,n11,p10,n10);
                }
            }
            // Its base: a dark disc under it (seen from below only when it lies down).
            {
                const float base[4]={0.55f,0.52f,0.48f,1};
                const XMVECTOR down=XMVector3Rotate(XMVectorSet(0,0,-1,0),turn);
                const Mesh::V n{XMVectorGetX(down),XMVectorGetZ(down),XMVectorGetY(down)};
                Mesh::V centre;
                {
                    const XMVECTOR g=XMVector3Rotate(XMVectorSet(0,0,-Bowling::PinCentre,0),turn);
                    centre={(p.x+XMVectorGetX(g))*k,(p.z+lift+XMVectorGetZ(g))*k,(p.y+XMVectorGetY(g))*k};
                }
                for(int j=0;j<around;++j)
                {
                    Mesh::V na,nb;
                    m.Tri(centre,at(0,j,na),at(0,j+1,nb),n,base);
                }
            }
        }

        // What does not move: the alley round your lane (and the lanes either side, their pins
        // standing), the approach, the seats behind it, the walls and the lights.
        std::vector<MascotRenderer::GeometryVertex> BuildAlley()
        {
            using namespace Bowling;
            const float k=state.k;
            Mesh m;
            const float maple[4]={0.84f,0.68f,0.45f,1},deckWood[4]={0.90f,0.76f,0.55f,1},approach[4]={0.78f,0.62f,0.41f,1};
            const float gutter[4]={0.30f,0.32f,0.36f,Metal},capping[4]={0.14f,0.14f,0.17f,1},darkArrow[4]={0.20f,0.10f,0.06f,1};
            const float pit[4]={0.03f,0.03f,0.04f,1},black[4]={0.05f,0.05f,0.07f,1},kick[4]={0.12f,0.10f,0.14f,1};
            const float carpet[4]={0.16f,0.10f,0.26f,1},carpet2[4]={0.22f,0.14f,0.36f,1};
            const float wall[4]={0.12f,0.12f,0.20f,1},ceiling[4]={0.07f,0.07f,0.11f,1};
            const float foul[4]={0.75f,0.10f,0.12f,1};
            const float back=DeckEnd+0.95f,front=-12.0f,ceilingY=3.4f*k;
            const int lanes=2;   // each side of yours
            const float sideX=(lanes+0.5f)*LanePitch+0.6f;
            // The floor behind the lanes: carpet with a pattern of squares, the approach's wood.
            for(int i=static_cast<int>(-sideX/0.6f);i<static_cast<int>(sideX/0.6f);++i)
                for(int j=static_cast<int>(front/0.6f);j<static_cast<int>(-4.9f/0.6f);++j)
                    m.Flat(i*0.6f*k,j*0.6f*k,(i+1)*0.6f*k,(j+1)*0.6f*k,0.008f*k,((i+j)&1)?carpet:carpet2);   // (8 mm up: level with the floor where they meet, the two flickered)
            // Boards of maple along the lane (plain, each its own shade a little apart: the
            // shader's wood blotched the lanes too much).
            auto boards=[&](float x0,float x1,float z0,float z1,const float (&c)[4],int seed){
                const float board=2*LaneHalf/39;
                const int n=std::max(1,static_cast<int>(std::round((x1-x0)/board)));
                for(int i=0;i<n;++i)
                {
                    std::uint32_t h=static_cast<std::uint32_t>(i*2654435761u)^static_cast<std::uint32_t>(seed*40503u);
                    h^=h>>15;h*=2246822519u;h^=h>>13;
                    const float shade=0.965f+0.07f*static_cast<float>(h%1000)/1000.0f;
                    const float col[4]={c[0]*shade,c[1]*shade,c[2]*shade,1};
                    m.Flat((x0+(x1-x0)*i/n)*k,z0*k,(x0+(x1-x0)*(i+1)/n)*k,z1*k,0,col);
                }
            };
            boards(-sideX,sideX,-4.9f,0,approach,7);
            for(int lane=-lanes;lane<=lanes;++lane)
            {
                const float cx=lane*LanePitch;
                // The lane's boards to the pin deck, the deck a little paler.
                boards(cx-LaneHalf,cx+LaneHalf,0,PinDeckY-0.45f,maple,lane+20);
                boards(cx-LaneHalf,cx+LaneHalf,PinDeckY-0.45f,DeckEnd,deckWood,lane+40);
                // The foul line, the dots and the arrows.
                m.Flat((cx-LaneHalf)*k,-0.01f*k,(cx+LaneHalf)*k,0.01f*k,0.001f*k,foul);
                const float board=2*LaneHalf/39;
                for(int b:{3,5,8,11,14})
                    for(float s:{-1.0f,1.0f})
                    {
                        m.Disc((cx+s*(LaneHalf-b*board))*k,2.1f*k,0.001f*k,0.008f*k,darkArrow,10);
                        m.Disc((cx+s*(LaneHalf-b*board))*k,-3.65f*k,0.001f*k,0.010f*k,darkArrow,10);
                    }
                for(int a=0;a<7;++a)
                {
                    const float x=cx+(a-3)*5*board,z=4.6f+std::abs(a-3)*0.30f;
                    const float tri[4]={0.18f,0.08f,0.05f,1};
                    m.Tri({x*k,0.001f*k,(z+0.16f)*k},{(x-0.013f)*k,0.001f*k,z*k},{(x+0.013f)*k,0.001f*k,z*k},{0,1,0},tri);
                }
                // The gutters: a hollow each side, down to their bottom and up again.
                for(float s:{-1.0f,1.0f})
                {
                    const float x0=cx+s*LaneHalf,x1=cx+s*(LaneHalf+GutterWidth/2),x2=cx+s*(LaneHalf+GutterWidth);
                    m.grain=1;m.grainScale=0.3f*k;
                    m.Face({x0*k,0,0},{x0*k,0,(DeckEnd+0.2f)*k},{x1*k,-GutterDepth*k,(DeckEnd+0.2f)*k},{x1*k,-GutterDepth*k,0},{-s*0.5f,0.86f,0},gutter);
                    m.Face({x1*k,-GutterDepth*k,0},{x1*k,-GutterDepth*k,(DeckEnd+0.2f)*k},{x2*k,0,(DeckEnd+0.2f)*k},{x2*k,0,0},{s*0.5f,0.86f,0},gutter);
                    // The kickbacks by the deck.
                    const float w0=std::min(x2,x2+s*0.08f),w1=std::max(x2,x2+s*0.08f);
                    m.Box(w0*k,0,(PinDeckY-0.62f)*k,w1*k,0.62f*k,back*k,kick);
                }
                // The capping between this lane's gutter and the next one's, up to the kickbacks.
                {
                    const float x0=cx+LaneHalf+GutterWidth,x1=cx+LanePitch-LaneHalf-GutterWidth;
                    m.Box(x0*k,0,0,x1*k,0.025f*k,(PinDeckY-0.62f)*k,capping);
                    if(lane==-lanes)m.Box((cx-LanePitch/2)*k,0,0,(cx-LaneHalf-GutterWidth)*k,0.025f*k,(PinDeckY-0.62f)*k,capping);
                }
                // The pit behind the deck, and the dark curtain at its back.
                m.Flat((cx-LaneHalf-GutterWidth)*k,DeckEnd*k,(cx+LaneHalf+GutterWidth)*k,back*k,-0.45f*k,pit);
                m.Face({(cx-LanePitch/2)*k,-0.5f*k,back*k},{(cx+LanePitch/2)*k,-0.5f*k,back*k},{(cx+LanePitch/2)*k,1.2f*k,back*k},{(cx-LanePitch/2)*k,1.2f*k,back*k},{0,0,-1},black);
                // The other lanes' racks, standing.
                if(lane!=0)
                    for(int n=0;n<10;++n)
                    {
                        Vec3 p=PinSpot(n);
                        p.x+=cx;p.z=PinCentre;
                        AddPin(m,p,{},0);
                    }
            }
            // The masking over the pins: a broad panel lit in colours, a band of light under it.
            {
                const float z=(PinDeckY-0.70f)*k,y0=0.95f*k,y1=ceilingY;
                const float panel[4]={0.08f,0.06f,0.14f,1};
                m.Face({-sideX*k,y0,z},{sideX*k,y0,z},{sideX*k,y1,z},{-sideX*k,y1,z},{0,0,-1},panel);
                static const float colours[5][3]={{1.0f,0.30f,0.70f},{0.30f,0.85f,1.0f},{1.0f,0.82f,0.25f},{0.55f,1.0f,0.45f},{0.75f,0.45f,1.0f}};
                for(int lane=-lanes;lane<=lanes;++lane)
                {
                    const float cx=lane*LanePitch;
                    const float* c=colours[(lane+lanes)%5];
                    const float glow[4]={c[0],c[1],c[2],Glow},soft[4]={c[0]*0.45f,c[1]*0.45f,c[2]*0.45f,Glow};
                    const float zz=z-0.01f*k;
                    // A star and swooshes: our own lane's picture.
                    const float sy=1.95f*k,sx=cx*k,zs=zz-0.006f*k;   // (above the swooshes and before them: where they met, the two flickered)
                    for(int i=0;i<5;++i)
                    {
                        const float a0=DirectX::XM_2PI*i/5-DirectX::XM_PIDIV2,a1=a0+DirectX::XM_2PI/5,am=a0+DirectX::XM_PI/5;
                        const float R=0.34f*k,r=0.14f*k;
                        m.Tri({sx,sy,zs},{sx+R*std::cos(a0),sy-R*std::sin(a0),zs},{sx+r*std::cos(am),sy-r*std::sin(am),zs},{0,0,-1},glow);
                        m.Tri({sx,sy,zs},{sx+r*std::cos(am),sy-r*std::sin(am),zs},{sx+R*std::cos(a1),sy-R*std::sin(a1),zs},{0,0,-1},glow);
                    }
                    for(int b=0;b<3;++b)
                    {
                        const float yb=(1.18f+0.07f*b)*k,lift=0.18f*k*(b+1)/3;
                        for(int s=0;s<12;++s)
                        {
                            const float u0=s/12.0f,u1=(s+1)/12.0f;
                            const float x0=(cx-0.8f+1.6f*u0)*k,x1=(cx-0.8f+1.6f*u1)*k;
                            const float y0b=yb+lift*std::sin(u0*DirectX::XM_PI),y1b=yb+lift*std::sin(u1*DirectX::XM_PI);
                            m.Face({x0,y0b,zz},{x1,y1b,zz},{x1,y1b+0.025f*k,zz},{x0,y0b+0.025f*k,zz},{0,0,-1},b==1?glow:soft);
                        }
                    }
                    // The lane's number over it.
                    const float bar[4]={0.95f,0.95f,1.0f,Glow};
                    m.Face({(cx-0.06f)*k,2.55f*k,zz},{(cx+0.06f)*k,2.55f*k,zz},{(cx+0.06f)*k,2.62f*k,zz},{(cx-0.06f)*k,2.62f*k,zz},{0,0,-1},bar);
                }
                const float strip[4]={0.95f,0.92f,0.80f,Glow};
                m.Face({-sideX*k,y0-0.04f*k,z-0.02f*k},{sideX*k,y0-0.04f*k,z-0.02f*k},{sideX*k,y0,z-0.02f*k},{-sideX*k,y0,z-0.02f*k},{0,0,-1},strip);
                // Inside, over the decks: dark.
                m.Face({-sideX*k,y0,z},{sideX*k,y0,z},{sideX*k,y0,back*k},{-sideX*k,y0,back*k},{0,-1,0},black);
            }
            // The ceiling: dark, with lines of light along the lanes and panels over the approach.
            {
                m.Face({-sideX*k,ceilingY,front*k},{sideX*k,ceilingY,front*k},{sideX*k,ceilingY,(PinDeckY-0.70f)*k},{-sideX*k,ceilingY,(PinDeckY-0.70f)*k},{0,-1,0},ceiling);
                const float neon[4]={0.35f,0.55f,1.0f,Glow},warm[4]={1.0f,0.95f,0.85f,Glow};
                for(int lane=-lanes;lane<=lanes+1;++lane)
                {
                    const float x=(lane-0.5f)*LanePitch;
                    m.Face({(x-0.02f)*k,ceilingY-0.01f*k,1.0f*k},{(x+0.02f)*k,ceilingY-0.01f*k,1.0f*k},{(x+0.02f)*k,ceilingY-0.01f*k,(PinDeckY-0.8f)*k},{(x-0.02f)*k,ceilingY-0.01f*k,(PinDeckY-0.8f)*k},{0,-1,0},neon);
                }
                for(float z:{-2.0f,-5.0f,-8.0f})
                    for(float x:{-2.7f,0.0f,2.7f})
                        m.Face({(x-0.5f)*k,ceilingY-0.01f*k,(z-0.3f)*k},{(x+0.5f)*k,ceilingY-0.01f*k,(z-0.3f)*k},{(x+0.5f)*k,ceilingY-0.01f*k,(z+0.3f)*k},{(x-0.5f)*k,ceilingY-0.01f*k,(z+0.3f)*k},{0,-1,0},warm);
            }
            // The walls: dark, with upright strips of light.
            {
                const float stripe[4]={1.0f,0.35f,0.75f,Glow};
                for(float s:{-1.0f,1.0f})
                {
                    const float x=s*sideX*k;
                    m.Face({x,0,front*k},{x,0,back*k},{x,ceilingY,back*k},{x,ceilingY,front*k},{-s,0,0},wall);
                    for(float z=front+1.5f;z<PinDeckY;z+=3.0f)
                        m.Face({x-s*0.01f*k,0.3f*k,z*k},{x-s*0.01f*k,0.3f*k,(z+0.05f)*k},{x-s*0.01f*k,ceilingY-0.3f*k,(z+0.05f)*k},{x-s*0.01f*k,ceilingY-0.3f*k,z*k},{-s,0,0},stripe);
                }
                m.Face({-sideX*k,0,front*k},{sideX*k,0,front*k},{sideX*k,ceilingY,front*k},{-sideX*k,ceilingY,front*k},{0,0,1},wall);
            }
            // The ball returns between the lanes, with balls of many colours on them.
            {
                const float hood[4]={0.20f,0.22f,0.30f,Metal},rail[4]={0.55f,0.56f,0.60f,Metal};
                static const float balls[6][4]={{0.85f,0.20f,0.30f,1},{0.20f,0.40f,0.85f,1},{0.95f,0.75f,0.15f,1},{0.30f,0.70f,0.35f,1},{0.55f,0.25f,0.75f,1},{0.10f,0.10f,0.12f,1}};
                int c=0;
                for(int lane=-lanes;lane<lanes;++lane)
                {
                    if(lane==-1)continue;   // (none between yours and the one on your left: she waits there)
                    const float x=(lane+0.5f)*LanePitch;
                    m.grain=2;m.grainScale=0.4f*k;
                    m.Box((x-0.22f)*k,0,-3.6f*k,(x+0.22f)*k,0.48f*k,-1.4f*k,hood);
                    m.Box((x-0.26f)*k,0,-1.4f*k,(x+0.26f)*k,0.86f*k,-1.0f*k,hood);
                    for(float s:{-1.0f,1.0f})m.Box((x+s*0.12f-0.015f)*k,0.48f*k,-3.5f*k,(x+s*0.12f+0.015f)*k,0.52f*k,-1.45f*k,rail);
                    for(int b=0;b<4;++b)
                    {
                        const float z=-3.3f+0.24f*b;
                        m.Ball({x*k,(0.48f+BallRadius)*k,z*k},BallRadius*k,balls[c++%6],{});
                    }
                }
            }
            // The score screens hanging over the approach.
            {
                const float frame[4]={0.10f,0.10f,0.12f,1},screen[4]={0.10f,0.22f,0.45f,Glow};
                for(int lane=-lanes;lane<=lanes;lane+=2)
                {
                    const float x=lane*LanePitch;
                    m.Box((x-0.75f)*k,2.35f*k,-0.6f*k,(x+0.75f)*k,2.95f*k,-0.5f*k,frame);
                    m.Face({(x-0.70f)*k,2.40f*k,-0.61f*k},{(x+0.70f)*k,2.40f*k,-0.61f*k},{(x+0.70f)*k,2.90f*k,-0.61f*k},{(x-0.70f)*k,2.90f*k,-0.61f*k},{0,0,-1},screen);
                    m.Box((x-0.02f)*k,2.95f*k,-0.57f*k,(x+0.02f)*k,ceilingY,-0.53f*k,frame);
                }
            }
            // Seats behind the approach: low sofas round small tables.
            {
                const float seat[4]={0.70f,0.16f,0.24f,1},seat2[4]={0.12f,0.50f,0.55f,1},table[4]={0.30f,0.26f,0.22f,Wood};
                for(int lane=-lanes;lane<=lanes;++lane)
                {
                    const float x=lane*LanePitch;
                    const auto& s=(lane&1)?seat2:seat;
                    m.Box((x-0.75f)*k,0,-7.3f*k,(x+0.75f)*k,0.42f*k,-6.7f*k,s);
                    m.Box((x-0.75f)*k,0.42f*k,-7.3f*k,(x+0.75f)*k,0.85f*k,-7.1f*k,s);
                    m.grain=0;m.grainScale=0.2f*k;
                    m.Box((x-0.30f)*k,0,-6.3f*k,(x+0.30f)*k,0.48f*k,-5.9f*k,table);
                }
            }
            return std::move(m.out);
        }

        // The sweep: the bar coming down before the deck, going back to the pit and up again.
        // Its height over the deck (metres; 0.6 is up out of sight) and how far back it is.
        void SweepBar(float u,float& height,float& at)
        {
            using namespace Bowling;
            const float down=Ease(u/0.30f)*(1-Ease((u-0.95f)/0.20f));
            height=0.60f-0.58f*down;
            at=PinDeckY-0.42f+(DeckEnd+0.25f-(PinDeckY-0.42f))*Ease((u-0.32f)/0.58f);
            if(u>1.10f)at=PinDeckY-0.42f;   // (back to the front, up out of sight)
        }

        // What moves: the pins (as the roll has them, or lifted and lowered by the sweep), the
        // ball (in her hand, held by you, rolling), the bar.
        XMFLOAT3 RackBall();

        std::vector<MascotRenderer::GeometryVertex> BuildMoving()
        {
            using namespace Bowling;
            const float k=state.k;
            Mesh m;
            const auto& shownPins=state.rolled?state.roll.Pins():state.pins;
            const float shade[4]={0.30f,0.22f,0.14f,1};
            if(state.stage==Stage::Sweep)
            {
                const float u=static_cast<float>(state.stageTime);
                float height=0,at=0;
                SweepBar(u,height,at);
                // Those still standing lifted while the bar goes by, then set down again; the
                // fallen ones swept off as it passes them; a fresh rack lowered after.
                const float lift=0.55f*(u<1.0f?Ease(u/0.35f):1-Ease((u-1.0f)/0.40f));
                if(u<1.0f)
                {
                    for(int n=0;n<10;++n)
                    {
                        const auto& p=shownPins[static_cast<size_t>(n)];
                        if(!p.there)continue;
                        const bool standing=state.kept[static_cast<size_t>(n)].there;
                        if(standing)AddPin(m,p.p,p.q,lift);
                        else if(p.p.y>at+0.08f&&p.p.z>-0.3f)AddPin(m,p.p,p.q,0);
                    }
                }
                else
                    for(const auto& p:state.nextPins)if(p.there)AddPin(m,p.p,p.q,lift);
                // The bar.
                const float bar[4]={0.25f,0.26f,0.30f,Metal};
                m.grain=0;m.grainScale=0.3f*k;
                m.Box(-(LaneHalf+GutterWidth)*k,height*k,(at-0.03f)*k,(LaneHalf+GutterWidth)*k,(height+0.24f)*k,(at+0.03f)*k,bar);
            }
            else
                for(const auto& p:shownPins)
                    if(p.there&&p.p.z>-0.6f)
                    {
                        AddPin(m,p.p,p.q,0);
                        if(p.p.z>0.0f&&p.p.z<0.30f&&std::abs(p.p.x)<LaneHalf&&p.p.y<DeckEnd)m.Disc(p.p.x*k,p.p.y*k,0.002f*k,0.07f*k,shade,12);
                    }
            // The ball.
            const float yours[4]={0.16f,0.34f,0.86f,1},hers[4]={0.86f,0.28f,0.62f,1};
            if(state.stage==Stage::Rolling||(state.stage==Stage::Result&&state.roll.Ball().y<DeckEnd+0.5f))
            {
                const auto& col=state.thrower==You?yours:hers;
                XMFLOAT3 p=World(state.roll.Ball());
                // Its first moments: from where it left the hand to where it rolls.
                const float blend=Ease(static_cast<float>(state.roll.Clock())/0.28f);
                if(blend<1)
                {
                    p.x=state.ballFrom.x+(p.x-state.ballFrom.x)*blend;
                    p.y=state.ballFrom.y+(p.y-state.ballFrom.y)*blend;
                    p.z=state.ballFrom.z+(p.z-state.ballFrom.z)*blend;
                }
                m.Ball({p.x,p.y,p.z},BallRadius*k,col,state.roll.BallTurn());
                if(!state.roll.InGutter()&&std::abs(state.roll.Ball().x)<LaneHalf)m.Disc(p.x,p.z,0.002f*k,BallRadius*k*1.05f,shade,16);
            }
            if(state.stage==Stage::Ready&&state.thrower==You&&!state.game.Over())
            {
                // Yours, held: drawn back as you draw the mouse back.
                const Vec3 g{state.standX,HeldZ-state.pull,HeldHeight+0.25f*std::max(0.0f,state.pull)-0.10f*std::max(0.0f,-state.pull)};
                const XMFLOAT3 p=World(g);
                m.Ball({p.x,p.y,p.z},BallRadius*k,yours,{0.38f,0,0,0.925f});
                m.Disc(p.x,p.z,0.002f*k,BallRadius*k,shade,16);
            }
            if(state.hasBall)
            {
                const XMFLOAT3& p=state.herBall;
                m.Ball({p.x,p.y,p.z},BallRadius*k,hers,{0.38f,0,0,0.925f});
            }
            else if(!(state.thrower==Her&&state.released&&state.stage==Stage::Ready))
            {
                // Waiting on the ball return for her next (not while her last is on its way).
                const XMFLOAT3 p=RackBall();
                m.Ball({p.x,p.y,p.z},BallRadius*k,hers,{0.38f,0,0,0.925f});
            }
            return std::move(m.out);
        }

        // A strip of light (the glowing pass: soft across), from a to b, half width w.
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

        constexpr double TrailSeconds=0.45;

        // Light: your line as you aim (where the ball would go, straight on, at a middling
        // speed), and the rolling ball's trail.
        // The neon's bloom (as MMD's AutoLuminous gives it): soft light spilling round the stars,
        // the swooshes under them and the upright strips on the side walls, a bright core over
        // each and a wide faint halo, added in the glowing pass. The same places as BuildAlley's.
        void NeonGlow(std::vector<MascotRenderer::GeometryVertex>& out)
        {
            const float k=state.k;
            const int lanes=2;
            const float sideX=(lanes+0.5f)*LanePitch+0.6f,front=-12.0f,ceilingY=3.4f*k;
            const float z=(Bowling::PinDeckY-0.70f)*k-0.02f*k;   // (just before the panel)
            static const float colours[5][3]={{1.0f,0.30f,0.70f},{0.30f,0.85f,1.0f},{1.0f,0.82f,0.25f},{0.55f,1.0f,0.45f},{0.75f,0.45f,1.0f}};
            auto tint=[](const float* c,float f){return std::array<float,3>{c[0]*f,c[1]*f,c[2]*f};};
            auto strip=[&](XMFLOAT3 a,XMFLOAT3 b,float w,const std::array<float,3>& ca,const std::array<float,3>& cb){
                const float A[3]={ca[0],ca[1],ca[2]},B[3]={cb[0],cb[1],cb[2]};
                LightStrip(out,a,b,w,A,B);
            };
            const std::array<float,3> none{0,0,0};
            for(int lane=-lanes;lane<=lanes;++lane)
            {
                const float cx=lane*LanePitch;
                const float* c=colours[(lane+lanes)%5];
                // The star: a halo out from its middle every way, and light along its edges.
                const float sy=1.95f*k,sx=cx*k,R=0.34f*k,r=0.14f*k;
                for(int i=0;i<48;++i)
                {
                    const float a=DirectX::XM_2PI*(i+0.5f)/48;
                    // (From the star's inner points out: from its middle, the spokes all met there in a white spot.)
                    strip({sx+r*std::cos(a),sy+r*std::sin(a),z},{sx+0.80f*k*std::cos(a),sy+0.80f*k*std::sin(a),z},0.08f*k,tint(c,0.10f),none);
                }
                for(int i=0;i<5;++i)
                {
                    const float a0=DirectX::XM_2PI*i/5-DirectX::XM_PIDIV2,a1=a0+DirectX::XM_2PI/5,am=a0+DirectX::XM_PI/5;
                    const XMFLOAT3 tip0{sx+R*std::cos(a0),sy-R*std::sin(a0),z},in{sx+r*std::cos(am),sy-r*std::sin(am),z},tip1{sx+R*std::cos(a1),sy-R*std::sin(a1),z};
                    for(const auto& [p,q]:{std::pair{tip0,in},std::pair{in,tip1}})
                    {
                        strip(p,q,0.06f*k,tint(c,0.55f),tint(c,0.55f));
                        strip(p,q,0.20f*k,tint(c,0.20f),tint(c,0.20f));
                    }
                }
                // The swooshes under it.
                for(int b=0;b<3;++b)
                {
                    const float yb=(1.18f+0.07f*b+0.0125f)*k,lift=0.18f*k*(b+1)/3,f=b==1?1.0f:0.5f;
                    for(int s=0;s<12;++s)
                    {
                        const float u0=s/12.0f,u1=(s+1)/12.0f;
                        const XMFLOAT3 p{(cx-0.8f+1.6f*u0)*k,yb+lift*std::sin(u0*DirectX::XM_PI),z},q{(cx-0.8f+1.6f*u1)*k,yb+lift*std::sin(u1*DirectX::XM_PI),z};
                        strip(p,q,0.05f*k,tint(c,0.50f*f),tint(c,0.50f*f));
                        strip(p,q,0.17f*k,tint(c,0.18f*f),tint(c,0.18f*f));
                    }
                }
            }
            // The upright strips on the side walls.
            const float pink[3]={1.0f,0.35f,0.75f};
            for(float s:{-1.0f,1.0f})
            {
                const float x=s*sideX*k-s*0.02f*k;
                for(float zz=front+1.5f;zz<Bowling::PinDeckY;zz+=3.0f)
                {
                    const XMFLOAT3 p{x,0.3f*k,(zz+0.025f)*k},q{x,ceilingY-0.3f*k,(zz+0.025f)*k};
                    strip(p,q,0.05f*k,tint(pink,0.55f),tint(pink,0.55f));
                    strip(p,q,0.24f*k,tint(pink,0.20f),tint(pink,0.20f));
                }
            }
        }

        std::vector<MascotRenderer::GeometryVertex> BuildGlow()
        {
            std::vector<MascotRenderer::GeometryVertex> out;
            const float k=state.k;
            NeonGlow(out);
            if(state.stage==Stage::Ready&&state.thrower==You&&!state.game.Over()&&!state.autoplay)
            {
                Throw t;
                t.x=state.standX;t.angle=0;t.speed=7.0f;t.spin=state.spin;
                const auto path=Bowling::Path(t,0.5f);
                for(size_t i=1;i<path.size();++i)
                {
                    const float u=static_cast<float>(i)/path.size(),fade=0.55f*(1-u)+0.05f;
                    const float c[3]={0.30f*fade,0.65f*fade,1.0f*fade};
                    Vec3 a=path[i-1],b=path[i];
                    a.z=b.z=0.004f;
                    LightStrip(out,World(a),World(b),0.018f*k,c,c);
                }
            }
            const auto& trail=state.trail;
            for(size_t i=1;i<trail.size();++i)
            {
                const auto& a=trail[i-1];
                const auto& b=trail[i];
                const float fade=std::clamp(1-static_cast<float>((state.clock-(a.at+b.at)/2)/TrailSeconds),0.0f,1.0f);
                if(fade<0.02f)continue;
                static const float tints[2][3]={{0.45f,0.70f,1.0f},{1.0f,0.50f,0.80f}};
                const float* tint=tints[state.thrower==You?0:1];
                const float c[3]={tint[0]*fade*0.6f,tint[1]*fade*0.6f,tint[2]*fade*0.6f};
                LightStrip(out,World(a.p),World(b.p),(0.02f+0.05f*fade)*k,c,c);
            }
            return out;
        }

        std::vector<MascotRenderer::Particle> BuildSparks()
        {
            std::vector<MascotRenderer::Particle> out;
            const float k=state.k,t=static_cast<float>(state.clock);
            for(const auto& sp:state.sparks)
            {
                const float age=sp.age/sp.life;
                MascotRenderer::Particle q;
                const XMFLOAT3 p=World(sp.p);
                q.position={p.x,p.y,p.z};
                const float twinkle=0.6f+0.4f*std::sin(t*30+sp.turn*7);
                q.size=sp.size*k*(1-0.5f*age)*(0.7f+0.5f*twinkle);
                q.color={sp.colour[0],sp.colour[1],sp.colour[2],(1-age*age)*twinkle};
                q.angle=sp.turn+t*4;
                q.nearer=true;
                out.push_back(q);
            }
            return out;
        }

        // Stars over the deck: a strike's (gold and many colours) or a spare's (fewer, silver).
        void Burst(bool strike)
        {
            std::uniform_real_distribution<float> u(-1,1);
            static const std::array<float,3> colours[5]={{1.0f,0.85f,0.35f},{1.0f,0.45f,0.75f},{0.45f,0.85f,1.0f},{0.6f,1.0f,0.5f},{1.0f,1.0f,1.0f}};
            const int count=strike?90:40;
            for(int i=0;i<count;++i)
            {
                State::Spark sp;
                sp.p={u(state.random)*0.45f,Bowling::PinDeckY+0.45f+u(state.random)*0.35f,0.15f+0.2f*std::abs(u(state.random))};
                const float speed=1.0f+1.6f*std::abs(u(state.random));
                sp.v={u(state.random)*speed*0.8f,u(state.random)*speed*0.5f,1.2f+speed};
                sp.life=0.9f+0.7f*std::abs(u(state.random));
                sp.size=0.020f+0.030f*std::abs(u(state.random));
                sp.turn=u(state.random)*3;
                sp.colour=strike?colours[i%5]:std::array<float,3>{0.85f,0.92f,1.0f};
                state.sparks.push_back(sp);
            }
        }

        // ---- Her on the approach -----------------------------------------------------------

        // Her model's transform: turned to face down the lane (+z) and then by yaw (the way she
        // walks), at (herX, herZ), her walks' sideways shift taken out.
        float HerTurn(){return DirectX::XM_PI+state.yaw;}
        DirectX::XMMATRIX HerWorld(MascotModel& model)
        {
            // (Her walks' shift across her model taken out, turned as she is.)
            const float r=model.RootOffset();
            return DirectX::XMMatrixMultiply(DirectX::XMMatrixRotationY(HerTurn()),DirectX::XMMatrixTranslation(state.herX-r*std::cos(HerTurn()),0,state.herZ+r*std::sin(HerTurn())));
        }
        XMFLOAT3 WorldPoint(MascotModel& model,const std::array<float,3>& p)
        {
            XMFLOAT3 w;
            DirectX::XMStoreFloat3(&w,DirectX::XMVector3Transform(DirectX::XMVectorSet(p[0],p[1],p[2],1),HerWorld(model)));
            return w;
        }
        std::array<float,3> ModelPoint(MascotModel& model,const XMFLOAT3& p)
        {
            DirectX::XMVECTOR det;
            XMFLOAT3 m;
            DirectX::XMStoreFloat3(&m,DirectX::XMVector3Transform(DirectX::XMVectorSet(p.x,p.y,p.z,1),DirectX::XMMatrixInverse(&det,HerWorld(model))));
            return {m.x,m.y,m.z};
        }
        // Her ball waiting on the ball return (world).
        XMFLOAT3 RackBall()
        {
            return {RackX*state.k,(0.48f+Bowling::BallRadius)*state.k,RackZ*state.k};
        }
        // Her feet where her model's ankles are now (after the generated walk), in the terms the
        // hold's feet are given in (Move's feet: the model's x from the hips' share, z from rest).
        void FeetFromModel(MascotModel& model)
        {
            const wchar_t* names[2]={L"左足首",L"右足首"};
            const float c=std::cos(HerTurn()),s=std::sin(HerTurn());
            for(int f=0;f<2;++f)
            {
                auto& foot=state.feet[static_cast<size_t>(f)];
                std::array<float,3> a{};
                foot.t=1;foot.lift=0;foot.placed=true;
                if(!model.BonePosition(names[f],a)){foot.placed=false;continue;}
                // (z from where the foot's IK stands at rest: the hold's feet go from there.)
                std::array<float,3> rest{};
                const float restZ=model.RestBonePosition(f==0?L"左足ＩＫ":L"右足ＩＫ",rest)?rest[2]:0.0f;
                const float mx=a[0]-state.track*state.restHip[static_cast<size_t>(f)],mz=a[2]-restZ;
                foot.x=state.herX+mx*c+mz*s;
                foot.z=state.herZ-mx*s+mz*c;
            }
        }
        // A world offset (x, z) from her as her model sees it, and her model's x across the world.
        std::array<float,2> ModelOffset(float dx,float dz)
        {
            const float c=std::cos(HerTurn()),s=std::sin(HerTurn());
            return {dx*c-dz*s,dx*s+dz*c};
        }
        std::array<float,2> WorldSide(float x)
        {
            return {x*std::cos(HerTurn()),-x*std::sin(HerTurn())};
        }

        // Her measures, once, as the alley opens: the alley made to her size, her arm.
        void MeasureHer(MascotModel& model)
        {
            std::array<float,3> lo{},hi{},legLo{},legHi{},head{};
            if(model.StandardBoneBounds(lo,hi)&&model.LegBounds(legLo,legHi)&&model.HeadPosition(head)&&head[1]>legHi[1])
            {
                state.hips=std::max(legHi[1]-lo[1],0.1f);
                state.upper=head[1]-legHi[1];
            }
            const float D=state.upper;
            // The alley made to her height: her head bone this far over her feet, as on a person
            // 1.6 m tall (by her upper body alone, a long-legged model stood near 3 m among the lanes).
            state.k=D/0.75f;
            if(head[1]>lo[1])state.k=(head[1]-lo[1])/1.47f;
            model.SetActivity(Activity::Idle);
            model.SetAutoGestures(false);
            model.EndGestures();
            model.StopMotion();
            for(int i=0;i<15||(i<90&&model.MotionPlaying());++i)model.Update(1/30.0);
            model.ClearArmStops();
            model.ResetPhysics();
            // Her right arm as it hangs at rest: the shoulder above the elbow, along the arm.
            std::array<float,3> middle{},racket{},palm{},wrist{},elbow{};
            state.armLength=0.95f*D;
            state.reach=0.84f*D;
            state.twist=0;
            state.shoulder={-0.20f*D,state.hips+0.80f*D,0};
            if(model.Fist(HerHand,middle,racket,palm,&wrist,&elbow))
            {
                const float fx=elbow[0]-wrist[0],fy=elbow[1]-wrist[1],fz=elbow[2]-wrist[2],fore=std::sqrt(fx*fx+fy*fy+fz*fz);
                if(fore>0.05f*D)
                {
                    const float upperArm=fore*1.12f;
                    state.shoulder={elbow[0]+fx/fore*upperArm,elbow[1]+fy/fore*upperArm,elbow[2]+fz/fore*upperArm};
                    state.armLength=fore+upperArm;
                    state.reach=upperArm+0.75f*fore;
                }
            }
            // (Kept to what a person's would be, whatever the rest pose.)
            state.shoulder[1]=std::clamp(state.shoulder[1],state.hips+0.68f*D,state.hips+0.92f*D);
            state.shoulder[0]=std::clamp(state.shoulder[0],-0.32f*D,-0.12f*D);
            state.shoulder[2]=std::clamp(state.shoulder[2],-0.08f*D,0.08f*D);
            state.armLength=std::clamp(state.armLength,0.75f*D,1.15f*D);
            state.reach=std::clamp(state.reach,0.65f*D,1.0f*D);
            state.handSide=-state.shoulder[0]+0.04f*D;
            // Her legs at rest: many models stand with the ankles well outside the hip joints, and
            // a walk or a bowler's approach puts the feet nearer the middle than that.
            if(std::array<float,4> legs{};model.RestLegs(legs)){state.restAnkle={legs[0],legs[1]};state.restHip={legs[2],legs[3]};}
            else{state.restAnkle={0.1f*D,-0.1f*D};state.restHip={0.1f*D,-0.1f*D};}
            if(std::array<float,3> w{};model.RestBonePosition(L"上半身",w))state.waist={w[1],w[2]};else state.waist={state.hips,0};
            state.track=1;
            Log(L"bowling: hips %.1f, upper body %.1f, %.2f units a metre; shoulder %.2f %.2f %.2f, arm %.2f",
                state.hips,state.upper,state.k,state.shoulder[0],state.shoulder[1],state.shoulder[2],state.armLength);
        }

        // How far she has gone along the approach t seconds into it (metres): her speed rises
        // through the first three steps and falls away in the slide.
        float ApproachDistance(float t)
        {
            static const float pts[7][2]={{0,0},{0.25f,1.0f},{0.8f,1.5f},{1.2f,1.9f},{1.5f,1.5f},{1.8f,0.35f},{1.95f,0}};
            auto speed=[](float s){
                if(s<=0||s>=1.95f)return 0.0f;
                for(int i=1;i<7;++i)
                    if(s<=pts[i][0])
                    {
                        // (Eased between the points, so her speed changes without a kink: straight
                        // lines between them gave a jolt at each.)
                        const float u=(s-pts[i-1][0])/(pts[i][0]-pts[i-1][0]);
                        return pts[i-1][1]+(pts[i][1]-pts[i-1][1])*u*u*(3-2*u);
                    }
                return 0.0f;
            };
            float d=0;
            const float h=0.005f,end=std::min(t,1.95f);
            for(float s=0;s<end;s+=h)d+=speed(s+std::min(h,end-s)/2)*std::min(h,end-s);
            return d;
        }

        // Her arm through the approach: degrees from hanging straight down (+ forward).
        float ArmAngle(float t)
        {
            if(t<0.40f)return 40.0f;   // (the push away: blended from the ball held before her)
            if(t<0.80f)return 40.0f+(-12.0f-40.0f)*Ease((t-0.40f)/0.40f);
            if(t<1.20f)return -12.0f+(-68.0f+12.0f)*std::sin(DirectX::XM_PIDIV2*(t-0.80f)/0.40f);
            if(t<ReleaseAt){const float u=(t-1.20f)/(ReleaseAt-1.20f);return -68.0f+(8.0f+68.0f)*(1-std::cos(DirectX::XM_PI*u))/2;}
            if(t<2.20f)return 8.0f+(100.0f-8.0f)*std::sin(DirectX::XM_PIDIV2*(t-ReleaseAt)/(2.20f-ReleaseAt));
            if(t<3.0f)return 100.0f;
            return 100.0f+(4.0f-100.0f)*Ease((t-3.0f)/(Relaxed-3.0f));
        }

        // Her shoulder (model space) with her body lowered by down and leant forward by lean (radians).
        std::array<float,3> Shoulder(float down,float lean)
        {
            // (Bent about 上半身, as the hold bends her.)
            const float hy=state.waist[0],hz=state.waist[1];
            const float ry=state.shoulder[1]-hy,rz=state.shoulder[2]-hz;
            return {state.shoulder[0],hy+ry*std::cos(lean)+rz*std::sin(lean)-down,hz+rz*std::cos(lean)-ry*std::sin(lean)};
        }

        // The ball held before her chest (model space), for the address.
        std::array<float,3> AddressBall()
        {
            const float D=state.upper;
            const auto s=Shoulder(0.03f*state.hips,DirectX::XMConvertToRadians(4.0f));
            return {s[0]+0.26f*D,s[1]-0.27f*D,s[2]-0.60f*D};   // (a little to her right of her middle, both hands on it with the elbows bent; well out before her: nearer, it was in her chest and showed through her back)
        }

        // Her other hand on the ball held before her: under it on her side (from the left, inner
        // half), not straight under it: reaching across to under the ball's middle (it is held to
        // her right) took her left elbow into her chest.
        std::array<float,3> UnderBall(const std::array<float,3>& ball)
        {
            const float r=Bowling::BallRadius*state.k;
            // (On its lower side toward her and her left, not under its far side: there her left arm
            // was out straight to it.)
            return {ball[0]+0.60f*r,ball[1]-0.45f*r,ball[2]+0.65f*r};
        }

        // The body's shape through the approach: down, forward, turn, lean, hips (HoldBody's).
        std::array<float,5> ApproachBody(float t)
        {
            using DirectX::XMConvertToRadians;
            const float Leg=state.hips;
            const float relax=1-Ease((t-2.6f)/(Relaxed-2.6f));
            const float down=Leg*(0.03f+(0.03f*Ease(t/1.2f)+0.25f*Ease((t-1.2f)/0.55f))*relax);
            const float lean=XMConvertToRadians(4.0f+(8.0f*Ease(t/1.2f)+28.0f*Ease((t-1.2f)/0.55f))*relax);
            const float turn=XMConvertToRadians(9.0f*Ease((t-0.4f)/0.7f)*(1-Ease((t-1.15f)/0.45f))-4.0f*Ease((t-1.6f)/0.3f)*(1-Ease((t-2.2f)/0.8f)));
            return {down,0,turn,lean,turn*0.5f};
        }

        // Her feet through the approach (world), t seconds into it; lift in model units.
        void ApproachFeet(float t)
        {
            const float k=state.k,Leg=state.hips;
            const float x0=state.herX,st=state.stance;
            auto bodyAt=[&](float s){return state.approachFrom+ApproachDistance(s)*k;};
            auto& left=state.feet[0];
            auto& right=state.feet[1];
            // Where each is planted before its step (set as each step begins).
            for(int i=0;i<4;++i)
            {
                auto& foot=state.feet[i%2==0?1:0];
                const float u=(t-StepAt[i])/StepLength[i];
                if(u<0||u>1.0f)continue;
                if(i<3)
                {
                    const float home=x0+(i%2==0?st:-st);
                    const float toZ=bodyAt(StepAt[i]+StepLength[i])+0.20f*k;
                    if(foot.t>=1||foot.toZ!=toZ){foot.fromX=foot.x;foot.fromZ=foot.z;foot.toX=home;foot.toZ=toZ;foot.t=0;}
                    foot.t=std::min(u,0.999f);
                    const float e=Ease(u);
                    foot.x=foot.fromX+(foot.toX-foot.fromX)*e;
                    foot.z=foot.fromZ+(foot.toZ-foot.fromZ)*e;
                    foot.lift=std::sin(DirectX::XM_PI*u)*0.09f*Leg;
                }
            }
            // A step done is set down where it was going, until that foot's next step begins (the
            // same foot's next step, the slide, or the right foot drawn back). Without that end,
            // the right foot was put at its second step's end the moment that step began: a jolt.
            for(int i=0;i<3;++i)
            {
                const float next=i+2<4?StepAt[i+2]:1.40f;
                if(t>=StepAt[i]+StepLength[i]&&t<next)
                {
                    auto& foot=state.feet[i%2==0?1:0];
                    if(foot.t<1){foot.x=foot.toX;foot.z=foot.toZ;}
                }
            }
            // The slide: the left foot set down early in the last step and carried on with her.
            if(t>=StepAt[3])
            {
                const float land=StepAt[3]+0.28f;
                if(t<land)
                {
                    if(left.t>=1){left.fromX=left.x;left.fromZ=left.z;left.t=0;}
                    const float u=(t-StepAt[3])/(land-StepAt[3]);
                    left.toX=x0-st+0.05f*k;
                    left.toZ=bodyAt(land)+0.10f*k;
                    left.x=left.fromX+(left.toX-left.fromX)*Ease(u);
                    left.z=left.fromZ+(left.toZ-left.fromZ)*Ease(u);
                    left.lift=std::sin(DirectX::XM_PI*u)*0.05f*Leg;
                }
                else
                {
                    left.x=x0-st+0.05f*k;
                    left.z=bodyAt(t)+(0.10f+0.20f*Ease((t-land)/0.40f))*k;
                    left.lift=0;
                }
                // The right foot drawn back behind her and across, its toe on the boards.
                if(t>=1.40f)
                {
                    static float fromX=0,fromZ=0;
                    static bool set=false;
                    if(t<1.42f||!set){fromX=right.x;fromZ=right.z;set=true;}
                    const float u=Ease((t-1.40f)/0.38f);
                    right.x=fromX+((x0+st-0.20f*k)-fromX)*u;
                    right.z=fromZ+((bodyAt(1.85f)-0.62f*k)-fromZ)*u;
                    right.lift=std::sin(DirectX::XM_PI*std::min(1.0f,(t-1.40f)/0.38f))*0.02f*Leg;
                    if(t>ApproachEnd)set=false;
                }
            }
            for(auto& f:state.feet){f.placed=true;if(t>ApproachEnd)f.t=1;}
            (void)right;
        }

        // Her feet as she walks about: each planted until her body has gone too far from it,
        // then lifted and set down ahead of her (never both off the floor).
        void WalkFeet(double dt)
        {
            const float D=state.upper;
            for(int f=0;f<2;++f)
            {
                auto& foot=state.feet[f];
                const auto side=WorldSide(f==0?state.stance:-state.stance);   // (her left is her model's +x)
                if(!foot.placed){foot.x=state.herX+side[0];foot.z=state.herZ+side[1];foot.placed=true;foot.t=1;}
            }
            for(int n=0;n<2;++n)
            {
                // (The foot further from where it belongs goes first.)
                const auto sl=WorldSide(state.stance),sr=WorldSide(-state.stance);
                const float d0=std::hypot(state.feet[0].x-(state.herX+sl[0]),state.feet[0].z-(state.herZ+sl[1]));
                const float d1=std::hypot(state.feet[1].x-(state.herX+sr[0]),state.feet[1].z-(state.herZ+sr[1]));
                const int f=(d0>=d1)==(n==0)?0:1;
                auto& foot=state.feet[f];
                const auto& other=state.feet[1-f];
                const auto side=WorldSide(f==0?state.stance:-state.stance);
                const float homeX=state.herX+side[0],homeZ=state.herZ+side[1];
                if(foot.t<1)
                {
                    foot.t=std::min(1.0f,foot.t+static_cast<float>(dt)/0.30f);
                    foot.toX=homeX+state.velX*0.20f;foot.toZ=homeZ+state.velZ*0.20f;
                    const float e=Ease(foot.t);
                    foot.x=foot.fromX+(foot.toX-foot.fromX)*e;
                    foot.z=foot.fromZ+(foot.toZ-foot.fromZ)*e;
                    foot.lift=std::sin(DirectX::XM_PI*foot.t)*0.07f*state.hips;
                }
                else if(other.t>=1&&std::hypot((homeX-foot.x)/0.24f,(homeZ-foot.z)/0.28f)>D)
                {
                    foot.fromX=foot.x;foot.fromZ=foot.z;
                    foot.toX=homeX+state.velX*0.20f;foot.toZ=homeZ+state.velZ*0.20f;
                    foot.t=0;
                    ++state.steps;
                }
                else foot.lift=0;
            }
        }

        // ---- Her words (as at table tennis) --------------------------------------------------

        constexpr ULONGLONG WordsHold=3000;

        const std::wstring& Pick(const std::vector<std::wstring>& lines){return lines[state.wordsRandom()%lines.size()];}

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
            w.weight=weight;
            w.since=now;
            w.told+=L"  she says: "+line.text+(line.voice.empty()?L"":L" ["+line.voice+L"]")+L"\n";
            const ULONGLONG reading=static_cast<ULONGLONG>(CharSeconds()*1000*line.text.size())+2400+90*static_cast<ULONGLONG>(line.text.size());
            w.until=now+(w.voiced?std::max(reading,static_cast<ULONGLONG>(state.voice.Seconds()*1000)+1800):reading);
            if(!line.text.empty()){w.recent.push_back(line.text);if(w.recent.size()>4)w.recent.erase(w.recent.begin());}
        }

        void Prefetch()
        {
            if(state.prefetching||!state.her.prefetch)return;
            const std::wstring ask=state.lines.NextAsk();
            if(!ask.empty())state.prefetching=state.her.prefetch(ask);
        }

        void Happened(int weight,const wchar_t* scene,double chance,const std::wstring& seen,const std::vector<std::wstring>& theirs)
        {
            const auto& g=state.game;
            state.words.told+=Fmt(L"  {0} s, weight {1}: {2}\n",{std::to_wstring(static_cast<int>(state.clock)),std::to_wstring(weight),seen});
            if(state.her.onEvent&&!theirs.empty())
                state.her.onEvent(seen+Fmt(T(L"（{0}が{1}点、持ち主が{2}点、{3}フレーム目）",L" ({0} has {1}, the owner {2}; frame {3})"),
                                           {HerName(),std::to_wstring(g.Score(Her)),std::to_wstring(g.Score(You)),std::to_wstring(std::min(10,g.Frame()+1))}),weight,Pick(theirs));
            if(!scene)return;
            auto& w=state.words;
            if(!w.text.empty()&&GetTickCount64()-w.since<WordsHold&&weight<w.weight)return;
            if(!state.lines.Roll(scene,chance,state.wordsRandom))return;
            GameLine line;
            if(state.lines.Pick(scene,w.recent,state.wordsRandom,line))SayLine(line,weight);
        }

        void Face(Expression e,double seconds)
        {
            if(!state.her.model)return;
            state.her.model->SetExpression(e,false);
            state.faceUntil=state.clock+seconds;
        }

        void ShowBanner(const std::wstring& big,const std::wstring& note,ULONGLONG ms)
        {
            state.banner=big;
            state.bannerNote=note;
            state.bannerUntil=GetTickCount64()+ms;
        }

        // ---- The game's course ----------------------------------------------------------------

        // A ball's turn begins: yours, or hers (she goes to her place, the ball comes back to her).
        void StartReady()
        {
            state.stage=Stage::Ready;
            state.stageTime=0;
            state.rolled=false;
            state.thrower=state.game.Turn();
            state.pull=0;
            state.pressing=false;
            state.samples.clear();
            state.trail.clear();
            const int frame=state.game.Frame();
            if(state.thrower==Her)
            {
                std::array<bool,10> standing{};
                for(int n=0;n<10;++n)standing[static_cast<size_t>(n)]=state.pins[static_cast<size_t>(n)].there;
                state.herThrow=Bowling::Choose(standing,Bowling::LevelStyle(state.her.level),state.random);
                state.plan=state.hasBall?Plan::Walk:Plan::Fetch;
                state.planTime=0;
                state.released=false;
                if(state.game.BallNo()==0)
                    ShowBanner(Fmt(T(L"{0}の番",L"{0}'s turn"),{HerName()}),Fmt(T(L"第{0}フレーム",L"Frame {0}"),{std::to_wstring(frame+1)}),1400);
            }
            else
            {
                state.plan=Plan::Walk;
                state.planTime=0;
                if(state.game.BallNo()==0)
                    ShowBanner(T(L"あなたの番",L"Your turn"),Fmt(T(L"第{0}フレーム",L"Frame {0}"),{std::to_wstring(frame+1)}),1400);
                if(frame==9&&state.game.BallNo()==0&&!state.lastFrameSaid)
                {
                    state.lastFrameSaid=true;
                    Happened(6,L"最後のフレーム",1,T(L"ボウリングが最後の10フレーム目に入った。",L"The bowling game has come to its tenth and last frame."),
                             {T(L"いよいよ最後だね！",L"Last frame!"),T(L"どっちが勝つかな？",L"Who's going to win?")});
                }
            }
        }

        void NewGame()
        {
            state.game.New();
            state.pins=Rack();
            state.gutters={};
            state.inRow={};
            state.trail.clear();
            state.sparks.clear();
            state.her.level=std::clamp(state.her.level,1,3);
            state.paused=state.asking=false;
            state.started=state.overSeen=state.cheered=state.lastFrameSaid=false;
            state.faceUntil=0;
            state.levelShownUntil=0;
            state.words.text.clear();
            state.afterword.clear();
            state.afterwordLine.clear();
            state.hasBall=false;
            state.approachTime=-1;
            state.hudShown.clear();
            StartReady();
        }

        // The ball goes: from (world) where it leaves the hand.
        void Release(const Throw& t,XMFLOAT3 from)
        {
            state.thrown=t;
            state.roll.Start(t,state.pins);
            state.rolled=true;
            state.stage=Stage::Rolling;
            state.stageTime=0;
            state.ballFrom=from;
            state.trail.clear();
            state.lastSpeed=t.speed;
            state.crashAt=-1;
            state.crashed=false;
            Play(Sound::Roll);
            state.words.told+=Fmt(L"  {0} throws: from {1} m, {2} degrees, {3} m/s, spin {4}\n",
                                  {state.thrower==You?L"you":L"she",std::to_wstring(t.x),std::to_wstring(t.angle*57.2958f),std::to_wstring(t.speed),std::to_wstring(t.spin)});
        }

        // The ball has done its work: the pins counted, the score kept, what she says of it.
        void Settle()
        {
            const int side=state.thrower,frame=state.game.Frame();
            const bool fresh=state.game.FreshRack();
            const int before=Count(state.pins);
            state.kept=state.roll.Left();
            const int down=before-Count(state.kept);
            const bool gutter=state.roll.InGutter();
            state.gutters[side][frame].push_back(gutter&&down==0);
            const int what=state.game.Record(down);
            const bool frameOver=state.game.Turn()!=side||state.game.Over();
            state.stage=Stage::Result;
            state.stageTime=0;
            state.words.told+=Fmt(L"  {0} down{1}\n",{std::to_wstring(down),gutter?L" (gutter)":L""});
            if(what==1)
            {
                ++state.inRow[side];
                const int row=state.inRow[side];
                ShowBanner(row>=3?T(L"ターキー！",L"Turkey!"):row==2?T(L"ダブル！",L"Double!"):T(L"ストライク！",L"Strike!"),L"",1800);
                Burst(true);
            }
            else
            {
                if(fresh||frameOver)state.inRow[side]=what==2?0:state.inRow[side];
                if(what==2){ShowBanner(T(L"スペア！",L"Spare!"),L"",1600);Burst(false);}
                else if(gutter&&down==0)ShowBanner(T(L"ガター",L"Gutter"),L"",1400);
                else ShowBanner(Fmt(T(L"{0}本",L"{0} down"),{std::to_wstring(down)}),
                                frameOver?Fmt(T(L"残り{0}本",L"{0} left"),{std::to_wstring(Count(state.kept))}):std::wstring(),1300);
                if(what!=2)state.inRow[side]=0;
            }
            const std::wstring name=HerName();
            if(side==Her)
            {
                if(what==1)
                {
                    Face(Expression::Smile,2.4);
                    if(state.inRow[side]>=2)Happened(7,L"ダブル",1,Fmt(T(L"ボウリングで、{0}がストライクを続けて取った。",L"At bowling, {0} threw strikes in a row."),{name}),
                                                     {T(L"また全部倒した！",L"All of them again!"),T(L"すごい連続ストライク！",L"Strike after strike!")});
                    else Happened(6,L"ストライクを取った",0.9,Fmt(T(L"ボウリングで、{0}がストライクを取った。",L"At bowling, {0} threw a strike."),{name}),
                                  {Fmt(T(L"{0}、ナイスストライク！",L"Nice strike, {0}!"),{name}),T(L"おお、全部倒れた！",L"Ooh, all of them!")});
                }
                else if(what==2)
                {
                    Face(Expression::Smile,1.8);
                    Happened(5,L"スペアを取った",0.7,Fmt(T(L"ボウリングで、{0}がスペアを取った。",L"At bowling, {0} picked up a spare."),{name}),
                             {T(L"ナイススペア！",L"Nice spare!")});
                }
                else if(gutter&&down==0)
                {
                    Face(Expression::Troubled,2.0);
                    Happened(5,L"ガターだった",0.8,Fmt(T(L"ボウリングで、{0}の球がガターに落ちた。",L"At bowling, {0}'s ball went into the gutter."),{name}),
                             {T(L"ドンマイ！",L"Never mind!")});
                }
                else if(frameOver)
                {
                    Face(Expression::Troubled,1.6);
                    Happened(4,L"ピンが残った",0.5,Fmt(T(L"ボウリングで、{0}がピンを{1}本残した。",L"At bowling, {0} left {1} pins standing."),{name,std::to_wstring(Count(state.kept))}),
                             {T(L"おしい！",L"So close!")});
                }
            }
            else
            {
                if(what==1)
                {
                    Face(Expression::Surprise,1.8);
                    Happened(6,L"ストライクを取られた",0.8,T(L"ボウリングで、持ち主がストライクを取った。",L"At bowling, the owner threw a strike."),
                             {T(L"ナイスストライク！",L"Nice strike!"),T(L"全部倒れた！",L"All of them down!")});
                }
                else if(what==2)Happened(5,L"スペアを取られた",0.55,T(L"ボウリングで、持ち主がスペアを取った。",L"At bowling, the owner picked up a spare."),{T(L"ナイススペア！",L"Nice spare!")});
                else if(gutter&&down==0)Happened(4,L"相手のガター",0.6,T(L"ボウリングで、持ち主の球がガターに落ちた。",L"At bowling, the owner's ball went into the gutter."),{T(L"ドンマイ！",L"Never mind!")});
            }
            if(state.game.Over()&&!state.overSeen)
            {
                state.overSeen=true;
                const int winner=state.game.Winner(),hers=state.game.Score(Her),yours=state.game.Score(You);
                const std::wstring score=Fmt(T(L"あなた{0}点、相手{1}点",L"you {0}, they {1}"),{std::to_wstring(hers),std::to_wstring(yours)});
                if(winner==Her)Happened(9,L"勝った",1,Fmt(T(L"{0}と持ち主のボウリングが終わり、{0}が勝った。",L"The bowling game between {0} and the owner is over: {0} won."),{name}),
                                        {T(L"おつかれさま！いい勝負だったね。",L"Well played, both of you!"),Fmt(T(L"{0}の勝ちだ！",L"{0} wins!"),{name})});
                else if(winner==You)Happened(9,L"負けた",1,Fmt(T(L"{0}と持ち主のボウリングが終わり、持ち主が勝った。",L"The bowling game between {0} and the owner is over: the owner won."),{name}),
                                             {T(L"おつかれさま！いい勝負だったね。",L"Well played, both of you!"),T(L"勝った！おめでとう！",L"You won! Congratulations!")});
                else Happened(9,L"引き分け",1,Fmt(T(L"{0}と持ち主のボウリングが同点で終わった。",L"The bowling game between {0} and the owner ended in a tie."),{name}),
                              {T(L"同点だ！",L"A tie!")});
                state.afterword=Fmt(T(L"いま、話し相手とボウリングを最後までして、デスクトップに戻ってきたところ。結果は{0}（{1}、ストライクはあなた{2}回、相手{3}回）。その感想を、話し相手にひと言（1〜2文）",
                                      L"You have just bowled a whole game with them and come back to the desktop. {0} ({1}; strikes: you {2}, they {3}). Tell them what you thought of it (one or two sentences)"),
                                    {winner==Her?T(L"あなたの勝ち",L"You won"):winner==You?T(L"あなたの負け",L"You lost"):T(L"引き分け",L"A tie"),score,
                                     std::to_wstring(state.game.Strikes(Her)),std::to_wstring(state.game.Strikes(You))});
                state.afterwordLine=winner==Her?T(L"ボウリング、楽しかったね！",L"Bowling was fun!"):T(L"くやしいなあ、次は勝つよ！",L"So close... I'll win next time!");
                state.overAt=state.clock;
                state.cheered=false;
            }
        }

        // ---- Your ball: the mouse ------------------------------------------------------------

        // Where the cursor puts your ball across the lane (on the plane where it is held).
        bool StandAt(float px,float py,float& x)
        {
            using namespace DirectX;
            RECT client{};
            GetClientRect(state.view,&client);
            const float w=static_cast<float>(std::max(1L,client.right)),h=static_cast<float>(std::max(1L,client.bottom));
            const XMVECTOR eye=XMLoadFloat3(&state.eye),forward=XMVector3Normalize(XMVectorSubtract(XMLoadFloat3(&state.look),eye));
            const XMVECTOR right=XMVector3Normalize(XMVector3Cross(XMVectorSet(0,1,0,0),forward)),up=XMVector3Cross(forward,right);
            const float t=std::tan(XMConvertToRadians(40.0f)/2);
            const XMVECTOR ray=XMVectorAdd(forward,XMVectorAdd(XMVectorScale(right,(2*px/w-1)*t*w/h),XMVectorScale(up,(1-2*py/h)*t)));
            const float dz=XMVectorGetZ(ray);
            if(dz<=1e-4f)return false;
            const float along=(HeldZ*state.k-XMVectorGetZ(eye))/dz;
            x=(XMVectorGetX(eye)+XMVectorGetX(ray)*along)/state.k;
            (void)py;
            return true;
        }

        bool YourBall(){return state.stage==Stage::Ready&&state.thrower==You&&Playing()&&!state.autoplay;}

        void PressBall(float x,float y)
        {
            if(!YourBall())return;
            state.pressing=true;
            state.pressY=y;
            state.samples.clear();
            state.samples.push_back({state.clock,x,y});
        }

        // The button let go: the stroke's last moments give the ball its speed and its way.
        void LetGo(float x,float y)
        {
            if(!state.pressing)return;
            state.pressing=false;
            state.samples.push_back({state.clock,x,y});
            RECT client{};
            GetClientRect(state.view,&client);
            const float h=static_cast<float>(std::max(1L,client.bottom));
            // Over the last 0.09 s of the stroke.
            const auto& last=state.samples.back();
            const State::Sample* from=&state.samples.front();
            for(const auto& s:state.samples)if(last.at-s.at<=0.09){from=&s;break;}
            const double span=std::max(1e-3,last.at-from->at);
            const float up=static_cast<float>((from->y-last.y)/span/h),across=static_cast<float>((last.x-from->x)/span/h);
            state.pull=0;
            if(up<0.6f||state.pressY-y<-0.05f*h*0)
            {
                if(up<0.6f)
                {
                    state.hint=T(L"マウスを前（上）へ速く押し出すと投げられます",L"Push the mouse forward (up) quickly to throw");
                    state.hintUntil=GetTickCount64()+2500;
                    return;
                }
            }
            Throw t;
            t.x=state.standX;
            t.speed=std::clamp(3.0f+1.6f*up,3.5f,10.5f);
            t.angle=std::clamp(std::atan2(across,up)*0.10f,-0.06f,0.06f);
            t.spin=state.spin;
            Release(t,World({state.standX,HeldZ,HeldHeight}));
            state.spin=0;   // (the spin set for one ball only: the next starts straight)
        }

        // ---- The words over the alley -----------------------------------------------------

        // The marks of a side's frame: X a strike, / a spare, - none down, G a gutter.
        std::vector<std::wstring> Marks(int side,int f)
        {
            const auto& r=state.game.Throws(side,f);
            const auto& g=state.gutters[side][f];
            std::vector<std::wstring> out;
            auto mark=[&](size_t i,int pins){
                if(i<g.size()&&g[i])return std::wstring(L"G");
                return pins==0?std::wstring(L"-"):std::to_wstring(pins);
            };
            if(f<9)
            {
                if(r.empty())return {L"",L""};
                if(r[0]==10)return {L"",L"X"};
                out.push_back(mark(0,r[0]));
                out.push_back(r.size()<2?L"":r[0]+r[1]==10?L"/":mark(1,r[1]));
                return out;
            }
            int standing=10;
            for(size_t i=0;i<3;++i)
            {
                if(i>=r.size()){out.push_back(L"");continue;}
                const int p=r[i];
                if(standing==10&&p==10){out.push_back(L"X");standing=10;}
                else if(p==standing){out.push_back(L"/");standing=10;}
                else{out.push_back(mark(i,p));standing-=p;if(standing<=0)standing=10;}
            }
            return out;
        }

        void DrawBoard(Graphics& g,float W,float s)
        {
            const float cell=std::min(64*s,(W-24*s-120*s-80*s)/10.5f),rowH=46*s,x0=12*s+120*s,y0=6*s;
            const Color line(255,70,80,110),soft(255,232,236,248),gold(255,250,214,120),dim(255,150,160,190);
            {
                GraphicsPath path;
                RoundRect(path,RectF(6*s,2*s,x0+cell*10.5f+80*s-6*s,rowH*2+12*s),10*s);
                SolidBrush back(Color(200,12,14,30));
                g.FillPath(&back,&path);
            }
            Pen pen(line,1.2f*s);
            const std::wstring names[2]={T(L"あなた",L"You"),HerName()};
            for(int side=0;side<2;++side)
            {
                const float y=y0+side*rowH;
                const bool now=!state.game.Over()&&state.game.Turn()==side;
                Text(g,(now?L"▶ ":L"")+names[side],RectF(12*s,y,120*s-8*s,rowH),15*s,now?gold:soft,now,StringAlignmentNear);
                for(int f=0;f<10;++f)
                {
                    const float w=f==9?cell*1.5f:cell,x=x0+(f==9?cell*9:cell*f);
                    const bool current=now&&state.game.Frame()==f;
                    if(current){SolidBrush lit(Color(70,250,214,120));g.FillRectangle(&lit,RectF(x,y,w,rowH));}
                    g.DrawRectangle(&pen,RectF(x,y,w,rowH));
                    const auto marks=Marks(side,f);
                    const float box=cell/2.2f;
                    for(size_t i=0;i<marks.size();++i)
                    {
                        const float bx=x+w-box*(marks.size()-i);
                        g.DrawRectangle(&pen,RectF(bx,y,box,rowH*0.45f));
                        const bool big=marks[i]==L"X"||marks[i]==L"/";
                        Text(g,marks[i],RectF(bx,y,box,rowH*0.45f),13*s,big?gold:soft,big);
                    }
                    const int total=state.game.Total(side,f);
                    if(total>=0)Text(g,std::to_wstring(total),RectF(x,y+rowH*0.42f,w,rowH*0.58f),16*s,soft,true);
                }
                const float tx=x0+cell*10.5f;
                g.DrawRectangle(&pen,RectF(tx,y,80*s-12*s,rowH));
                Text(g,std::to_wstring(state.game.Score(side)),RectF(tx,y,80*s-12*s,rowH),20*s,gold,true);
            }
            (void)dim;
        }

        void DrawHud(Graphics& g,int w,int h,float s)
        {
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAlias);
            const Color white(255,240,242,248),soft(255,232,238,250),gold(255,246,210,120);
            const float W=static_cast<float>(w),H=static_cast<float>(h);
            DrawBoard(g,W,s);
            // Your ball: its spin, the last ball's speed, her strength.
            {
                const float bw=std::min(W-24*s,560*s),bh=30*s,y=104*s;
                GraphicsPath path;
                RoundRect(path,RectF(12*s,y,bw,bh),8*s);
                SolidBrush back(Color(160,12,14,30));
                g.FillPath(&back,&path);
                const float sp=state.spin;
                const std::wstring spin=std::abs(sp)<0.05f?T(L"まっすぐ",L"straight"):
                                        Fmt(sp>0?T(L"左へ曲がる {0}",L"hooks left {0}"):T(L"右へ曲がる {0}",L"hooks right {0}"),{std::wstring(static_cast<size_t>(std::round(std::abs(sp)*10)),L'●')});
                std::wstring line=Fmt(T(L"回転：{0}（ホイール）",L"Spin: {0} (wheel)"),{spin});
                if(state.lastSpeed>0)line+=Fmt(T(L"　前の球 {0} km/h",L"   last ball {0} km/h"),{std::to_wstring(static_cast<int>(std::round(state.lastSpeed*3.6f)))});
                line+=Fmt(T(L"　相手：{0}（1〜3キー）",L"   Opponent: {0} (keys 1-3)"),{LevelName(state.her.level)});
                Text(g,line,RectF(22*s,y,bw-20*s,bh),13*s,soft,false,StringAlignmentNear);
            }
            {
                const std::wstring keys=YourBall()?T(L"マウス左右：立ち位置　ホイール：回転　左ボタンを押したまま手前へ引き、前へ押し出して離す：投げる　P：一時停止　Esc：やめる",
                                                    L"Mouse sideways: where you stand   Wheel: spin   Hold the left button, draw back, push forward and let go: throw   P: pause   Esc: stop"):
                                                    T(L"中ボタンでドラッグ：カメラ　P：一時停止　Esc：やめる",L"Middle drag: camera   P: pause   Esc: stop");
                const float bh=26*s;
                SolidBrush back(Color(120,14,20,34));
                g.FillRectangle(&back,RectF(0,H-bh,W,bh));
                Text(g,keys,RectF(0,H-bh,W,bh),12.5f*s,soft);
            }
            const float mid=H*0.42f;
            if(!state.words.text.empty())
            {
                const std::wstring& said=state.words.text;
                const float most=std::max(160*s,W*0.34f);
                const WordsBubble bubble(g,said,s,most);
                const float bw=bubble.Width(),bh=bubble.Height();
                if(state.headSeen)
                {
                    const float x=std::clamp(static_cast<float>(state.besideHead.x),14*s,W-14*s-bw);
                    const float headY=static_cast<float>(state.headScreen.y);
                    const float y=std::clamp(headY-bh*0.6f,140*s,std::max(140*s,H-60*s-bh));
                    const float ty=std::clamp(headY,y+12*s,y+bh-12*s);
                    bubble.Draw(g,x,y,ty,ShownWords());
                }
                else
                {
                    // She is out of sight (the camera down the lane with the ball): at the lower
                    // left, the tail pointing down toward where she stands, behind the view.
                    const float x=16*s,y=std::max(140*s,H-26*s-40*s-bh);
                    bubble.DrawPointing(g,x,y,x+44*s,H-30*s,ShownWords(),true);
                }
            }
            auto banner=[&](const std::wstring& big,const std::wstring& note,float at){
                const float bw=std::min(W-40*s,460*s),bh=note.empty()?64*s:96*s;
                const RectF box(W/2-bw/2,at-bh/2,bw,bh);
                GraphicsPath path;
                RoundRect(path,box,12*s);
                SolidBrush back(Color(215,16,22,36));
                g.FillPath(&back,&path);
                Text(g,big,RectF(box.X,box.Y+8*s,box.Width,note.empty()?box.Height-16*s:48*s),28*s,gold,true);
                if(!note.empty())Text(g,note,RectF(box.X,box.Y+56*s,box.Width,30*s),14*s,white);
            };
            const ULONGLONG now=GetTickCount64();
            if(state.game.Over()&&state.stage==Stage::Over)
            {
                const int winner=state.game.Winner();
                std::wstring note=std::to_wstring(state.game.Score(You))+L" - "+std::to_wstring(state.game.Score(Her));
                note+=T(L"　Enterでもう一度・Escで閉じる",L"   Enter: again, Esc: close");
                // (High, under the scores: she stands facing the camera below it.)
                banner(winner==You?T(L"あなたの勝ち！",L"You win!"):winner==Her?HerName()+T(L"の勝ち",L" wins"):std::wstring(T(L"引き分け",L"A tie")),note,std::max(H*0.21f,198*s));
            }
            else if(state.asking)
            {
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
            else if(state.paused)banner(T(L"一時停止",L"Paused"),T(L"Pかクリックで再開",L"P or a click to go on"),mid);
            else if(now<state.bannerUntil)banner(state.banner,state.bannerNote,H*0.30f);
            else if(now<state.hintUntil)banner(state.hint,L"",H*0.30f);
        }

        void UpdateOverlay()
        {
            RECT client{};
            GetClientRect(state.window,&client);
            const int w=std::max(1L,client.right),h=std::max(1L,client.bottom);
            const ULONGLONG now=GetTickCount64();
            std::wstring board;
            for(int side=0;side<2;++side)for(int f=0;f<10;++f){for(const auto& m:Marks(side,f))board+=m+L",";board+=std::to_wstring(state.game.Total(side,f))+L";";}
            const std::wstring shown=std::to_wstring(w)+L"x"+std::to_wstring(h)+L" "+board+std::to_wstring(state.game.Turn())+std::to_wstring(state.game.Frame())+
                                     std::to_wstring(static_cast<int>(state.stage))+(YourBall()?L"y":L"")+(state.paused?L"p":L"")+(state.asking?L"a":L"")+
                                     (now<state.levelShownUntil?L"l":L"")+(now<state.bannerUntil?state.banner+state.bannerNote:L"")+(now<state.hintUntil?L"h":L"")+
                                     std::to_wstring(state.her.level)+L"|"+std::to_wstring(static_cast<int>(std::round(state.spin*10)))+L"|"+std::to_wstring(static_cast<int>(state.lastSpeed*36))+L"|"+
                                     ShownWords()+(state.headSeen?L"|s":L"|")+std::to_wstring(state.besideHead.x/12)+L","+std::to_wstring(state.headScreen.y/12);
            if(shown==state.hudShown)return;
            state.hudShown=shown;
            Bitmap canvas(w,h,PixelFormat32bppPARGB);
            {
                Graphics gr(&canvas);
                gr.Clear(Color(0,0,0,0));
                DrawHud(gr,w,h,Scale());
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

        // ---- The window ------------------------------------------------------------------

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
            const auto& g=state.game;
            if(state.afterword.empty()&&!g.Over()&&(g.Frame()>0||g.BallNo()>0||g.Turn()==Her)&&state.renderer)
            {
                state.afterword=Fmt(T(L"いま、話し相手とボウリングをしていたが、途中でやめてデスクトップに戻ってきたところ（{2}フレーム目まで、あなた{0}点、相手{1}点だった）。そのことを話し相手にひと言（1〜2文）",
                                      L"You were bowling with them, but they stopped before the end and you have come back to the desktop (up to frame {2}, you had {0}, they had {1}). Say a word about it to them (one or two sentences)"),
                                    {std::to_wstring(g.Score(Her)),std::to_wstring(g.Score(You)),std::to_wstring(std::min(10,g.Frame()+1))});
                state.afterwordLine=T(L"また今度、続きしようね。",L"Let's play again another time.");
            }
            if(MascotModel* model=state.her.model)
            {
                model->Hold(HerHand,{},false);
                if(state.walking){model->SetActivity(Activity::Idle);model->SetFacingWalk(false);}
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

        void ChooseLevel(int level)
        {
            state.her.level=level;
            state.levelShownUntil=GetTickCount64()+1200;
            if(state.her.onLevel)state.her.onLevel(level);
        }

        void ChangeSpin(int notches)
        {
            if(!YourBall()||state.pressing)return;
            state.spin=std::clamp(std::round((state.spin+0.1f*notches)*10)/10,-1.0f,1.0f);
        }

        // The game on by dt: your stroke, the ball, the pins, the sweep.
        void StepGame(double dt)
        {
            const bool front=GetForegroundWindow()==state.window;
            if(!front&&Playing()&&!state.autoplay)state.paused=true;
            HoldCursor(YourBall()&&!state.dragging&&front&&!IsIconic(state.window));
            auto& w=state.words;
            if(!w.text.empty()&&GetTickCount64()>w.until&&state.testStage!=6)w.text.clear();   // (6: diagnostics, kept up)
            if(state.faceUntil>0&&state.clock>state.faceUntil){state.faceUntil=0;if(state.her.model)state.her.model->SetExpression(Expression::Neutral,false);}
            if(!state.started&&(!state.prefetching||state.clock>4))
            {
                state.started=true;
                Happened(7,L"始まり",1,Fmt(T(L"{0}と、このパソコンの持ち主のボウリングが始まる。",L"A bowling game between {0} and the owner of this computer is about to begin."),{HerName()}),
                         {T(L"がんばれー！",L"Good luck, you two!"),T(L"どっちが勝つかな？",L"Who's going to win?")});
            }
            if(state.paused||state.asking)return;
            state.clock+=dt;
            state.stageTime+=dt;
            // The sparks fly and fall.
            {
                const float step=static_cast<float>(dt);
                for(auto& sp:state.sparks)
                {
                    sp.age+=step;
                    sp.p.x+=sp.v.x*step;sp.p.y+=sp.v.y*step;sp.p.z+=sp.v.z*step;
                    sp.v.z-=3.0f*step;
                }
                std::erase_if(state.sparks,[](const State::Spark& sp){return sp.age>=sp.life;});
            }
            // Your stroke: the cursor sets where you stand; drawn back with the button down.
            POINT cursor;
            if(YourBall()&&!state.dragging&&GetCursorPos(&cursor)&&ScreenToClient(state.view,&cursor))
            {
                const float cx=static_cast<float>(cursor.x),cy=static_cast<float>(cursor.y);
                if(state.pressing)
                {
                    RECT client{};
                    GetClientRect(state.view,&client);
                    const float h=static_cast<float>(std::max(1L,client.bottom));
                    state.samples.push_back({state.clock,cx,cy});
                    while(state.samples.size()>2&&state.clock-state.samples.front().at>0.5)state.samples.pop_front();
                    state.pull=std::clamp((cy-state.pressY)/h*1.6f,-0.25f,0.70f);
                    // Let go when the stroke has come forward past where it began (or the button is up).
                    if(!(GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON)?VK_RBUTTON:VK_LBUTTON)&0x8000))LetGo(cx,cy);
                }
                else
                {
                    // By how far the cursor moves, not where it is: taken far past the lane's edge,
                    // the ball would otherwise wait until the cursor came all the way back.
                    float x=0,was=0;
                    if(state.cursorKnown&&StandAt(cx,cy,x)&&StandAt(state.cursorX,cy,was))
                        state.standX=std::clamp(state.standX+(x-was),-(Bowling::LaneHalf-0.12f),Bowling::LaneHalf-0.12f);
                    state.cursorX=cx;
                    state.cursorKnown=true;
                }
            }
            else state.cursorKnown=false;
            if(state.pressing)state.cursorKnown=false;
            switch(state.stage)
            {
            case Stage::Ready:
                if(state.thrower==You&&state.autoplay&&state.stageTime>1.0&&state.testStage!=4)
                {
                    std::array<bool,10> standing{};
                    for(int n=0;n<10;++n)standing[static_cast<size_t>(n)]=state.pins[static_cast<size_t>(n)].there;
                    Throw t=Bowling::Choose(standing,Bowling::LevelStyle(2),state.random);
                    state.standX=t.x;
                    Release(t,World({t.x,HeldZ,HeldHeight}));
                }
                if(state.thrower==You&&state.autoplay&&state.testStage==4)state.pull=0.6f;
                break;
            case Stage::Rolling:
            {
                state.roll.Step(dt);
                for(const auto& e:state.roll.TakeEvents())
                {
                    if(e.kind==Bowling::Event::BallPin&&state.crashAt<0)state.crashAt=state.clock+0.12;
                    else if(e.kind==Bowling::Event::Gutter)Play(Sound::Gutter);
                }
                if(state.crashAt>=0&&!state.crashed&&state.clock>=state.crashAt)
                {
                    state.crashed=true;
                    int fallen=0;
                    for(int n=0;n<10;++n)if(state.roll.Pins()[static_cast<size_t>(n)].there&&!state.roll.Standing(n))++fallen;
                    Play(fallen>=7?Sound::Big:fallen>=3?Sound::Crash:Sound::Light);
                }
                const Vec3 b=state.roll.Ball();
                state.trail.push_back({b,state.clock});
                while(!state.trail.empty()&&state.clock-state.trail.front().at>TrailSeconds)state.trail.erase(state.trail.begin());
                if(state.roll.Done())Settle();
                break;
            }
            case Stage::Result:
                while(!state.trail.empty()&&state.clock-state.trail.front().at>TrailSeconds)state.trail.erase(state.trail.begin());
                if(state.stageTime>ResultSeconds)
                {
                    state.nextPins=state.game.Over()?state.kept:state.game.FreshRack()?Rack():state.kept;
                    state.stage=Stage::Sweep;
                    state.stageTime=0;
                }
                break;
            case Stage::Sweep:
                if(state.stageTime>SweepSeconds)
                {
                    state.pins=state.nextPins;
                    state.rolled=false;
                    if(state.game.Over()){state.stage=Stage::Over;state.stageTime=0;state.plan=Plan::Walk;}
                    else StartReady();
                }
                break;
            case Stage::Over:
                break;
            }
        }

        // ---- Her motion each frame -------------------------------------------------------------

        void MoveHer(MascotModel& model,double dt)
        {
            using namespace DirectX;
            const float k=state.k,D=state.upper,Leg=state.hips;
            const auto& g=state.game;
            const float step=static_cast<float>(dt);
            const bool live=!state.paused&&!state.asking;
            state.stance=0.015f*Leg;   // (her feet a little apart beyond her rest pose: more looked splayed)
            // Her plan: to her place, the ball held, the approach.
            const bool herBall=state.thrower==Her&&(state.stage==Stage::Ready||state.released);
            if(live)
            {
                state.planTime+=dt;
                if(state.stage==Stage::Ready&&state.thrower==Her&&!state.released)
                {
                    if(state.plan==Plan::Fetch)
                    {
                        // Beside the ball return, facing it: then down for the ball.
                        // (Reaching already in her last steps, so the stop and the reach are one move:
                        // the walk goes on to the spot under her while she bends for the ball.)
                        const float there=std::hypot(state.herX-FetchX*k,state.herZ-FetchZ*k);
                        if((state.walking&&there<0.45f*k)
                           ||(!state.walking&&there<0.06f*k&&std::hypot(state.velX,state.velZ)<0.15f*k&&std::abs(std::remainder(state.yaw-XM_PIDIV2,XM_2PI))<0.30f))
                        {
                            state.plan=Plan::Lift;
                            state.planTime=0;
                            state.picked=false;
                            state.reachFix={};
                            state.stoop=0;
                            if(state.autoplay&&state.swingTrace.size()<20000){wchar_t row[80];swprintf_s(row,L"  fetch at clock %.2f\n",state.clock);state.swingTrace+=row;}
                        }
                    }
                    else if(state.plan==Plan::Lift)
                    {
                        if(state.testStage==8)state.planTime=std::min(state.planTime,static_cast<double>(ReachSeconds+GripSeconds-0.02f));   // (diagnostics: her hands on the ball)
                        // (Where it is as she takes it: her ball's place was still where she last threw it
                        // for that frame, and her left hand went for it.)
                        if(!state.hasBall&&state.planTime>ReachSeconds+GripSeconds){state.hasBall=true;state.herBall=RackBall();}
                        if(state.planTime>ReachSeconds+GripSeconds+LiftSeconds){state.plan=Plan::Walk;state.planTime=0;}
                    }
                    else if(state.plan==Plan::Walk)
                    {
                        const float gx=state.herThrow.x*k-state.handSide,gz=StanceZ*k;
                        if(std::hypot(state.herX-gx,state.herZ-gz)<0.03f*k&&std::hypot(state.velX,state.velZ)<0.08f*k&&state.planTime>0.4&&std::abs(state.yaw)<0.05f)
                        {
                            state.plan=Plan::Address;
                            state.planTime=0;
                            state.hasBall=true;
                        }
                    }
                    else if(state.plan==Plan::Address&&state.testStage==5)state.planTime=0;   // (diagnostics: held at her address)
                    else if(state.plan==Plan::Address&&state.planTime>AddressSeconds)
                    {
                        state.plan=Plan::Approach;
                        state.planTime=0;
                        state.approachTime=0;
                        state.approachFrom=state.herZ;
                        state.herX=state.herThrow.x*k-state.handSide;
                    }
                }
                if(state.approachTime>=0)
                {
                    float t=static_cast<float>(state.approachTime)+step;
                    // (Diagnostics: held at one moment of it.)
                    if(state.testStage>=1&&state.testStage<=3)t=std::min(t,state.testStage==1?1.20f:state.testStage==2?ReleaseAt-0.02f:2.4f);
                    state.approachTime=t;
                    if(t>=ReleaseAt&&!state.released&&state.stage==Stage::Ready&&state.thrower==Her)
                    {
                        state.released=true;
                        state.hasBall=false;
                        const XMFLOAT3 from=state.herBall;
                        Release(state.herThrow,from);
                        const XMFLOAT3 start=World(state.roll.Ball());
                        state.releaseOff=std::hypot(from.x-start.x,from.z-start.z)/k;
                        state.releaseSide=(from.x-start.x)/k;
                        // Where her ball really hangs at the release, learned for the next: she
                        // stands so that it comes to her aim.
                        if(state.testStage==0)state.handSide+=0.8f*((from.x-state.herX)-state.handSide);
                    }
                    if(t>Relaxed)
                    {
                        state.approachTime=-1;
                        state.plan=Plan::Walk;
                        state.planTime=0;
                    }
                }
            }
            // Where her body goes.
            const bool approaching=state.approachTime>=0;
            if(approaching)
            {
                const float t=static_cast<float>(state.approachTime);
                const float z=state.approachFrom+ApproachDistance(t)*k;
                state.velZ=live&&step>0?(z-state.herZ)/step:0;
                state.velX=0;
                state.herZ=z;
                ApproachFeet(t);
                if(state.autoplay&&state.swingTrace.size()<20000&&t>1.6f&&t<2.3f)
                {
                    wchar_t row[220];
                    const auto& r=state.feet[1];const auto& l=state.feet[0];const auto ro=ModelOffset(r.x-state.herX,r.z-state.herZ);
                    swprintf_s(row,L"  feet t %.3f body z %.2f right %.2f %.2f lift %.2f model %.2f %.2f left %.2f %.2f lift %.2f most %.2f\n",t,state.herZ,r.x,r.z,r.lift,ro[0],ro[1],l.x,l.z,l.lift,0.85f*D);
                    state.swingTrace+=row;
                }
            }
            else if(live)
            {
                // Her place: on the approach for her ball, by the lane on your left for yours;
                // after her ball she stays where she is until the next one is hers or yours.
                float gx=WaitX*k,gz=WaitZ*k;
                const bool fetching=state.thrower==Her&&!state.released&&state.stage==Stage::Ready&&(state.plan==Plan::Fetch||state.plan==Plan::Lift);
                if(fetching){gx=FetchX*k;gz=FetchZ*k;}
                else if(state.thrower==Her&&!state.released&&state.stage==Stage::Ready)
                {
                    gx=state.herThrow.x*k-state.handSide;gz=StanceZ*k;
                    if(state.testStage==9&&state.hasBall)gz-=6.0f*k;   // (diagnostics: she walks on with her ball, toward you)
                }
                else if(state.thrower==Her&&state.released&&state.stage!=Stage::Ready){gx=state.herX;gz=state.herZ;}
                if(state.stage==Stage::Over){gx=OverX*k;gz=OverZ*k;}
                // A long way (to her place, back to wait, to the end): the generated walk (her arms
                // swinging, her steps as on the desktop), facing the way she goes and turning as she
                // sets off; near her place she stands again (the hold) and steps the last of it
                // facing the lane. (Stepped by the hold all the way, her arms hung still and she
                // walked backwards toward you.)
                const float away=std::hypot(gx-state.herX,gz-state.herZ);
                auto wrap=[](float a){while(a>XM_PI)a-=XM_2PI;while(a<-XM_PI)a+=XM_2PI;return a;};
                // (Carrying her ball to her place, a shorter way: she walks it too, the hold keeping
                // the ball before her: the hold's own steps slid her backwards there.)
                if(!state.walking&&state.testStage!=5&&state.plan!=Plan::Lift&&(state.hasBall?away>0.20f*k:away>0.9f*k))
                {
                    state.walking=true;
                    model.SetFacingWalk(true);
                    model.SetActivity(Activity::Walk,1);
                }
                if(state.walking)
                {
                    // The way she goes; coming to the ball return, turned toward it over the last
                    // half metre, so she arrives facing it and reaches at once (no stop and step).
                    float wantYaw=std::atan2(gx-state.herX,gz-state.herZ);
                    if(fetching)wantYaw+=wrap(XM_PIDIV2-wantYaw)*(1-std::clamp(away/(0.5f*k),0.0f,1.0f));
                    state.yaw=wrap(state.yaw+wrap(wantYaw-state.yaw)*static_cast<float>(1.0-std::exp(-dt/0.15)));
                    // (Turning on the spot first: square to the way, she hardly moves; slowing a
                    // little over the last third of a metre.)
                    // (She goes straight to her place; her turn only sets how fast, so the turn
                    // toward the return at the end does not hold her short of it.)
                    const float aligned=std::clamp(std::cos(wrap(wantYaw-state.yaw)),0.0f,1.0f);
                    const float slow=std::clamp(away/(0.35f*k),0.45f,1.0f);
                    const float go=std::min(away,std::max(model.GroundSpeed(),0.25f*D)*step*aligned*aligned*slow);
                    const float ux=(gx-state.herX)/std::max(away,1e-4f),uz=(gz-state.herZ)/std::max(away,1e-4f);
                    state.velX=ux*go/std::max(step,1e-4f);
                    state.velZ=uz*go/std::max(step,1e-4f);
                    state.herX+=ux*go;
                    state.herZ+=uz*go;
                    if(away-go<0.03f*k)
                    {
                        // There: standing again (the hold), her feet left where the walk set them
                        // (set down under her, they slid there as the hold came in).
                        state.walking=false;
                        model.SetActivity(Activity::Idle);
                        model.SetFacingWalk(false);
                        model.EndGestures();
                        state.velX=state.velZ=0;
                        FeetFromModel(model);
                    }
                }
                else
                {
                    // (Facing the lane, or the ball return as she comes to it.)
                    const float face=fetching&&(away<0.6f*k||state.plan==Plan::Lift)?XM_PIDIV2:0.0f;
                    state.yaw=wrap(state.yaw+wrap(face-state.yaw)*static_cast<float>(1.0-std::exp(-dt/0.22)));
                    constexpr float W=2.4f;
                    const int n=std::max(1,static_cast<int>(std::ceil(dt*240)));
                    const float h=step/n;
                    for(int i=0;i<n;++i)
                    {
                        state.velX+=(W*W*(gx-state.herX)-2*W*state.velX)*h;
                        state.velZ+=(W*W*(gz-state.herZ)-2*W*state.velZ)*h;
                        const float speed=std::hypot(state.velX,state.velZ),most=1.3f*k;
                        if(speed>most){state.velX*=most/speed;state.velZ*=most/speed;}
                        state.herX+=state.velX*h;
                        state.herZ+=state.velZ*h;
                    }
                    WalkFeet(dt);
                }
            }
            // Her body's shape, her ball arm, her other hand.
            std::array<float,5> want{state.walking?0.0f:0.03f*Leg,0,0,XMConvertToRadians(2.0f),0};   // (walking, the walk's own height)
            std::array<float,3> wristWant{};
            std::array<float,3> leadWant{};
            bool freeAt=false;
            float fingers=1;   // her ball hand's fingers: 1 closed round the ball, 0 straight
            std::array<float,3> freePoint{};
            {
                // (Within the hold's reach: further, it draws the hand in toward her.)
                const auto rest=Shoulder(want[0],want[3]);
                wristWant={rest[0]-0.03f*D,rest[1]-state.reach*0.93f,rest[2]-state.reach*0.14f};
            }
            if(approaching)
            {
                const float t=static_cast<float>(state.approachTime);
                want=ApproachBody(t);
                const auto s=Shoulder(want[0],want[3]);
                const float a=XMConvertToRadians(ArmAngle(t)),R=state.armLength*0.97f;   // (the arm straight, as a pendulum)
                // (Wide of her side, the more as it goes back: the hold moves a hand whose forearm is in
                // her body, coat and all, forward out of it, and behind her hip that left the ball at her chest.)
                // Held wide the whole swing through, back and forward alike: wide only while it was
                // behind her, the ball came in sideways as it swung forward to the release.
                const float swing=Ease((t-0.30f)/0.50f)*(1-Ease((t-ReleaseAt-0.10f)/0.60f));
                const float wide=std::max(0.12f*swing,0.05f*std::max(0.0f,std::cos(a)));   // (close by her side, the elbow by the hip; nearer, the arm went into her body)
                std::array<float,3> pend{s[0]-wide*D,s[1]-R*std::cos(a),s[2]-R*std::sin(a)};
                if(t<0.40f)
                {
                    // The push away: from the ball held before her to the arm out and down.
                    const auto ball=AddressBall();
                    const std::array<float,3> held{ball[0]-0.10f*D,ball[1]-0.10f*D,ball[2]+0.04f*D};
                    const float u=Ease(t/0.40f);
                    for(int i=0;i<3;++i)pend[i]=held[i]+(pend[i]-held[i])*u;
                    if(t<0.22f){freeAt=true;freePoint=UnderBall(ModelPoint(model,state.herBall));}
                }
                wristWant=pend;
                // Her other arm out to her left for balance through the swing.
                const float out=Ease((t-0.30f)/0.6f)*(1-Ease((t-2.8f)/(Relaxed-2.8f)));
                leadWant={-0.95f*out,0.30f*out,0.10f*out};
            }
            else if(state.plan==Plan::Lift&&state.thrower==Her)
            {
                // Down to her ball on the return (her hand over it, the other at its far side),
                // bending at the knees and the waist; taking hold, she sinks a little more under its
                // weight; then up with it to her chest, the arms slow to start, her legs and back
                // rising after them, leaning back a little as she brings it in.
                const float t=static_cast<float>(state.planTime);
                const auto rack=ModelPoint(model,RackBall());
                const float r=Bowling::BallRadius*k;
                // (The wrist level with the ball's middle, against its side toward her shoulder:
                // her straight fingers then go over it into its holes. Sent above it, her hand
                // stopped short.)
                const auto sh=Shoulder(Leg*0.16f,XMConvertToRadians(50.0f));
                float dx=sh[0]-rack[0],dz=sh[2]-rack[2];
                const float dl=std::max(1e-4f,std::hypot(dx,dz));dx/=dl;dz/=dl;
                const float out=r+0.03f*k;
                const std::array<float,3> over{rack[0]+dx*out,rack[1],rack[2]+dz*out};
                const auto ball=AddressBall();
                const std::array<float,3> held{ball[0]-0.10f*D,ball[1]-0.10f*D,ball[2]+0.04f*D};
                const float down=Ease(t/ReachSeconds);
                const float g=std::clamp((t-ReachSeconds)/GripSeconds,0.0f,1.0f);
                const float u=std::clamp((t-ReachSeconds-GripSeconds)/LiftSeconds,0.0f,1.0f);
                const float up=std::pow(Ease(u),1.6f);   // (slow off the rack: it is heavy)
                for(int i=0;i<3;++i)wristWant[static_cast<size_t>(i)]=t<ReachSeconds?wristWant[static_cast<size_t>(i)]+(over[static_cast<size_t>(i)]-wristWant[static_cast<size_t>(i)])*down:over[static_cast<size_t>(i)]+(held[static_cast<size_t>(i)]-over[static_cast<size_t>(i)])*up;
                const float bend=down*(1-Ease((u-0.12f)/0.88f));
                // (Bent mostly at the hips, the hips going back, the knees a little: squatting, her
                // knees went into the return's base.)
                // Her arm straight and still short of the ball (her shoulder too high for it): she
                // bends lower, at the knees and the hips, a little at a time.
                if(live&&t<ReachSeconds+GripSeconds&&down>0.6f)
                {
                    auto d=[](const std::array<float,3>& a,const std::array<float,3>& b){return std::sqrt((a[0]-b[0])*(a[0]-b[0])+(a[1]-b[1])*(a[1]-b[1])+(a[2]-b[2])*(a[2]-b[2]));};
                    const float arm=d(state.shoulderGot,state.elbowGot)+d(state.elbowGot,state.wristGot);
                    std::array<float,3> aim{};
                    for(int i=0;i<3;++i)aim[static_cast<size_t>(i)]=state.wrist[static_cast<size_t>(i)]-state.reachFix[static_cast<size_t>(i)];
                    if(d(state.shoulderGot,state.wristGot)>0.96f*arm&&d(state.shoulderGot,aim)>0.98f*arm)
                        state.stoop=std::min(1.0f,state.stoop+static_cast<float>(dt)/0.5f);
                }
                const float more=state.stoop*bend;
                want[0]=Leg*(0.03f+0.13f*bend+0.12f*more+0.03f*std::sin(XM_PI*g)*(1-u));
                want[1]=-Leg*(0.03f*bend+0.05f*more);
                want[3]=XMConvertToRadians(2.0f+52.0f*bend+10.0f*more-6.0f*std::sin(XM_PI*u));
                // Bent this far, the hold's own idea of where her body is is off by a few
                // centimetres, and her hand stopped short of the ball: what her wrist falls short
                // by is learned as she reaches (the closer, the more) and added to where it is sent.
                if(live&&t<ReachSeconds+GripSeconds)
                {
                    const float f=std::min(1.0f,static_cast<float>(dt)/0.12f)*down*down;
                    for(int i=0;i<3;++i)state.reachFix[static_cast<size_t>(i)]+=(state.wrist[static_cast<size_t>(i)]-state.reachFix[static_cast<size_t>(i)]-state.wristGot[static_cast<size_t>(i)])*f*0.5f;
                    float n=std::sqrt(state.reachFix[0]*state.reachFix[0]+state.reachFix[1]*state.reachFix[1]+state.reachFix[2]*state.reachFix[2]);
                    if(n>0.30f*D)for(float& v:state.reachFix)v*=0.30f*D/n;
                }
                const float fix=1-u;   // (let go of as she lifts it to her chest)
                for(int i=0;i<3;++i)wristWant[static_cast<size_t>(i)]+=state.reachFix[static_cast<size_t>(i)]*fix;
                // Her fingers straight as they go into the ball's holes, closing a little on it as
                // she takes hold.
                fingers=0.15f+0.85f*std::max(1-down,g*0.8f+0.2f*u);
                freeAt=t>0.15f;
                // (Both from where the ball really is, on the return and then in her hands: from where
                // it lay and from where it should be held, her left hand left it as it came up and
                // went back to it fast once the lift was done.)
                const auto real=state.hasBall?ModelPoint(model,state.herBall):rack;
                const std::array<float,3> side{real[0]+0.75f*r,real[1]+0.35f*r,real[2]+0.75f*r};   // (on its side toward her left: round the far side was out of her reach)
                const std::array<float,3> under=UnderBall(real);
                for(int i=0;i<3;++i)freePoint[static_cast<size_t>(i)]=side[static_cast<size_t>(i)]+(under[static_cast<size_t>(i)]-side[static_cast<size_t>(i)])*up;
            }
            else if(state.hasBall)
            {
                // The ball held before her, her other hand under it.
                const auto ball=AddressBall();
                wristWant={ball[0]-0.10f*D,ball[1]-0.10f*D,ball[2]+0.04f*D};
                freeAt=true;
                // (Under the ball where it really is, in her right hand: where it should be, her left
                // hand came off it as she walked with it.)
                freePoint=UnderBall(ModelPoint(model,state.herBall));
            }
            if(live)
            {
                const int n=std::max(1,static_cast<int>(std::ceil(dt*240)));
                const float h=step/n;
                for(int i=0;i<5;++i)
                {
                    const float W=approaching?20.0f:9.0f;
                    for(int j=0;j<n;++j)
                    {
                        state.bodySpeed[i]+=(W*W*(want[i]-state.body[i])-2*W*state.bodySpeed[i])*h;
                        state.body[i]+=state.bodySpeed[i]*h;
                    }
                }
                if(!state.wristSet){state.wrist=wristWant;state.wristSet=true;}
                for(int i=0;i<3;++i)
                {
                    const float W=approaching?30.0f:10.0f;
                    for(int j=0;j<n;++j)
                    {
                        state.wristSpeed[i]+=(W*W*(wristWant[i]-state.wrist[i])-2*W*state.wristSpeed[i])*h;
                        state.wrist[i]+=state.wristSpeed[i]*h;
                    }
                    const float WL=10.0f;
                    for(int j=0;j<n;++j)
                    {
                        state.leadSpeed[i]+=(WL*WL*(leadWant[i]-state.lead[i])-2*WL*state.leadSpeed[i])*h;
                        state.lead[i]+=state.leadSpeed[i]*h;
                    }
                }
            }
            // The hold leans her forward by itself (18 degrees and more toward a far hand) and
            // turns her shoulders toward her hand (up to 22, eased): what it gives is taken off
            // hers, so she leans and turns as her approach has it, whichever way the arm swings.
            {
                const float R=state.reach;
                const float tx=state.wrist[0],ty=state.wrist[1]+state.body[0],tz=state.wrist[2]+state.body[1];
                const float d=std::sqrt((tx-state.shoulder[0])*(tx-state.shoulder[0])+(ty-state.shoulder[1])*(ty-state.shoulder[1])+(tz-state.shoulder[2])*(tz-state.shoulder[2]));
                const float autoLean=XMConvertToRadians(18.0f+std::clamp((d-0.7f*R)/(0.8f*R),0.0f,1.0f)*17.0f);
                const float autoTwist=std::clamp(0.45f*std::atan2(-tx,std::max(-tz,0.1f*R)),XMConvertToRadians(-22.0f),XMConvertToRadians(22.0f));
                if(live)state.twist+=(autoTwist-state.twist)*static_cast<float>(1.0-std::exp(-dt/0.22));
                model.HoldBody(HerHand,state.body[0],state.body[1],state.body[2]-state.twist,state.body[3]-autoLean,state.body[4],1.0f);
            }
            // Her feet near the line under her middle on the approach (as a walk's), under her hips
            // as she stands.
            if(live)state.track+=((approaching?0.55f:1.0f)-state.track)*static_cast<float>(1.0-std::exp(-dt/0.3));
            // Her feet, given in her model's terms (turned about: its x and z are the world's reversed).
            std::array<std::array<float,3>,2> feet{};
            for(int f=0;f<2;++f)
            {
                const auto& foot=state.feet[f];
                const float most=0.85f*D;
                // (The ankle moved in from where the model stands at rest to its share of the hips' spacing.)
                const float in=state.track*state.restHip[f]-state.restAnkle[f];
                const auto off=ModelOffset(foot.x-state.herX,foot.z-state.herZ);
                feet[f]={std::clamp(off[0]+in,-most,most),foot.lift,std::clamp(off[1],-most,most)};
            }
            // (On the generated walk the hold lets go, or, carrying the ball, keeps her arms and
            // leaves her legs and hips to the walk.)
            model.HoldFingers(HerHand,fingers);
            model.HoldGaitFeet(HerHand,state.walking);
            model.HoldFree(HerHand,freeAt,freePoint,feet,state.lead);
            model.Hold(HerHand,{state.wrist[0]+model.RootOffset(),state.wrist[1],state.wrist[2]},!state.walking||state.hasBall||state.plan==Plan::Lift,4);
            if(approaching)state.inBody=std::max(state.inBody,model.HoldInBody(HerHand));
            {
                std::array<float,3> mid{},al{},pa{},wr{};
                if(model.Fist(HerHand,mid,al,pa,&wr,&state.elbowGot))state.wristGot={wr[0]-model.RootOffset(),wr[1],wr[2]};
                model.BonePosition(L"右腕",state.shoulderGot);
                // (Diagnostics: how far her other hand is from the ball she carries.)
                if(state.hasBall&&state.plan==Plan::Lift)
                    if(std::array<float,3> lh{};model.BonePosition(L"左手首",lh))
                    {
                        const auto b=ModelPoint(model,state.herBall);
                        state.liftLeftOff=std::max(state.liftLeftOff,std::sqrt((lh[0]-b[0])*(lh[0]-b[0])+(lh[1]-b[1])*(lh[1]-b[1])+(lh[2]-b[2])*(lh[2]-b[2]))/k);
                    }
                if(state.hasBall&&!approaching&&state.plan!=Plan::Lift)
                    if(std::array<float,3> lh{};model.BonePosition(L"左手首",lh))
                    {
                        const auto b=ModelPoint(model,state.herBall);
                        const float off=std::sqrt((lh[0]-b[0])*(lh[0]-b[0])+(lh[1]-b[1])*(lh[1]-b[1])+(lh[2]-b[2])*(lh[2]-b[2]))/k;
                        (state.walking?state.leftOff:state.leftOffStand)=std::max(state.walking?state.leftOff:state.leftOffStand,off);
                        if(std::array<float,3> la{},le{};state.walking&&model.BonePosition(L"左腕",la)&&model.BonePosition(L"左ひじ",le))
                        {
                            const float ax=la[0]-le[0],ay=la[1]-le[1],az=la[2]-le[2],bx=lh[0]-le[0],by=lh[1]-le[1],bz=lh[2]-le[2];
                            const float c=(ax*bx+ay*by+az*bz)/std::max(1e-6f,std::sqrt((ax*ax+ay*ay+az*az)*(bx*bx+by*by+bz*bz)));
                            const float bendNow=180-std::acos(std::clamp(c,-1.0f,1.0f))*57.2958f;
                            state.leftBend=std::min(state.leftBend,bendNow);
                            state.leftBendSum+=bendNow;++state.leftBendN;
                        }
                        if(state.walking)state.rightMiss=std::max(state.rightMiss,std::sqrt((state.wristGot[0]-state.wrist[0])*(state.wristGot[0]-state.wrist[0])+(state.wristGot[1]-state.wrist[1])*(state.wristGot[1]-state.wrist[1])+(state.wristGot[2]-state.wrist[2])*(state.wristGot[2]-state.wrist[2]))/k);
                    }
            }
            // Her ball as her hand holds it: against her palm, out from it.
            if(state.hasBall)
            {
                std::array<float,3> palm{},middle{},along{},facing{},wr{},el{};
                if(model.PalmPosition(HerHand,palm)&&model.Fist(HerHand,middle,along,facing,&wr,&el))
                {
                    // Hanging from her fingers (on in the hand's way), a little off her palm: by
                    // the palm alone it lay against her hip as her arm swung.
                    const float r=Bowling::BallRadius*k*1.05f;
                    std::array<float,3> f{palm[0]-wr[0],palm[1]-wr[1],palm[2]-wr[2]};
                    const float fl=std::max(1e-4f,std::sqrt(f[0]*f[0]+f[1]*f[1]+f[2]*f[2]));
                    for(float& v:f)v/=fl;
                    std::array<float,3> out=facing;
                    if(const float yz=std::hypot(facing[1],facing[2]);approaching&&yz>0.2f)out={0,facing[1]/yz,facing[2]/yz};
                    std::array<float,3> c{palm[0]+(f[0]*0.75f+out[0]*0.55f)*r,palm[1]+(f[1]*0.75f+out[1]*0.55f)*r,palm[2]+(f[2]*0.75f+out[2]*0.55f)*r};
                    // Swung, it rides on the forward side of her hand in the swing's plane: out
                    // from her palm ahead of it (its middle by the palm), the hand behind it back and forward
                    // alike (by the hand's own facing it hung behind the hand at the backswing's top).
                    if(const float w=approaching?Ease((static_cast<float>(state.approachTime)-0.15f)/0.35f):0.0f;w>0)
                    {
                        float ry=wr[1]-el[1],rz=wr[2]-el[2];
                        const float rl=std::hypot(ry,rz);
                        if(rl>1e-4f)
                        {
                            ry/=rl;rz/=rl;
                            // (Forward square to the forearm: model forward is -z.)
                            float ty=rz*ry,tz=-1+rz*rz;
                            if(const float tl=std::hypot(ty,tz);tl>0.2f){ty/=tl;tz/=tl;}else{ty=1;tz=0;}
                            const float py=palm[1]-wr[1],pz=palm[2]-wr[2],reachOut=py*ry+pz*rz;   // (the palm's way out along the forearm)
                            const float cy=wr[1]+ry*(reachOut+0.15f*r)+ty*0.95f*r,cz=wr[2]+rz*(reachOut+0.15f*r)+tz*0.95f*r;   // (its middle by the palm, the fingers round its side)
                            c[1]+=(cy-c[1])*w;
                            c[2]+=(cz-c[2])*w;
                        }
                    }
                    // Through the approach it keeps the side of her wrist it had at the address and
                    // swings in one plane: following her palm, it went 9 cm sideways in a tenth of a
                    // second as the hand rolled over at the bottom of the downswing.
                    // (From the address's side of the wrist to hanging just off its outside, over
                    // the push away.)
                    if(approaching)c[0]=wr[0]+state.ballSide+(-0.005f*k-state.ballSide)*Ease(static_cast<float>(state.approachTime)/0.6f);
                    else state.ballSide=c[0]-wr[0];
                    state.herBall=WorldPoint(model,c);
                    // Taken up from the return: from where it lay, into her hands over most of the lift
                    // (at once, it jumped from the rack to her palm).
                    if(state.plan==Plan::Lift&&state.thrower==Her)
                    {
                        if(!state.picked){state.picked=true;const XMFLOAT3 r=RackBall();state.pickOffset={r.x-state.herBall.x,r.y-state.herBall.y,r.z-state.herBall.z};}
                        const float keep=1-Ease(static_cast<float>(state.planTime-ReachSeconds-GripSeconds)/(0.8f*LiftSeconds));
                        state.herBall.x+=state.pickOffset.x*keep;state.herBall.y+=state.pickOffset.y*keep;state.herBall.z+=state.pickOffset.z*keep;
                    }
                    if(state.autoplay&&approaching&&state.swingTrace.size()<20000)
                    {
                        wchar_t row[200];
                        swprintf_s(row,L"  t %.3f ball x %.3f want %.3f wrist %.3f twist %.1f palm-wrist %.2f %.2f %.2f c-wrist %.2f %.2f %.2f elbow-wrist %.2f %.2f\n",
                                   state.approachTime,(state.herBall.x-state.herX)/k,state.wrist[0],state.wristGot[0],state.twist*57.3f,
                                   palm[0]-wr[0],palm[1]-wr[1],palm[2]-wr[2],c[0]-wr[0],c[1]-wr[1],c[2]-wr[2],wr[1]-el[1],wr[2]-el[2]);
                        state.swingTrace+=row;
                    }
                }
                else state.herBall=WorldPoint(model,AddressBall());
            }
            (void)herBall;(void)g;
        }

        // ---- The camera ----------------------------------------------------------------------

        void PlaceCamera(double dt)
        {
            using namespace DirectX;
            using namespace Bowling;
            const float k=state.k;
            // (Close behind the approach: further back, the lane was small to aim along.)
            constexpr float BackZ=-5.0f;
            XMFLOAT3 eye{0.25f*k,1.70f*k,BackZ*k},look{0,0.40f*k,7.0f*k};
            float quick=0.35f;
            if(state.stage==Stage::Rolling)
            {
                // Down the lane after the ball, kept over your lane (a ball off to the side, in the
                // gutter or the pit took the camera over the next lane), and no further than the pins.
                const Vec3 b=state.roll.Ball();
                const float ez=std::clamp(b.y-5.5f,BackZ,PinDeckY-5.0f);
                const float u=std::clamp((ez-BackZ)/(PinDeckY-5.0f-BackZ),0.0f,1.0f);
                const float bx=std::clamp(b.x,-0.25f,0.25f);
                eye={(0.25f*(1-u)+bx*0.2f*u)*k,(1.70f-0.55f*u)*k,ez*k};
                look={bx*0.3f*k,(0.40f-0.10f*u)*k,std::clamp(b.y+7.0f,7.0f,PinDeckY+0.4f)*k};
                quick=0.18f;
            }
            else if(state.stage==Stage::Result||state.stage==Stage::Sweep)
            {
                eye={0,1.15f*k,(PinDeckY-5.0f)*k};
                look={0,0.30f*k,(PinDeckY+0.4f)*k};
                quick=0.25f;
            }
            else if(state.stage==Stage::Over)
            {
                // Before her, from the approach looking back up it, so she faces the camera; her
                // head about 0.4 down the view, under the result's banner (which sits high).
                eye={OverX*k+0.25f*k,1.45f*k,(OverZ+4.0f)*k};
                look={OverX*k,1.22f*k,OverZ*k};
            }
            // (Diagnostics, her approach held at one moment: the camera on her, to be turned round her.)
            if((state.testStage>=1&&state.testStage<=3&&state.approachTime>=0)||(state.testStage==5&&state.plan==Plan::Address)||(state.testStage==8&&state.plan==Plan::Lift)||(state.testStage==9&&state.hasBall&&state.walking))
            {
                look={state.herX,0.85f*k,state.herZ};
                eye={state.herX,1.25f*k,state.herZ-3.0f*k};
                quick=0.05f;
                // (Her fetch: from behind her left, the ball return before her.)
                if(state.testStage==8){look={state.herX+0.35f*k,0.65f*k,state.herZ};eye={state.herX-1.6f*k,1.35f*k,state.herZ-1.9f*k};}
                if(state.testStage==9){look={state.herX,1.0f*k,state.herZ};eye={state.herX+1.2f*k,1.3f*k,state.herZ-2.4f*k};}
            }
            if(!state.cameraPlaced){state.eyeAt=eye;state.lookAt=look;state.cameraPlaced=true;}
            const float f=static_cast<float>(1-std::exp(-dt/quick));
            auto ease=[&](XMFLOAT3& a,const XMFLOAT3& b){a.x+=(b.x-a.x)*f;a.y+=(b.y-a.y)*f;a.z+=(b.z-a.z)*f;};
            ease(state.eyeAt,eye);
            ease(state.lookAt,look);
            // Turned and moved by the middle button.
            const auto& c=state.camera;
            const XMVECTOR at=XMVectorAdd(XMLoadFloat3(&state.lookAt),XMVectorSet(c.x*k,c.y*k,c.z*k,0));
            XMVECTOR off=XMVectorSubtract(XMLoadFloat3(&state.eyeAt),XMLoadFloat3(&state.lookAt));
            const float length=XMVectorGetX(XMVector3Length(off))*c.distance;
            const float yaw=std::atan2(XMVectorGetX(off),XMVectorGetZ(off))+c.yaw;
            const float pitch=std::clamp(std::asin(std::clamp(XMVectorGetY(off)/std::max(1e-3f,XMVectorGetX(XMVector3Length(off))),-1.0f,1.0f))+c.pitch,0.02f,1.45f);
            off=XMVectorSet(std::sin(yaw)*std::cos(pitch)*length,std::sin(pitch)*length,std::cos(yaw)*std::cos(pitch)*length,0);
            const XMVECTOR eyeV=XMVectorAdd(at,off);
            XMStoreFloat3(&state.eye,eyeV);
            XMStoreFloat3(&state.look,at);
            state.renderer->SetCamera(eyeV,at,XMConvertToRadians(40.0f));
        }

        LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam);

        LRESULT CALLBACK ViewProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            auto& c=state.camera;
            switch(message)
            {
            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
                SetFocus(state.window);
                if(state.asking||state.paused)return Proc(state.window,WM_LBUTTONDOWN,wParam,lParam);
                PressBall(static_cast<float>(GET_X_LPARAM(lParam)),static_cast<float>(GET_Y_LPARAM(lParam)));
                return 0;
            case WM_LBUTTONUP:
                LetGo(static_cast<float>(GET_X_LPARAM(lParam)),static_cast<float>(GET_Y_LPARAM(lParam)));
                return 0;
            case WM_RBUTTONDOWN:
                SetFocus(state.window);
                if(YourBall()&&!state.pressing)state.spin=0;
                return 0;
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
                    const float s=1/Scale();
                    if(state.dragging==1)
                    {
                        c.yaw+=dx*s*0.008f;
                        c.pitch=std::clamp(c.pitch+dy*s*0.006f,-1.2f,1.2f);
                    }
                    else
                    {
                        const float move=0.01f*s;
                        c.x-=dx*move;
                        c.y+=dy*move;
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
                return Proc(state.window,message,wParam,lParam);
            case WM_MBUTTONDBLCLK:
                c={};
                return 0;
            case WM_SETCURSOR:
                if(LOWORD(lParam)==HTCLIENT&&YourBall()&&!state.dragging){SetCursor(nullptr);return TRUE;}
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
                Log(L"bowling: resize: %s",error.c_str());
            state.hudShown.clear();
        }

        void ReleaseView()
        {
            if(state.renderer){state.renderer->WaitIdle();state.renderer.reset();Log(L"bowling: 3D view released");}
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
            renderer->SetBackground(0.04f,0.04f,0.07f,1);
            std::wstring error;
            if(!renderer->Initialize(state.view,w,h,state.her.light?1:state.her.msaa,false,error)||
               !renderer->LoadModel(*state.her.model,state.her.toonDir,state.her.light?512:state.her.maxTexture,error))
            {
                Log(L"bowling: no 3D view (%s)",error.c_str());
                renderer->WaitIdle();
                DestroyWindow(state.view);
                state.view=nullptr;
                return false;
            }
            renderer->SetLight(25,40);
            MeasureHer(*state.her.model);
            renderer->SetDepthRange(0.05f*state.k,60*state.k);
            state.renderer=std::move(renderer);
            state.renderer->SetGeometry(BuildAlley());
            state.camera={};
            state.cameraPlaced=false;
            state.dragging=0;
            state.herX=WaitX*state.k;
            state.herZ=WaitZ*state.k;
            state.velX=state.velZ=0;state.yaw=0;state.walking=false;
            state.feet={};
            state.body={};state.bodySpeed={};
            state.lead={};state.leadSpeed={};
            state.wristSet=false;
            state.wristSpeed={};
            state.steps=0;
            state.inBody=0;
            state.hudShown.clear();
            Log(L"bowling: 3D view %ux%u on %s",w,h,state.renderer->AdapterName().c_str());
            return true;
        }

        LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            switch(message)
            {
            case WM_TIMER:
                return 0;
            case WM_KEYDOWN:
                if(state.asking)
                {
                    if(wParam=='Y'){Finish();return 0;}
                    if(wParam=='N'||wParam==VK_ESCAPE)state.asking=false;
                    return 0;
                }
                if(wParam==VK_ESCAPE)
                {
                    if(state.game.Over())Finish();
                    else{state.asking=true;state.pressing=false;state.pull=0;}
                }
                else if(wParam=='P'&&!state.game.Over())state.paused=!state.paused;
                else if(wParam==VK_RETURN&&state.game.Over()&&state.stage==Stage::Over)NewGame();
                else if(wParam==VK_F1)state.camera={};
                else if(wParam>='1'&&wParam<='3')ChooseLevel(static_cast<int>(wParam-'1')+1);
                else if(wParam>=VK_NUMPAD1&&wParam<=VK_NUMPAD3)ChooseLevel(static_cast<int>(wParam-VK_NUMPAD1)+1);
                else if(wParam==VK_LEFT||wParam=='A')ChangeSpin(1);
                else if(wParam==VK_RIGHT||wParam=='D')ChangeSpin(-1);
                return 0;
            case WM_LBUTTONDOWN:
                if(state.asking)
                {
                    const float x=static_cast<float>(GET_X_LPARAM(lParam)),y=static_cast<float>(GET_Y_LPARAM(lParam));
                    if(state.askButtons[0].Contains(x,y)){Finish();return 0;}
                    if(state.askButtons[1].Contains(x,y))state.asking=false;
                    return 0;
                }
                if(state.paused){state.paused=false;return 0;}
                return 0;
            case WM_MOUSEWHEEL:
            {
                const int notches=GET_WHEEL_DELTA_WPARAM(wParam)/WHEEL_DELTA;
                if(GET_KEYSTATE_WPARAM(wParam)&MK_CONTROL)state.camera.distance=std::clamp(state.camera.distance*std::pow(0.9f,static_cast<float>(notches)),0.3f,2.5f);
                else ChangeSpin(notches);
                return 0;
            }
            case WM_SETCURSOR:
                if(LOWORD(lParam)==HTCLIENT&&YourBall()){SetCursor(nullptr);return TRUE;}
                break;
            case WM_CONTEXTMENU:
                return 0;
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                BeginPaint(hwnd,&ps);
                EndPaint(hwnd,&ps);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_ACTIVATE:
                if(LOWORD(wParam)==WA_INACTIVE){HoldCursor(false);state.pressing=false;state.pull=0;}
                break;
            case WM_MOVE:
                HoldCursor(false);
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

    void ShowBowlingWindow(HWND owner,const BowlingOpponent& her,std::function<void()> onEnd)
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
        state.lines.Load(L"BOWLINGLINES",L"bowling",her.ownLinesFile,her.linesFile,her.styleKey,English(),
                         {L"ボウリング",L"bowling",L"",L""});
        state.voice.Stop();
        state.prefetching=false;
        state.words={};
        state.words.told=state.lines.Report();
        if(!her.linesFile.empty())ShowLinesProblem(owner,state.lines);
        state.frames=0;
        state.clock=0;
        state.random.seed(Seed());
        state.spin=0;
        state.standX=0;
        state.lastSpeed=0;

        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&info);
        const RECT work=info.rcWork;
        const int dpi=static_cast<int>(GetDpiForSystem());
        const int h=std::min(static_cast<int>(work.bottom-work.top)*9/10,MulDiv(820,dpi,96));
        const int w=std::min(static_cast<int>(work.right-work.left)*9/10,h*4/3);
        const std::wstring title=std::wstring(T(L"ボウリング",L"Bowling"))+L" - "+HerName();
        state.window=CreateWindowExW(0,ClassName,title.c_str(),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                                     work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,
                                     nullptr,nullptr,instance,nullptr);
        if(!CreateView())
        {
            if(!her.linesFile.empty())
                MessageBoxW(owner,T(L"3D表示を作れなかったため、ボウリングを開けません。",L"Bowling could not open: the 3D view could not be made."),
                            T(L"ボウリング",L"Bowling"),MB_OK|MB_ICONINFORMATION);
            Finish();
            return;
        }
        PlaceView();
        NewGame();
        Prefetch();
        SetTimer(state.window,TimerId,100,nullptr);
        ShowWindow(state.window,SW_SHOWNORMAL);
        SetForegroundWindow(state.window);
    }

    bool BowlingWindowOpen(){return state.window!=nullptr;}
    HWND BowlingWindowHandle(){return state.window;}
    bool BowlingWindowRect(RECT& r)
    {
        return state.window&&IsWindowVisible(state.window)&&!IsIconic(state.window)&&GetWindowRect(state.window,&r);
    }

    void TickBowlingWindow(MascotModel& model,double dt)
    {
        if(!state.window||!state.renderer)return;
        using namespace DirectX;
        dt=std::min(dt,0.1);
        auto& r=*state.renderer;
        StepGame(dt);
        if(!state.window||!state.renderer)return;
        MoveHer(model,dt);
        // The game over: she claps when she won, "oops" when not.
        if(state.stage==Stage::Over&&!state.cheered&&state.clock>state.overAt+1.0)
        {
            const int winner=state.game.Winner();
            if(model.EmoteSeated(winner==Her?L"clap":winner==You?L"oops":L"thumbsup",3.2))state.cheered=true;
        }
        model.SetSolids({});
        r.SetMovingGeometry(BuildMoving());
        r.SetGlowGeometry(BuildGlow());
        r.SetParticles(BuildSparks());
        r.SetModelTransform(HerWorld(model));
        PlaceCamera(dt);
        if(std::array<float,3> head{};model.HeadPosition(head))
        {
            const XMVECTOR at=XMVector3Transform(XMVectorSet(head[0],head[1],head[2],1),HerWorld(model));
            const XMVECTOR right=XMVector3Normalize(XMVector3Cross(XMVectorSet(0,1,0,0),XMVectorSubtract(XMLoadFloat3(&state.look),XMLoadFloat3(&state.eye))));
            const XMVECTOR beside=XMVectorAdd(at,XMVectorScale(right,0.3f*state.upper));
            state.headScreen=r.Project({XMVectorGetX(at),XMVectorGetY(at),XMVectorGetZ(at)});
            state.besideHead=r.Project({XMVectorGetX(beside),XMVectorGetY(beside),XMVectorGetZ(beside)});
            // In sight: before the camera and inside the window (behind it, Project mirrors).
            RECT client{};
            GetClientRect(state.view,&client);
            const float ahead=XMVectorGetX(XMVector3Dot(XMVectorSubtract(at,XMLoadFloat3(&state.eye)),XMVector3Normalize(XMVectorSubtract(XMLoadFloat3(&state.look),XMLoadFloat3(&state.eye)))));
            const POINT p=state.headScreen;
            state.headSeen=ahead>0.5f*state.k&&p.x>=0&&p.x<client.right&&p.y>=0&&p.y<client.bottom;
        }
        else state.headSeen=false;
        // Her eyes: on the ball while it rolls, down the lane as she bowls, otherwise on you.
        {
            XMFLOAT3 at=World({0,4.6f,0});
            if(state.stage==Stage::Rolling||state.stage==Stage::Result)at=World(state.roll.Ball());
            else if(state.stage==Stage::Sweep)at=World({0,Bowling::PinDeckY,0.2f});
            else if(state.thrower==You&&state.stage==Stage::Ready)at=World({state.standX,HeldZ,HeldHeight});
            if((!state.words.text.empty()&&state.stage!=Stage::Rolling)||state.stage==Stage::Over)at=state.eye;
            XMVECTOR det;
            const XMVECTOR there=XMVector3Transform(XMLoadFloat3(&at),XMMatrixInverse(&det,HerWorld(model)));
            model.LookToward({XMVectorGetX(there),XMVectorGetY(there),XMVectorGetZ(there)},true);
        }
        state.voice.Tick(dt);
        if(state.voice.Playing())model.SetMouth(state.voice.Mouth(),true);
        else model.SetMouth({},false);
        UpdateOverlay();
        r.Render(model,state.her.edgeScale);
        ++state.frames;
    }

    void BowlingLines(const std::wstring& answer)
    {
        state.prefetching=false;
        if(!state.window)return;
        state.lines.Answered(answer);
        Prefetch();
    }

    bool BowlingAfterword(std::wstring& situation,std::wstring& fallback)
    {
        if(state.afterword.empty())return false;
        situation=std::move(state.afterword);
        fallback=std::move(state.afterwordLine);
        state.afterword.clear();
        state.afterwordLine.clear();
        return true;
    }

    void PlayBowlingForTest(int stage)
    {
        state.autoplay=true;
        state.testStage=stage;
        // 7: nine open frames each already bowled (7 and 2), so the game ends soon.
        if(stage==7&&state.game.Frame()==0)
            for(int i=0;i<36;++i)state.game.Record(i%2==0?7:2);
    }

    bool SaveBowlingView(const std::wstring& path)
    {
        return state.renderer&&state.renderer->SaveLastFrame(path);
    }

    void SetBowlingCamera(float pitch,float distance,float yaw)
    {
        state.camera.pitch=pitch;
        state.camera.distance=distance;
        state.camera.yaw=yaw;
    }

    std::wstring DescribeBowling()
    {
        const auto& g=state.game;
        std::wstring text;
        wchar_t line[400];
        static const wchar_t* stages[]={L"ready",L"rolling",L"result",L"sweep",L"over"};
        static const wchar_t* plans[]={L"walk",L"address",L"approach"};
        swprintf_s(line,L"bowling: 3D %d, %.2f world units a metre; stage %s (%.1f s), frame %d ball %d, %s to throw; you %d, her %d; over %d, winner %d; level %d; frames %d\n",
                   state.renderer?1:0,state.k,stages[static_cast<int>(state.stage)],state.stageTime,g.Frame()+1,g.BallNo()+1,g.Turn()==You?L"you":L"her",
                   g.Score(You),g.Score(Her),g.Over()?1:0,g.Winner(),state.her.level,state.frames);
        text+=line;
        swprintf_s(line,L"bowling: she is at %.2f, %.2f m (%s, %.2f s; approach %.2f s), %d steps; her arm in her body %.3f at most; her ball left her hand %.2f m from where it rolled (%.2f across)\n",
                   state.herX/state.k,state.herZ/state.k,plans[static_cast<int>(state.plan)],state.planTime,state.approachTime,state.steps,state.inBody,state.releaseOff,state.releaseSide);
        text+=line;
        swprintf_s(line,L"bowling: her shoulder %.2f %.2f %.2f, arm %.2f, ball held %.2f m to her right; strikes you %d her %d, spares you %d her %d\n",
                   state.shoulder[0],state.shoulder[1],state.shoulder[2],state.armLength,state.handSide/state.k,g.Strikes(You),g.Strikes(Her),g.Spares(You),g.Spares(Her));
        text+=line;
        swprintf_s(line,L"bowling: her wrist sent to %.2f %.2f %.2f, is at %.2f %.2f %.2f (model units); body %.2f %.2f %.1f %.1f, the hold's own turn %.1f (degrees)\n",
                   state.wrist[0],state.wrist[1],state.wrist[2],state.wristGot[0],state.wristGot[1],state.wristGot[2],state.body[0],state.body[1],
                   state.body[2]*57.3f,state.body[3]*57.3f,state.twist*57.3f);
        text+=line;
        {
            const auto& s=state.shoulderGot;const auto& e=state.elbowGot;const auto& w=state.wristGot;
            auto dist=[](const std::array<float,3>& a,const std::array<float,3>& b){return std::sqrt((a[0]-b[0])*(a[0]-b[0])+(a[1]-b[1])*(a[1]-b[1])+(a[2]-b[2])*(a[2]-b[2]));};
            swprintf_s(line,L"bowling: her left wrist from the ball she carries, at most %.2f m walking, %.2f m standing; her right wrist off where it is sent, walking, %.2f m; her left elbow bent at least %.0f deg walking (%.0f on average); left wrist from the ball as she lifts it, at most %.2f m\n",state.leftOff,state.leftOffStand,state.rightMiss,state.leftBend,state.leftBendN?state.leftBendSum/state.leftBendN:0.0f,state.liftLeftOff);
            text+=line;
            swprintf_s(line,L"bowling: her ball arm as posed: shoulder %.2f %.2f %.2f, elbow %.2f %.2f %.2f; shoulder to wrist %.2f (upper arm %.2f, forearm %.2f), to where the wrist is sent %.2f\n",
                       s[0],s[1],s[2],e[0],e[1],e[2],dist(s,w),dist(s,e),dist(e,w),dist(s,state.wrist));
            text+=line;
        }
        if(!state.swingTrace.empty())text+=L"bowling: her first approach (ball x from her, m; wrist model units)\n"+state.swingTrace;
        for(int side=0;side<2;++side)
        {
            std::wstring row=side==You?L"bowling: you  ":L"bowling: her  ";
            for(int f=0;f<10;++f)
            {
                for(const auto& m:Marks(side,f))row+=m.empty()?L".":m;
                const int t=g.Total(side,f);
                row+=L"("+(t>=0?std::to_wstring(t):std::wstring(L"?"))+L") ";
            }
            text+=row+L"\n";
        }
        if(!state.words.told.empty())text+=L"bowling words:\n"+state.words.told;
        return text;
    }
}
