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
#include <shlwapi.h>
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
#pragma comment(lib,"shlwapi.lib")

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
        constexpr float BeamHigh=25;   // where the pin's distance is written on its beam, metres up
        constexpr float BeamSky=700;   // how high the beam goes (into the sky's dome)
        float FovY(){return 45.0f*Deg;}   // the camera's (the gauge in front of it is placed by it)

        enum class Phase {Intro,Aim,Gauge,Swing,Flying,Settle,HoleOver,Over};
        // A drop of water thrown up (metres, metres a second, seconds).
        struct Drop {V3 at,v;float age=0,life=1,size=0.05f;};
        // The gauge: waiting; the marker running out for the power (and back: no click by the
        // time it is home again lets the shot go); running back past the line for the strike
        // (not for the putter); done.
        enum class GaugeState {Ready,Power,Impact,Done};
        constexpr double PowerSeconds=1.7;    // the marker out to full power
        constexpr double ImpactSeconds=1.4;   // back from full power to the line
        constexpr float Pink=0.09f;           // the curving zone's half width either side of the line (gauge widths)

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
            std::shared_ptr<const Golf::Ground> ground;
            int clubhouse=1;                 // this round's (1-4)
            bool sceneryBuilt=false;
            float greenColour[3]{0.4f,0.65f,0.3f};
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
            float gaugeWay=1;           // the marker going out (+1) or back (-1) while the power is chosen
            float spotX=0,spotY=0;      // where on the ball you strike (Ctrl and the arrow keys)
            float spotShownX=0,spotShownY=0;   // where its mark is drawn, easing after it
            bool overhead=false;        // 0: the aim's circle seen from above
            bool help=false;            // Tab: the keys shown
            // Diagnostics (--golf-gauge): your gauge held at power, the marker at; flags: help,
            // overhead, putter, spot.
            float frozenPower=-1,frozenAt=0;
            std::wstring frozenFlags;
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
            size_t treesShown=0;   // trees drawn this frame
            bool justShot=false;   // the shot in the air was struck square (its trail glitters)
            bool splashed=false;   // the ball in the air has fallen into the pond
            std::vector<MascotRenderer::Ripple> ripples;   // on the pond (world units)
            std::vector<Drop> drops;
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
            POINT beamTop{-1,-1};   // the top of the pin's beam on the screen (x < 0: not seen)
            int frames=0,shots=0;
            std::wstring log;     // diagnostics: what was played
            bool askingQuit=false,overTold=false;
            RectF askButtons[2];
        } state;

        void Splash(V3 p);   // (below)

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

        // The geometry's look for the course (GeometryPS: look 6, its kind plus 10 in minus uv.z).
        constexpr float CourseLook=0.12f,Unlit=0.25f;   // GeometryPS: look 6, the course's kinds being 10 on
        struct Mesh
        {
            std::vector<MascotRenderer::GeometryVertex> out;
            using V=std::array<float,3>;
            void Vertex(const V& p,const V& n,const float (&c)[4],float kind=4,const std::array<float,4>& weights={})
            {
                MascotRenderer::GeometryVertex v{{p[0],p[1],p[2]},{n[0],n[1],n[2]},{c[0],c[1],c[2],c[3]}};
                v.wall[0]=weights[0];v.wall[1]=weights[1];v.wall[2]=0;v.wall[3]=weights[2];
                v.uv[0]=weights[3];v.uv[1]=0;v.uv[2]=c[3]==CourseLook?-(10+kind):0;v.uv[3]=0;
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
            // One with its normal given (lit the same whichever way it waves).
            void QuadN(const V& a,const V& b,const V& c,const V& d,const V& n,const float (&col)[4])
            {
                for(const V* q:{&a,&b,&c,&a,&c,&d})Vertex(*q,n,col);
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

        // A picture of the course's (a JPEG resource of type GOLFTEX), with its mipmaps.
        bool LoadPicture(const std::string& name,ImageRgba8& image)
        {
            const std::wstring wide(name.begin(),name.end());
            const HRSRC info=FindResourceW(nullptr,wide.c_str(),L"GOLFTEX");
            const HGLOBAL data=info?LoadResource(nullptr,info):nullptr;
            if(!data)return false;
            IStream* stream=SHCreateMemStream(static_cast<const BYTE*>(LockResource(data)),SizeofResource(nullptr,info));
            if(!stream)return false;
            bool ok=false;
            {
                Bitmap bitmap(stream);
                const UINT w=bitmap.GetWidth(),h=bitmap.GetHeight();
                BitmapData bits{};
                Rect whole(0,0,static_cast<INT>(w),static_cast<INT>(h));
                if(w&&h&&bitmap.LockBits(&whole,ImageLockModeRead,PixelFormat32bppARGB,&bits)==Ok)
                {
                    image={};
                    image.levels.resize(1);
                    auto& level=image.levels[0];
                    level.width=w;level.height=h;
                    level.pixels.resize(static_cast<size_t>(w)*h*4);
                    for(UINT y=0;y<h;++y)
                    {
                        const BYTE* from=static_cast<const BYTE*>(bits.Scan0)+static_cast<size_t>(y)*bits.Stride;
                        BYTE* to=level.pixels.data()+static_cast<size_t>(y)*w*4;
                        for(UINT x=0;x<w;++x,from+=4,to+=4){to[0]=from[2];to[1]=from[1];to[2]=from[0];to[3]=from[3];}
                    }
                    bitmap.UnlockBits(&bits);
                    GenerateMips(image);
                    ok=true;
                }
            }
            stream->Release();
            return ok;
        }

        // The course, its sky and the clubhouse of this round (1-4), as the renderer's
        // scenery: each material a stretch, its picture tiled over it (GeometryPS: 2 in uv.z),
        // under the course's look (its haze); the sky unlit; the pond's water with its waves.
        bool BuildScenery(int clubhouse)
        {
            std::vector<MascotRenderer::GeometryVertex> vertices;
            std::vector<MascotRenderer::SceneryDraw> draws;
            std::vector<ImageRgba8> pictures;
            std::vector<std::string> named;
            bool all=true;
            auto add=[&](const wchar_t* resource,XMFLOAT3 offset,float yaw=0,float scale=1,int group=-1){
                const float turnC=std::cos(yaw),turnS=std::sin(yaw);
                Golf::SceneModel m;
                if(!Golf::LoadScene(resource,m)){all=false;Log(L"golf: no model %s",resource);return;}
                for(const auto& mt:m.materials)
                {
                    if(mt.kind==9||mt.count==0)continue;   // the model's own flag and cup: ours are drawn
                    int picture=-1;
                    if(mt.texture>=0&&static_cast<size_t>(mt.texture)<m.textures.size())
                    {
                        const std::string& name=m.textures[static_cast<size_t>(mt.texture)];
                        const auto it=std::find(named.begin(),named.end(),name);
                        if(it!=named.end())picture=static_cast<int>(it-named.begin());
                        else if(pictures.size()<MascotRenderer::MaxSceneryTextures)
                        {
                            ImageRgba8 image;
                            if(LoadPicture(name,image))
                            {
                                picture=static_cast<int>(pictures.size());
                                pictures.push_back(std::move(image));
                                named.push_back(name);
                                // The green's colour, for filling the model's own cup when the pin is elsewhere.
                                if(name=="TURF_GREEN")
                                {
                                    const auto& l=pictures.back().levels.back();
                                    for(int i=0;i<3;++i)state.greenColour[i]=l.pixels[static_cast<size_t>(i)]/255.0f;
                                }
                            }
                            else{all=false;Log(L"golf: no picture %S",name.c_str());}
                        }
                    }
                    const bool sky=mt.kind==10,water=mt.kind==8;
                    float c[4]={mt.rgb[0],mt.rgb[1],mt.rgb[2],sky?Unlit:CourseLook};
                    if(sky)c[0]=c[1]=c[2]=1;
                    if(water){c[0]=0.07f;c[1]=0.22f;c[2]=0.25f;}   // the water's own colour, under what it mirrors
                    const bool grass=mt.kind==1||(mt.kind>=2&&mt.kind<=5)||mt.kind==11;
                    if(mt.kind==3)for(int i=0;i<3;++i)state.greenColour[i]=mt.rgb[i];   // for filling the model's own cup
                    MascotRenderer::SceneryDraw d;
                    d.start=static_cast<UINT>(vertices.size());
                    d.texture=picture;
                    d.group=group;
                    for(std::uint32_t i=mt.first;i<mt.first+mt.count;++i)
                    {
                        const float* v=&m.vertices[static_cast<size_t>(m.indices[i])*8];
                        // Turned about the up axis and sized (the trees), then put in place.
                        const float px=v[0]*turnC+v[2]*turnS,pz=-v[0]*turnS+v[2]*turnC,nx=v[3]*turnC+v[5]*turnS,nz=-v[3]*turnS+v[5]*turnC;
                        MascotRenderer::GeometryVertex g{{px*scale+offset.x,v[1]*scale+offset.y,pz*scale+offset.z},{nx,v[4],nz},{c[0],c[1],c[2],c[3]}};
                        g.wall[0]=g.wall[1]=g.wall[2]=g.wall[3]=0;
                        g.uv[0]=v[6];g.uv[1]=v[7];g.uv[3]=0;
                        if(picture>=0)g.uv[2]=2;
                        else if(water){g.uv[0]=g.uv[1]=0;g.uv[2]=-15;}   // GeometryPS's CourseWater
                        else if(grass)
                        {
                            // The grass drawn by GeometryPS's CourseGround: mown stripes on the
                            // fairway, faint squares on the green, fine on the tee and fringe.
                            g.wall[0]=mt.kind==2?1.0f:0.0f;
                            g.wall[1]=mt.kind==3?1.0f:0.0f;
                            g.uv[0]=0;g.uv[1]=mt.kind==4||mt.kind==5?1.0f:0.0f;g.uv[2]=-11;
                        }
                        else{g.uv[0]=g.uv[1]=0;g.uv[2]=sky?0.0f:-14.0f;}
                        vertices.push_back(g);
                    }
                    d.count=static_cast<UINT>(vertices.size())-d.start;
                    draws.push_back(d);
                }
            };
            add(L"COURSE",{0,0,0});
            add(L"SKY",{0,0,0});
            const V3 house=Course().Clubhouse();
            const std::wstring name=L"CLUBHOUSE"+std::to_wstring(std::clamp(clubhouse,1,4));
            add(name.c_str(),W(house));
            // The wood round the course: each of the five trees once, drawn at its places
            // (PlaceTrees, every frame).
            for(int k=0;k<Golf::TreeKinds;++k)
            {
                const std::wstring tree=L"TREE"+std::to_wstring(k+1);
                add(tree.c_str(),{0,0,0},0,1,k);
            }
            Log(L"golf: %zu trees in the wood",Course().Trees().size());
            const bool ok=state.renderer->SetScenery(vertices,draws,pictures);
            Log(L"golf: scenery %zu triangles in %zu stretches, %zu pictures, clubhouse %d%s",vertices.size()/3,draws.size(),pictures.size(),clubhouse,ok&&all?L"":L" (something missing)");
            return ok&&all;
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

        // Your aim's circle: where the ball comes down struck at full power with this club,
        // as on level ground with no wind (the spot as set); for the putter, where full power
        // rolls to on a level green.
        void UpdateLanding()
        {
            const Ball& b=state.balls[You];
            const V3 dir{std::sin(state.aim),0,std::cos(state.aim)},side{std::cos(state.aim),0,-std::sin(state.aim)};
            if(state.club==Golf::Putter)
                state.landing=b.at+dir*Golf::PutterRange(Golf::Flat(b.at,Course().Pin()),Course().Pin().y-b.at.y);
            else
            {
                const V3 l=Golf::FlatLanding(state.club,state.spotX,state.spotY);
                state.landing=b.at+dir*l.z+side*l.x;
            }
            state.landing.y=Course().Height(state.landing.x,state.landing.z);
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
            // The same hole each time, the pin moved: first in the model's own cup.
            state.course=std::make_shared<const Golf::Course>(state.ground,state.hole==0?0u:state.firstSeed*7919u+static_cast<std::uint32_t>(state.hole));
            const int par=Course().Par();
            state.pars[state.hole]=par;
            std::uniform_real_distribution<float> u(0,1);
            state.wind={u(state.random)*6.5f,u(state.random)*2*Pi};
            for(Ball& b:state.balls){b={};b.at=Course().Tee();b.lie=Lie::Tee;}
            state.turn=You;
            state.club=Golf::Driver;
            state.aim=PinAim(Course().Tee())*0.4f;
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
            state.pars.fill(3);
            state.hole=0;
            state.started=false;
            state.overTold=false;
            state.askingQuit=false;
            state.afterword.clear();
            state.afterwordLine.clear();
            state.words.text.clear();
            if(!state.firstSeed)state.firstSeed=static_cast<std::uint32_t>(GetTickCount64()*2654435761ull);
            StartHole();
            // A clubhouse of the four, this round's.
            const int house=1+static_cast<int>((state.firstSeed/7)%4);
            if(state.renderer&&(house!=state.clubhouse||!state.sceneryBuilt))
            {
                state.clubhouse=house;
                state.sceneryBuilt=true;
                BuildScenery(house);
            }
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
                state.spotX=state.spotY=0;
                state.overhead=false;
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
            state.justShot=state.shot.club!=Golf::Putter&&std::abs(state.shot.accuracy)<0.08f;
            state.splashed=false;
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
                // The score's name, large; who and how many strokes under it.
                const std::wstring score=n==1?T(L"ホールインワン！",L"Hole in one!"):n<=par-3?T(L"アルバトロス！",L"Albatross!"):n==par-2?T(L"イーグル！",L"Eagle!"):
                                         n==par-1?T(L"バーディー！",L"Birdie!"):n==par?T(L"パー",L"Par"):n==par+1?T(L"ボギー",L"Bogey"):n==par+2?T(L"ダブルボギー",L"Double bogey"):
                                         n==par+3?T(L"トリプルボギー",L"Triple bogey"):Fmt(T(L"+{0}",L"+{0}"),{std::to_wstring(n-par)});
                Banner(score,(hers?name+L"  ":std::wstring(T(L"あなた  ",L"You  ")))+Fmt(T(L"{0}打",L"{0} strokes"),{std::to_wstring(n)}),2.6);
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

        // The strike's white zone (half width, in gauge widths): narrow for the long clubs, wide
        // for the short ones.
        float JustWidth(int club)
        {
            static constexpr float widths[Golf::ClubCount]={0.010f,0.012f,0.015f,0.019f,0.023f,0.027f,0.032f,0.04f};
            return widths[std::clamp(club,0,Golf::ClubCount-1)];
        }
        // The strike from where the marker was stopped: square in the white, curving more the
        // further into the pink (+ early: right), far off beyond it.
        float AccuracyAt(float at,int club)
        {
            const float w=JustWidth(club),a=std::abs(at),sign=at>=0?1.0f:-1.0f;
            if(a<=w)return 0;
            if(a<=Pink)return sign*(0.15f+0.85f*(a-w)/(Pink-w));
            return sign*3.0f;
        }

        // The gauge: clicked (or Space).
        void GaugeClick()
        {
            if(state.phase!=Phase::Aim&&state.phase!=Phase::Gauge)return;
            if(state.turn!=You||state.askingQuit||state.autoplay)return;
            switch(state.gauge)
            {
            case GaugeState::Ready:
                state.gauge=GaugeState::Power;
                state.gaugeAt=0;
                state.gaugeWay=1;
                state.overhead=false;
                state.phase=Phase::Gauge;
                break;
            case GaugeState::Power:
                state.gaugePower=std::clamp(state.gaugeAt,0.02f,1.0f);
                // The putter's stroke is its power alone.
                if(state.club==Golf::Putter){state.gaugeAccuracy=0;state.gauge=GaugeState::Done;}
                else state.gauge=GaugeState::Impact;
                break;
            case GaugeState::Impact:
                state.gaugeAccuracy=AccuracyAt(state.gaugeAt,state.club);
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
            state.shot.range=Golf::PutterRange(Golf::Flat(b.at,Course().Pin()),Course().Pin().y-b.at.y);
            if(state.club!=Golf::Putter){state.shot.spotX=state.spotX;state.shot.spotY=state.spotY;}
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
            // (Once she is there to be seen: not over the look across the hole.)
            if(!state.started&&(!state.prefetching||state.clock>4)&&state.phase!=Phase::Intro)
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
                    // The spot on the ball, moved smoothly while Ctrl and an arrow are held; up or
                    // down alone also brings it back to the middle across.
                    if(state.gauge==GaugeState::Ready&&state.phase==Phase::Aim&&state.club!=Golf::Putter&&GetKeyState(VK_CONTROL)<0&&!state.autoplay)
                    {
                        const float across=(GetKeyState(VK_RIGHT)<0?1.0f:0.0f)-(GetKeyState(VK_LEFT)<0?1.0f:0.0f);
                        const float upDown=(GetKeyState(VK_UP)<0?1.0f:0.0f)-(GetKeyState(VK_DOWN)<0?1.0f:0.0f);
                        if(across!=0||upDown!=0)
                        {
                            constexpr float Speed=1.1f;   // ball radii a second
                            const float step=Speed*static_cast<float>(dt);
                            state.spotX+=across*step;
                            state.spotY+=upDown*step;
                            if(across==0)state.spotX=std::abs(state.spotX)<=step?0:state.spotX-std::copysign(step,state.spotX);
                            const float r=std::hypot(state.spotX,state.spotY);
                            if(r>1){state.spotX/=r;state.spotY/=r;}
                            UpdateLanding();
                        }
                    }
                    if(state.aimKeys!=0&&state.gauge==GaugeState::Ready&&GetKeyState(VK_CONTROL)>=0)
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
                    if(state.frozenPower>=0&&state.phaseTime>0.5)
                    {
                        const std::wstring& f=state.frozenFlags;
                        if(f.find(L"putter")!=std::wstring::npos&&state.club!=Golf::Putter){state.club=Golf::Putter;UpdateLanding();}
                        if(f.find(L"spot")!=std::wstring::npos&&state.spotY==0){state.spotX=-0.5f;state.spotY=-0.5f;UpdateLanding();}
                        state.help=f.find(L"help")!=std::wstring::npos;
                        state.overhead=f.find(L"overhead")!=std::wstring::npos;
                        state.gaugePower=state.frozenPower;
                        state.gaugeAt=state.frozenAt;
                        state.gauge=state.frozenPower>0?GaugeState::Impact:GaugeState::Power;
                        state.phase=Phase::Gauge;
                        break;
                    }
                    if(state.gauge==GaugeState::Power)
                    {
                        // Out to full power and back; home again with no click: no shot.
                        state.gaugeAt+=state.gaugeWay*static_cast<float>(dt/PowerSeconds);
                        if(state.gaugeAt>=1){state.gaugeAt=2-state.gaugeAt;state.gaugeWay=-1;}
                        if(state.gaugeAt<=0&&state.gaugeWay<0){state.gaugeAt=0;state.gauge=GaugeState::Ready;state.phase=Phase::Aim;}
                    }
                    else if(state.gauge==GaugeState::Impact)
                    {
                        state.gaugeAt-=static_cast<float>(dt/ImpactSeconds);
                        if(state.gaugeAt<-Pink-0.08f){state.gaugeAccuracy=-3.0f;state.gauge=GaugeState::Done;}
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
                if(state.flight.water&&!state.splashed&&state.flightTime*60>=state.flight.path.size()-1)
                {
                    state.splashed=true;
                    Splash(state.flight.path.back());
                }
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
                if(state.turn==You&&state.overhead&&state.landingKnown&&(state.phase==Phase::Aim||state.phase==Phase::Gauge))
                {
                    // 0: the circle from high above, a little behind it.
                    const V3 dir{std::sin(state.aim),0,std::cos(state.aim)};
                    const float high=(state.club==Golf::Putter?9.0f:38.0f)*state.camera.zoom;
                    set(state.landing-dir*(high*0.25f)+V3{0,high,0},state.landing);
                }
                else if(state.turn==You||state.phase==Phase::HoleOver||state.phase==Phase::Over)
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
            state.renderer->SetCamera(XMLoadFloat3(&state.eye),XMLoadFloat3(&state.look),FovY());
        }

        // The trees in sight this frame, as the renderer's places for each kind: those within
        // the view's cone (with room for their size) or close by. A wood of several hundred is
        // drawn a few hundred at a time.
        void PlaceTrees()
        {
            using namespace DirectX;
            std::vector<std::vector<MascotRenderer::SceneryInstance>> groups(Golf::TreeKinds);
            const XMVECTOR eye=XMLoadFloat3(&state.eye);
            const XMVECTOR ahead=XMVector3Normalize(XMVectorSubtract(XMLoadFloat3(&state.look),eye));
            RECT rc{};
            GetClientRect(state.window,&rc);
            const float aspect=rc.bottom>rc.top?static_cast<float>(rc.right-rc.left)/static_cast<float>(rc.bottom-rc.top):1.6f;
            const float half=std::atan(std::tan(FovY()/2)*std::max(1.0f,aspect));   // the wider half-angle
            const float coneCos=std::cos(std::min(1.5f,half+0.08f));
            size_t shown=0;
            for(const auto& t:Course().Trees())
            {
                const XMFLOAT3 at=W(V3{t.x,t.y,t.z});
                const XMVECTOR to=XMVectorSubtract(XMLoadFloat3(&at),eye);
                const float d=XMVectorGetX(XMVector3Length(to));
                const float reach=9*K*t.scale;   // a tree's size, round its foot
                if(d>reach)
                {
                    const float c=XMVectorGetX(XMVector3Dot(to,ahead))/d;
                    // Within the cone widened by the angle the tree's size takes at that distance.
                    if(c<std::cos(std::min(1.5f,std::acos(std::clamp(coneCos,-1.0f,1.0f))+std::asin(std::min(1.0f,reach/d)))))continue;
                }
                MascotRenderer::SceneryInstance q;
                q.x=at.x;q.y=at.y-0.05f*K;q.z=at.z;q.yaw=t.yaw;q.scale=t.scale;
                auto& g=groups[static_cast<size_t>(t.kind)];
                if(shown<MascotRenderer::MaxSceneryInstances){g.push_back(q);++shown;}
            }
            state.treesShown=shown;
            state.renderer->SetSceneryInstances(std::move(groups));
        }

        // Putting now.
        bool Putting()
        {
            if(state.phase==Phase::Flying||state.phase==Phase::Settle)return state.shot.club==Golf::Putter;
            if(state.phase!=Phase::Aim&&state.phase!=Phase::Gauge&&state.phase!=Phase::Swing)return false;
            if(state.turn==You)return state.club==Golf::Putter;
            return state.thought?state.shot.club==Golf::Putter:state.balls[Her].lie==Lie::Green;
        }

        // A soft strip of light for the glowing pass (GeometryPS: 9 in minus uv.z, bright along
        // its middle): from a to b, half width w across the view, as bright as ca at a, cb at b.
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
                g.color[0]=c[0];g.color[1]=c[1];g.color[2]=c[2];g.color[3]=Unlit;
                g.uv[0]=u;g.uv[2]=-9;
                out.push_back(g);
            };
            const XMVECTOR a0=XMVectorSubtract(A,side),a1=XMVectorAdd(A,side),b0=XMVectorSubtract(B,side),b1=XMVectorAdd(B,side);
            v(a0,0,ca);v(a1,1,ca);v(b1,1,cb);
            v(a0,0,ca);v(b1,1,cb);v(b0,0,cb);
        }

        // What glows this frame: the pin's beam up into the sky, and the
        // ball's trail while it is in the air, the brighter the faster it goes.
        std::vector<MascotRenderer::GeometryVertex> BuildGlow()
        {
            std::vector<MascotRenderer::GeometryVertex> out;
            const auto& c=Course();
            {
                const V3 p=c.Pin();
                const float far_=std::sqrt((state.eye.x-p.x*K)*(state.eye.x-p.x*K)+(state.eye.z-p.z*K)*(state.eye.z-p.z*K));
                const float w=std::max(0.18f*K,far_*0.006f);
                const float pulse=0.85f+0.15f*std::sin(static_cast<float>(state.clock)*2.4f);
                const float low[3]={0.10f*pulse,0.42f*pulse,0.38f*pulse},high[3]={0.04f,0.20f,0.22f};
                const float coreLow[3]={0.35f*pulse,0.55f*pulse,0.50f*pulse},coreHigh[3]={0.10f,0.22f,0.22f};
                for(int k=0;k<6;++k)   // in pieces up the sky, so its brightness fades smoothly
                {
                    const float h0=k*k*BeamSky/36,h1=(k+1)*(k+1)*BeamSky/36;
                    const float t0=static_cast<float>(k)/6,t1=static_cast<float>(k+1)/6;
                    float l0[3],l1[3],c0[3],c1[3];
                    for(int i=0;i<3;++i){l0[i]=low[i]+(high[i]-low[i])*t0;l1[i]=low[i]+(high[i]-low[i])*t1;c0[i]=coreLow[i]+(coreHigh[i]-coreLow[i])*t0;c1[i]=coreLow[i]+(coreHigh[i]-coreLow[i])*t1;}
                    LightStrip(out,W(p,h0),W(p,h1),w,l0,l1);
                    LightStrip(out,W(p,h0),W(p,h1),w*0.3f,c0,c1);
                }
            }
            // The trail: from the strike to where it first comes down (not along the ground
            // after), fading behind it, as bright as the ball is fast.
            if(state.phase==Phase::Flying&&!state.flight.path.empty())
            {
                const auto& path=state.flight.path;
                const size_t now=std::min(static_cast<size_t>(state.flightTime*60),path.size()-1);
                const size_t end=std::min(now,static_cast<size_t>(std::max(0,state.flight.landing)));
                const size_t from=now>150?now-150:0;
                const bool hers=state.turn==Her;
                const float tint[3]={hers?1.0f:0.55f,hers?0.55f:0.8f,hers?0.8f:1.0f};
                for(size_t i=from;i+3<=end;i+=3)
                {
                    const V3 a=path[i],b=path[i+3];
                    const float speed=Golf::Length(b-a)*20;   // m/s
                    const float age=static_cast<float>(i-from)/std::max<size_t>(1,now-from);
                    const float k=std::clamp(speed/55.0f,0.0f,1.0f)*age*0.9f;
                    if(k<0.01f)continue;
                    const float ca[3]={tint[0]*k,tint[1]*k,tint[2]*k};
                    LightStrip(out,W(a),W(b),(0.03f+0.05f*age)*K,ca,ca);
                }
            }
            return out;
        }

        // The sparks this frame: rising round the pin's beam, glittering along a square strike's
        // trail, the water thrown up where a ball fell in.
        std::vector<MascotRenderer::Particle> BuildSparks(double dt)
        {
            std::vector<MascotRenderer::Particle> out;
            const auto& c=Course();
            const float t=static_cast<float>(state.clock);
            {
                // Fine motes winding up close round the beam, many of them.
                const V3 p=c.Pin();
                const float far_=std::sqrt((state.eye.x-p.x*K)*(state.eye.x-p.x*K)+(state.eye.z-p.z*K)*(state.eye.z-p.z*K));
                const float size=std::max(0.035f*K,far_*0.0022f);
                const float beam=std::max(0.18f*K,far_*0.006f)/K;   // the beam's half width, metres
                constexpr int N=180;
                for(int i=0;i<N;++i)
                {
                    const float u=std::fmod(t*0.05f+static_cast<float>(i)*0.61803f,1.0f);   // up the beam, over and again
                    const float h=0.2f+u*u*80,a=t*(1.4f+0.6f*std::sin(i*0.9f))+i*2.39996f,r=beam*(0.9f+0.5f*std::sin(i*1.7f)+u*0.8f);
                    MascotRenderer::Particle q;
                    q.position={(p.x+r*std::cos(a))*K,(p.y+h)*K,(p.z+r*std::sin(a))*K};
                    const float twinkle=0.6f+0.4f*std::sin(t*9+i*1.3f);
                    q.size=size*(1-0.35f*u)*(0.7f+0.5f*twinkle);
                    const float fade=std::min(1.0f,u*10)*(1-u);
                    q.color={0.6f,1.0f,0.93f,0.95f*fade*twinkle};
                    q.glow=true;
                    q.angle=a;
                    out.push_back(q);
                }
            }
            if(state.phase==Phase::Flying&&state.justShot&&!state.flight.path.empty())
            {
                const auto& path=state.flight.path;
                const size_t now=std::min(static_cast<size_t>(state.flightTime*60),path.size()-1);
                const size_t end=std::min(now,static_cast<size_t>(std::max(0,state.flight.landing)));
                std::uniform_real_distribution<float> u(-1,1);
                // Fine glitter, three motes a frame of the path, drifting out and down as it ages.
                for(size_t i=end>100?end-100:0;i<end;++i)
                    for(int k=0;k<3;++k)
                    {
                        const float age=static_cast<float>(now-i)/100;
                        if(age>1)continue;
                        std::mt19937 r(static_cast<std::uint32_t>(i*2654435761u+k*40503u+state.shots*97u));
                        const V3 a=path[i];
                        const float spread=(0.06f+0.7f*age);
                        MascotRenderer::Particle q;
                        q.position={(a.x+u(r)*spread)*K,(a.y+u(r)*spread-age*age*1.2f)*K,(a.z+u(r)*spread)*K};
                        const float twinkle=0.5f+0.5f*std::sin(t*18+i*1.3f+k*2.1f);
                        q.size=(0.035f+0.05f*twinkle)*K*(1-0.5f*age);
                        q.color={1.0f,0.92f,0.55f+0.45f*twinkle,(1-age)*(0.6f+0.4f*twinkle)};
                        q.glow=true;
                        q.angle=t*3+i;
                        out.push_back(q);
                    }
            }
            // The splash: drops thrown up, falling back.
            for(auto& d:state.drops)
            {
                d.age+=static_cast<float>(dt);
                d.v.y-=9.81f*static_cast<float>(dt);
                d.at=d.at+d.v*static_cast<float>(dt);
            }
            state.drops.erase(std::remove_if(state.drops.begin(),state.drops.end(),[](const Drop& d){return d.age>d.life;}),state.drops.end());
            for(const auto& d:state.drops)
            {
                MascotRenderer::Particle q;
                q.position={d.at.x*K,d.at.y*K,d.at.z*K};
                q.size=d.size*K;
                q.color={0.85f,0.93f,1.0f,0.85f*(1-d.age/d.life)};
                q.glow=true;
                out.push_back(q);
            }
            return out;
        }

        // A ball fell into the pond at p: rings spread, water is thrown up.
        void Splash(V3 p)
        {
            state.ripples.push_back({p.x*K,p.z*K,0,1.0f});
            std::uniform_real_distribution<float> u(0,1);
            for(int i=0;i<70;++i)
            {
                const float a=u(state.random)*2*Pi,out=0.6f+u(state.random)*2.2f,up=2.5f+u(state.random)*4.5f;
                Drop d;
                d.at=p+V3{0,0.05f,0};
                d.v={std::cos(a)*out,i<14?up*1.5f:up,std::sin(a)*out};
                d.life=0.6f+u(state.random)*0.7f;
                d.size=0.03f+u(state.random)*0.05f;
                state.drops.push_back(d);
            }
        }

        // The gauge's bar on the screen (window pixels): thin, along the bottom.
        RectF GaugeBar(float w,float h,float s)
        {
            const float gw=std::min(w*0.5f,560*s),gh=9*s;
            return RectF(w/2-gw/2,h-48*s,gw,gh);
        }
        // Where on the bar a gauge position (-0.25 the strike's far end, 0 the line, 1 full power) is.
        float GaugeX(const RectF& bar,float at){return bar.X+(at+0.25f)/1.25f*bar.Width;}
        // The ball showing where you strike it: its middle on the screen, right of the bar.
        static constexpr float SpotRadius=22;   // pixels at 96 dpi
        PointF SpotMiddle(const RectF& bar,float s){return PointF(bar.X+bar.Width+(SpotRadius+34)*s,bar.Y+bar.Height/2-8*s);}

        // The gauge, drawn in the 3D view just in front of the camera so its marker moves with
        // every frame (the picture over the view is made again only now and then): light blue
        // to full power, pink round the strike's line, the line white (narrow for long clubs),
        // where the power was set, and the marker.
        void BuildGauge(Mesh& m)
        {
            using namespace DirectX;
            RECT client{};
            GetClientRect(state.view,&client);
            const float w=static_cast<float>(std::max(1L,client.right)),h=static_cast<float>(std::max(1L,client.bottom)),s=Scale();
            const XMVECTOR eye=XMLoadFloat3(&state.eye),forward=XMVector3Normalize(XMVectorSubtract(XMLoadFloat3(&state.look),eye));
            const XMVECTOR right=XMVector3Normalize(XMVector3Cross(XMVectorSet(0,1,0,0),forward)),up=XMVector3Cross(forward,right);
            // World units in front of the eye (the near plane is at 4); each layer drawn a little
            // nearer than the one under it, so they never fight over the same depth.
            float D=6.5f;
            const float t=std::tan(FovY()/2);
            auto at=[&](float px,float py){
                const XMVECTOR q=XMVectorAdd(eye,XMVectorAdd(XMVectorScale(forward,D),XMVectorAdd(XMVectorScale(right,(2*px/w-1)*t*w/h*D),XMVectorScale(up,(1-2*py/h)*t*D))));
                XMFLOAT3 f;XMStoreFloat3(&f,q);
                return Mesh::V{f.x,f.y,f.z};
            };
            XMFLOAT3 n;XMStoreFloat3(&n,XMVectorNegate(forward));
            auto rect=[&](float x0,float y0,float x1,float y1,float r,float g,float b){
                D-=0.05f;
                const float col[4]={r,g,b,Unlit};
                m.QuadN(at(x0,y0),at(x1,y0),at(x1,y1),at(x0,y1),{n.x,n.y,n.z},col);
            };
            const RectF bar=GaugeBar(w,h,s);
            const float y0=bar.Y,y1=bar.Y+bar.Height;
            const bool putt=state.club==Golf::Putter;
            rect(bar.X-2*s,y0-2*s,bar.X+bar.Width+2*s,y1+2*s,0.06f,0.10f,0.16f);              // the frame
            rect(GaugeX(bar,putt?0:-0.25f),y0,GaugeX(bar,0),y1,0.20f,0.26f,0.34f);             // past the line
            rect(GaugeX(bar,0),y0,GaugeX(bar,1),y1,0.42f,0.82f,0.98f);                          // light blue to full power
            for(int k=1;k<4;++k)rect(GaugeX(bar,k*0.25f)-0.75f*s,y0,GaugeX(bar,k*0.25f)+0.75f*s,y0+bar.Height*0.45f,0.9f,0.97f,1.0f);
            if(!putt)
            {
                // pink: it curves; white: square; each edged dark, as the bar is
                const float px0=GaugeX(bar,-Pink),px1=GaugeX(bar,Pink);
                rect(px0-1.5f*s,y0-4.5f*s,px1+1.5f*s,y1+4.5f*s,0.06f,0.10f,0.16f);
                rect(px0,y0-3*s,px1,y1+3*s,0.98f,0.42f,0.70f);
                const float jw=std::max(1.2f*s,GaugeX(bar,JustWidth(state.club))-GaugeX(bar,0));
                rect(GaugeX(bar,0)-jw-1.5f*s,y0-5.5f*s,GaugeX(bar,0)+jw+1.5f*s,y1+5.5f*s,0.06f,0.10f,0.16f);
                rect(GaugeX(bar,0)-jw,y0-4*s,GaugeX(bar,0)+jw,y1+4*s,1,1,1);
            }
            // The ball and where you strike it, the mark easing after the spot.
            if(!putt)
            {
                const PointF mid=SpotMiddle(bar,s);
                const float r=SpotRadius*s;
                auto disc=[&](float cx,float cy,float r0,float r1,const float (&col)[3],int k){
                    D-=0.05f;
                    const float c4[4]={col[0],col[1],col[2],Unlit};
                    for(int i=0;i<k;++i)
                    {
                        const float a0=2*Pi*i/k,a1=2*Pi*(i+1)/k;
                        const Mesh::V p0=at(cx+r1*std::cos(a0),cy+r1*std::sin(a0)),p1=at(cx+r1*std::cos(a1),cy+r1*std::sin(a1));
                        const Mesh::V q0=at(cx+r0*std::cos(a0),cy+r0*std::sin(a0)),q1=at(cx+r0*std::cos(a1),cy+r0*std::sin(a1));
                        m.QuadN(q0,q1,p1,p0,{n.x,n.y,n.z},c4);
                    }
                };
                disc(mid.X,mid.Y,0,r+1.5f*s,{0.35f,0.40f,0.46f},40);       // its edge
                disc(mid.X,mid.Y,0,r,{0.96f,0.97f,0.98f},40);                // the ball
                rect(mid.X-r,mid.Y-0.5f*s,mid.X+r,mid.Y+0.5f*s,0.62f,0.66f,0.72f);
                rect(mid.X-0.5f*s,mid.Y-r,mid.X+0.5f*s,mid.Y+r,0.62f,0.66f,0.72f);
                const float x=mid.X+state.spotShownX*r*0.8f,y=mid.Y-state.spotShownY*r*0.8f;
                disc(x,y,0,5.5f*s,{0.45f,0.05f,0.10f},20);
                disc(x,y,0,4.2f*s,{0.92f,0.16f,0.24f},20);
            }
            if(state.gauge==GaugeState::Impact||state.gauge==GaugeState::Done)
                rect(GaugeX(bar,state.gaugePower)-1.5f*s,y0-5*s,GaugeX(bar,state.gaugePower)+1.5f*s,y1+5*s,1.0f,0.82f,0.25f);
            if(state.gauge!=GaugeState::Ready)
            {
                const float x=GaugeX(bar,state.gaugeAt);
                rect(x-1.5f*s,y0-8*s,x+1.5f*s,y1+8*s,1,1,0.6f);
                const float col[4]={1,1,0.6f,Unlit};
                D-=0.05f;
                const Mesh::V a=at(x,y1+8*s),b=at(x-6*s,y1+16*s),c=at(x+6*s,y1+16*s);
                m.Vertex(a,{n.x,n.y,n.z},col);m.Vertex(b,{n.x,n.y,n.z},col);m.Vertex(c,{n.x,n.y,n.z},col);
            }
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
                // Sitting on the ground, never in it (the path keeps its middle a radius up while
                // it rolls, its resting place on the ground), except going down the cup.
                const float ground=c.Height(p.x,p.z)*K;
                const bool cup=Golf::Flat(p,c.Pin())<0.08f;
                const float bottom=cup?(p.y-Golf::BallRadius)*K:std::max((p.y-Golf::BallRadius)*K,ground);
                m.Sphere({p.x*K,bottom+r,p.z*K},r,white,4,10,6);
                // Its shadow on the ground under it.
                const float g=ground+0.02f*K;
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
            // The cup: a dark hole with a white rim; the model's own cup filled with green when
            // the pin is elsewhere.
            {
                const V3 p=c.Pin();
                const float dark[4]={0.03f,0.03f,0.03f,1},white[4]={0.95f,0.95f,0.95f,1};
                const float y=(p.y+0.006f)*K;
                for(int i=0;i<24;++i)
                {
                    const float a0=2*Pi*i/24,a1=2*Pi*(i+1)/24,r0=Golf::CupRadius*K,r1=r0*1.12f;
                    m.Vertex({p.x*K,y,p.z*K},{0,1,0},dark);
                    m.Vertex({p.x*K+r0*std::cos(a1),y,p.z*K+r0*std::sin(a1)},{0,1,0},dark);
                    m.Vertex({p.x*K+r0*std::cos(a0),y,p.z*K+r0*std::sin(a0)},{0,1,0},dark);
                    m.Quad({p.x*K+r0*std::cos(a0),y,p.z*K+r0*std::sin(a0)},{p.x*K+r0*std::cos(a1),y,p.z*K+r0*std::sin(a1)},
                           {p.x*K+r1*std::cos(a1),y,p.z*K+r1*std::sin(a1)},{p.x*K+r1*std::cos(a0),y,p.z*K+r1*std::sin(a0)},white);
                }
                const V3 old=c.ModelCup();
                if(Golf::Flat(old,p)>0.3f)
                {
                    const float fill[4]={state.greenColour[0],state.greenColour[1],state.greenColour[2],CourseLook};
                    const float oy=(c.Height(old.x+0.3f,old.z)+0.004f)*K,r=0.12f*K;
                    for(int i=0;i<16;++i)
                    {
                        const float a0=2*Pi*i/16,a1=2*Pi*(i+1)/16;
                        m.Vertex({old.x*K,oy,old.z*K},{0,1,0},fill);
                        m.Vertex({old.x*K+r*std::cos(a1),oy,old.z*K+r*std::sin(a1)},{0,1,0},fill);
                        m.Vertex({old.x*K+r*std::cos(a0),oy,old.z*K+r*std::sin(a0)},{0,1,0},fill);
                    }
                }
            }
            // Your aim: a ring where the ball comes down at full power.
            if(state.turn==You&&(state.phase==Phase::Aim||state.phase==Phase::Gauge)&&state.landingKnown&&!state.autoplay)
            {
                const V3 l=state.landing;
                const float col[4]={1.0f,0.92f,0.35f,Unlit};
                const bool putt=state.club==Golf::Putter;
                const float r0=(putt?0.12f:1.6f)*K,r1=r0*1.35f,lift=(putt?0.02f:0.12f)*K;
                // Laid on the ground as it rises and falls under it (a flat ring sank into slopes).
                auto on=[&](float a,float r){
                    const float x=l.x*K+r*std::cos(a),z=l.z*K+r*std::sin(a);
                    return Mesh::V{x,c.Height(x/K,z/K)*K+lift,z};
                };
                for(int i=0;i<32;++i)
                {
                    const float a0=2*Pi*i/32,a1=2*Pi*(i+1)/32;
                    m.QuadN(on(a0,r1),on(a1,r1),on(a1,r0),on(a0,r0),{0,1,0},col);
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
            if(state.turn==You&&(state.phase==Phase::Aim||state.phase==Phase::Gauge)&&!state.autoplay)BuildGauge(m);
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
                const RectF box(10*s,H-112*s,300*s,102*s);
                Panel(g,box,s);
                Text(g,Golf::ClubName(state.club),RectF(box.X+12*s,box.Y+6*s,box.Width-90*s,28*s),19*s,gold,true,StringAlignmentNear);
                std::wstring info=std::wstring(Golf::LieName(b.lie))+T(L"　ピンまで ",L"   to the pin ")+std::to_wstring(static_cast<int>(toPin+0.5f))+L" m";
                Text(g,info,RectF(box.X+12*s,box.Y+36*s,box.Width-90*s,22*s),14*s,white,false,StringAlignmentNear);
                if(state.club==Golf::Putter)
                {
                    const float rise=(c.Pin().y-b.at.y)*100;
                    wchar_t t[120];
                    swprintf_s(t,T(L"%ls %.0f cm",L"%ls %.0f cm"),rise>=0?T(L"上り",L"uphill"):T(L"下り",L"downhill"),std::abs(rise));
                    Text(g,t,RectF(box.X+12*s,box.Y+58*s,box.Width-90*s,22*s),14*s,soft,false,StringAlignmentNear);
                }
                Text(g,T(L"Tabで操作説明",L"Tab: the keys"),RectF(box.X+12*s,box.Y+78*s,box.Width-20*s,20*s),12*s,soft,false,StringAlignmentNear);
                // Where on the ball you strike (the ball and its mark are drawn in 3D by
                // BuildGauge, right of the bar; only the word here).
                const bool gauge=state.turn==You&&(state.phase==Phase::Aim||state.phase==Phase::Gauge)&&!state.autoplay;
                if(gauge&&state.club!=Golf::Putter)
                {
                    const RectF bar=GaugeBar(W,H,s);
                    const PointF mid=SpotMiddle(bar,s);
                    Text(g,T(L"打点",L"Spot"),RectF(mid.X-SpotRadius*s,mid.Y+SpotRadius*s+2*s,2*SpotRadius*s,16*s),11*s,soft);
                }
                // The gauge's numbers over its bar (the bar itself is drawn in 3D): how far full
                // power carries with this club on level ground, half that at the middle.
                const RectF bar=GaugeBar(W,H,s);
                const float full=state.club==Golf::Putter?Golf::PutterRange(toPin,c.Pin().y-b.at.y):Golf::FlatCarry(state.club,state.spotX,state.spotY);
                auto number=[&](float at,float metres,bool bold){
                    wchar_t t[32];
                    swprintf_s(t,metres<20?L"%.1f m":L"%.0f m",metres);
                    Text(g,t,RectF(GaugeX(bar,at)-40*s,bar.Y-24*s,80*s,18*s),12*s,bold?gold:white,bold);
                };
                if(gauge)
                {
                    number(0.5f,full*0.5f,false);
                    number(1,full,true);
                }
                if(state.gauge==GaugeState::Impact||state.gauge==GaugeState::Done)
                    Text(g,std::to_wstring(static_cast<int>(state.gaugePower*100+0.5f))+L"%",RectF(GaugeX(bar,state.gaugePower)-30*s,bar.Y+14*s,60*s,18*s),12*s,gold,true);
            }
            // The pin's distance and height over its beam (from the ball whose turn it is).
            if(state.beamTop.x>=0&&state.phase!=Phase::HoleOver&&state.phase!=Phase::Over)
            {
                const V3 from=state.balls[state.turn].at;
                const float d=Golf::Flat(from,c.Pin()),rise=c.Pin().y-from.y;
                wchar_t t[80];
                swprintf_s(t,T(L"%.1f m　%ls%.1f m",L"%.1f m   %ls%.1f m"),d,rise>=0?L"↑":L"↓",std::abs(rise));
                const float tx=static_cast<float>(state.beamTop.x),ty=static_cast<float>(state.beamTop.y);
                const RectF box(tx-80*s,std::max(4*s,ty-34*s),160*s,26*s);
                Panel(g,box,s,150);
                Text(g,t,box,13*s,white,true);
            }
            // The keys, while Tab is on.
            if(state.help)
            {
                const wchar_t* lines[]={
                    T(L"←→　狙い（Shiftで細かく）",L"←→   aim (Shift: finely)"),
                    T(L"↑↓　クラブ",L"↑↓   club"),
                    T(L"Ctrl＋矢印　打点（下で高く少し遠く・止まる、上で低く転がる、左で右へ・右で左へ曲がる）",L"Ctrl+arrows   spot (low: high, a little further, stops; high: low, rolls; left: curves right; right: curves left)"),
                    T(L"Space／クリック　スタート → パワー → インパクト（白で真っすぐ、ピンクで曲がる）",L"Space/click   start, power, strike (white: straight; pink: it curves)"),
                    T(L"パターはパワーを決めたら打ちます",L"The putter strikes once the power is set"),
                    T(L"0　黄色い輪を上から見る",L"0   see the yellow circle from above"),
                    T(L"右ドラッグ・ホイール　カメラ（F1で戻す）",L"Right drag, wheel   camera (F1 puts it back)"),
                    T(L"Esc　やめる　　Tab　この説明を閉じる",L"Esc   stop      Tab   close this")};
                const float bw=std::min(W-40*s,720*s),lh=26*s,bh=lh*std::size(lines)+28*s;
                const RectF box(W/2-bw/2,H*0.45f-bh/2,bw,bh);
                Panel(g,box,s,215);
                for(size_t i=0;i<std::size(lines);++i)
                    Text(g,lines[i],RectF(box.X+18*s,box.Y+14*s+lh*i,box.Width-36*s,lh),14*s,white,false,StringAlignmentNear);
            }
            // Her words beside her head.
            const bool herSeen=state.besideHead.x>=0&&state.besideHead.x<w&&state.headScreen.y>0&&state.headScreen.y<h&&state.phase!=Phase::Intro;
            if(!state.words.text.empty()&&herSeen)
            {
                const std::wstring& said=state.words.text;
                const float most=std::max(160*s,W*0.32f);
                const WordsBubble bubble(g,said,s,most);
                const float bw=bubble.Width(),bh=bubble.Height();
                const bool beside=true;
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
                // (Low during the look over the hole, which shows the clubhouse in the middle.)
                const RectF box(W/2-bw/2,(state.phase==Phase::Intro?H*0.62f:H*0.36f)-bh/2,bw,bh);
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
            const bool moving=state.phase==Phase::Flying;
            const ULONGLONG now=GetTickCount64();
            std::wstring shown=std::to_wstring(w)+L"x"+std::to_wstring(h)+L" "+std::to_wstring(static_cast<int>(state.phase))+L" "+std::to_wstring(state.turn)+L" "+
                               std::to_wstring(state.club)+L" "+std::to_wstring(static_cast<int>(state.aim*200))+L" "+std::to_wstring(static_cast<int>(state.gauge))+
                               L" "+std::to_wstring(static_cast<int>(ViewYaw()*30))+L"|"+ShownWords()+L"|"+(state.clock<state.bannerUntil?state.banner+state.bannerNote:L"")+
                               L"|"+std::to_wstring(state.besideHead.x/12)+L","+std::to_wstring(state.headScreen.y/12)+(state.askingQuit?L"a":L"")+
                               std::to_wstring(state.balls[0].strokes)+std::to_wstring(state.balls[1].strokes)+
                               L"|"+std::to_wstring(static_cast<int>(state.spotX*4))+L","+std::to_wstring(static_cast<int>(state.spotY*4))+(state.help?L"h":L"")+
                               L"|"+std::to_wstring(state.beamTop.x/6)+L","+std::to_wstring(state.beamTop.y/6);
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
            state.sceneryBuilt=false;
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
                else if(wParam==VK_TAB)state.help=!state.help;
                else if((wParam=='0'||wParam==VK_NUMPAD0)&&state.turn==You&&(state.phase==Phase::Aim||state.phase==Phase::Gauge))state.overhead=!state.overhead;
                else if(state.turn==You&&state.phase==Phase::Aim&&state.gauge==GaugeState::Ready&&GetKeyState(VK_CONTROL)<0&&
                        (wParam==VK_LEFT||wParam==VK_RIGHT||wParam==VK_UP||wParam==VK_DOWN))
                {
                    // The spot on the ball: moved while held, in the tick.
                }
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
        state.ground=Golf::LoadGround();
        if(!state.ground)
        {
            Log(L"golf: the course model could not be read");
            auto end=std::move(state.onEnd);
            if(end)end();
            return;
        }
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
        state.renderer->SetModelTransform(HerWorld(model));
        PlaceCamera();
        {
            const float ease=1-std::exp(-14*static_cast<float>(dt));
            state.spotShownX+=(state.spotX-state.spotShownX)*ease;
            state.spotShownY+=(state.spotY-state.spotShownY)*ease;
        }
        state.renderer->SetMovingGeometry(BuildMoving(model));
        PlaceTrees();
        state.renderer->SetGlowGeometry(BuildGlow());
        state.renderer->SetParticles(BuildSparks(dt));
        for(auto& r:state.ripples)r.age+=static_cast<float>(dt);
        state.ripples.erase(std::remove_if(state.ripples.begin(),state.ripples.end(),[](const MascotRenderer::Ripple& r){return r.age>7;}),state.ripples.end());
        state.renderer->SetWater(true,static_cast<float>(state.clock),state.ripples);
        {
            const V3 w=Golf::WindVector(state.wind);
            state.renderer->SetWaterWind(w.x*K,w.z*K);
        }
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
        // The top of the pin's beam, for the distance written there.
        {
            const V3 p=Course().Pin();
            const XMFLOAT3 top=W(p,BeamHigh);
            const XMVECTOR forward=XMVectorSubtract(XMLoadFloat3(&state.look),XMLoadFloat3(&state.eye));
            const bool ahead=XMVectorGetX(XMVector3Dot(forward,XMVectorSubtract(XMLoadFloat3(&top),XMLoadFloat3(&state.eye))))>0;
            state.beamTop=ahead?state.renderer->Project({top.x,top.y,top.z}):POINT{-1,-1};
            RECT client{};
            GetClientRect(state.view,&client);
            if(state.beamTop.x<0||state.beamTop.x>client.right||state.beamTop.y>client.bottom)state.beamTop={-1,-1};
            else state.beamTop.y=std::max(state.beamTop.y,40L);
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
    void SetGolfGauge(float power,float at,const std::wstring& flags){state.frozenPower=power;state.frozenAt=at;state.frozenFlags=flags;}

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
        swprintf_s(line,L"golf: %zu trees, %zu drawn this frame\n",Course().Trees().size(),state.treesShown);
        text+=line;
        text+=L"golf play:\n"+state.log;
        if(!state.words.told.empty())text+=L"golf words:\n"+state.words.told;
        return text;
    }
}
