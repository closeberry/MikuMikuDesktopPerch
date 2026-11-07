#define NOMINMAX
#include "GolfWindow.hpp"
#include "Golf.hpp"
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
#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <future>
#include <memory>
#include <random>
#include <vector>
#pragma comment(lib,"gdiplus.lib")

namespace MikuMikuDesktopPerch
{
    namespace
    {
        using namespace Gdiplus;
        using Golf::V3;
        using Golf::Lie;
        using DirectX::XMFLOAT3;
        constexpr wchar_t ClassName[]=L"MikuMikuDesktopPerchGolf";
        constexpr wchar_t ViewClassName[]=L"MikuMikuDesktopPerchGolfView";
        constexpr UINT_PTR TimerId=1;
        constexpr int You=0,Her=1,Holes=3,MostStrokes=10;
        constexpr float K=12.5f;   // world units a metre (MMD's 8 cm a unit)
        constexpr float Pi=3.14159265f,Deg=Pi/180;

        enum class Phase {Intro,Aim,Gauge,Swing,Flying,Settle,HoleOver,Over};
        enum class GaugeState {Ready,Rising,Falling,Done};

        struct Ball
        {
            V3 at;
            Lie lie=Lie::Tee;
            bool holed=false,teed=false;
            int strokes=0;
        };

        struct State
        {
            HWND window=nullptr,view=nullptr;
            ULONG_PTR gdiplus=0;
            GolfOpponent her;
            std::function<void()> onEnd;
            std::unique_ptr<MascotRenderer> renderer;
            std::shared_ptr<const Golf::Course> course;
            std::uint32_t firstSeed=0;
            int hole=0;
            std::array<std::array<int,Holes>,2> card{};   // strokes per hole (0: not played)
            std::array<int,Holes> pars{};
            std::array<Ball,2> balls;
            Golf::Wind wind;
            Phase phase=Phase::Intro;
            double phaseTime=0;
            int turn=You;
            // Your shot: club, aim (radians), the gauge.
            int club=Golf::Driver;
            float aim=0;
            GaugeState gauge=GaugeState::Ready;
            float gaugeAt=0,gaugePower=0,gaugeAccuracy=0;
            int aimKeys=0;              // -1 left held, +1 right
            V3 landing;                 // where your shot comes down at full power (no wind)
            bool landingKnown=false;
            // The shot in the air: its way, how far along.
            Golf::Shot shot;
            Golf::Flight flight;
            V3 shotFrom;
            Lie shotFromLie=Lie::Tee;
            double flightTime=0;
            bool launched=false;
            // Her: her thinking (a thread), her swing's clock.
            std::future<Golf::Shot> thinking;
            bool thought=false;
            double swingTime=0;
            float swingPower=1;
            std::mt19937 random{12345};
            float care=0.85f;
            bool autoplay=false;
            // Her measures: hips and upper body (model units), the club's length and where its
            // head is at the address (model space; the ball goes there).
            float hips=10,upper=10,clubLength=12;
            std::array<float,3> head{0,0,-8};
            // Where she is: on the course (metres), turned by yaw (radians, the model's
            // rotation about the up axis), swinging or watching.
            V3 herAt;
            float herYaw=0;
            bool herSwinging=false;
            bool moved=false;      // put somewhere else this frame (her hair and clothes settled again)
            // Diagnostics: frozen at a stage of the swing (-1: no), the camera round her.
            float posing=-1,poseYaw=0;
            // The camera: where it looks from and at (world units), eased; the user's turn and
            // nearness on top.
            XMFLOAT3 eye{},look{};
            bool cut=true;
            struct Camera {float yaw=0,pitch=0,zoom=1;} camera;
            POINT dragFrom{};
            int dragging=0;
            std::wstring hudShown;
            ULONGLONG hudAt=0;
            std::unique_ptr<Bitmap> map;      // the hole from above (made per hole)
            float mapScale=1;
            // Her words, as at hover hockey.
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
            bool prefetching=false,started=false;
            double faceUntil=0,clock=0;
            std::wstring banner,bannerNote;
            double bannerUntil=0;
            std::wstring afterword,afterwordLine;
            POINT headScreen{-1,-1},besideHead{-1,-1};
            int frames=0,shots=0;
            std::wstring log;     // diagnostics: what was played
            bool askingQuit=false,overTold=false;
            RectF askButtons[2];
        } state;

        std::wstring HerName(){return state.her.name.empty()?std::wstring(T(L"相手",L"Her")):state.her.name;}
        float Scale(){return state.window?static_cast<float>(GetDpiForWindow(state.window))/96.0f:1;}
        double CharSeconds(){return English()?0.045:0.12;}
        std::wstring ShownWords()
        {
            const auto& w=state.words;
            if(w.text.empty())return {};
            const size_t n=w.voiced?static_cast<size_t>(state.voice.Revealed()):static_cast<size_t>((GetTickCount64()-w.since)/1000.0/CharSeconds())+1;
            return w.text.substr(0,std::min(n,w.text.size()));
        }
        const Golf::Course& Course(){return *state.course;}
        XMFLOAT3 W(V3 p,float above=0){return {p.x*K,(p.y+above)*K,p.z*K};}
        int Total(int who){int t=0;for(int h=0;h<Holes;++h)t+=state.card[who][h];return t;}

        // ---- Her words ------------------------------------------------------------------

        constexpr ULONGLONG WordsHold=3000;
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
            w.told+=L"  she says: "+line.text+L"\n";
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
        std::uint32_t& WordsRandom(){static std::uint32_t r=0x2545F491u;return r;}
        const std::wstring& Pick(const std::vector<std::wstring>& lines)
        {
            auto& r=WordsRandom();
            r^=r<<13;r^=r>>17;r^=r<<5;
            return lines[r%lines.size()];
        }
        // Something happened: she speaks of it (scene: her lines' scene, null for none; chance:
        // how likely she says something), those watching hear of it.
        void Happened(int weight,const wchar_t* scene,double chance,const std::wstring& seen,const std::vector<std::wstring>& theirs)
        {
            state.words.told+=Fmt(L"  {0} s, weight {1}: {2}\n",{std::to_wstring(static_cast<int>(state.clock)),std::to_wstring(weight),seen});
            if(state.her.onEvent&&!theirs.empty())
                state.her.onEvent(seen+Fmt(T(L"（ここまで{0}が{1}打、持ち主が{2}打）",L" (so far {0} has taken {1} strokes, the owner {2})"),
                                           {HerName(),std::to_wstring(Total(Her)),std::to_wstring(Total(You))}),weight,Pick(theirs));
            if(!scene)return;
            auto& w=state.words;
            if(!w.text.empty()&&GetTickCount64()-w.since<WordsHold&&weight<w.weight)return;
            static std::mt19937 dice{777};
            if(!state.lines.Roll(scene,chance,dice))return;
            GameLine line;
            if(state.lines.Pick(scene,w.recent,dice,line))SayLine(line,weight);
        }
        void Face(Expression e,double seconds)
        {
            if(!state.her.model)return;
            state.her.model->SetExpression(e,false);
            state.faceUntil=state.clock+seconds;
        }
        void Banner(const std::wstring& big,const std::wstring& note,double seconds)
        {
            state.banner=big;
            state.bannerNote=note;
            state.bannerUntil=state.clock+seconds;
        }

        // ---- The course in 3D ----------------------------------------------------------------

        // The geometry's look 7 (GeometryPS: Course): alpha 0.14, kind in minus uv.z.
        constexpr float CourseLook=0.14f,Unlit=0.25f;
        struct Mesh
        {
            std::vector<MascotRenderer::GeometryVertex> out;
            using V=std::array<float,3>;
            void Vertex(const V& p,const V& n,const float (&c)[4],float kind=4,const std::array<float,4>& weights={})
            {
                MascotRenderer::GeometryVertex v{{p[0],p[1],p[2]},{n[0],n[1],n[2]},{c[0],c[1],c[2],c[3]}};
                v.wall[0]=weights[0];v.wall[1]=weights[1];v.wall[2]=0;v.wall[3]=weights[2];
                v.uv[0]=weights[3];v.uv[1]=0;v.uv[2]=c[3]==CourseLook?-kind:0;v.uv[3]=0;
                out.push_back(v);
            }
            void Tri(const V& a,const V& b,const V& c,const float (&col)[4],float kind=4)
            {
                const V u{b[0]-a[0],b[1]-a[1],b[2]-a[2]},w{c[0]-a[0],c[1]-a[1],c[2]-a[2]};
                V n{u[1]*w[2]-u[2]*w[1],u[2]*w[0]-u[0]*w[2],u[0]*w[1]-u[1]*w[0]};
                const float l=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
                if(l<1e-9f)return;
                for(float& x:n)x/=l;
                if(n[1]<0)for(float& x:n)x=-x;   // lit from above whichever way it is wound (none are culled)
                Vertex(a,n,col,kind);Vertex(b,n,col,kind);Vertex(c,n,col,kind);
            }
            void Quad(const V& a,const V& b,const V& c,const V& d,const float (&col)[4],float kind=4)
            {
                Tri(a,b,c,col,kind);Tri(a,c,d,col,kind);
            }
            // An upright cylinder (or a cone, r1 0) round (x, z) from y0 to y1.
            void Cylinder(float x,float z,float y0,float y1,float r0,float r1,const float (&c)[4],float kind=4,int sides=10,bool cap=true)
            {
                for(int i=0;i<sides;++i)
                {
                    const float a0=2*Pi*i/sides,a1=2*Pi*(i+1)/sides;
                    const V p0{x+r0*std::cos(a0),y0,z+r0*std::sin(a0)},p1{x+r0*std::cos(a1),y0,z+r0*std::sin(a1)};
                    const V q0{x+r1*std::cos(a0),y1,z+r1*std::sin(a0)},q1{x+r1*std::cos(a1),y1,z+r1*std::sin(a1)};
                    // Normals smooth round the side (leaning with the cone).
                    const float lean=(r0-r1)/std::max(1e-4f,y1-y0);
                    auto n=[&](float a){const float l=std::sqrt(1+lean*lean);return V{std::cos(a)/l,lean/l,std::sin(a)/l};};
                    Vertex(p0,n(a0),c,kind);Vertex(q1,n(a1),c,kind);Vertex(p1,n(a1),c,kind);
                    if(r1>0){Vertex(p0,n(a0),c,kind);Vertex(q0,n(a0),c,kind);Vertex(q1,n(a1),c,kind);}
                    if(cap&&r1>0){Vertex({x,y1,z},{0,1,0},c,kind);Vertex(q1,{0,1,0},c,kind);Vertex(q0,{0,1,0},c,kind);}
                }
            }
            // A ball (lat-long), radius r round p.
            void Sphere(const V& p,float r,const float (&c)[4],float kind=4,int around=12,int down=7,float squash=1)
            {
                auto at=[&](int i,int j,V& pos,V& n){
                    const float th=Pi*j/down,ph=2*Pi*i/around;
                    n={std::sin(th)*std::cos(ph),std::cos(th),std::sin(th)*std::sin(ph)};
                    pos={p[0]+r*n[0],p[1]+r*squash*n[1],p[2]+r*n[2]};
                };
                for(int j=0;j<down;++j)
                    for(int i=0;i<around;++i)
                    {
                        V a,b,cc,d,na,nb,nc,nd;
                        at(i,j,a,na);at(i+1,j,b,nb);at(i+1,j+1,cc,nc);at(i,j+1,d,nd);
                        Vertex(a,na,c,kind);Vertex(b,nb,c,kind);Vertex(cc,nc,c,kind);
                        Vertex(a,na,c,kind);Vertex(cc,nc,c,kind);Vertex(d,nd,c,kind);
                    }
            }
        };

        void BlendColour(float fairway,float green,float sand,float water,float tee,float (&c)[4])
        {
            const float rough[3]={0.25f,0.42f,0.14f},fw[3]={0.36f,0.60f,0.21f},gr[3]={0.38f,0.66f,0.25f},sa[3]={0.88f,0.80f,0.60f},wa[3]={0.13f,0.30f,0.42f},te[3]={0.36f,0.63f,0.24f};
            const float r=std::max(0.0f,1-fairway-green-sand-water-tee);
            const float sum=r+fairway+green+sand+water+tee;
            for(int i=0;i<3;++i)c[i]=(rough[i]*r+fw[i]*fairway+gr[i]*green+sa[i]*sand+wa[i]*water+te[i]*tee)/std::max(sum,1e-4f);
            c[3]=CourseLook;
        }

        // What does not move on this hole: the ground, the pond's water, trees, the sky, the
        // tee markers, the cup.
        std::vector<MascotRenderer::GeometryVertex> BuildCourse()
        {
            const auto& c=Course();
            Mesh m;
            constexpr float Step=2.0f;
            const int nx=static_cast<int>(std::ceil((c.MaxX()-c.MinX())/Step)),nz=static_cast<int>(std::ceil((c.MaxZ()-c.MinZ())/Step));
            struct P {Mesh::V p,n;float col[4];std::array<float,4> w;};
            std::vector<P> grid(static_cast<size_t>(nx+1)*(nz+1));
            for(int j=0;j<=nz;++j)
                for(int i=0;i<=nx;++i)
                {
                    const float x=c.MinX()+i*Step,z=c.MinZ()+j*Step;
                    P& q=grid[static_cast<size_t>(j)*(nx+1)+i];
                    const float h=c.Height(x,z);
                    const V3 n=c.Normal(x,z);
                    float fw,gr,sa,wa,te;
                    c.Surface(x,z,fw,gr,sa,wa,te);
                    // Under the water: mud (the water's plane is drawn over it).
                    const bool under=c.LieAt(x,z)==Lie::Water;
                    if(under){fw=gr=sa=te=0;wa=0;}
                    q.p={x*K,h*K,z*K};
                    q.n={n.x,n.y,n.z};
                    BlendColour(fw,gr,sa,wa,te,q.col);
                    if(under){q.col[0]=0.22f;q.col[1]=0.20f;q.col[2]=0.14f;}
                    q.w={fw,gr,sa,0};
                    (void)te;
                }
            auto put=[&](const P& q){
                MascotRenderer::GeometryVertex v{{q.p[0],q.p[1],q.p[2]},{q.n[0],q.n[1],q.n[2]},{q.col[0],q.col[1],q.col[2],q.col[3]}};
                v.wall[0]=q.w[0];v.wall[1]=q.w[1];v.wall[2]=0;v.wall[3]=q.w[2];
                v.uv[0]=0;v.uv[1]=0;v.uv[2]=-1;v.uv[3]=0;
                m.out.push_back(v);
            };
            for(int j=0;j<nz;++j)
                for(int i=0;i<nx;++i)
                {
                    const P& a=grid[static_cast<size_t>(j)*(nx+1)+i];
                    const P& b=grid[static_cast<size_t>(j)*(nx+1)+i+1];
                    const P& cc=grid[static_cast<size_t>(j+1)*(nx+1)+i+1];
                    const P& d=grid[static_cast<size_t>(j+1)*(nx+1)+i];
                    // Wound so the faces look up (left-handed: clockwise seen from above).
                    put(a);put(d);put(cc);
                    put(a);put(cc);put(b);
                }
            // The world round the course: a wide plain below its edges, fading into the haze.
            {
                float low=1e9f;
                for(const P& q:grid)low=std::min(low,q.p[1]);
                const float y=low-1.5f*K,R=3000*K,cx=(c.MinX()+c.MaxX())/2*K,cz=(c.MinZ()+c.MaxZ())/2*K;
                float col[4];
                BlendColour(0,0,0,0,0,col);
                for(int i=0;i<24;++i)
                {
                    const float a0=2*Pi*i/24,a1=2*Pi*(i+1)/24;
                    MascotRenderer::GeometryVertex v{{cx,y,cz},{0,1,0},{col[0],col[1],col[2],col[3]}};
                    v.uv[2]=-1;
                    MascotRenderer::GeometryVertex p1=v,p2=v;
                    p1.position[0]=cx+R*std::cos(a1);p1.position[2]=cz+R*std::sin(a1);
                    p2.position[0]=cx+R*std::cos(a0);p2.position[2]=cz+R*std::sin(a0);
                    m.out.push_back(v);m.out.push_back(p1);m.out.push_back(p2);
                }
            }
            // The pond's water: a flat sheet at its level.
            if(c.HasWater())
            {
                const float y=c.WaterLevel()*K;
                float col[4];
                BlendColour(0,0,0,1,0,col);
                for(int j=0;j<nz;++j)
                    for(int i=0;i<nx;++i)
                    {
                        const float x=c.MinX()+i*Step,z=c.MinZ()+j*Step;
                        bool any=false;
                        for(float dx:{0.0f,Step})for(float dz:{0.0f,Step})if(c.LieAt(x+dx,z+dz)==Lie::Water)any=true;
                        if(!any)continue;
                        for(const auto& tri:{std::array<std::array<float,2>,3>{{{0,0},{0,Step},{Step,Step}}},std::array<std::array<float,2>,3>{{{0,0},{Step,Step},{Step,0}}}})
                            for(const auto& q:tri)m.Vertex({(x+q[0])*K,y,(z+q[1])*K},{0,1,0},col,1,{0,0,0,1});
                    }
            }
            // Trees: firs (cones on a trunk) and round-headed ones.
            for(const auto& t:c.Trees())
            {
                const float base=c.Height(t.x,t.z)*K,x=t.x*K,z=t.z*K,h=t.height*K,r=t.radius*K;
                const float bark[4]={0.30f,0.22f,0.15f,CourseLook};
                m.Cylinder(x,z,base-0.3f*K,base+h*0.45f,0.08f*r,0.06f*r,bark,2,6,false);
                const float shade=0.85f+0.3f*std::fmod(std::abs(t.x*7.3f+t.z*3.1f),1.0f);
                if(t.kind==0)
                {
                    const float leaf[4]={0.13f*shade,0.30f*shade,0.15f*shade,CourseLook};
                    for(int k=0;k<3;++k)
                    {
                        const float y0=base+h*(0.22f+0.22f*k),y1=y0+h*(0.5f-0.08f*k);
                        m.Cylinder(x,z,y0,y1,r*(1.0f-0.22f*k),0,leaf,3,9);
                    }
                }
                else
                {
                    const float leaf[4]={0.22f*shade,0.40f*shade,0.13f*shade,CourseLook};
                    m.Sphere({x,base+h*0.62f,z},r*1.05f,leaf,3,10,6,0.9f);
                    m.Sphere({x+r*0.5f,base+h*0.5f,z+r*0.3f},r*0.7f,leaf,3,8,5,0.9f);
                    m.Sphere({x-r*0.45f,base+h*0.55f,z-r*0.35f},r*0.75f,leaf,3,8,5,0.9f);
                }
            }
            // A line of far woods round the course's edge, dark against the haze.
            {
                const float leaf[4]={0.16f,0.30f,0.16f,CourseLook};
                std::mt19937 r(c.Seed()+99);
                std::uniform_real_distribution<float> u(0,1);
                const float x0=c.MinX()-8,x1=c.MaxX()+8,z0=c.MinZ()-8,z1=c.MaxZ()+8;
                auto tree=[&](float x,float z){
                    const float h=(9+8*u(r))*K,rad=(3+2*u(r))*K,base=c.Height(std::clamp(x,c.MinX(),c.MaxX()),std::clamp(z,c.MinZ(),c.MaxZ()))*K-1.5f*K;
                    m.Cylinder(x*K,z*K,base,base+h,rad,0,leaf,3,7);
                };
                for(float z=z0;z<=z1;z+=4+3*u(r)){tree(x0-6*u(r),z);tree(x1+6*u(r),z);}
                for(float x=x0;x<=x1;x+=4+3*u(r)){tree(x,z0-6*u(r));tree(x,z1+6*u(r));}
            }
            // The tee's markers.
            {
                const V3 t=c.Tee();
                const float col[4]={0.92f,0.25f,0.25f,1};
                for(float side:{-2.5f,2.5f})m.Sphere({side*K,(t.y+0.06f)*K,1.5f*K},0.09f*K,col,4,8,5);
            }
            // The cup: a dark hole with a white rim.
            {
                const V3 p=c.Pin();
                const float dark[4]={0.03f,0.03f,0.03f,1},white[4]={0.95f,0.95f,0.95f,1};
                const float y=(p.y+0.004f)*K;
                for(int i=0;i<24;++i)
                {
                    const float a0=2*Pi*i/24,a1=2*Pi*(i+1)/24,r0=Golf::CupRadius*K,r1=r0*1.12f;
                    m.Vertex({p.x*K,y,p.z*K},{0,1,0},dark);
                    m.Vertex({p.x*K+r0*std::cos(a1),y,p.z*K+r0*std::sin(a1)},{0,1,0},dark);
                    m.Vertex({p.x*K+r0*std::cos(a0),y,p.z*K+r0*std::sin(a0)},{0,1,0},dark);
                    m.Quad({p.x*K+r0*std::cos(a0),y,p.z*K+r0*std::sin(a0)},{p.x*K+r0*std::cos(a1),y,p.z*K+r0*std::sin(a1)},
                           {p.x*K+r1*std::cos(a1),y,p.z*K+r1*std::sin(a1)},{p.x*K+r1*std::cos(a0),y,p.z*K+r1*std::sin(a0)},white);
                }
            }
            // The sky: a dome, pale at the horizon (the haze's colour), deep blue overhead, unlit.
            {
                const float cx=(c.MinX()+c.MaxX())/2*K,cz=(c.MinZ()+c.MaxZ())/2*K,R=3500*K;
                const int around=32,down=10;
                auto colour=[&](float t,float (&col)[4]){
                    const float lo[3]={0.72f,0.80f,0.88f},hi[3]={0.30f,0.52f,0.86f};
                    const float k=std::pow(std::clamp(t,0.0f,1.0f),0.6f);
                    for(int i=0;i<3;++i)col[i]=lo[i]+(hi[i]-lo[i])*k;
                    col[3]=Unlit;
                };
                for(int j=-1;j<down;++j)
                    for(int i=0;i<around;++i)
                    {
                        const float e0=j<0?-0.2f:(Pi/2)*j/down,e1=(Pi/2)*(j+1)/down;
                        const float a0=2*Pi*i/around,a1=2*Pi*(i+1)/around;
                        auto at=[&](float a,float e){return Mesh::V{cx+R*std::cos(e)*std::cos(a),R*std::sin(e),cz+R*std::cos(e)*std::sin(a)};};
                        float c0[4],c1[4];
                        colour(std::max(0.0f,e0/(Pi/2)),c0);
                        colour(e1/(Pi/2),c1);
                        const Mesh::V n{0,-1,0};
                        m.Vertex(at(a0,e0),n,c0);m.Vertex(at(a1,e0),n,c0);m.Vertex(at(a1,e1),n,c1);
                        m.Vertex(at(a0,e0),n,c0);m.Vertex(at(a1,e1),n,c1);m.Vertex(at(a0,e1),n,c1);
                    }
            }
            return std::move(m.out);
        }

        // ---- Her on the course -------------------------------------------------------------

        DirectX::XMMATRIX HerWorld(MascotModel& model)
        {
            using namespace DirectX;
            return XMMatrixTranslation(-model.RootOffset(),0,0)*XMMatrixRotationY(state.herYaw)*XMMatrixTranslation(state.herAt.x*K,state.herAt.y*K,state.herAt.z*K);
        }
        std::array<float,3> ToModel(MascotModel& model,XMFLOAT3 p)
        {
            using namespace DirectX;
            XMVECTOR det;
            const XMVECTOR q=XMVector3Transform(XMLoadFloat3(&p),XMMatrixInverse(&det,HerWorld(model)));
            return {XMVectorGetX(q),XMVectorGetY(q),XMVectorGetZ(q)};
        }

        // Standing at the ball to hit it toward aim: the model turned so its left (+X) points
        // there, and placed so the club's head at the address is on the ball.
        void PlaceToSwing(V3 ball,float aim)
        {
            state.herYaw=aim-Pi/2;
            const float c=std::cos(state.herYaw),s=std::sin(state.herYaw);
            const float hx=state.head[0]*c+state.head[2]*s,hz=-state.head[0]*s+state.head[2]*c;
            const V3 at{ball.x-hx/K,0,ball.z-hz/K};
            if(Golf::Flat(at,state.herAt)>0.5f)state.moved=true;
            state.herAt=at;
            state.herAt.y=Course().Height(state.herAt.x,state.herAt.z);
            state.herSwinging=true;
        }
        // Watching a ball from the side, a little ahead of it (where the camera behind the ball
        // sees her, clear of the shot's way), turned to it.
        void PlaceToWatch(V3 ball,float aim)
        {
            const V3 dir{std::sin(aim),0,std::cos(aim)},right{std::cos(aim),0,-std::sin(aim)};
            V3 at=ball+dir*1.2f+right*2.6f;
            if(Course().LieAt(at.x,at.z)==Lie::Water)at=ball+dir*1.2f-right*2.6f;
            at.y=Course().Height(at.x,at.z);
            if(Golf::Flat(at,state.herAt)>0.5f)state.moved=true;
            state.herAt=at;
            const V3 to=ball-at;
            state.herYaw=std::atan2(-to.x,-to.z);
            state.herSwinging=false;
        }

        void MeasureHer(MascotModel& model)
        {
            std::array<float,3> lo{},hi{},legLo{},legHi{},head{};
            if(model.StandardBoneBounds(lo,hi)&&model.LegBounds(legLo,legHi)&&model.HeadPosition(head)&&head[1]>legHi[1])
            {
                state.hips=std::max(legHi[1]-lo[1],0.1f);
                state.upper=head[1]-legHi[1];
            }
            model.SetActivity(Activity::Idle);
            model.SetAutoGestures(false);
            model.EndGestures();
            model.StopMotion();
            for(int i=0;i<15||(i<90&&model.MotionPlaying());++i)model.Update(1/30.0);
            // At the address for a while: where the grip is and the club's way give its length
            // (its head on the ground) and where the ball lies.
            for(int i=0;i<60;++i){model.Swing(true,0,1);model.Update(1/30.0);}
            std::array<float,3> middle{},along{},club{};
            if(model.Grip(0,middle,along)&&model.SwingClub(club)&&club[1]<-0.2f)
            {
                state.clubLength=std::clamp((middle[1]-Golf::BallRadius*K)/-club[1],0.6f*K,1.4f*K);
                state.head={middle[0]+club[0]*state.clubLength,0,middle[2]+club[2]*state.clubLength};
            }
            else state.head={0,0,-0.9f*K};
            for(int i=0;i<20;++i){model.Swing(false,0,1);model.Update(1/30.0);}
            model.ResetPhysics();
            Log(L"golf: hips %.1f, upper body %.1f, club %.2f m, ball at %.2f,%.2f m from her",state.hips,state.upper,state.clubLength/K,state.head[0]/K,state.head[2]/K);
        }

        // ---- The game ----------------------------------------------------------------------

        float PinAim(V3 from){const V3 p=Course().Pin();return std::atan2(p.x-from.x,p.z-from.z);}

        void UpdateLanding()
        {
            const Ball& b=state.balls[You];
            Golf::Shot s;
            s.club=state.club;s.aim=state.aim;s.power=1;
            s.range=Golf::PutterRange(Golf::Flat(b.at,Course().Pin()));
            const Golf::Flight f=Golf::Fly(Course(),b.at,b.lie,s,{},true,1/60.0f);
            state.landing=state.club==Golf::Putter||f.path.empty()?f.rest:f.path[std::min<size_t>(f.landing,f.path.size()-1)];
            state.landingKnown=true;
        }

        void BuildMap()
        {
            const auto& c=Course();
            const int h=220;
            const float span=c.MaxZ()-c.MinZ(),scale=h/span;
            const int w=std::max(60,static_cast<int>((c.MaxX()-c.MinX())*scale));
            state.map=std::make_unique<Bitmap>(w,h,PixelFormat32bppARGB);
            state.mapScale=scale;
            BitmapData bits{};
            Rect whole(0,0,w,h);
            if(state.map->LockBits(&whole,ImageLockModeWrite,PixelFormat32bppARGB,&bits)!=Ok)return;
            for(int y=0;y<h;++y)
            {
                BYTE* row=static_cast<BYTE*>(bits.Scan0)+static_cast<size_t>(y)*bits.Stride;
                for(int x=0;x<w;++x)
                {
                    const float wx=c.MinX()+(x+0.5f)/scale,wz=c.MaxZ()-(y+0.5f)/scale;
                    BYTE r=60,g=110,b=40;
                    switch(c.LieAt(wx,wz))
                    {
                    case Lie::Fairway:case Lie::Tee:r=96;g=160;b=60;break;
                    case Lie::Green:r=120;g=200;b=90;break;
                    case Lie::Bunker:r=225;g=205;b=150;break;
                    case Lie::Water:r=50;g=110;b=170;break;
                    case Lie::Out:r=40;g=60;b=36;break;
                    default:break;
                    }
                    row[x*4+0]=b;row[x*4+1]=g;row[x*4+2]=r;row[x*4+3]=230;
                }
            }
            state.map->UnlockBits(&bits);
        }

        void StartHole()
        {
            const int par=state.hole==0?4:state.hole==1?3:5;
            state.pars[state.hole]=par;
            state.course=std::make_shared<const Golf::Course>(state.firstSeed+static_cast<std::uint32_t>(state.hole)*7919u,par);
            std::uniform_real_distribution<float> u(0,1);
            state.wind={u(state.random)*6.5f,u(state.random)*2*Pi};
            for(Ball& b:state.balls){b={};b.at=Course().Tee();b.lie=Lie::Tee;}
            state.turn=You;
            state.club=Golf::Driver;
            state.aim=PinAim(Course().Tee())*0.4f;
            if(state.renderer)state.renderer->SetRoomGeometry(BuildCourse());
            BuildMap();
            state.phase=Phase::Intro;
            state.phaseTime=0;
            state.cut=true;
            state.landingKnown=false;
            PlaceToWatch(Course().Tee(),state.aim);
            state.log+=Fmt(L"hole {0}: par {1}, {2} m, seed {3}, wind {4} m/s\n",{std::to_wstring(state.hole+1),std::to_wstring(par),std::to_wstring(static_cast<int>(Course().Length())),
                                                                              std::to_wstring(Course().Seed()),std::to_wstring(static_cast<int>(state.wind.speed*10)/10.0f).substr(0,3)});
            Banner(Fmt(T(L"{0}番ホール　パー{1}",L"Hole {0}   Par {1}"),{std::to_wstring(state.hole+1),std::to_wstring(par)}),
                   Fmt(T(L"{0} m",L"{0} m"),{std::to_wstring(static_cast<int>(Course().Length()))}),3.0);
        }

        void NewRound()
        {
            if(state.thinking.valid())state.thinking.wait();
            state.card={};
            state.hole=0;
            state.started=false;
            state.overTold=false;
            state.askingQuit=false;
            state.afterword.clear();
            state.afterwordLine.clear();
            state.words.text.clear();
            if(!state.firstSeed)state.firstSeed=static_cast<std::uint32_t>(GetTickCount64()*2654435761ull);
            StartHole();
        }

        // Who plays next: one still on the tee (you first), else the one farther from the pin.
        int NextPlayer()
        {
            for(int who:{You,Her})if(!state.balls[who].teed&&!state.balls[who].holed)return who;
            const V3 pin=Course().Pin();
            int next=-1;
            float far_=-1;
            for(int who:{You,Her})
            {
                const Ball& b=state.balls[who];
                if(b.holed)continue;
                const float d=Golf::Flat(b.at,pin);
                if(d>far_){far_=d;next=who;}
            }
            return next;
        }

        void BeginTurn()
        {
            const int who=NextPlayer();
            if(who<0)
            {
                // Both holed out: the hole's strokes on the card.
                for(int w:{You,Her})state.card[w][state.hole]=state.balls[w].strokes;
                state.phase=Phase::HoleOver;
                state.phaseTime=0;
                const int you=state.balls[You].strokes,her=state.balls[Her].strokes;
                Banner(Fmt(T(L"{0}番ホール終了",L"Hole {0} done"),{std::to_wstring(state.hole+1)}),
                       Fmt(T(L"あなた {0}打　{1} {2}打",L"You {0}   {1} {2}"),{std::to_wstring(you),HerName(),std::to_wstring(her)}),3.5);
                return;
            }
            state.turn=who;
            state.phase=Phase::Aim;
            state.phaseTime=0;
            state.cut=true;
            state.camera={};
            Ball& b=state.balls[who];
            const float toPin=Golf::Flat(b.at,Course().Pin());
            if(who==You)
            {
                state.aim=PinAim(b.at);
                // From the tee of a long hole, down the line rather than at the far-off pin.
                if(b.lie==Lie::Tee&&toPin>260)state.aim=std::atan2(Course().LineX(230)-b.at.x,230-b.at.z);
                state.club=Golf::ClubFor(b.lie,toPin,Course(),b.at,{});
                state.gauge=GaugeState::Ready;
                state.gaugeAt=0;
                UpdateLanding();
                PlaceToWatch(b.at,state.aim);
                if(state.autoplay)
                {
                    state.thought=false;
                    const auto course=state.course;
                    const V3 at=b.at;
                    const Lie lie=b.lie;
                    const Golf::Wind wind=state.wind;
                    state.thinking=std::async(std::launch::async,[course,at,lie,wind]{return Golf::Choose(*course,at,lie,wind);});
                }
            }
            else
            {
                state.thought=false;
                state.swingTime=0;
                const auto course=state.course;
                const V3 at=b.at;
                const Lie lie=b.lie;
                const Golf::Wind wind=state.wind;
                state.thinking=std::async(std::launch::async,[course,at,lie,wind]{return Golf::Choose(*course,at,lie,wind);});
                // At her ball already, turned toward the pin (turned again once she has chosen).
                PlaceToSwing(b.at,PinAim(b.at));
            }
        }

        void Launch()
        {
            Ball& b=state.balls[state.turn];
            state.shotFrom=b.at;
            state.shotFromLie=b.lie;
            state.flight=Golf::Fly(Course(),b.at,b.lie,state.shot,state.wind,true,1/60.0f);
            state.flightTime=0;
            state.launched=true;
            b.teed=true;
            ++b.strokes;
            ++state.shots;
            state.phase=Phase::Flying;
            state.phaseTime=0;
            wchar_t line[200];
            swprintf_s(line,L"  %ls: %ls power %.2f accuracy %+.2f aim %.1f -> %ls%ls%ls carry %.0f m, %.1f m left\n",state.turn==You?L"you":L"her",Golf::ClubOf(state.shot.club).en,
                       state.shot.power,state.shot.accuracy,state.shot.aim/Deg,Golf::LieName(state.flight.lie),state.flight.holed?L" HOLED":L"",state.flight.water?L" WATER":state.flight.out?L" OB":L"",
                       state.flight.carry,Golf::Flat(state.flight.rest,Course().Pin()));
            state.log+=line;
        }

        // The ball has stopped: where it lies now, penalties, words.
        void Settled()
        {
            const int who=state.turn;
            Ball& b=state.balls[who];
            const auto& f=state.flight;
            const std::wstring name=HerName();
            const bool hers=who==Her;
            const bool putt=state.shot.club==Golf::Putter;
            if(f.holed)
            {
                b.holed=true;
                b.at=Course().Pin();
                const int par=Course().Par(),n=b.strokes;
                const std::wstring score=n==1?T(L"ホールインワン！",L"Hole in one!"):n<=par-2?T(L"イーグル！",L"Eagle!"):n==par-1?T(L"バーディー！",L"Birdie!"):
                                         n==par?T(L"パー",L"Par"):n==par+1?T(L"ボギー",L"Bogey"):n==par+2?T(L"ダブルボギー",L"Double bogey"):Fmt(T(L"{0}打",L"{0} strokes"),{std::to_wstring(n)});
                Banner(T(L"カップイン！",L"In the cup!"),(hers?name+L"  ":std::wstring(T(L"あなた  ",L"You  ")))+score,2.6);
                if(hers){Face(Expression::Smile,3);Happened(7,L"カップイン",1,Fmt(T(L"ゴルフで、{0}がカップに入れた（{1}打）。",L"In the golf game, {0} holed out ({1} strokes)."),{name,std::to_wstring(n)}),{T(L"ナイスイン！",L"Nice putt!")});}
                else{Face(Expression::Smile,2.5);Happened(7,L"あなたのカップイン",1,Fmt(T(L"ゴルフで、持ち主がカップに入れた（{0}打）。",L"In the golf game, the owner holed out ({0} strokes)."),{std::to_wstring(n)}),{T(L"入った！",L"In!")});}
                return;
            }
            const bool penalty=f.out||f.water;
            if(f.out)
            {
                ++b.strokes;
                Banner(T(L"OB",L"Out of bounds"),T(L"1打罰で打ち直し",L"One stroke penalty, play again"),2.4);
            }
            else if(f.water)
            {
                ++b.strokes;
                b.at=f.drop;
                b.at.y=Course().Height(b.at.x,b.at.z);
                b.lie=Course().LieAt(b.at.x,b.at.z);
                if(b.lie==Lie::Water||b.lie==Lie::Out)b.lie=Lie::Rough;
                Banner(T(L"池ポチャ",L"In the water"),T(L"1打罰",L"One stroke penalty"),2.4);
            }
            else
            {
                b.at=f.rest;
                b.at.y=Course().Height(b.at.x,b.at.z);
                b.lie=f.lie;
                if(!putt)Banner(Golf::LieName(b.lie),Fmt(T(L"{0} m　ピンまで {1} m",L"{0} m   {1} m to the pin"),{std::to_wstring(static_cast<int>(Golf::Flat(state.shotFrom,f.rest))),std::to_wstring(static_cast<int>(Golf::Flat(f.rest,Course().Pin())))}),2.0);
            }
            if(b.strokes>=MostStrokes&&!b.holed)
            {
                b.holed=true;
                b.strokes=MostStrokes;
                Banner(T(L"ギブアップ",L"Picked up"),Fmt(T(L"{0}打",L"{0} strokes"),{std::to_wstring(MostStrokes)}),2.4);
            }
            // Words: a good one, or one gone astray.
            if(putt)return;
            const float acc=std::abs(state.shot.accuracy);
            const bool good=!penalty&&acc<0.3f&&(b.lie==Lie::Fairway||b.lie==Lie::Green);
            const bool bad=penalty||acc>0.75f||b.lie==Lie::Bunker||(b.lie==Lie::Rough&&acc>0.4f);
            if(hers)
            {
                if(penalty){Face(Expression::Troubled,3);Happened(6,L"池やOB",1,Fmt(T(L"ゴルフで、{0}の球が{1}。",L"In the golf game, {0}'s ball went {1}."),{name,f.water?T(L"池に入った",L"into the water"):T(L"OBになった",L"out of bounds")}),{T(L"ああっ…",L"Oh no...")});}
                else if(good){Face(Expression::Smile,2.5);Happened(5,L"ナイスショット",0.7,Fmt(T(L"ゴルフで、{0}がいいショットを打った。",L"In the golf game, {0} hit a good shot."),{name}),{T(L"ナイスショット！",L"Nice shot!")});}
                else if(bad){Face(Expression::Troubled,2.5);Happened(5,L"ミスショット",0.8,Fmt(T(L"ゴルフで、{0}のショットが曲がった。",L"In the golf game, {0}'s shot went astray."),{name}),{T(L"ドンマイ！",L"Never mind!")});}
            }
            else
            {
                if(good){Face(Expression::Smile,2.5);Happened(5,L"あなたのナイスショット",0.7,T(L"ゴルフで、持ち主がいいショットを打った。",L"In the golf game, the owner hit a good shot."),{T(L"ナイスショット！",L"Nice shot!")});}
                else if(bad){Happened(5,L"あなたのミスショット",0.7,T(L"ゴルフで、持ち主のショットが曲がった。",L"In the golf game, the owner's shot went astray."),{T(L"ドンマイ！",L"Never mind!")});}
            }
        }

        void RoundOver()
        {
            state.phase=Phase::Over;
            state.phaseTime=0;
            const int you=Total(You),her=Total(Her);
            const std::wstring name=HerName();
            const int par=state.pars[0]+state.pars[1]+state.pars[2];
            auto vsPar=[&](int n){const int d=n-par;return d==0?std::wstring(L"±0"):(d>0?L"+":L"")+std::to_wstring(d);};
            Banner(you<her?T(L"あなたの勝ち！",L"You win!"):her<you?name+T(L"の勝ち",L" wins"):std::wstring(T(L"引き分け",L"A draw")),
                   Fmt(T(L"あなた {0}（{1}）　{2} {3}（{4}）　Enterでもう一度・Escで閉じる",L"You {0} ({1})   {2} {3} ({4})   Enter: again, Esc: close"),
                       {std::to_wstring(you),vsPar(you),name,std::to_wstring(her),vsPar(her)}),1e9);
            const bool won=her<you;
            if(won)Happened(9,L"勝った",1,Fmt(T(L"{0}と持ち主のゴルフが終わり、{0}が勝った。",L"The golf round between {0} and the owner is over: {0} won."),{name}),{T(L"おつかれさま！",L"Well played!")});
            else if(you<her)Happened(9,L"負けた",1,Fmt(T(L"{0}と持ち主のゴルフが終わり、持ち主が勝った。",L"The golf round between {0} and the owner is over: the owner won."),{name}),{T(L"おめでとう！",L"Congratulations!")});
            else Happened(9,L"引き分け",1,Fmt(T(L"{0}と持ち主のゴルフが終わり、引き分けだった。",L"The golf round between {0} and the owner is over: a draw."),{name}),{T(L"いい勝負！",L"A close one!")});
            state.afterword=Fmt(T(L"いま、話し相手とゴルフを3ホール回って、デスクトップに戻ってきたところ。結果は{0}（あなた{1}打、相手{2}打）。その感想を、話し相手にひと言（1〜2文）",
                                  L"You have just played three holes of golf with them and come back to the desktop. {0} (you took {1} strokes, they took {2}). Tell them what you thought of it (one or two sentences)"),
                                {won?T(L"あなたの勝ち",L"You won"):you<her?T(L"あなたの負け",L"You lost"):T(L"引き分け",L"A draw"),std::to_wstring(her),std::to_wstring(you)});
            state.afterwordLine=won?T(L"ゴルフ、楽しかったね！",L"Golf was fun!"):T(L"くやしいなあ、次は勝つよ！",L"So close... I'll win next time!");
            if(!state.overTold){state.overTold=true;if(state.her.onGameOver)state.her.onGameOver();}
        }

        // The gauge: clicked (or Space).
        void GaugeClick()
        {
            if(state.phase!=Phase::Aim&&state.phase!=Phase::Gauge)return;
            if(state.turn!=You||state.askingQuit)return;
            switch(state.gauge)
            {
            case GaugeState::Ready:
                state.gauge=GaugeState::Rising;
                state.gaugeAt=0;
                state.phase=Phase::Gauge;
                break;
            case GaugeState::Rising:
                state.gaugePower=std::max(0.02f,state.gaugeAt);
                state.gauge=GaugeState::Falling;
                break;
            case GaugeState::Falling:
                state.gaugeAccuracy=std::clamp(state.gaugeAt/0.1f,-2.5f,2.5f);
                state.gauge=GaugeState::Done;
                break;
            default:break;
            }
        }

        void ShootYours()
        {
            const Ball& b=state.balls[You];
            state.shot={};
            state.shot.club=state.club;
            state.shot.aim=state.aim;
            state.shot.power=state.gaugePower;
            state.shot.accuracy=state.gaugeAccuracy;
            state.shot.range=Golf::PutterRange(Golf::Flat(b.at,Course().Pin()));
            Launch();
        }

        // Her care now: less while she is well ahead.
        float CareNow()
        {
            const int lead=(Total(You)+state.balls[You].strokes)-(Total(Her)+state.balls[Her].strokes);
            return std::clamp(state.care-(lead>=4?0.3f:lead>=2?0.15f:0.0f),0.3f,1.0f);
        }

        // Her swing's stage at seconds into it: the address (until she has chosen), the
        // backswing, the top, down, through; when it strikes.
        constexpr double Address=1.6,Back=0.95,Top=0.12,Down=0.28,Through=0.55,HoldFinish=1.5;
        float SwingStage(double t,double addressEnd)
        {
            if(t<addressEnd)return 0;
            t-=addressEnd;
            if(t<Back){const float u=static_cast<float>(t/Back);return u*u*(3-2*u);}
            t-=Back;
            if(t<Top)return 1;
            t-=Top;
            if(t<Down){const float u=static_cast<float>(t/Down);return 1+std::pow(u,1.7f);}
            t-=Down;
            if(t<Through){const float u=static_cast<float>(t/Through);return 2+(1-(1-u)*(1-u));}
            return 3;
        }

        void Step(double dt)
        {
            state.clock+=dt;
            state.phaseTime+=dt;
            auto& w=state.words;
            if(!w.text.empty()&&GetTickCount64()>w.until)w.text.clear();
            if(state.faceUntil>0&&state.clock>state.faceUntil){state.faceUntil=0;if(state.her.model)state.her.model->SetExpression(Expression::Neutral,false);}
            if(!state.started&&(!state.prefetching||state.clock>4))
            {
                state.started=true;
                Happened(7,L"始まり",1,Fmt(T(L"{0}と、このパソコンの持ち主のゴルフ（3ホール）が始まる。",L"A three-hole round of golf between {0} and the owner of this computer is about to begin."),{HerName()}),
                         {T(L"がんばれー！",L"Good luck, you two!")});
            }
            if(state.askingQuit||state.posing>=0)return;
            switch(state.phase)
            {
            case Phase::Intro:
                if(state.phaseTime>4.0)BeginTurn();
                break;
            case Phase::Aim:
            case Phase::Gauge:
                if(state.turn==You)
                {
                    if(state.aimKeys!=0&&state.gauge==GaugeState::Ready)
                    {
                        const float speed=(GetKeyState(VK_SHIFT)<0?3.0f:(state.club==Golf::Putter?8.0f:18.0f))*Deg;
                        state.aim+=state.aimKeys*speed*static_cast<float>(dt);
                        UpdateLanding();
                    }
                    if(state.autoplay&&state.gauge==GaugeState::Ready&&state.thinking.valid()&&state.phaseTime>1.5&&
                       state.thinking.wait_for(std::chrono::seconds(0))==std::future_status::ready)
                    {
                        Golf::Shot s=state.thinking.get();
                        Golf::Execute(s,0.9f,state.random);
                        state.club=s.club;state.aim=s.aim;
                        state.gaugePower=s.power;state.gaugeAccuracy=s.accuracy;
                        state.gauge=GaugeState::Done;
                    }
                    if(state.gauge==GaugeState::Rising)
                    {
                        state.gaugeAt+=static_cast<float>(dt/1.05);
                        if(state.gaugeAt>=1){state.gaugeAt=1;state.gaugePower=1;state.gauge=GaugeState::Falling;}
                    }
                    else if(state.gauge==GaugeState::Falling)
                    {
                        state.gaugeAt-=static_cast<float>(dt/0.8);
                        if(state.gaugeAt<-0.25f){state.gaugeAccuracy=-2.5f;state.gauge=GaugeState::Done;}
                    }
                    else if(state.gauge==GaugeState::Done)ShootYours();
                }
                else
                {
                    // Her: she stands over it while she thinks, then swings.
                    if(!state.thought&&state.thinking.valid()&&state.thinking.wait_for(std::chrono::seconds(0))==std::future_status::ready)
                    {
                        state.shot=state.thinking.get();
                        Golf::Execute(state.shot,CareNow(),state.random);
                        state.thought=true;
                        state.swingPower=state.shot.club==Golf::Putter?std::clamp(0.2f+0.25f*state.shot.power,0.2f,0.45f):std::clamp(0.45f+0.55f*state.shot.power,0.3f,1.0f);
                        PlaceToSwing(state.balls[Her].at,state.shot.aim);
                        state.phase=Phase::Swing;
                        state.swingTime=0;
                    }
                }
                break;
            case Phase::Swing:
            {
                const double before=state.swingTime;
                state.swingTime+=dt;
                const double strike=Address+Back+Top+Down;
                if(before<strike&&state.swingTime>=strike)Launch();
                break;
            }
            case Phase::Flying:
            {
                // Faster while it rolls a long way on the ground.
                state.flightTime+=dt;
                if(state.turn==Her)state.swingTime+=dt;
                const double end=state.flight.path.size()/60.0;
                if(state.flightTime>=end+0.3)
                {
                    state.phase=Phase::Settle;
                    state.phaseTime=0;
                    Settled();
                }
                break;
            }
            case Phase::Settle:
                if(state.turn==Her)state.swingTime+=dt;
                if(state.phaseTime>2.0)BeginTurn();
                break;
            case Phase::HoleOver:
                if(state.phaseTime>3.6)
                {
                    if(state.hole+1<Holes){++state.hole;StartHole();}
                    else RoundOver();
                }
                break;
            case Phase::Over:
                break;
            }
        }

        // Where the ball is now (in flight, its path played back).
        V3 BallNow(int who)
        {
            if(who==state.turn&&state.phase==Phase::Flying&&!state.flight.path.empty())
            {
                const double i=std::min(state.flightTime*60,static_cast<double>(state.flight.path.size()-1));
                const size_t a=static_cast<size_t>(i),b=std::min(a+1,state.flight.path.size()-1);
                const float t=static_cast<float>(i-a);
                return state.flight.path[a]+(state.flight.path[b]-state.flight.path[a])*t;
            }
            return state.balls[who].at;
        }

        // ---- Drawing --------------------------------------------------------------------------

        void PlaceCamera()
        {
            using namespace DirectX;
            const auto& c=Course();
            XMFLOAT3 eye{},look{};
            float follow=0.35f;
            auto set=[&](V3 e,V3 l){eye=W(e);look=W(l);};
            if(state.posing>=0)
            {
                // Round her (0: in front), at her chest's height.
                const V3 ball=state.balls[Her].at;
                const float face=state.herYaw;   // her front is (-sin, -cos) of it
                const V3 front{-std::sin(face),0,-std::cos(face)},side{std::cos(face),0,-std::sin(face)};
                const float a=state.poseYaw;
                const V3 toEye=front*std::cos(a)+side*std::sin(a);
                const V3 mid=state.herAt+V3{0,0.8f,0};
                set(mid+toEye*3.6f+V3{0,0.35f,0},mid);
                state.eye=eye;state.look=look;
                state.renderer->SetCamera(XMLoadFloat3(&eye),XMLoadFloat3(&look),XMConvertToRadians(40.0f));
                return;
            }
            const Ball& b=state.balls[state.turn];
            switch(state.phase)
            {
            case Phase::Intro:
            {
                // Over the hole from the tee toward the green.
                const float t=static_cast<float>(std::min(state.phaseTime/4.0,1.0));
                const float z=c.Length()*(0.05f+0.55f*t);
                const V3 from{c.LineX(z)*0.6f,c.Height(c.LineX(z),z)+38-14*t,z-70};
                set(from,c.GreenMiddle());
                follow=0.6f;
                break;
            }
            case Phase::Aim:
            case Phase::Gauge:
            case Phase::HoleOver:
            case Phase::Over:
            {
                if(state.turn==You||state.phase==Phase::HoleOver||state.phase==Phase::Over)
                {
                    const V3 at=state.phase==Phase::HoleOver||state.phase==Phase::Over?c.Pin():b.at;
                    const float aim=state.phase==Phase::HoleOver||state.phase==Phase::Over?PinAim(c.Tee())+Pi*0.85f:state.aim;
                    const bool putt=state.club==Golf::Putter&&state.turn==You;
                    const float a=aim+state.camera.yaw,back=(putt?3.2f:4.6f)*state.camera.zoom;
                    const V3 dir{std::sin(a),0,std::cos(a)};
                    const float up=(putt?1.3f:1.7f)*state.camera.zoom+state.camera.pitch*3;
                    V3 e=at-dir*back+V3{0,up,0};
                    e.y=std::max(e.y,c.Height(e.x,e.z)+0.8f);
                    const float ahead=putt?std::min(Golf::Flat(at,c.Pin()),9.0f):35.0f;
                    V3 l=at+dir*ahead;
                    l.y=c.Height(l.x,l.z)+(putt?0.1f:1.5f);
                    set(e,l);
                }
                else
                {
                    // Her at the address, seen from in front of her and a little toward the target.
                    const V3 front{-std::sin(state.herYaw),0,-std::cos(state.herYaw)};
                    const float aim=state.herYaw+Pi/2;
                    const V3 dir{std::sin(aim),0,std::cos(aim)};
                    const float a=state.camera.yaw;
                    const V3 toEye=front*std::cos(a)+dir*std::sin(a);
                    const V3 mid=state.herAt+V3{0,0.85f,0};
                    set(mid+(toEye*3.9f+dir*1.2f)*state.camera.zoom+V3{0,0.25f+state.camera.pitch*3,0},mid+dir*0.5f);
                }
                break;
            }
            case Phase::Swing:
            {
                const V3 front{-std::sin(state.herYaw),0,-std::cos(state.herYaw)};
                const float aim=state.herYaw+Pi/2;
                const V3 dir{std::sin(aim),0,std::cos(aim)};
                const float a=state.camera.yaw;
                const V3 toEye=front*std::cos(a)+dir*std::sin(a);
                const V3 mid=state.herAt+V3{0,0.85f,0};
                set(mid+(toEye*3.9f+dir*1.2f)*state.camera.zoom+V3{0,0.25f+state.camera.pitch*3,0},mid+dir*0.5f);
                break;
            }
            case Phase::Flying:
            case Phase::Settle:
            {
                // Behind the ball as it flies, a little above; held where it is once it settles.
                // Her swing is seen through for a moment first.
                if(state.turn==Her&&state.flightTime<0.7&&state.phase==Phase::Flying)
                {
                    const V3 front{-std::sin(state.herYaw),0,-std::cos(state.herYaw)};
                    const float aim=state.herYaw+Pi/2;
                    const V3 dir{std::sin(aim),0,std::cos(aim)};
                    const V3 mid=state.herAt+V3{0,0.85f,0};
                    set(mid+front*3.9f*state.camera.zoom+dir*1.2f+V3{0,0.25f,0},mid+dir*0.5f);
                    break;
                }
                const V3 p=BallNow(state.turn);
                const float aim=state.shot.aim;
                const V3 dir{std::sin(aim),0,std::cos(aim)};
                const bool putt=state.shot.club==Golf::Putter;
                const float back=putt?3.5f:9.0f,up=putt?1.4f:3.2f;
                V3 e=p-dir*back+V3{0,up,0};
                e.y=std::max(e.y,c.Height(e.x,e.z)+1.2f);
                set(e,p);
                follow=putt?0.25f:0.18f;
                break;
            }
            }
            if(state.cut){state.eye=eye;state.look=look;state.cut=false;}
            else
            {
                const float k=static_cast<float>(1.0-std::exp(-1.0/60/std::max(0.02f,follow)));
                auto ease=[&](XMFLOAT3& a,const XMFLOAT3& b){a.x+=(b.x-a.x)*k;a.y+=(b.y-a.y)*k;a.z+=(b.z-a.z)*k;};
                ease(state.eye,eye);
                ease(state.look,look);
            }
            state.renderer->SetCamera(XMLoadFloat3(&state.eye),XMLoadFloat3(&state.look),XMConvertToRadians(45.0f));
        }

        // What moves: the balls, their shadows, the flag, the club in her hands, the trail of the
        // ball in the air, your aim's landing mark.
        std::vector<MascotRenderer::GeometryVertex> BuildMoving(MascotModel& model)
        {
            using namespace DirectX;
            const auto& c=Course();
            Mesh m;
            const float white[4]={0.97f,0.97f,0.97f,1},shadow[4]={0.05f,0.08f,0.04f,1};
            for(int who:{You,Her})
            {
                const Ball& b=state.balls[who];
                if(b.holed&&!(who==state.turn&&state.phase==Phase::Flying))continue;
                const V3 p=BallNow(who);
                const float r=Golf::BallRadius*K*1.6f;   // a little large, to be seen
                m.Sphere({p.x*K,p.y*K+r*0.4f,p.z*K},r,white,4,10,6);
                // Its shadow on the ground under it.
                const float g=c.Height(p.x,p.z)*K+0.02f*K;
                const float s=r*(1.2f+std::min(2.0f,(p.y*K-g)/(6*K)));
                for(int i=0;i<10;++i)
                {
                    const float a0=2*Pi*i/10,a1=2*Pi*(i+1)/10;
                    m.Vertex({p.x*K,g,p.z*K},{0,1,0},shadow);
                    m.Vertex({p.x*K+s*std::cos(a1),g,p.z*K+s*std::sin(a1)},{0,1,0},shadow);
                    m.Vertex({p.x*K+s*std::cos(a0),g,p.z*K+s*std::sin(a0)},{0,1,0},shadow);
                }
                // A mark over the other's ball, so it is found (pink hers, blue yours).
                if(!(who==state.turn&&state.phase==Phase::Flying))
                {
                    const float col[4]={who==Her?1.0f:0.35f,who==Her?0.45f:0.65f,who==Her?0.7f:1.0f,Unlit};
                    m.Cylinder(p.x*K,p.z*K,(p.y+0.5f)*K,(p.y+0.8f)*K,0.06f*K,0,col,4,6,false);
                }
            }
            // The ball's trail in the air.
            if(state.phase==Phase::Flying&&!state.flight.path.empty())
            {
                const size_t now=std::min(static_cast<size_t>(state.flightTime*60),state.flight.path.size()-1);
                const size_t from=now>90?now-90:0;
                const float col[4]={state.turn==Her?1.0f:0.6f,state.turn==Her?0.7f:0.85f,1.0f,Unlit};
                const XMVECTOR view=XMVector3Normalize(XMVectorSubtract(XMLoadFloat3(&state.look),XMLoadFloat3(&state.eye)));
                for(size_t i=from;i+3<=now;i+=3)
                {
                    const V3 a=state.flight.path[i],b=state.flight.path[i+3];
                    if(a.y-c.Height(a.x,a.z)<0.15f)continue;
                    const float fade=static_cast<float>(i-from)/std::max<size_t>(1,now-from);
                    const float wdt=(0.01f+0.03f*fade)*K;
                    const XMVECTOR along=XMVectorSet(b.x-a.x,b.y-a.y,b.z-a.z,0);
                    const XMVECTOR side=XMVectorScale(XMVector3Normalize(XMVector3Cross(along,view)),wdt);
                    const XMFLOAT3 A=W(a),B=W(b);
                    XMFLOAT3 s;XMStoreFloat3(&s,side);
                    m.Quad({A.x-s.x,A.y-s.y,A.z-s.z},{B.x-s.x,B.y-s.y,B.z-s.z},{B.x+s.x,B.y+s.y,B.z+s.z},{A.x+s.x,A.y+s.y,A.z+s.z},col);
                    m.Quad({A.x+s.x,A.y+s.y,A.z+s.z},{B.x+s.x,B.y+s.y,B.z+s.z},{B.x-s.x,B.y-s.y,B.z-s.z},{A.x-s.x,A.y-s.y,A.z-s.z},col);
                }
            }
            // The pin and its flag, the flag waving with the wind.
            {
                const V3 p=c.Pin();
                const float pole[4]={0.95f,0.95f,0.92f,1},flag[4]={0.95f,0.25f,0.20f,1};
                const float top=(p.y+2.3f)*K;
                m.Cylinder(p.x*K,p.z*K,p.y*K-0.1f*K,top,0.012f*K,0.012f*K,pole,4,6);
                const float toward=state.wind.toward;
                const V3 d{std::sin(toward),0,std::cos(toward)};
                const float wave=static_cast<float>(state.clock*6);
                const float lenF=0.55f*K*(0.5f+0.5f*std::min(1.0f,state.wind.speed/4)),h=0.38f*K;
                for(int i=0;i<6;++i)
                {
                    const float u0=i/6.0f,u1=(i+1)/6.0f;
                    auto at=[&](float u,float y){
                        const float sway=0.08f*K*std::sin(wave+u*5)*u;
                        return Mesh::V{p.x*K+d.x*lenF*u-d.z*sway,top-y-0.02f*K*u,p.z*K+d.z*lenF*u+d.x*sway};
                    };
                    m.Quad(at(u0,0),at(u1,0),at(u1,h*(1-0.15f*u1)),at(u0,h*(1-0.15f*u0)),flag);
                    m.Quad(at(u0,h*(1-0.15f*u0)),at(u1,h*(1-0.15f*u1)),at(u1,0),at(u0,0),flag);
                }
            }
            // Your aim: a ring where the ball comes down at full power, and a dotted line to it.
            if(state.turn==You&&(state.phase==Phase::Aim||state.phase==Phase::Gauge)&&state.landingKnown&&!state.autoplay)
            {
                const V3 l=state.landing;
                const float y=(c.Height(l.x,l.z)+0.05f)*K;
                const float col[4]={1.0f,0.92f,0.35f,Unlit};
                const float r0=(state.club==Golf::Putter?0.12f:1.6f)*K,r1=r0*1.35f;
                for(int i=0;i<32;++i)
                {
                    const float a0=2*Pi*i/32,a1=2*Pi*(i+1)/32;
                    m.Quad({l.x*K+r1*std::cos(a0),y,l.z*K+r1*std::sin(a0)},{l.x*K+r1*std::cos(a1),y,l.z*K+r1*std::sin(a1)},
                           {l.x*K+r0*std::cos(a1),y,l.z*K+r0*std::sin(a1)},{l.x*K+r0*std::cos(a0),y,l.z*K+r0*std::sin(a0)},col);
                }
                const V3 from=state.balls[You].at;
                const float len=Golf::Flat(from,l);
                const int dots=std::clamp(static_cast<int>(len/(state.club==Golf::Putter?0.25f:3.0f)),2,60);
                for(int i=1;i<dots;++i)
                {
                    const float t=static_cast<float>(i)/dots;
                    const V3 q=from+(l-from)*t;
                    const float qy=(c.Height(q.x,q.z)+0.04f)*K,s=(state.club==Golf::Putter?0.025f:0.25f)*K;
                    m.Quad({q.x*K-s,qy,q.z*K-s},{q.x*K-s,qy,q.z*K+s},{q.x*K+s,qy,q.z*K+s},{q.x*K+s,qy,q.z*K-s},col);
                }
            }
            // The club in her hands: from her grip along the way the swing holds it.
            std::array<float,3> middle{},along{},club{};
            if(state.herSwinging&&model.Grip(0,middle,along)&&model.SwingClub(club))
            {
                const XMMATRIX world=HerWorld(model);
                const XMVECTOR g=XMVector3Transform(XMVectorSet(middle[0],middle[1],middle[2],1),world);
                const XMVECTOR d=XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(club[0],club[1],club[2],0),world));
                const XMVECTOR top=XMVectorSubtract(g,XMVectorScale(d,0.06f*K));
                const XMVECTOR end=XMVectorAdd(g,XMVectorScale(d,state.clubLength));
                const float shaft[4]={0.75f,0.76f,0.80f,1},grip[4]={0.10f,0.10f,0.12f,1},headCol[4]={0.55f,0.56f,0.60f,1},wood[4]={0.12f,0.12f,0.14f,1};
                auto rod=[&](FXMVECTOR a,FXMVECTOR b,float r,const float (&col)[4]){
                    const XMVECTOR ax=XMVector3Normalize(XMVectorSubtract(b,a));
                    XMVECTOR u=XMVector3Cross(ax,XMVectorSet(0,1,0,0));
                    if(XMVectorGetX(XMVector3LengthSq(u))<1e-6f)u=XMVectorSet(1,0,0,0);
                    u=XMVector3Normalize(u);
                    const XMVECTOR v=XMVector3Cross(ax,u);
                    for(int i=0;i<6;++i)
                    {
                        const float a0=2*Pi*i/6,a1=2*Pi*(i+1)/6;
                        const XMVECTOR o0=XMVectorScale(XMVectorAdd(XMVectorScale(u,std::cos(a0)),XMVectorScale(v,std::sin(a0))),r);
                        const XMVECTOR o1=XMVectorScale(XMVectorAdd(XMVectorScale(u,std::cos(a1)),XMVectorScale(v,std::sin(a1))),r);
                        XMFLOAT3 p0,p1,q0,q1;
                        XMStoreFloat3(&p0,XMVectorAdd(a,o0));XMStoreFloat3(&p1,XMVectorAdd(a,o1));
                        XMStoreFloat3(&q0,XMVectorAdd(b,o0));XMStoreFloat3(&q1,XMVectorAdd(b,o1));
                        m.Quad({p0.x,p0.y,p0.z},{p1.x,p1.y,p1.z},{q1.x,q1.y,q1.z},{q0.x,q0.y,q0.z},col);
                        m.Quad({q0.x,q0.y,q0.z},{q1.x,q1.y,q1.z},{p1.x,p1.y,p1.z},{p0.x,p0.y,p0.z},col);
                    }
                };
                const bool putter=state.shot.club==Golf::Putter&&state.phase!=Phase::Aim;
                rod(top,XMVectorAdd(g,XMVectorScale(d,0.2f*K)),0.014f*K,grip);
                rod(XMVectorAdd(g,XMVectorScale(d,0.2f*K)),end,0.007f*K,shaft);
                // The head: a block at the end, its face toward the target.
                const XMVECTOR aim=XMVector3TransformNormal(XMVectorSet(1,0,0,0),world);
                const XMVECTOR toe=XMVector3Normalize(XMVector3Cross(aim,XMVectorSet(0,1,0,0)));
                const float big=putter?0.05f:state.shot.club<=1?0.06f:0.04f;
                rod(XMVectorSubtract(end,XMVectorScale(toe,0.02f*K)),XMVectorAdd(end,XMVectorScale(toe,(putter?0.1f:0.09f)*K)),big*K,state.shot.club<=1&&!putter?wood:headCol);
            }
            return std::move(m.out);
        }

        // ---- The picture over the 3D view ----------------------------------------------------

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
        void Panel(Graphics& g,RectF r,float s,int alpha=160)
        {
            GraphicsPath path;
            RoundRect(path,r,10*s);
            SolidBrush back(Color(static_cast<BYTE>(alpha),14,22,30));
            g.FillPath(&back,&path);
        }

        float ViewYaw(){return std::atan2(state.look.x-state.eye.x,state.look.z-state.eye.z);}

        void DrawHud(Graphics& g,int w,int h,float s)
        {
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAlias);
            const Color white(255,240,244,248),soft(255,176,190,206),gold(255,246,212,120),pink(255,255,150,190),blue(255,140,190,255);
            const auto& c=Course();
            const float W=static_cast<float>(w),H=static_cast<float>(h);
            // The hole, the wind.
            {
                const RectF box(10*s,10*s,250*s,92*s);
                Panel(g,box,s);
                Text(g,Fmt(T(L"{0}/{1}番ホール　パー{2}",L"Hole {0}/{1}   Par {2}"),{std::to_wstring(state.hole+1),std::to_wstring(Holes),std::to_wstring(c.Par())}),
                     RectF(box.X+12*s,box.Y+6*s,box.Width-20*s,24*s),15*s,white,true,StringAlignmentNear);
                Text(g,Fmt(T(L"{0} m",L"{0} m"),{std::to_wstring(static_cast<int>(c.Length()))}),RectF(box.X+12*s,box.Y+30*s,120*s,22*s),13*s,soft,false,StringAlignmentNear);
                // The wind's arrow, as the camera sees it (up: away from the camera).
                const PointF mid(box.X+box.Width-46*s,box.Y+52*s);
                const float a=state.wind.toward-ViewYaw();
                const float r=20*s;
                Pen ring(Color(120,200,210,220),1.5f*s);
                g.DrawEllipse(&ring,mid.X-r-4*s,mid.Y-r-4*s,2*r+8*s,2*r+8*s);
                const PointF tip(mid.X+r*std::sin(a),mid.Y-r*std::cos(a)),tail(mid.X-r*std::sin(a),mid.Y+r*std::cos(a));
                Pen arrow(Color(255,120,200,255),4*s);
                arrow.SetEndCap(LineCapArrowAnchor);
                g.DrawLine(&arrow,tail,tip);
                wchar_t speed[32];
                swprintf_s(speed,L"%.1f m/s",state.wind.speed);
                Text(g,std::wstring(T(L"風 ",L"Wind "))+speed,RectF(box.X+12*s,box.Y+54*s,140*s,24*s),14*s,white,false,StringAlignmentNear);
            }
            // The card: strokes per hole, the total against par.
            {
                const float cw=58*s,rw=150*s,bw=rw+cw*(Holes+1)+20*s,bh=86*s;
                const RectF box(W-bw-10*s,10*s,bw,bh);
                Panel(g,box,s);
                const float x0=box.X+10*s;
                Text(g,T(L"ホール",L"Hole"),RectF(x0,box.Y+4*s,rw,22*s),12*s,soft,false,StringAlignmentNear);
                for(int h2=0;h2<Holes;++h2)
                    Text(g,std::to_wstring(h2+1)+L" (P"+std::to_wstring(state.pars[h2]?state.pars[h2]:0)+L")",RectF(x0+rw+cw*h2,box.Y+4*s,cw,22*s),12*s,soft);
                Text(g,T(L"計",L"Total"),RectF(x0+rw+cw*Holes,box.Y+4*s,cw,22*s),12*s,soft);
                for(int who:{You,Her})
                {
                    const float y=box.Y+(who==You?28:54)*s;
                    Text(g,who==You?std::wstring(T(L"あなた",L"You")):HerName(),RectF(x0,y,rw,24*s),14*s,who==You?blue:pink,true,StringAlignmentNear);
                    for(int h2=0;h2<Holes;++h2)
                    {
                        int n=state.card[who][h2];
                        if(h2==state.hole&&n==0&&state.phase!=Phase::Over)n=state.balls[who].strokes;
                        if(n>0||h2<=state.hole)Text(g,n>0?std::to_wstring(n):L"-",RectF(x0+rw+cw*h2,y,cw,24*s),15*s,white,h2==state.hole);
                    }
                    int total=0;
                    for(int h2=0;h2<Holes;++h2)total+=h2==state.hole&&state.card[who][h2]==0?state.balls[who].strokes:state.card[who][h2];
                    Text(g,std::to_wstring(total),RectF(x0+rw+cw*Holes,y,cw,24*s),15*s,gold,true);
                }
            }
            // The hole from above, bottom right: the balls, your aim.
            if(state.map)
            {
                const float mw=static_cast<float>(state.map->GetWidth()),mh=static_cast<float>(state.map->GetHeight());
                const float k=std::min(1.0f,(H*0.42f)/mh)*s;
                const RectF box(W-mw*k-16*s,H-mh*k-16*s,mw*k,mh*k);
                Panel(g,RectF(box.X-6*s,box.Y-6*s,box.Width+12*s,box.Height+12*s),s,140);
                g.DrawImage(state.map.get(),box);
                auto at=[&](V3 p){return PointF(box.X+(p.x-c.MinX())*state.mapScale*k,box.Y+(c.MaxZ()-p.z)*state.mapScale*k);};
                const PointF pin=at(c.Pin());
                SolidBrush red(Color(255,240,60,60));
                g.FillEllipse(&red,pin.X-3*s,pin.Y-3*s,6*s,6*s);
                if(state.turn==You&&(state.phase==Phase::Aim||state.phase==Phase::Gauge)&&state.landingKnown)
                {
                    Pen aimLine(Color(220,255,236,120),1.5f*s);
                    aimLine.SetDashStyle(DashStyleDash);
                    g.DrawLine(&aimLine,at(state.balls[You].at),at(state.landing));
                }
                for(int who:{Her,You})
                {
                    if(state.balls[who].holed&&!(who==state.turn&&state.phase==Phase::Flying))continue;
                    const PointF p=at(BallNow(who));
                    SolidBrush dot(who==You?Color(255,120,180,255):Color(255,255,130,180));
                    Pen edge(Color(255,255,255,255),1.2f*s);
                    g.FillEllipse(&dot,p.X-4*s,p.Y-4*s,8*s,8*s);
                    g.DrawEllipse(&edge,p.X-4*s,p.Y-4*s,8*s,8*s);
                }
            }
            // Your shot: the club, the lie, how far; the gauge.
            if(state.turn==You&&(state.phase==Phase::Aim||state.phase==Phase::Gauge))
            {
                const Ball& b=state.balls[You];
                const float toPin=Golf::Flat(b.at,c.Pin());
                const RectF box(10*s,H-128*s,300*s,118*s);
                Panel(g,box,s);
                Text(g,Golf::ClubName(state.club),RectF(box.X+12*s,box.Y+6*s,box.Width-20*s,28*s),19*s,gold,true,StringAlignmentNear);
                std::wstring info=std::wstring(Golf::LieName(b.lie))+T(L"　ピンまで ",L"   to the pin ")+std::to_wstring(static_cast<int>(toPin+0.5f))+L" m";
                Text(g,info,RectF(box.X+12*s,box.Y+36*s,box.Width-20*s,22*s),14*s,white,false,StringAlignmentNear);
                std::wstring more;
                if(state.club==Golf::Putter)
                {
                    const float rise=(c.Pin().y-b.at.y)*100;
                    wchar_t t[120];
                    swprintf_s(t,T(L"最大 %.0f m　%ls %.0f cm",L"Max %.0f m   %ls %.0f cm"),Golf::PutterRange(toPin),rise>=0?T(L"上り",L"uphill"):T(L"下り",L"downhill"),std::abs(rise));
                    more=t;
                }
                else
                {
                    const float full=Golf::Flat(b.at,state.landing);
                    more=Fmt(T(L"最大 {0} m（無風）",L"Full carry {0} m (no wind)"),{std::to_wstring(static_cast<int>(full))});
                }
                Text(g,more,RectF(box.X+12*s,box.Y+58*s,box.Width-20*s,22*s),14*s,soft,false,StringAlignmentNear);
                Text(g,T(L"←→ 狙い　↑↓ クラブ　Space/クリック ショット",L"←→ aim   ↑↓ club   Space/click: shot"),RectF(box.X+12*s,box.Y+84*s,box.Width-20*s,22*s),12*s,soft,false,StringAlignmentNear);
                // The gauge: from the strike's line (left) to full power (right).
                const float gw=std::min(W*0.46f,520*s),gh=26*s;
                const RectF bar(W/2-gw/2,H-70*s,gw,gh);
                Panel(g,RectF(bar.X-10*s,bar.Y-26*s,bar.Width+20*s,bar.Height+40*s),s,170);
                auto x=[&](float p){return bar.X+(p+0.25f)/1.25f*bar.Width;};
                LinearGradientBrush fill(RectF(x(0),bar.Y,x(1)-x(0),gh),Color(255,80,190,110),Color(255,240,80,60),LinearGradientModeHorizontal);
                g.FillRectangle(&fill,RectF(x(0),bar.Y,x(1)-x(0),gh));
                SolidBrush miss(Color(255,60,70,90));
                g.FillRectangle(&miss,RectF(bar.X,bar.Y,x(0)-bar.X,gh));
                SolidBrush zone(Color(255,250,250,250));
                g.FillRectangle(&zone,RectF(x(-0.03f),bar.Y-3*s,x(0.03f)-x(-0.03f),gh+6*s));
                Pen tick(Color(140,255,255,255),1*s);
                for(int i=1;i<10;++i)g.DrawLine(&tick,PointF(x(i/10.0f),bar.Y),PointF(x(i/10.0f),bar.Y+gh*0.35f));
                Text(g,L"100%",RectF(x(1)-30*s,bar.Y-24*s,60*s,20*s),11*s,soft);
                if(state.gauge==GaugeState::Falling||state.gauge==GaugeState::Done)
                {
                    SolidBrush set(Color(255,255,230,90));
                    g.FillRectangle(&set,RectF(x(state.gaugePower)-2*s,bar.Y-6*s,4*s,gh+12*s));
                    Text(g,std::to_wstring(static_cast<int>(state.gaugePower*100+0.5f))+L"%",RectF(x(state.gaugePower)-30*s,bar.Y-24*s,60*s,20*s),12*s,gold,true);
                }
                if(state.gauge!=GaugeState::Ready)
                {
                    SolidBrush marker(Color(255,255,255,255));
                    PointF tri[3]={PointF(x(state.gaugeAt),bar.Y+gh+2*s),PointF(x(state.gaugeAt)-7*s,bar.Y+gh+14*s),PointF(x(state.gaugeAt)+7*s,bar.Y+gh+14*s)};
                    g.FillPolygon(&marker,tri,3);
                    Pen line(Color(255,255,255,255),2.5f*s);
                    g.DrawLine(&line,PointF(x(state.gaugeAt),bar.Y-2*s),PointF(x(state.gaugeAt),bar.Y+gh+2*s));
                }
                else Text(g,T(L"クリックでスタート",L"Click to start"),RectF(bar.X,bar.Y,bar.Width,gh),14*s,white,true);
            }
            // Her words beside her head.
            if(!state.words.text.empty())
            {
                const std::wstring& said=state.words.text;
                const float most=std::max(160*s,W*0.32f);
                const WordsBubble bubble(g,said,s,most);
                const float bw=bubble.Width(),bh=bubble.Height();
                const bool beside=state.besideHead.x>=0&&state.besideHead.x<w&&state.headScreen.y>0&&state.headScreen.y<h;
                const float x=std::clamp(beside?static_cast<float>(state.besideHead.x):W/2+60*s,14*s,W-14*s-bw);
                const float headY=beside?static_cast<float>(state.headScreen.y):H*0.3f;
                const float y=std::clamp(headY-bh*0.6f,110*s,std::max(110*s,H-bh-140*s));
                bubble.Draw(g,x,y,std::clamp(headY,y+12*s,y+bh-12*s),ShownWords());
            }
            // Whose turn, when it is hers.
            if(state.turn==Her&&(state.phase==Phase::Aim||state.phase==Phase::Swing))
                Text(g,Fmt(T(L"{0}のショット",L"{0}'s shot"),{HerName()}),RectF(0,H-60*s,W,30*s),18*s,pink,true);
            // The banner.
            if(state.clock<state.bannerUntil&&!state.banner.empty())
            {
                const float bw=std::min(W-40*s,560*s),bh=state.bannerNote.empty()?64*s:96*s;
                const RectF box(W/2-bw/2,H*0.36f-bh/2,bw,bh);
                Panel(g,box,s,205);
                Text(g,state.banner,RectF(box.X,box.Y+8*s,box.Width,state.bannerNote.empty()?box.Height-16*s:48*s),26*s,gold,true);
                if(!state.bannerNote.empty())Text(g,state.bannerNote,RectF(box.X,box.Y+56*s,box.Width,30*s),14*s,white);
            }
            if(state.askingQuit)
            {
                const float bw=std::min(W-40*s,420*s),bh=120*s;
                const RectF box(W/2-bw/2,H*0.42f-bh/2,bw,bh);
                Panel(g,box,s,215);
                Text(g,T(L"やめますか？",L"Stop playing?"),RectF(box.X,box.Y+8*s,box.Width,48*s),26*s,gold,true);
                const float gap=16*s,bw2=(box.Width-3*gap)/2;
                const std::wstring labels[2]={T(L"やめる（Y）",L"Stop (Y)"),T(L"続ける（N）",L"Go on (N)")};
                for(int i=0;i<2;++i)
                {
                    const RectF button(box.X+gap+i*(bw2+gap),box.Y+62*s,bw2,44*s);
                    state.askButtons[i]=button;
                    GraphicsPath shape;
                    RoundRect(shape,button,10*s);
                    SolidBrush face(i==0?Color(235,150,46,52):Color(235,46,92,150));
                    g.FillPath(&face,&shape);
                    Text(g,labels[i],RectF(button.X,button.Y+10*s,button.Width,26*s),17*s,white,true);
                }
            }
        }

        void UpdateOverlay()
        {
            RECT client{};
            GetClientRect(state.window,&client);
            const int w=std::max(1L,client.right),h=std::max(1L,client.bottom);
            const bool moving=state.phase==Phase::Gauge||state.phase==Phase::Flying;
            const ULONGLONG now=GetTickCount64();
            std::wstring shown=std::to_wstring(w)+L"x"+std::to_wstring(h)+L" "+std::to_wstring(static_cast<int>(state.phase))+L" "+std::to_wstring(state.turn)+L" "+
                               std::to_wstring(state.club)+L" "+std::to_wstring(static_cast<int>(state.aim*200))+L" "+std::to_wstring(static_cast<int>(state.gauge))+
                               L" "+std::to_wstring(static_cast<int>(ViewYaw()*30))+L"|"+ShownWords()+L"|"+(state.clock<state.bannerUntil?state.banner+state.bannerNote:L"")+
                               L"|"+std::to_wstring(state.besideHead.x/12)+L","+std::to_wstring(state.headScreen.y/12)+(state.askingQuit?L"a":L"")+
                               std::to_wstring(state.balls[0].strokes)+std::to_wstring(state.balls[1].strokes);
            if(moving)shown+=L"|"+std::to_wstring(now/40);
            if(shown==state.hudShown)return;
            if(moving&&now-state.hudAt<33)return;
            state.hudShown=shown;
            state.hudAt=now;
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

        // ---- The window ---------------------------------------------------------------------

        LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam);

        LRESULT CALLBACK ViewProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
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
                state.dragging=1;
                state.dragFrom={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
                SetCapture(hwnd);
                return 0;
            case WM_MOUSEMOVE:
                if(state.dragging)
                {
                    const float dx=static_cast<float>(GET_X_LPARAM(lParam)-state.dragFrom.x),dy=static_cast<float>(GET_Y_LPARAM(lParam)-state.dragFrom.y);
                    state.dragFrom={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
                    const float k=1/Scale();
                    c.yaw+=dx*k*0.008f;
                    c.pitch=std::clamp(c.pitch+dy*k*0.004f,-0.3f,1.5f);
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
                c.zoom=std::clamp(c.zoom*std::pow(0.9f,GET_WHEEL_DELTA_WPARAM(wParam)/120.0f),0.4f,4.0f);
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
                Log(L"golf: resize: %s",error.c_str());
            state.hudShown.clear();
        }

        void ReleaseView()
        {
            if(state.renderer){state.renderer->WaitIdle();state.renderer.reset();Log(L"golf: 3D view released");}
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
            renderer->SetBackground(0.72f,0.80f,0.88f,1);
            renderer->SetDepthRange(4,60000);
            std::wstring error;
            if(!renderer->Initialize(state.view,w,h,state.her.light?1:state.her.msaa,false,error)||
               !renderer->LoadModel(*state.her.model,state.her.toonDir,state.her.light?512:state.her.maxTexture,error))
            {
                Log(L"golf: no 3D view (%s)",error.c_str());
                renderer->WaitIdle();
                DestroyWindow(state.view);
                state.view=nullptr;
                return false;
            }
            renderer->SetLight(35,50);
            MeasureHer(*state.her.model);
            state.renderer=std::move(renderer);
            state.renderer->SetGeometry({});
            state.camera={};
            state.dragging=0;
            state.hudShown.clear();
            Log(L"golf: 3D view %ux%u on %s",w,h,state.renderer->AdapterName().c_str());
            return true;
        }

        void Finish()
        {
            if(!state.window)return;
            KillTimer(state.window,TimerId);
            state.voice.Stop();
            if(state.thinking.valid())state.thinking.wait();
            if(state.afterword.empty()&&state.phase!=Phase::Over&&state.shots>0)
            {
                state.afterword=Fmt(T(L"いま、話し相手とゴルフをしていたが、途中でやめてデスクトップに戻ってきたところ（{0}番ホールまで、あなた{1}打、相手{2}打だった）。そのことを話し相手にひと言（1〜2文）",
                                      L"You were playing golf with them, but they stopped before the end and you have come back to the desktop (up to hole {0}, you had taken {1} strokes, they {2}). Say a word about it to them (one or two sentences)"),
                                    {std::to_wstring(state.hole+1),std::to_wstring(Total(Her)+state.balls[Her].strokes),std::to_wstring(Total(You)+state.balls[You].strokes)});
                state.afterwordLine=T(L"また今度、続きを回ろうね。",L"Let's finish the round another time.");
            }
            if(MascotModel* model=state.her.model)
            {
                model->Swing(false,0,1);
                model->SetExpression(Expression::Neutral,false);
                model->SetAutoGestures(true);
            }
            auto onEnd=std::move(state.onEnd);
            DestroyWindow(state.window);
            if(onEnd)onEnd();
        }

        LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            switch(message)
            {
            case WM_TIMER:
                return 0;
            case WM_KEYDOWN:
                if(state.askingQuit)
                {
                    if(wParam=='Y'){Finish();return 0;}
                    if(wParam=='N'||wParam==VK_ESCAPE)state.askingQuit=false;
                    return 0;
                }
                if(wParam==VK_ESCAPE)
                {
                    if(state.phase==Phase::Over)Finish();
                    else state.askingQuit=true;
                }
                else if(wParam==VK_RETURN&&state.phase==Phase::Over){state.firstSeed+=104729u;NewRound();}
                else if(wParam==VK_F1)state.camera={};
                else if(wParam==VK_SPACE&&!(lParam&0x40000000))GaugeClick();
                else if(state.turn==You&&state.phase==Phase::Aim&&state.gauge==GaugeState::Ready)
                {
                    if(wParam==VK_LEFT)state.aimKeys=-1;
                    else if(wParam==VK_RIGHT)state.aimKeys=1;
                    else if(wParam==VK_UP||wParam==VK_DOWN)
                    {
                        const auto clubs=Golf::ClubsFor(state.balls[You].lie,Golf::Flat(state.balls[You].at,Course().Pin()));
                        auto it=std::find(clubs.begin(),clubs.end(),state.club);
                        int i=it==clubs.end()?0:static_cast<int>(it-clubs.begin());
                        i=std::clamp(i+(wParam==VK_DOWN?1:-1),0,static_cast<int>(clubs.size())-1);
                        state.club=clubs[static_cast<size_t>(i)];
                        UpdateLanding();
                    }
                }
                return 0;
            case WM_KEYUP:
                if((wParam==VK_LEFT&&state.aimKeys<0)||(wParam==VK_RIGHT&&state.aimKeys>0))state.aimKeys=0;
                return 0;
            case WM_KILLFOCUS:
                state.aimKeys=0;
                break;
            case WM_LBUTTONDOWN:
                if(state.askingQuit)
                {
                    const float x=static_cast<float>(GET_X_LPARAM(lParam)),y=static_cast<float>(GET_Y_LPARAM(lParam));
                    if(state.askButtons[0].Contains(x,y)){Finish();return 0;}
                    if(state.askButtons[1].Contains(x,y))state.askingQuit=false;
                    return 0;
                }
                GaugeClick();
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
            case WM_SIZE:
                PlaceView();
                return 0;
            case WM_CLOSE:
                Finish();
                return 0;
            case WM_DESTROY:
                state.window=nullptr;
                ReleaseView();
                return 0;
            }
            return DefWindowProcW(hwnd,message,wParam,lParam);
        }
    }

    void ShowGolfWindow(HWND owner,const GolfOpponent& her,std::function<void()> onEnd)
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
        state.lines.Load(L"GOLFLINES",L"golf",her.ownLinesFile,her.linesFile,her.styleKey,English(),{L"ゴルフ",L"golf",L"",L""});
        state.voice.Stop();
        state.prefetching=false;
        state.words={};
        state.words.told=state.lines.Report();
        state.log.clear();
        state.shots=0;
        state.frames=0;
        state.clock=0;
        state.posing=-1;
        if(!her.linesFile.empty())ShowLinesProblem(owner,state.lines);
        Prefetch();

        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&info);
        const RECT work=info.rcWork;
        const int dpi=static_cast<int>(GetDpiForSystem());
        const int h=std::min(static_cast<int>(work.bottom-work.top)*9/10,MulDiv(820,dpi,96));
        const int w=std::min(static_cast<int>(work.right-work.left)*9/10,h*16/10);
        const std::wstring title=std::wstring(T(L"ゴルフ",L"Golf"))+L" - "+HerName();
        state.window=CreateWindowExW(0,ClassName,title.c_str(),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                                     work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,
                                     nullptr,nullptr,instance,nullptr);
        if(!CreateView())
        {
            MessageBoxW(state.window,T(L"ゴルフは3D表示でしか遊べません（3D表示を作れませんでした）。",L"Golf can only be played in 3D, and the 3D view could not be made."),title.c_str(),MB_ICONINFORMATION);
            auto end=std::move(state.onEnd);
            DestroyWindow(state.window);
            if(end)end();
            return;
        }
        PlaceView();
        NewRound();
        SetTimer(state.window,TimerId,100,nullptr);
        ShowWindow(state.window,SW_SHOWNORMAL);
        SetForegroundWindow(state.window);
    }

    bool GolfWindowOpen(){return state.window!=nullptr;}
    HWND GolfWindowHandle(){return state.window;}
    bool GolfWindowRect(RECT& r){return state.window&&IsWindowVisible(state.window)&&!IsIconic(state.window)&&GetWindowRect(state.window,&r);}

    void TickGolfWindow(MascotModel& model,double dt)
    {
        if(!state.window||!state.renderer||!state.course)return;
        using namespace DirectX;
        dt=std::min(dt,0.1);
        Step(dt);
        // Her: swinging at her ball (its stage by the clock), or watching yours.
        if(state.posing>=0)
        {
            PlaceToSwing(state.balls[Her].at,PinAim(state.balls[Her].at));
            model.Swing(true,state.posing,1);
        }
        else if(state.turn==Her&&(state.phase==Phase::Aim||state.phase==Phase::Swing||state.phase==Phase::Flying||state.phase==Phase::Settle))
        {
            const float stage=state.phase==Phase::Aim?0.0f:SwingStage(state.swingTime,Address);
            const bool hold=state.swingTime<Address+Back+Top+Down+Through+HoldFinish||state.phase==Phase::Aim;
            model.Swing(hold,stage,state.swingPower);
            if(!hold)state.herSwinging=false;   // the club put down
        }
        else
        {
            model.Swing(false,0,1);
            if(state.herSwinging&&state.phase!=Phase::Settle)
            {
                // Off to watch your ball, once she has put her club down.
                const Ball& yours=state.balls[You];
                PlaceToWatch(yours.at,state.turn==You?state.aim:PinAim(yours.at));
            }
        }
        // Her eyes: on the ball (in flight, the one flying), or on the camera when she talks.
        {
            const bool talking=!state.words.text.empty();
            V3 target=BallNow(state.turn);
            if(state.herSwinging&&state.phase!=Phase::Flying)target=state.balls[Her].at;
            const XMFLOAT3 at=talking&&!state.herSwinging?state.eye:W(target);
            model.LookToward(ToModel(model,at),true);
        }
        if(state.moved){state.moved=false;model.ResetPhysics();}
        state.renderer->SetMovingGeometry(BuildMoving(model));
        state.renderer->SetModelTransform(HerWorld(model));
        PlaceCamera();
        if(std::array<float,3> head{};model.HeadPosition(head))
        {
            const XMVECTOR at=XMVector3Transform(XMVectorSet(head[0],head[1],head[2],1),HerWorld(model));
            const XMVECTOR right=XMVector3Normalize(XMVector3Cross(XMVectorSet(0,1,0,0),XMVectorSubtract(XMLoadFloat3(&state.look),XMLoadFloat3(&state.eye))));
            const XMVECTOR beside=XMVectorAdd(at,XMVectorScale(right,0.3f*state.upper));
            state.headScreen=state.renderer->Project({XMVectorGetX(at),XMVectorGetY(at),XMVectorGetZ(at)});
            state.besideHead=state.renderer->Project({XMVectorGetX(beside),XMVectorGetY(beside),XMVectorGetZ(beside)});
            // Behind the camera: not shown.
            const XMVECTOR forward=XMVectorSubtract(XMLoadFloat3(&state.look),XMLoadFloat3(&state.eye));
            if(XMVectorGetX(XMVector3Dot(forward,XMVectorSubtract(at,XMLoadFloat3(&state.eye))))<0)state.headScreen=state.besideHead={-1,-1};
        }
        state.voice.Tick(dt);
        if(state.voice.Playing())model.SetMouth(state.voice.Mouth(),true);
        else model.SetMouth({},false);
        UpdateOverlay();
        state.renderer->Render(model,state.her.edgeScale);
        ++state.frames;
    }

    void GolfLines(const std::wstring& answer)
    {
        state.prefetching=false;
        if(!state.window)return;
        state.lines.Answered(answer);
        Prefetch();
    }

    bool GolfAfterword(std::wstring& situation,std::wstring& fallback)
    {
        if(state.afterword.empty())return false;
        situation=std::move(state.afterword);
        fallback=std::move(state.afterwordLine);
        state.afterword.clear();
        state.afterwordLine.clear();
        return true;
    }

    void PlayGolfForTest(){state.autoplay=true;}
    bool SaveGolfView(const std::wstring& path){return state.renderer&&state.renderer->SaveLastFrame(path);}
    void SetGolfSwingPose(float stage,float yaw){state.posing=stage;state.poseYaw=yaw;}
    void SetGolfSeed(std::uint32_t seed){state.firstSeed=seed;}

    std::wstring DescribeGolf()
    {
        if(!state.course)return L"";
        std::wstring text;
        wchar_t line[300];
        const char* phases[]={"intro","aim","gauge","swing","flying","settle","hole over","over"};
        swprintf_s(line,L"golf: hole %d par %d (seed %u), phase %hs, turn %ls, you %d her %d this hole, totals you %d her %d, frames %d, shots %d\n",state.hole+1,Course().Par(),Course().Seed(),
                   phases[static_cast<int>(state.phase)],state.turn==You?L"you":L"her",state.balls[You].strokes,state.balls[Her].strokes,Total(You),Total(Her),state.frames,state.shots);
        text+=line;
        swprintf_s(line,L"golf: her hips %.1f upper %.1f, club %.2f m, ball %.2f,%.2f from her (m); at %.1f,%.1f yaw %.0f, swinging %d\n",state.hips,state.upper,state.clubLength/K,state.head[0]/K,state.head[2]/K,
                   state.herAt.x,state.herAt.z,state.herYaw/Deg,state.herSwinging?1:0);
        text+=line;
        text+=L"golf play:\n"+state.log;
        if(!state.words.told.empty())text+=L"golf words:\n"+state.words.told;
        return text;
    }
}
