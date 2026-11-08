#define NOMINMAX
#include "GolfWindow.hpp"
#include "Golf.hpp"
#include "GolfProgress.hpp"
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
        constexpr int You=0,Her=1,Holes=Golf::HoleCount,MostStrokes=10;
        constexpr float K=12.5f;   // world units a metre (MMD's 8 cm a unit)
        // Distances along the ground are shown in yards (as golf counts them), heights in metres.
        constexpr float YardM=0.9144f;
        inline float Yards(float metres){return metres/YardM;}
        inline std::wstring YardText(float metres,bool fine=false)
        {
            wchar_t t[32];
            swprintf_s(t,fine?L"%.1f":L"%.0f",Yards(metres));
            return std::wstring(t)+T(L"y",L" yd");
        }
        constexpr float Pi=3.14159265f,Deg=Pi/180;
        constexpr float BeamHigh=25;   // where the pin's distance is written on its beam, metres up
        constexpr float BeamSky=700;
        constexpr double AceZoom=1.6;  // a hole in one: the camera down by the cup for its last seconds   // how high the beam goes (into the sky's dome)
        float FovY(){return 45.0f*Deg;}   // the camera's (the gauge in front of it is placed by it)

        enum class Phase {Intro,Aim,Gauge,Swing,Flying,Settle,HoleOver,Over};
        // A drop of water thrown up (metres, metres a second, seconds).
        constexpr float PuttLineShown=4.0f;   // metres of your putt's likely way shown
        struct Drop {V3 at,v;float age=0,life=1,size=0.05f;float rgb[3]{0.85f,0.93f,1.0f};float fall=9.81f;bool rain=false;};
        // The gauge: waiting; the marker running out for the power (and back: no click by the
        // time it is home again lets the shot go); running back past the line for the strike
        // (not for the putter); done.
        enum class GaugeState {Ready,Power,Impact,Done};
        constexpr double PowerSeconds=1.7;    // the marker out to full power
        constexpr double ImpactSeconds=1.4;   // back from full power to the line
        constexpr float Pink=0.09f;           // the curving zone's half width either side of the line (gauge widths)

        // Her swing's timing, made for each swing from the way the club's head goes: the
        // backswing eases away and back to rest at the top; coming down the head gathers speed
        // (its path as the cube of the time) to be as fast as the ball leaves at the strike, and
        // through the ball it starts at that speed and slows to the finish. Stage 0 address, 1
        // top, 2 strike, 3 finish (GeneratedMotion::Shape).
        constexpr double Address=1.6,HoldFinish=1.5;
        struct SwingPlan
        {
            double back=0.95,top=0.12,down=0.28,through=0.55;
            static constexpr int PerStage=100;
            std::vector<float> path;   // metres the head has gone by stage i/PerStage
            float speed=0;             // m/s at the strike
            double Strike() const {return back+top+down;}
            double Length() const {return back+top+down+through;}
        };
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
            int round=0;   // rounds played in this window (the first in the models' own cups)
            int startHole=0;   // diagnostics (--golf-hole): the first round starts here (0-8)
            // The round chosen from the menu: how many holes, and whether they are drawn at random
            // (else the first ones in order); the holes of this round (numbers 1-9, in play order).
            int roundHoles=Holes;
            bool roundRandom=false;
            std::vector<int> order;
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
            SwingPlan plan;           // her swing's timing (MakePlan)
            std::vector<float> pathScale;   // her posed club head's way over the reckoned one, a twentieth of a stage each (MeasureHer)
            std::mt19937 random{12345};
            float care=0.85f;
            bool autoplay=false;
            // Her measures: hips and upper body (model units), the club's length and where its
            // head is at the address (model space; the ball goes there).
            float hips=10,upper=10,clubLength=12;
            std::array<float,3> head{0,0,-8};
            std::array<float,3> putterHead{0,0,-8};   // the same for the putter
            float putterLength=1;
            bool herPutting=false;   // her club now is the putter (PlaceToSwing)
            int herClub=3;           // the club in her hands (PlaceToSwing)
            // Each club: its length (model units; the measured length is the 7 iron's), how much
            // flatter it lies in her hands (radians), where its head rests at the address (the
            // ball goes there: straight out from her across the aim, further for longer clubs).
            std::array<float,Golf::ClubCount> clubLen{},clubTilt{};
            std::array<std::array<float,3>,Golf::ClubCount> clubHead{};
            float faceSign=1;   // her left palm's normal or its opposite is the club face's way
            // Where she is: on the course (metres), turned by yaw (radians, the model's
            // rotation about the up axis), swinging or watching.
            V3 herAt;
            float herYaw=0;
            bool herSwinging=false;
            bool moved=false;      // put somewhere else this frame (her hair and clothes settled again)
            size_t treesShown=0;   // trees drawn this frame
            std::vector<float> treeFade;   // each tree's fade, 0 gone .. 1 there (PlaceTrees)
            bool cutNow=false;             // the camera jumped this frame (no fading)
            bool justShot=false;   // the shot in the air was struck square (its trail glitters)
            bool splashed=false;   // the ball in the air has fallen into the pond
            bool spunShown=false,boomShown=false,beamShown=false;   // this flight's effects, shown
            // The special gauge (0-200, two bars: the boost takes one, the meteor two), yours and hers;
            // what you have chosen for this shot (0 none, 1 boost, 2 meteor); the spin your strike made.
            std::array<float,2> meter{};
            int special=0,gaugeSpin=0;
            // The special shots' menu (S): open, the line picked (0 boost, 1 meteor, 2 cancel),
            // where its lines are on screen.
            bool menuOpen=false;
            int diagSpecial=0;
            bool diagAce=false;
            float diagMeteor=0;   // (--golf-ace: the meteor's speed found)   // diagnostics (--golf-ace): no wind, and your tee shot a meteor with backspin found to go in
            // Progress kept for the user (achievements, records, looks, her rank, stars).
            GolfProgress::Data progress;
            std::wstring progressFile;
            // The day's conditions for this round (Weather; forced: diagnostics, -1 none).
            int weather=0,forcedWeather=-1;
            // This hole, for your missions and achievements: the mission (-1 none) and whether it
            // is done, a penalty, a meteor used, your shots and putts; this round's square-strike
            // run, a penalty, missions done.
            int mission=-1;
            bool missionDone=false,holePenalty=false,meteorUsed=false,roundPenalty=false;
            int shotsHere=0,putts=0,squareRun=0,missionsDone=0;
            // Short notices (an achievement, a mission cleared), shown one after another.
            std::vector<std::wstring> toasts;
            double toastUntil=0;
            // The album (C): open, the line of looks picked.
            bool album=false;
            int albumRow=0;
            double nextPoseAt=0;   // the result: when she next does her pose (thumb up, or "oh no")
            // The meteor's landing: where, how long ago (seconds; below 0 none), the camera's shake.
            V3 blastAt;
            float blastAge=-1,shake=0;
            float lastHeadX=-1e9f;   // her club head along the aim, the frame before (HeadAtBall)
            std::wstring measured;
            int groundVertices=0,shadedVertices=0;   // diagnostics: the ground's vertices, those in the trees' shade   // diagnostics: where her club head was found (for the report)   // diagnostics (--golf-special): your autoplayed shots use this special
            int menuPick=0;
            RectF menuBoxes[3];
            // Your putt's likely way (on the green, the strength that would roll the flat
            // distance to the cup), as far as it is shown.
            std::vector<V3> puttLine;
            std::vector<MascotRenderer::Ripple> ripples;   // on the pond (world units)
            std::vector<Drop> drops;
            // Diagnostics: frozen at a stage of the swing (-1: no), the camera round her.
            float posing=-1,poseYaw=0,posePower=1;
            bool posePutter=false;
            std::wstring swingVmd;   // --golf-swing-vmd: where to write her swing
            bool bonesView=false;    // --golf-bones: her skeleton drawn instead of her
            // The camera: where it looks from and at (world units), eased; the user's turn and
            // nearness on top.
            XMFLOAT3 eye{},look{};
            bool cut=true;
            struct Camera {float yaw=0,pitch=0,zoom=1;} camera;
            POINT dragFrom{};
            int dragging=0;             // 1 turning the camera (right or middle), 2 moving the view from above (middle, in the 0 view)
            POINT dragStart{};          // where the right button went down (a click without a drag: the 0 view)
            bool activating=false;      // the window was not active when this click came: it only brings the window up
            V3 overheadPan;             // the 0 view moved by a middle drag (metres)
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
            POINT beamTop{-1,-1};   // the top of the light pillar on the screen (x < 0: not seen)
            int frames=0,shots=0;
            std::wstring log;     // diagnostics: what was played
            bool askingQuit=false,overTold=false;
            RectF askButtons[2];
        } state;

        void Splash(V3 p);   // (below)
        void MeteorBlast(V3 p);   // (below)
        std::array<float,3> FlameColour(bool hers);   // (below)
        void Burst(V3 p,int n,const std::array<float,3>& rgb,float speed,float life,float size);   // (below)
        float JustWidth(int club);   // (below)

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
        // How many holes this round has, and the course's number for the h-th of them (0: now).
        int RoundLength(){return state.order.empty()?Holes:static_cast<int>(state.order.size());}
        int HoleNo(int h){return state.order.empty()?h+1:state.order[static_cast<size_t>(std::clamp(h,0,RoundLength()-1))];}
        int HoleNo(){return HoleNo(state.hole);}

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
        constexpr float SkyExposure=1.8f;   // GeometryPS lifts the sky picture's middle tones by this
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
            // The wood's shade on the ground under it (metres; 1 in the open), on a 2 m grid.
            float shadeX0=0,shadeZ0=0;int shadeW=0,shadeH=0;
            std::vector<float> shade;
            {
                const auto& trees=Course().Trees();
                const auto& shapes=Golf::TreeShapes();
                float x0=Course().MinX(),x1=Course().MaxX(),z0=Course().MinZ(),z1=Course().MaxZ();
                for(const auto& t:trees){x0=std::min(x0,t.x-12);x1=std::max(x1,t.x+12);z0=std::min(z0,t.z-12);z1=std::max(z1,t.z+12);}
                constexpr float Cell=2;
                shadeX0=x0;shadeZ0=z0;
                shadeW=static_cast<int>((x1-x0)/Cell)+2;shadeH=static_cast<int>((z1-z0)/Cell)+2;
                shade.assign(static_cast<size_t>(shadeW)*shadeH,1.0f);
                for(const auto& t:trees)
                {
                    const float r=std::max(2.0f,shapes[static_cast<size_t>(t.kind)].across*t.scale*1.3f);
                    const int i0=std::max(0,static_cast<int>((t.x-r-x0)/Cell)),i1=std::min(shadeW-1,static_cast<int>((t.x+r-x0)/Cell)+1);
                    const int j0=std::max(0,static_cast<int>((t.z-r-z0)/Cell)),j1=std::min(shadeH-1,static_cast<int>((t.z+r-z0)/Cell)+1);
                    for(int j=j0;j<=j1;++j)
                        for(int i=i0;i<=i1;++i)
                        {
                            const float dx=x0+i*Cell-t.x,dz=z0+j*Cell-t.z,d2=(dx*dx+dz*dz)/(r*r);
                            if(d2>=1)continue;
                            float& v=shade[static_cast<size_t>(j)*shadeW+i];
                            v*=1-0.55f*(1-d2);
                        }
                }
                for(float& v:shade)v=std::max(v,0.32f);
            }
            auto shadeAt=[&](float x,float z){
                if(shade.empty())return 1.0f;
                const float fx=std::clamp((x-shadeX0)/2,0.0f,static_cast<float>(shadeW-1)),fz=std::clamp((z-shadeZ0)/2,0.0f,static_cast<float>(shadeH-1));
                const int i=std::min(static_cast<int>(fx),shadeW-2),j=std::min(static_cast<int>(fz),shadeH-2);
                const float u=fx-i,v=fz-j;
                auto at=[&](int a,int b){return shade[static_cast<size_t>(b)*shadeW+a];};
                return (at(i,j)*(1-u)+at(i+1,j)*u)*(1-v)+(at(i,j+1)*(1-u)+at(i+1,j+1)*u)*v;
            };
            auto add=[&](const wchar_t* resource,XMFLOAT3 offset,float yaw=0,float scale=1,int group=-1){
                const bool ground=std::wstring(resource).rfind(L"HOLE",0)==0;
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
                    if(sky)c[0]=c[1]=c[2]=SkyExposure;
                    if(water){c[0]=0.09f;c[1]=0.27f;c[2]=0.35f;}   // the clear water's colour, over its bottom
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
                        if(ground&&!water&&!sky)
                        {
                            // Darker under the trees.
                            const float k=shadeAt(g.position[0]/K,g.position[2]/K);
                            g.color[0]*=k;g.color[1]*=k;g.color[2]*=k;
                            ++state.groundVertices;if(k<0.9f)++state.shadedVertices;
                        }
                        g.wall[0]=g.wall[1]=g.wall[2]=g.wall[3]=0;
                        g.uv[0]=v[6];g.uv[1]=v[7];g.uv[3]=0;
                        if(water){g.uv[0]=g.uv[1]=0;g.uv[2]=-15;}   // GeometryPS's CourseWater
                        else if(picture>=0)g.uv[2]=2;
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
            {
                const std::wstring hole=L"HOLE"+std::to_wstring(Course().Hole());
                add(hole.c_str(),{0,0,0});
            }
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
        void PlaceToSwing(V3 ball,float aim,int club)
        {
            const bool putter=club==Golf::Putter;
            state.herYaw=aim-Pi/2;
            state.herPutting=putter;
            state.herClub=club;
            const auto& head=putter?state.putterHead:state.clubHead[static_cast<size_t>(club)];
            const float c=std::cos(state.herYaw),s=std::sin(state.herYaw);
            const float hx=head[0]*c+head[2]*s,hz=-head[0]*s+head[2]*c;
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

        // Each club at the address (she is posed there now): longer for the woods, shorter for
        // the wedges, each turned in her hands until its head rests on the ground; where that
        // head is, the ball goes.
        void MeasureClubs(MascotModel& model)
        {
            // Lengths against the 7 iron's (the measured one): about as real clubs go.
            constexpr float Share[Golf::ClubCount]={1.2f,1.14f,1.05f,1.0f,0.96f,0.95f,0.94f,1.0f};
            for(int c=0;c<Golf::ClubCount;++c)
            {
                const size_t k=static_cast<size_t>(c);
                state.clubLen[k]=state.clubLength*Share[k];
                state.clubTilt[k]=0;
                std::array<float,3> g{},d{};
                auto below=[&](float tilt){   // how far the head is under the ball's middle
                    if(!model.ClubInHand(g,d,tilt))return 0.0f;
                    return g[1]+d[1]*state.clubLen[k]-Golf::BallRadius*K*0.3f;
                };
                float lo=-0.6f,hi=0.6f;
                const float flo=below(lo),fhi=below(hi);
                if(flo*fhi<0)
                {
                    for(int i=0;i<30;++i)
                    {
                        const float mid=0.5f*(lo+hi),fm=below(mid);
                        if((fm<0)==(flo<0))lo=mid;else hi=mid;
                    }
                    // Of the two ways to bring it to the ground, the one nearer the hand's own.
                    state.clubTilt[k]=0.5f*(lo+hi);
                }
                std::array<float,3> palm{};
                if(model.ClubInHand(g,d,state.clubTilt[k],&palm))
                {
                    // (The ball at the middle of the face: a little toward the toe, out from her,
                    // and just in front of the face, toward the target.)
                    state.clubHead[k]={g[0]+d[0]*state.clubLen[k]+(Golf::BallRadius+0.004f)*K,0,g[2]+d[2]*state.clubLen[k]-(c<=1?0.05f:0.035f)*K};
                    if(c==3)state.faceSign=palm[0]>=0?1.0f:-1.0f;   // the face toward the target (+X) at the address
                }
                else state.clubHead[k]=state.head;
            }
        }
        // Her posed club head has come to the ball (along the aim: the model's +X).
        bool HeadAtBall()
        {
            if(!state.her.model)return true;
            std::array<float,3> g{},d{};
            const size_t k=static_cast<size_t>(state.herClub);
            const float len=state.herPutting?state.putterLength:state.clubLen[k];
            if(!state.her.model->ClubInHand(g,d,state.herPutting?0.0f:state.clubTilt[k]))return true;
            const float ball=state.herPutting?state.putterHead[0]:state.clubHead[k][0];
            // On it, or nearer it now than it will be next frame (the head goes as far again as
            // it came since the last: so the frame shown is the nearest).
            const float x=g[0]+d[0]*len+(Golf::BallRadius+0.004f)*K,step=x-state.lastHeadX;
            state.lastHeadX=x;
            return x>=ball||(step>0&&x+step*0.5f>=ball);
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
            // Held at the strike for a while: where the grip is and the club's way there give its
            // length (its head on the ground) and where the ball lies, so the head meets the
            // ball as she swings through (at the address it hovers or rests a little off it).
            // The putter's the same way, its own stroke.
            for(int putter=0;putter<2;++putter)
            {
                // Swung there from the address as in play (the hands' hold carries on from frame
                // to frame; the strike posed on its own held the club otherwise, and the ball
                // was left off the head).
                for(int i=0;i<20;++i){model.Swing(true,0,1,putter!=0);model.Update(1/30.0);}
                std::array<float,3> g0{},d0{};
                const bool atAddress=model.ClubInHand(g0,d0);
                for(int i=1;i<=40;++i){model.Swing(true,2.0f*i/40,1,putter!=0);model.Update(1/30.0);}
                std::array<float,3> middle{},club{};
                float& length=putter?state.putterLength:state.clubLength;
                std::array<float,3>& head=putter?state.putterHead:state.head;
                if(model.ClubInHand(middle,club)&&club[1]<-0.2f)
                {
                    length=std::clamp((middle[1]-Golf::BallRadius*K)/-club[1],0.6f*K,1.4f*K);
                    head={middle[0]+club[0]*length+(putter?(Golf::BallRadius+0.004f)*K:0.0f),0,middle[2]+club[2]*length-(putter?0.045f*K:0.0f)};
                }
                else head={0,0,-0.9f*K};
                if(atAddress)
                {
                    wchar_t t[200];
                    swprintf_s(t,L"golf: %ls head at the address %.2f,%.2f,%.2f, at the strike %.2f,%.2f m (from her)\n",putter?L"putter":L"club",
                               (g0[0]+d0[0]*length)/K,(g0[1]+d0[1]*length)/K,(g0[2]+d0[2]*length)/K,head[0]/K,head[2]/K);
                    state.measured+=t;
                }
            }
            for(int i=0;i<20;++i){model.Swing(true,0,1,false);model.Update(1/30.0);}
            MeasureClubs(model);
            // How much longer or shorter the head's way is on her posed arms than
            // GeneratedMotion::SwingHead reckons, stage by stage (her swing's timing is made from
            // the latter): near the strike her arms are at full stretch.
            {
                constexpr int N=60;
                state.pathScale.assign(N,1.0f);
                std::array<float,3> last{},lastR{};
                for(int i=0;i<=N;++i)
                {
                    const float stage=3.0f*i/N;
                    for(int k=0;k<(i==0?20:2);++k){model.Swing(true,stage,1);model.Update(1/30.0);}
                    std::array<float,3> g{},d{};
                    if(!model.ClubInHand(g,d)){state.pathScale.assign(N,1.0f);break;}
                    const std::array<float,3> h{g[0]+d[0]*state.clubLength,g[1]+d[1]*state.clubLength,g[2]+d[2]*state.clubLength};
                    const auto r=model.SwingHead(stage,1,false,state.clubLength);
                    if(i>0)
                    {
                        const float posed=std::sqrt((h[0]-last[0])*(h[0]-last[0])+(h[1]-last[1])*(h[1]-last[1])+(h[2]-last[2])*(h[2]-last[2]));
                        const float reckoned=std::sqrt((r[0]-lastR[0])*(r[0]-lastR[0])+(r[1]-lastR[1])*(r[1]-lastR[1])+(r[2]-lastR[2])*(r[2]-lastR[2]));
                        state.pathScale[i-1]=reckoned>0.02f?std::clamp(posed/reckoned,0.5f,1.5f):1.0f;
                    }
                    last=h;lastR=r;
                }
            }
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
            // The putt's likely way: struck at the strength that rolls the flat distance to the
            // cup, no wind; only its first PuttLineShown metres (a long putt's end is not shown).
            state.puttLine.clear();
            if(state.club==Golf::Putter&&b.lie==Lie::Green)
            {
                const float toPin=Golf::Flat(b.at,Course().Pin());
                Golf::Shot s;
                s.club=Golf::Putter;s.aim=state.aim;
                s.range=Golf::PutterRange(toPin,Course().Pin().y-b.at.y);
                s.power=std::clamp(toPin/std::max(s.range,0.1f),0.02f,1.0f);
                const Golf::Flight f=Golf::Fly(Course(),b.at,b.lie,s,{},true,1/60.0f);
                float along=0;
                for(size_t i=0;i<f.path.size();i+=2)
                {
                    if(!state.puttLine.empty())along+=Golf::Length(f.path[i]-state.puttLine.back());
                    state.puttLine.push_back(f.path[i]);
                    if(along>PuttLineShown)break;
                }
            }
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

        // The day's conditions.
        enum Weather:int {Sunny,Windy,Rain,Dusk,HardPin,WeatherCount};
        const wchar_t* WeatherName(int w)
        {
            switch(w)
            {
            case Windy:return T(L"強風",L"Windy");
            case Rain:return T(L"雨",L"Rain");
            case Dusk:return T(L"夕暮れ",L"Dusk");
            case HardPin:return T(L"難しいピン",L"Hard pins");
            default:return T(L"晴れ",L"Sunny");
            }
        }

        // Hole missions: the one for this hole, what it asks.
        enum Mission:int {OneOn,PinClose,FairwayKeep,ParOn,BirdieOrBetter,TwoPutts,CleanHole,MissionCount};
        const wchar_t* MissionText(int m)
        {
            switch(m)
            {
            case OneOn:return T(L"1打でグリーンに乗せる",L"On the green in one");
            case PinClose:return T(L"1打目でピンから5ヤード以内",L"Within 5 yards of the pin in one");
            case FairwayKeep:return T(L"ティーショットをフェアウェイに",L"Tee shot on the fairway");
            case ParOn:return T(L"パーオン（パーより2打少なくグリーンに）",L"Green in regulation");
            case BirdieOrBetter:return T(L"バーディー以上",L"Birdie or better");
            case TwoPutts:return T(L"2パット以内",L"Two putts or fewer");
            default:return T(L"池にもOBにも入れない",L"No water, no out of bounds");
            }
        }

        void SaveProgress(){GolfProgress::Save(state.progressFile,state.progress);}
        void Toast(const std::wstring& text){state.toasts.push_back(text);}
        // An achievement of yours: kept, told, and any look it unlocks named.
        void Achieve(int a)
        {
            if(state.autoplay||a<0||a>=GolfProgress::AchievementCount||state.progress.got[static_cast<size_t>(a)])return;
            state.progress.got[static_cast<size_t>(a)]=true;
            const auto& info=GolfProgress::InfoOf(a);
            std::wstring text=std::wstring(T(L"実績：",L"Achievement: "))+T(info.jp,info.en);
            for(int k=0;k<GolfProgress::LookKindCount;++k)
                for(const auto& look:GolfProgress::LooksOf(k))
                    if(look.needs==a)text+=std::wstring(T(L"　新しい見た目「",L"   new look: "))+T(look.jp,look.en)+T(L"」",L"");
            Toast(text);
            SaveProgress();
        }
        void MissionCleared()
        {
            if(state.missionDone||state.mission<0)return;
            state.missionDone=true;
            ++state.missionsDone;
            if(!state.autoplay){++state.progress.stars;SaveProgress();}
            Toast(std::wstring(T(L"ミッション達成！★　",L"Mission cleared! *   "))+MissionText(state.mission));
        }
        // Your ball came to rest (not in the cup): the missions and drives that count now.
        void YouSettled(bool penalty)
        {
            const Ball& b=state.balls[You];
            const auto& f=state.flight;
            if(penalty){state.holePenalty=true;state.roundPenalty=true;}
            const int par=Course().Par();
            const bool first=state.shotsHere==1;
            if(!penalty)
            {
                if(state.mission==OneOn&&first&&f.lie==Lie::Green)MissionCleared();
                if(state.mission==PinClose&&first&&Golf::Flat(f.rest,Course().Pin())<=5*YardM)MissionCleared();
                if(state.mission==FairwayKeep&&first&&(f.lie==Lie::Fairway||f.lie==Lie::Green))MissionCleared();
                if(state.mission==ParOn&&f.lie==Lie::Green&&b.strokes<=par-2)MissionCleared();
                if(state.shot.club==Golf::Driver&&Golf::Flat(state.shotFrom,f.rest)>=280*YardM)Achieve(GolfProgress::LongDrive);
            }
        }
        // You holed out in n.
        void YouHoled(int n,int par)
        {
            const auto& f=state.flight;
            const bool first=state.shotsHere==1;
            if(state.mission==OneOn&&first)MissionCleared();
            if(state.mission==PinClose&&first)MissionCleared();
            if(state.mission==FairwayKeep&&first)MissionCleared();
            if(state.mission==ParOn&&n<=par-1)MissionCleared();
            if(state.mission==BirdieOrBetter&&n<=par-1)MissionCleared();
            if(state.mission==TwoPutts&&state.putts<=2)MissionCleared();
            if(state.mission==CleanHole&&!state.holePenalty)MissionCleared();
            if(n==1)Achieve(GolfProgress::HoleInOne);
            if(n<=par-3)Achieve(GolfProgress::Albatross);
            else if(n==par-2)Achieve(GolfProgress::Eagle);
            else if(n==par-1)Achieve(GolfProgress::Birdie);
            if(state.shot.club!=Golf::Putter&&state.shotFromLie!=Lie::Green&&n>1)Achieve(GolfProgress::ChipIn);
            if(f.beamed)Achieve(GolfProgress::Lightfall);
            if(state.shot.spin==1)Achieve(GolfProgress::SpinIn);
            if(state.meteorUsed&&n<=par-2)Achieve(GolfProgress::MeteorEagle);
            if(state.shot.club==Golf::Putter&&Golf::Flat(state.shotFrom,Course().Pin())>=10*YardM)Achieve(GolfProgress::LongPutt);
            if(state.weather==HardPin&&n<=par-1)Achieve(GolfProgress::HardPinBirdie);
        }
        // The round is over: records, wins, her rank, the round's achievements.
        void RoundProgress()
        {
            if(state.autoplay)return;
            auto& pr=state.progress;
            const int you=Total(You),her=Total(Her),n=RoundLength();
            int par=0;
            bool overPar=false;
            for(int h=0;h<n;++h)
            {
                const int hp=Golf::HolePar(HoleNo(h)),strokes=state.card[You][static_cast<size_t>(h)];
                if(strokes<=0)continue;
                par+=hp;
                if(strokes>hp)overPar=true;
                int& best=pr.bestHole[static_cast<size_t>(HoleNo(h)-1)];
                if(best==0||strokes<best)best=strokes;
            }
            const int slot=n<=3?0:n<=6?1:2;
            if((n==3||n==6||n==9)&&(pr.bestRound[static_cast<size_t>(slot)]==0||you-par+100<pr.bestRound[static_cast<size_t>(slot)]))pr.bestRound[static_cast<size_t>(slot)]=you-par+100;
            ++pr.rounds;
            const bool won=you<her;
            if(won)
            {
                ++pr.wins;
                Achieve(GolfProgress::FirstWin);
                if(pr.rank>=9)Achieve(GolfProgress::RankTop);
                if(state.weather==Windy)Achieve(GolfProgress::WindWin);
                if(state.weather==Rain)Achieve(GolfProgress::RainWin);
            }
            if(state.weather==Dusk)Achieve(GolfProgress::DuskRound);
            if(n>=9&&you<par)Achieve(GolfProgress::UnderPar);
            if(n>=3&&!overPar)Achieve(GolfProgress::BogeyFree);
            if(n>=9&&!state.roundPenalty)Achieve(GolfProgress::DryRound);
            if(n>=6&&state.missionsDone>=n)Achieve(GolfProgress::AllMissions);
            // Her rank: up when you beat her, down when she beats you.
            const int before=pr.rank;
            if(won)pr.rank=std::min(9,pr.rank+1);
            else if(her<you)pr.rank=std::max(1,pr.rank-1);
            if(pr.rank>5)Achieve(GolfProgress::RankFive);
            if(pr.rank!=before)Toast(Fmt(T(L"{0}の段位：{1} → {2}",L"{0}'s rank: {1} -> {2}"),{HerName(),std::to_wstring(before),std::to_wstring(pr.rank)}));
            SaveProgress();
        }

        void StartHole()
        {
            // Each hole its own model: the pin in the model's own cup the first round, moved on
            // the green in later ones.
            state.ground=Golf::LoadGround(HoleNo());
            const bool hard=state.weather==HardPin;
            state.course=std::make_shared<const Golf::Course>(state.ground,state.round==0&&!hard?0u:state.firstSeed*7919u+static_cast<std::uint32_t>(HoleNo())+1u,hard);
            if(state.renderer&&state.clubhouse>0)
            {
                state.sceneryBuilt=true;
                BuildScenery(state.clubhouse);
            }
            const int par=Course().Par();
            state.pars[state.hole]=par;
            std::uniform_real_distribution<float> u(0,1);
            state.wind={state.weather==Windy?6.5f+u(state.random)*6.0f:u(state.random)*6.5f,u(state.random)*2*Pi};
            if(state.diagAce)state.wind={0,0};
            // This hole's mission (one that suits its par), and its tallies.
            {
                std::vector<int> fit={ParOn,BirdieOrBetter,TwoPutts,CleanHole};
                if(par==3){fit.push_back(OneOn);fit.push_back(PinClose);}
                else fit.push_back(FairwayKeep);
                state.mission=fit[std::min(fit.size()-1,static_cast<size_t>(u(state.random)*fit.size()))];
            }
            state.missionDone=false;state.holePenalty=false;state.meteorUsed=false;state.shotsHere=0;state.putts=0;
            for(Ball& b:state.balls){b={};b.at=Course().Tee();b.lie=Lie::Tee;}
            state.turn=You;
            state.club=Golf::Driver;
            state.aim=PinAim(Course().Tee())*0.4f;
            BuildMap();
            state.treeFade.clear();
            state.phase=Phase::Intro;
            state.phaseTime=0;
            state.cut=true;
            state.landingKnown=false;
            PlaceToWatch(Course().Tee(),state.aim);
            state.log+=Fmt(L"hole {0}: par {1}, {2} m, seed {3}, wind {4} m/s\n",{std::to_wstring(HoleNo()),std::to_wstring(par),std::to_wstring(static_cast<int>(Course().Length())),
                                                                              std::to_wstring(Course().Seed()),std::to_wstring(static_cast<int>(state.wind.speed*10)/10.0f).substr(0,3)});
            Banner(Fmt(T(L"{0}番ホール　パー{1}",L"Hole {0}   Par {1}"),{std::to_wstring(HoleNo()),std::to_wstring(par)}),
                   YardText(Course().Length()),3.0);
        }

        void NewRound()
        {
            if(state.thinking.valid())state.thinking.wait();
            state.card={};
            state.pars.fill(0);
            // The holes: the first ones in order, or as many drawn at random (in a random order);
            // a diagnostic start (--golf-hole) plays from there to the last.
            state.order.clear();
            if(state.round==0&&state.startHole>0)
                for(int h=state.startHole+1;h<=Holes;++h)state.order.push_back(h);
            else if(state.roundRandom)
            {
                std::vector<int> all;
                for(int h=1;h<=Holes;++h)all.push_back(h);
                std::shuffle(all.begin(),all.end(),state.random);
                state.order.assign(all.begin(),all.begin()+std::clamp(state.roundHoles,1,Holes));
            }
            else for(int h=1;h<=std::clamp(state.roundHoles,1,Holes);++h)state.order.push_back(h);
            state.hole=0;
            state.started=false;
            state.overTold=false;
            state.askingQuit=false;
            state.afterword.clear();
            state.afterwordLine.clear();
            state.words.text.clear();
            if(!state.firstSeed)state.firstSeed=static_cast<std::uint32_t>(GetTickCount64()*2654435761ull);
            // A clubhouse of the four, this round's (at each hole's own place for it).
            state.clubhouse=1+static_cast<int>((state.firstSeed/7)%4);
            // The day: mostly fine, now and then windy, wet, late or with hard pins.
            {
                std::uniform_real_distribution<float> u(0,1);
                const float d=u(state.random);
                state.weather=d<0.4f?Sunny:d<0.58f?Windy:d<0.74f?Rain:d<0.88f?Dusk:HardPin;
                if(state.forcedWeather>=0)state.weather=state.forcedWeather;
                Golf::SetWet(state.weather==Rain);
                if(state.renderer)state.renderer->SetLight(35,state.weather==Dusk?12.0f:50.0f);
            }
            // How hard she plays: her rank (raised by your wins).
            state.care=0.55f+0.05f*static_cast<float>(state.progress.rank);
            state.squareRun=0;state.roundPenalty=false;state.missionsDone=0;
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
                Banner(Fmt(T(L"{0}番ホール終了",L"Hole {0} done"),{std::to_wstring(HoleNo())}),
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
                // Down the fairway's way rather than at a far-off pin round a bend (or over the wood).
                if(toPin>120)
                {
                    const V3 to=Course().AimPoint(b.at,std::min(230.0f,toPin));
                    state.aim=std::atan2(to.x-b.at.x,to.z-b.at.z);
                }
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
                PlaceToSwing(b.at,PinAim(b.at),b.lie==Lie::Green?Golf::Putter:3);
            }
        }

        void Launch()
        {
            // Diagnostics: where her club head is (as last posed) when the ball leaves, against
            // where the ball was laid for her.
            if(state.turn==Her&&state.her.model&&state.measured.size()<4000)
            {
                std::array<float,3> g{},d{};
                const bool putt=state.shot.club==Golf::Putter;
                const size_t k=static_cast<size_t>(state.herClub);
                const float len=putt?state.putterLength:state.clubLen[k];
                const auto& want=putt?state.putterHead:state.clubHead[k];
                if(state.her.model->ClubInHand(g,d,putt?0.0f:state.clubTilt[k]))
                {
                    wchar_t t[200];
                    swprintf_s(t,L"golf: her strike (%ls, power %.2f): head %.2f,%.2f,%.2f, ball %.2f,%.2f m\n",putt?L"putter":L"club",state.swingPower,
                               (g[0]+d[0]*len)/K,(g[1]+d[1]*len)/K,(g[2]+d[2]*len)/K,want[0]/K,want[2]/K);
                    state.measured+=t;
                }
            }
            Ball& b=state.balls[state.turn];
            state.shotFrom=b.at;
            state.shotFromLie=b.lie;
            state.flight=Golf::Fly(Course(),b.at,b.lie,state.shot,state.wind,true,1/60.0f);
            state.flightTime=0;
            state.launched=true;
            state.justShot=state.shot.club!=Golf::Putter&&std::abs(state.shot.accuracy)<0.08f;
            state.splashed=false;
            state.spunShown=state.boomShown=state.beamShown=false;
            if(state.turn==You)
            {
                ++state.shotsHere;
                if(state.shot.club==Golf::Putter)++state.putts;
                else
                {
                    state.squareRun=std::abs(state.shot.accuracy)<JustWidth(state.shot.club)?state.squareRun+1:0;
                    if(state.squareRun>=5)Achieve(GolfProgress::JustStreak);
                }
                if(state.shot.special==2)state.meteorUsed=true;
            }
            // The special gauge: each shot fills it some, a square strike more (two square
            // strikes fill a bar), a spin shot more again.
            if(state.shot.club!=Golf::Putter)
            {
                float& m=state.meter[static_cast<size_t>(state.turn)];
                m=std::min(200.0f,m+25+(std::abs(state.shot.accuracy)<JustWidth(state.shot.club)?25.0f:0.0f)+(state.shot.spin!=0?15.0f:0.0f));
            }
            if(state.shot.special!=0||state.shot.spin!=0)
            {
                std::wstring what=state.shot.special==2?T(L"メテオショット",L"Meteor shot"):state.shot.special==1?T(L"ブーストショット",L"Boost shot"):L"";
                const std::wstring spinName=state.shot.spin==1?T(L"バックスピン",L"backspin"):state.shot.spin==-1?T(L"トップスピン",L"topspin"):L"";
                if(!spinName.empty())what=what.empty()?spinName+T(L"ショット",L" shot"):what+L" + "+spinName;
                Banner(what,L"",1.4);
            }
            b.teed=true;
            ++b.strokes;
            ++state.shots;
            state.phase=Phase::Flying;
            state.phaseTime=0;
            wchar_t line[200];
            swprintf_s(line,L"  %ls: %ls power %.2f accuracy %+.2f aim %.1f spin %d special %d -> %ls%ls%ls%ls carry %.0f m, %.1f m left\n",state.turn==You?L"you":L"her",Golf::ClubOf(state.shot.club).en,
                       state.shot.power,state.shot.accuracy,state.shot.aim/Deg,state.shot.spin,state.shot.special,Golf::LieName(state.flight.lie),state.flight.holed?L" HOLED":L"",state.flight.water?L" WATER":state.flight.out?L" OB":L"",
                       state.flight.beamed?L" BEAM":L"",state.flight.carry,Golf::Flat(state.flight.rest,Course().Pin()));
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
                if(!hers)YouHoled(n,par);
                if(hers){Face(Expression::Smile,3);Happened(7,L"カップイン",1,Fmt(T(L"ゴルフで、{0}がカップに入れた（{1}打）。",L"In the golf game, {0} holed out ({1} strokes)."),{name,std::to_wstring(n)}),{T(L"ナイスイン！",L"Nice putt!")});}
                else{Face(Expression::Smile,2.5);Happened(7,L"あなたのカップイン",1,Fmt(T(L"ゴルフで、持ち主がカップに入れた（{0}打）。",L"In the golf game, the owner holed out ({0} strokes)."),{std::to_wstring(n)}),{T(L"入った！",L"In!")});}
                return;
            }
            const bool penalty=f.out||f.water;
            if(!hers)YouSettled(penalty);
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
                if(!putt)Banner(Golf::LieName(b.lie),Fmt(T(L"{0}　ピンまで {1}",L"{0}   {1} to the pin"),{YardText(Golf::Flat(state.shotFrom,f.rest)),YardText(Golf::Flat(f.rest,Course().Pin()))}),2.0);
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
            // She stands by the last green, turned to the camera (PlaceCamera: Over).
            PlaceToWatch(Course().Pin(),PinAim(Course().Tee()));
            if(state.her.model)state.her.model->Swing(false,0,1);
            state.nextPoseAt=0.8;   // once the camera has come round to her
            RoundProgress();
            const int you=Total(You),her=Total(Her);
            const std::wstring name=HerName();
            int par=0;   // over the holes played (a round may start partway, --golf-hole)
            for(int h=0;h<RoundLength();++h)if(state.card[You][static_cast<size_t>(h)]>0||state.card[Her][static_cast<size_t>(h)]>0)par+=Golf::HolePar(HoleNo(h));
            auto vsPar=[&](int n){const int d=n-par;return d==0?std::wstring(L"±0"):(d>0?L"+":L"")+std::to_wstring(d);};
            Banner(you<her?T(L"あなたの勝ち！",L"You win!"):her<you?name+T(L"の勝ち",L" wins"):std::wstring(T(L"引き分け",L"A draw")),
                   Fmt(T(L"あなた {0}（{1}）　{2} {3}（{4}）　Enterでもう一度・Escで閉じる",L"You {0} ({1})   {2} {3} ({4})   Enter: again, Esc: close"),
                       {std::to_wstring(you),vsPar(you),name,std::to_wstring(her),vsPar(her)}),1e9);
            const bool won=her<you;
            if(won)Happened(9,L"勝った",1,Fmt(T(L"{0}と持ち主のゴルフが終わり、{0}が勝った。",L"The golf round between {0} and the owner is over: {0} won."),{name}),{T(L"おつかれさま！",L"Well played!")});
            else if(you<her)Happened(9,L"負けた",1,Fmt(T(L"{0}と持ち主のゴルフが終わり、持ち主が勝った。",L"The golf round between {0} and the owner is over: the owner won."),{name}),{T(L"おめでとう！",L"Congratulations!")});
            else Happened(9,L"引き分け",1,Fmt(T(L"{0}と持ち主のゴルフが終わり、引き分けだった。",L"The golf round between {0} and the owner is over: a draw."),{name}),{T(L"いい勝負！",L"A close one!")});
            state.afterword=Fmt(T(L"いま、話し相手とゴルフを{3}ホール回って、デスクトップに戻ってきたところ。結果は{0}（あなた{1}打、相手{2}打）。その感想を、話し相手にひと言（1〜2文）",
                                  L"You have just played {3} holes of golf with them and come back to the desktop. {0} (you took {1} strokes, they took {2}). Tell them what you thought of it (one or two sentences)"),
                                {won?T(L"あなたの勝ち",L"You won"):you<her?T(L"あなたの負け",L"You lost"):T(L"引き分け",L"A draw"),std::to_wstring(her),std::to_wstring(you),std::to_wstring(RoundLength())});
            state.afterwordLine=won?T(L"ゴルフ、楽しかったね！",L"Golf was fun!"):T(L"くやしいなあ、次は勝つよ！",L"So close... I'll win next time!");
            if(!state.overTold){state.overTold=true;if(state.her.onGameOver)state.her.onGameOver();}
        }

        // The gauge's speed for your shot: the driver off a poor lie is hard to time (twice as
        // fast from the rough, four times from a bunker).
        // The gauge's speed for your shot from a poor lie: the longer the club, the harder to
        // time (from the rough 2x with the driver down to 1x at the 9 iron; from a bunker twice
        // the rough's with the driver, 4x, down to 1x as well).
        float GaugeHaste()
        {
            const Lie lie=state.balls[You].lie;
            if(state.club==Golf::Putter||(lie!=Lie::Rough&&lie!=Lie::Bunker))return 1;
            const float t=std::clamp((4.0f-static_cast<float>(state.club))/4.0f,0.0f,1.0f);   // 1W 1 .. 9I 0
            const float rough=1+t;
            return lie==Lie::Bunker?rough*rough:rough;
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
        void LeaveOverhead();
        // The special shots' menu (S), on your turn while aiming with a club.
        void OpenSpecialMenu()
        {
            if(state.turn!=You||state.phase!=Phase::Aim||state.gauge!=GaugeState::Ready||state.club==Golf::Putter||state.autoplay)return;
            state.menuOpen=true;
            state.menuPick=state.special>0?state.special-1:(state.meter[You]>=100?0:2);
        }
        // A line of it taken: 0 the boost, 1 the meteor (each a bar of the gauge), 2 none.
        void ChooseSpecial(int i)
        {
            if(i<2&&state.meter[You]<100)return;
            state.special=i<2?i+1:0;
            state.menuOpen=false;
            UpdateLanding();
        }

        // The 0 view (your landing circle from above): on, or off back to the usual view as it
        // was at the start of the turn.
        void ToggleOverhead()
        {
            if(state.overhead){LeaveOverhead();return;}
            if(state.turn!=You||(state.phase!=Phase::Aim&&state.phase!=Phase::Gauge)||state.autoplay)return;
            state.overhead=true;
            state.overheadPan={};
            state.camera={};
        }
        void LeaveOverhead()
        {
            state.overhead=false;
            state.overheadPan={};
            state.camera={};
        }

        void GaugeClick()
        {
            if(state.phase!=Phase::Aim&&state.phase!=Phase::Gauge)return;
            if(state.turn!=You||state.askingQuit||state.autoplay)return;
            switch(state.gauge)
            {
            case GaugeState::Ready:
                state.gaugeSpin=0;
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
                // A square strike at half power or more, struck low with ↓ held: the back-spin
                // shot; struck high with ↑ held: the top-spin shot.
                state.gaugeSpin=0;
                if(state.gaugePower>=0.5f&&std::abs(state.gaugeAccuracy)<JustWidth(state.club))
                {
                    if(state.spotY<=-0.3f&&GetKeyState(VK_DOWN)<0)state.gaugeSpin=1;
                    else if(state.spotY>=0.3f&&GetKeyState(VK_UP)<0)state.gaugeSpin=-1;
                }
                break;
            default:break;
            }
        }

        // Diagnostics (--golf-ace): a meteor with backspin from your tee that goes in, found by
        // trying clubs, powers and aims (coarse, then finer round the nearest).
        void FindAce()
        {
            // The meteor's speed is searched for itself (as the game works it out from the plain
            // shot's roll, the distance jumps about with the power); the power sets the spin back.
            state.wind={0,0};
            const Ball& b=state.balls[You];
            const V3 pin=Course().Pin();
            Golf::Shot best;float bestMiss=1e9f;
            auto tryShot=[&](int club,float meteor,float aim,float power=0.85f){
                Golf::Shot s;s.club=club;s.power=std::clamp(power,0.5f,1.0f);s.aim=aim;s.accuracy=0;s.spotY=-0.5f;s.spin=1;s.special=2;s.meteor=std::max(meteor,0.2f);
                const Golf::Flight f=Golf::Fly(Course(),b.at,b.lie,s,state.wind,true,1/60.0f);
                // (Through the light pillar just above the cup, down behind it, spun back in.)
                // Not through the pillar: how near it came over the cup (a few metres up);
                // through it: how near it came to the cup spinning back.
                float miss;
                if(f.holed&&f.beamed&&f.spunAt>=0)miss=-1;
                else if(f.water||f.out||(f.holed&&f.spunAt<0))miss=1e8f;
                else if(!f.beamed)
                {
                    miss=1e7f;
                    for(const V3& q:f.path)
                    {
                        const float up=q.y-Course().Height(q.x,q.z);
                        if(up>0.3f&&up<5)miss=std::min(miss,10+Golf::Flat(q,pin));
                    }
                }
                else
                {
                    miss=5+Golf::Flat(f.rest,pin);
                    if(f.spunAt>=0)
                        for(size_t i=static_cast<size_t>(f.spunAt);i<f.path.size();++i)miss=std::min(miss,Golf::Flat(f.path[i],pin));
                }
                if(miss<bestMiss){bestMiss=miss;best=s;}
            };
            for(int club=0;club<Golf::Putter;++club)
                for(float m=0.4f;m<=2.5f;m+=0.02f)
                    for(float a=-0.1f;a<=0.1f;a+=0.004f)tryShot(club,m,a);
            for(int round=0;round<4&&bestMiss>=0;++round)
                for(float step=0.01f;step>0.0002f&&bestMiss>=0;step*=0.5f)
                {
                    const Golf::Shot c=best;
                    for(int i=-5;i<=5;++i)for(int j=-5;j<=5;++j)for(int k=-2;k<=2;++k)tryShot(c.club,c.meteor+i*step,c.aim+j*step*0.25f,c.power+k*step*4);
                }
            state.club=best.club;state.aim=best.aim;state.gaugePower=best.power;state.gaugeAccuracy=0;
            state.spotX=0;state.spotY=-0.5f;state.gaugeSpin=1;state.special=2;state.meter[You]=100;state.diagMeteor=best.meteor;
            state.measured+=Fmt(L"golf: ace search: club {0}, meteor {1}, aim {2} deg, miss {3} m\n",
                                {std::to_wstring(best.club),std::to_wstring(best.meteor),std::to_wstring(best.aim/Deg),std::to_wstring(bestMiss)});
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
            if(state.club!=Golf::Putter)
            {
                state.shot.spotX=state.spotX;state.shot.spotY=state.spotY;
                state.shot.spin=state.gaugeSpin;
                // The special chosen, paid from the gauge.
                if(state.special>0&&state.meter[You]>=100)
                {
                    state.shot.special=state.special;
                    state.meter[You]-=100;
                    if(state.special==2&&state.diagMeteor>0){state.shot.meteor=state.diagMeteor;state.diagMeteor=0;}
                }
            }
            state.special=0;
            state.gaugeSpin=0;
            Launch();
        }

        // Her care now: less while she is well ahead.
        float CareNow()
        {
            const int lead=(Total(You)+state.balls[You].strokes)-(Total(Her)+state.balls[Her].strokes);
            return std::clamp(state.care-(lead>=4?0.3f:lead>=2?0.15f:0.0f),0.3f,1.0f);
        }

        SwingPlan MakePlan(MascotModel& model,float power,bool putter,float launch)
        {
            SwingPlan plan;
            plan.path.assign(3*SwingPlan::PerStage+1,0);
            std::array<float,3> last=model.SwingHead(0,power,putter,state.clubLength);
            for(int i=1;i<=3*SwingPlan::PerStage;++i)
            {
                const auto h=model.SwingHead(static_cast<float>(i)/SwingPlan::PerStage,power,putter,state.clubLength);
                const size_t seg=std::min(state.pathScale.size()-1,static_cast<size_t>((i-0.5f)/SwingPlan::PerStage*state.pathScale.size()/3));
                plan.path[i]=plan.path[i-1]+(state.pathScale.empty()?1.0f:state.pathScale[seg])*std::sqrt((h[0]-last[0])*(h[0]-last[0])+(h[1]-last[1])*(h[1]-last[1])+(h[2]-last[2])*(h[2]-last[2]))/K;
                last=h;
            }
            const float v=std::max(launch,0.3f);
            const float down=plan.path[2*SwingPlan::PerStage]-plan.path[SwingPlan::PerStage];
            const float through=plan.path[3*SwingPlan::PerStage]-plan.path[2*SwingPlan::PerStage];
            plan.down=std::clamp(3*down/v,0.12f,1.2f);
            plan.through=std::clamp(3*through/v,0.15f,1.5f);
            plan.speed=3*down/static_cast<float>(plan.down);
            plan.back=putter?std::clamp(plan.down*1.5,0.35,1.2):0.65+0.3*power;
            plan.top=putter?0.05:0.1;
            return plan;
        }
        // The stage where the head has gone so far along its path.
        float StageAt(const SwingPlan& plan,float metres)
        {
            const auto& p=plan.path;
            const auto it=std::lower_bound(p.begin(),p.end(),metres);
            if(it==p.begin())return 0;
            if(it==p.end())return 3;
            const size_t i=static_cast<size_t>(it-p.begin());
            const float a=p[i-1],b=p[i],u=b>a?(metres-a)/(b-a):1;
            return (static_cast<float>(i-1)+u)/SwingPlan::PerStage;
        }
        float SwingStage(const SwingPlan& plan,double t,double addressEnd)
        {
            if(t<addressEnd||plan.path.empty())return 0;
            t-=addressEnd;
            const float* p=plan.path.data();
            const int N=SwingPlan::PerStage;
            if(t<plan.back){const float u=static_cast<float>(t/plan.back);return StageAt(plan,p[N]*u*u*(3-2*u));}
            t-=plan.back;
            if(t<plan.top)return 1;
            t-=plan.top;
            if(t<plan.down){const float u=static_cast<float>(t/plan.down);return StageAt(plan,p[N]+(p[2*N]-p[N])*u*u*u);}
            t-=plan.down;
            if(t<plan.through){const float u=static_cast<float>(t/plan.through);return StageAt(plan,p[2*N]+(p[3*N]-p[2*N])*(1-(1-u)*(1-u)*(1-u)));}
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
                // The opening line suits the day (sunny ones have the plain scene).
                const wchar_t* opening=state.weather==Rain?L"雨の始まり":state.weather==Dusk?L"夕暮れの始まり":state.weather==Windy?L"強風の始まり":state.weather==HardPin?L"難ピンの始まり":L"始まり";
                Happened(7,opening,1,Fmt(T(L"{0}と、このパソコンの持ち主のゴルフ（{1}ホール、{2}）が始まる。",L"A {1}-hole round of golf between {0} and the owner of this computer is about to begin ({2})."),{HerName(),std::to_wstring(RoundLength()),WeatherName(state.weather)}),
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
                    // The 0 view: up and down move it along your aim (Shift with them: nearer, further).
                    if(state.overhead&&GetKeyState(VK_CONTROL)>=0&&GetKeyState(VK_SHIFT)>=0)
                    {
                        const float ud=(GetKeyState(VK_UP)<0?1.0f:0.0f)-(GetKeyState(VK_DOWN)<0?1.0f:0.0f);
                        if(ud!=0)
                        {
                            const float high=(state.club==Golf::Putter?9.0f:38.0f)*state.camera.zoom;
                            const V3 dir{std::sin(state.aim),0,std::cos(state.aim)};
                            state.overheadPan=state.overheadPan+dir*(ud*high*0.9f*static_cast<float>(dt));
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
                        if(state.diagSpecial>0&&s.club!=Golf::Putter){state.special=state.diagSpecial;state.meter[You]=100;}
                        if(state.diagAce&&state.balls[You].strokes==0)FindAce();
                    }
                    if(state.frozenPower>=0&&state.phaseTime>0.5)
                    {
                        const std::wstring& f=state.frozenFlags;
                        if(f.find(L"putter")!=std::wstring::npos&&state.club!=Golf::Putter){state.club=Golf::Putter;UpdateLanding();}
                        if(f.find(L"spot")!=std::wstring::npos&&state.spotY==0){state.spotX=-0.5f;state.spotY=-0.5f;UpdateLanding();}
                        if(f.find(L"green")!=std::wstring::npos&&state.balls[You].lie!=Lie::Green)
                        {
                            // Your ball put on the green, 7 m short of the pin (to see a putt).
                            const V3 pin=Course().Pin(),from=Course().Tee();
                            V3 d=from-pin;d.y=0;d=d*(7/std::max(Golf::Length(d),1.0f));
                            state.balls[You].at=pin+d;state.balls[You].at.y=Course().Height(state.balls[You].at.x,state.balls[You].at.z);
                            state.balls[You].lie=Course().LieAt(state.balls[You].at.x,state.balls[You].at.z);
                            state.club=Golf::Putter;state.aim=std::atan2(-d.x,-d.z)+0.06f;UpdateLanding();
                        }
                        if(f.find(L"album")!=std::wstring::npos){state.album=true;state.albumRow=1;}
                        if(f.find(L"menu")!=std::wstring::npos&&!state.menuOpen){state.meter[You]=130;state.autoplay=false;OpenSpecialMenu();}
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
                        state.gaugeAt+=state.gaugeWay*static_cast<float>(dt/PowerSeconds)*GaugeHaste();
                        if(state.gaugeAt>=1){state.gaugeAt=2-state.gaugeAt;state.gaugeWay=-1;}
                        if(state.gaugeAt<=0&&state.gaugeWay<0){state.gaugeAt=0;state.gauge=GaugeState::Ready;state.phase=Phase::Aim;}
                    }
                    else if(state.gauge==GaugeState::Impact)
                    {
                        state.gaugeAt-=static_cast<float>(dt/ImpactSeconds)*GaugeHaste();
                        // Past the end with no second press: no shot (back to aiming).
                        if(state.gaugeAt<-Pink-0.08f){state.gaugeAt=0;state.gauge=GaugeState::Ready;state.phase=Phase::Aim;}
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
                        // Her boost: when even her full shot falls short of the pin and she has a bar.
                        if(state.shot.club!=Golf::Putter&&state.meter[Her]>=100&&state.shot.power>=0.95f&&
                           Golf::Flat(state.balls[Her].at,Course().Pin())>Golf::FlatCarry(state.shot.club)*1.15f)
                        {
                            state.shot.special=1;
                            state.meter[Her]-=100;
                        }
                        state.swingPower=state.shot.club==Golf::Putter?std::clamp(0.12f+0.88f*state.shot.power,0.12f,1.0f):std::clamp(0.45f+0.55f*state.shot.power,0.3f,1.0f);
                        PlaceToSwing(state.balls[Her].at,state.shot.aim,state.shot.club);
                        if(state.her.model)
                            state.plan=MakePlan(*state.her.model,state.swingPower,state.shot.club==Golf::Putter,Golf::LaunchSpeed(state.shot,state.balls[Her].lie));
                        state.phase=Phase::Swing;
                        state.swingTime=0;
                    }
                }
                break;
            case Phase::Swing:
            {
                const double before=state.swingTime;
                state.swingTime+=dt;
                const double strike=Address+state.plan.Strike();
                // The ball leaves on the frame her posed club head has come to it (near the
                // strike the head goes some 0.6 m a frame: by the clock alone it left with the
                // head still short of it).
                (void)before;
                const bool atBall=HeadAtBall();   // (every frame, so it knows the head's last step)
                if(state.swingTime>=strike-0.08&&(atBall||state.swingTime>=strike+0.1))Launch();
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
                {
                    const auto& f=state.flight;
                    const double at=state.flightTime*60;
                    if(f.spunAt>=0&&!state.spunShown&&at>=f.spunAt){state.spunShown=true;Burst(f.path[static_cast<size_t>(f.spunAt)],40,{1.0f,0.95f,0.55f},2.5f,0.8f,0.03f);}
                    if(f.beamed&&!state.beamShown&&at>=f.beamAt){state.beamShown=true;Burst(f.path[static_cast<size_t>(std::min<size_t>(f.beamAt,f.path.size()-1))],60,{0.55f,1.0f,0.9f},3.0f,0.7f,0.02f);}
                    if(f.boomAt>=0&&!state.boomShown&&at>=f.boomAt)
                    {
                        // The meteor's landing: a blast of fire and sparks, and a ring of dust.
                        state.boomShown=true;
                        const V3 p=f.path[static_cast<size_t>(f.boomAt)];
                        MeteorBlast(p);
                    }
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
                    if(state.hole+1<RoundLength()){++state.hole;StartHole();}
                    else RoundOver();
                }
                break;
            case Phase::Over:
                // Her pose at the result, now and again: a thumb up if she won, a hand to the
                // forehead ("oh no") if she lost.
                if(state.her.model&&state.phaseTime>=state.nextPoseAt)
                {
                    const int you=Total(You),her=Total(Her);
                    if(you==her)state.nextPoseAt=1e9;
                    else if(state.her.model->EmoteSeated(her<you?L"thumbsup":L"oops",3.2))
                    {
                        state.her.model->SetExpression(her<you?Expression::Smile:Expression::Troubled,false);
                        state.nextPoseAt=state.phaseTime+7.0;
                    }
                }
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

        void PlaceCamera(double dt)
        {
            using namespace DirectX;
            const auto& c=Course();
            XMFLOAT3 eye{},look{};
            float follow=0.12f;
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
                // Over the hole from the tee toward the green: on a straight line from behind the
                // tee to partway down the fairway's way, eased only at its ends, so it never
                // swings from side to side (that made one feel sick on the bending holes).
                const float t0=static_cast<float>(std::min(state.phaseTime/4.0,1.0)),t=t0*t0*(3-2*t0);
                auto behind=[&](float along){
                    const V3 at=c.RouteAt(along),ahead=c.RouteAt(along+40);
                    V3 back=at-ahead;back.y=0;
                    const float n=std::max(Golf::Length(back),1e-3f);
                    return at+back*(70/n);
                };
                const V3 a0=behind(c.Length()*0.05f),a1=behind(c.Length()*0.6f);
                V3 from=a0+(a1-a0)*t;
                from.y=std::max(a0.y,a1.y)+38-14*t;
                set(from,c.GreenMiddle());
                follow=0.05f;
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
                    const V3 at=state.landing+state.overheadPan;
                    set(at-dir*(high*0.25f)+V3{0,high,0},at);
                }
                else if(state.phase==Phase::Over)
                {
                    // The result: her, from in front, her upper body and face.
                    const V3 front{-std::sin(state.herYaw),0,-std::cos(state.herYaw)};
                    const float a=state.camera.yaw;
                    const V3 side{std::cos(state.herYaw),0,-std::sin(state.herYaw)};
                    const V3 toEye=front*std::cos(a)+side*std::sin(a);
                    const V3 head=state.herAt+V3{0,1.1f,0};
                    set(head+toEye*(2.4f*state.camera.zoom)+V3{0,0.15f+state.camera.pitch*3,0},head);
                }
                else if(state.turn==You||state.phase==Phase::HoleOver)
                {
                    const V3 at=state.phase==Phase::HoleOver?c.Pin():b.at;
                    const float aim=state.phase==Phase::HoleOver?PinAim(c.Tee())+Pi*0.85f:state.aim;
                    const bool putt=state.club==Golf::Putter&&state.turn==You&&state.phase!=Phase::HoleOver;
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
                // A hole in one: in its last moments, down by the cup, the ball coming toward us.
                const auto& path=state.flight.path;
                if(state.flight.holed&&state.balls[state.turn].strokes==1&&path.size()>40)
                {
                    const double left=static_cast<double>(path.size()-1)/60-state.flightTime;
                    if(left<AceZoom||state.phase==Phase::Settle)
                    {
                        const V3 pin=c.Pin(),from=path[path.size()-30];
                        V3 toward=pin-from;toward.y=0;
                        const float n=Golf::Length(toward);
                        toward=n>0.05f?toward*(1/n):V3{std::sin(state.shot.aim),0,std::cos(state.shot.aim)};
                        const V3 side{toward.z,0,-toward.x};
                        V3 e=pin+toward*1.5f+side*0.45f+V3{0,0.38f,0};
                        e.y=std::max(e.y,c.Height(e.x,e.z)+0.25f);
                        set(e,pin-toward*0.5f+V3{0,0.03f,0});
                        follow=0.10f;
                        break;
                    }
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
            state.cutNow=state.cut;
            if(state.cut){state.eye=eye;state.look=look;state.cut=false;}
            else
            {
                // By the time passed (a fixed share each frame made the camera lag and catch up
                // unevenly as the frames' times varied: the ball in flight shook).
                const float k=static_cast<float>(1.0-std::exp(-std::clamp(dt,0.0,0.1)/std::max(0.02f,follow)));
                auto ease=[&](XMFLOAT3& a,const XMFLOAT3& b){a.x+=(b.x-a.x)*k;a.y+=(b.y-a.y)*k;a.z+=(b.z-a.z)*k;};
                ease(state.eye,eye);
                ease(state.look,look);
            }
            // The meteor's landing shakes the view a moment.
            XMFLOAT3 eyeNow=state.eye,lookNow=state.look;
            if(state.shake>0)
            {
                state.shake=std::max(0.0f,state.shake-static_cast<float>(dt));
                const float t=static_cast<float>(state.clock),a=state.shake*state.shake*0.9f*K;
                const float sx=std::sin(t*61)*a,sy=std::sin(t*47+1)*a,sz=std::sin(t*53+2)*a;
                eyeNow.x+=sx;eyeNow.y+=sy;eyeNow.z+=sz;lookNow.x+=sx;lookNow.y+=sy;lookNow.z+=sz;
            }
            state.renderer->SetCamera(XMLoadFloat3(&eyeNow),XMLoadFloat3(&lookNow),FovY());
        }

        // The trees in sight this frame, as the renderer's places for each kind: those within
        // the view's cone (with room for their size) or close by. A wood of several hundred is
        // drawn a few hundred at a time.
        void PlaceTrees(double dt)
        {
            using namespace DirectX;
            std::vector<std::vector<MascotRenderer::SceneryInstance>> groups(Golf::TreeKinds);
            const XMVECTOR eye=XMLoadFloat3(&state.eye);
            const XMVECTOR ahead=XMVector3Normalize(XMVectorSubtract(XMLoadFloat3(&state.look),eye));
            RECT rc{};
            GetClientRect(state.view?state.view:state.window,&rc);
            const float aspect=rc.bottom>rc.top?static_cast<float>(rc.right-rc.left)/static_cast<float>(rc.bottom-rc.top):1.6f;
            // To the view's corners (a cone as wide as the screen across left the corners out:
            // trees there sprang up as the view turned or came near).
            const float half=std::atan(std::tan(FovY()/2)*std::sqrt(1+aspect*aspect));
            const float coneAngle=std::min(1.5f,half+0.1f);
            const auto& trees=Course().Trees();
            if(state.treeFade.size()!=trees.size())state.treeFade.assign(trees.size(),0.0f);
            // Each tree fades in as it comes into view and out as it leaves (no popping).
            const float step=static_cast<float>(std::clamp(dt,0.0,0.1)/0.4);
            size_t shown=0;
            for(size_t i=0;i<trees.size();++i)
            {
                const auto& t=trees[i];
                const XMFLOAT3 at=W(V3{t.x,t.y,t.z});
                const XMVECTOR to=XMVectorSubtract(XMLoadFloat3(&at),eye);
                const float d=XMVectorGetX(XMVector3Length(to));
                const float reach=9*K*t.scale;   // a tree's size, round its foot
                bool seen=true;
                if(d>reach)
                {
                    const float c=XMVectorGetX(XMVector3Dot(to,ahead))/d;
                    // Within the cone widened by the angle the tree's size takes at that distance.
                    seen=c>=std::cos(std::min(1.55f,coneAngle+std::asin(std::min(1.0f,reach/d))));
                }
                float& fade=state.treeFade[i];
                if(state.cutNow)fade=seen?1.0f:0.0f;
                else fade=std::clamp(fade+(seen?step:-step),0.0f,1.0f);
                if(fade<=0)continue;
                MascotRenderer::SceneryInstance q;
                q.x=at.x;q.y=at.y-0.05f*K;q.z=at.z;q.yaw=t.yaw;q.scale=t.scale;q.clear=fade>=1?0.0f:1-fade;
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
        // flat: laid on the ground (across it level) rather than turned to the view.
        void LightStrip(std::vector<MascotRenderer::GeometryVertex>& out,XMFLOAT3 a,XMFLOAT3 b,float w,const float (&ca)[3],const float (&cb)[3],bool flat=false)
        {
            using namespace DirectX;
            const XMVECTOR A=XMLoadFloat3(&a),B=XMLoadFloat3(&b);
            const XMVECTOR toEye=flat?XMVectorSet(0,1,0,0):XMVectorSubtract(XMLoadFloat3(&state.eye),XMVectorScale(XMVectorAdd(A,B),0.5f));
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

        // What glows this frame: the light pillar up into the sky, and the
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
            // Your aim's circle where the ball comes down at full power: a faint magic circle laid
            // on the ground, turning (its rings, a six-pointed star one way, runes the other).
            if(state.turn==You&&(state.phase==Phase::Aim||state.phase==Phase::Gauge)&&state.landingKnown&&!state.autoplay)
            {
                const V3 l=state.landing;
                const bool putt=state.club==Golf::Putter;
                const float R=putt?0.2f:1.9f,lift=putt?0.012f:0.06f;
                const float t=static_cast<float>(state.clock),pulse=0.8f+0.2f*std::sin(t*2.2f);
                // Its design and colour, as you chose: the hexagram (gold), the pentagram (violet),
                // the flower (cherry), the cogwheel (blue).
                const int design=std::clamp(state.progress.chosen[GolfProgress::Circle],0,3);
                static const float hues[4][3]={{0.75f,0.60f,0.18f},{0.62f,0.35f,0.85f},{0.85f,0.45f,0.60f},{0.30f,0.55f,0.90f}};
                const float gold[3]={hues[design][0]*pulse,hues[design][1]*pulse,hues[design][2]*pulse},faint[3]={gold[0]*0.6f,gold[1]*0.6f,gold[2]*0.6f};
                // Seen from far off (the tee), its lines grow, so they are not lost under a pixel.
                const float far_=std::hypot(state.eye.x/K-l.x,state.eye.z/K-l.z),thick=std::max(1.0f,far_/(R*25));
                auto P=[&](float x,float z){return XMFLOAT3{x*K,(c.Height(x,z)+lift)*K,z*K};};
                auto polar=[&](float a,float r){return std::pair<float,float>{l.x+r*std::cos(a),l.z+r*std::sin(a)};};
                auto line=[&](std::pair<float,float> a,std::pair<float,float> b,float w,const float (&col)[3]){
                    // In pieces, each laid on the ground under it.
                    const int n=std::max(1,static_cast<int>(std::hypot(b.first-a.first,b.second-a.second)/(R*0.12f)));
                    for(int i=0;i<n;++i)
                    {
                        const float u0=static_cast<float>(i)/n,u1=static_cast<float>(i+1)/n;
                        LightStrip(out,P(a.first+(b.first-a.first)*u0,a.second+(b.second-a.second)*u0),P(a.first+(b.first-a.first)*u1,a.second+(b.second-a.second)*u1),w*thick*K,col,col,true);
                    }
                };
                auto ring=[&](float r,float w,const float (&col)[3]){
                    constexpr int N=48;
                    for(int i=0;i<N;++i)line(polar(2*Pi*i/N,r),polar(2*Pi*(i+1)/N,r),w,col);
                };
                ring(R,R*0.05f,gold);
                ring(R*0.88f,R*0.025f,faint);
                ring(R*0.52f,R*0.03f,gold);
                ring(R*0.16f,R*0.02f,faint);
                const float turn=t*0.35f,back=-t*0.55f;
                if(design==0)
                    for(int k=0;k<2;++k)   // the star: two triangles
                        for(int i=0;i<3;++i)
                        {
                            const float a0=turn+k*Pi/3+i*2*Pi/3,a1=a0+2*Pi/3;
                            line(polar(a0,R*0.88f),polar(a1,R*0.88f),R*0.016f,gold);
                        }
                else if(design==1)
                    for(int i=0;i<5;++i)   // the pentagram: every second of five points
                    {
                        const float a0=turn+i*2*Pi/5,a1=a0+4*Pi/5;
                        line(polar(a0,R*0.88f),polar(a1,R*0.88f),R*0.016f,gold);
                    }
                else if(design==2)
                    for(int i=0;i<6;++i)   // the flower: six petals, each a small ring
                    {
                        const float a=turn+i*Pi/3;
                        const auto c0=polar(a,R*0.45f);
                        constexpr int N=20;
                        for(int k=0;k<N;++k)
                        {
                            const float b0=2*Pi*k/N,b1=2*Pi*(k+1)/N;
                            line({c0.first+R*0.3f*std::cos(b0),c0.second+R*0.3f*std::sin(b0)},{c0.first+R*0.3f*std::cos(b1),c0.second+R*0.3f*std::sin(b1)},R*0.013f,gold);
                        }
                    }
                else
                    for(int i=0;i<18;++i)   // the cogwheel: teeth round a ring, spokes within
                    {
                        const float a0=turn+i*2*Pi/18,a1=a0+Pi/18,a2=a0+2*Pi/18;
                        line(polar(a0,R*0.72f),polar(a0,R*0.84f),R*0.014f,gold);
                        line(polar(a0,R*0.84f),polar(a1,R*0.84f),R*0.014f,gold);
                        line(polar(a1,R*0.84f),polar(a1,R*0.72f),R*0.014f,gold);
                        line(polar(a1,R*0.72f),polar(a2,R*0.72f),R*0.014f,gold);
                        if(i%3==0)line(polar(a0,R*0.18f),polar(a0,R*0.5f),R*0.012f,faint);
                    }
                for(int i=0;i<4;++i)   // a square inside, the other way
                {
                    const float a0=back+i*Pi/2,a1=a0+Pi/2;
                    line(polar(a0,R*0.52f),polar(a1,R*0.52f),R*0.012f,faint);
                }
                for(int i=0;i<24;++i)   // runes round the rim
                {
                    const float a=back+i*2*Pi/24;
                    const float r0=R*0.905f,r1=R*(i%3==0?0.985f:0.95f);
                    line(polar(a,r0),polar(a,r1),R*0.012f,gold);
                    if(i%3==1)line(polar(a-0.05f,R*0.94f),polar(a+0.05f,R*0.94f),R*0.01f,faint);
                }
            }
            // Your putt's likely way: dashes along the green, fading as they go.
            if(state.turn==You&&(state.phase==Phase::Aim||state.phase==Phase::Gauge)&&state.club==Golf::Putter&&state.puttLine.size()>1&&!state.autoplay)
            {
                float along=0;
                for(size_t i=1;i<state.puttLine.size();++i)
                {
                    const V3 a=state.puttLine[i-1],b=state.puttLine[i];
                    const float d=Golf::Length(b-a);
                    const float mid=along+d/2;
                    along+=d;
                    if(std::fmod(mid+1000-static_cast<float>(state.clock)*0.25f,0.3f)>0.17f)continue;   // dashes, drifting toward the cup
                    const float k=0.55f*(1-mid/PuttLineShown);
                    if(k<=0)break;
                    const float col[3]={0.75f*k,0.9f*k,1.0f*k};
                    LightStrip(out,XMFLOAT3{a.x*K,(c.Height(a.x,a.z)+0.012f)*K,a.z*K},XMFLOAT3{b.x*K,(c.Height(b.x,b.z)+0.012f)*K,b.z*K},0.025f*K,col,col,true);
                }
            }
            // The meteor's landing: two rings of light racing out over the ground, and a pillar of
            // fire going up, all fading.
            if(state.blastAge>=0&&state.blastAge<1.6f)
            {
                const V3 b=state.blastAt;
                for(int ring=0;ring<2;++ring)
                {
                    const float t=state.blastAge-ring*0.12f;
                    if(t<0)continue;
                    const float r=(ring==0?22.0f:13.0f)*(1-std::exp(-t*3.2f)),fade=std::max(0.0f,1-t/(ring==0?1.0f:1.4f));
                    const float col[3]={1.0f*fade,(ring==0?0.75f:0.45f)*fade,(ring==0?0.35f:0.12f)*fade};
                    constexpr int N=64;
                    for(int i=0;i<N;++i)
                    {
                        const float a0=2*Pi*i/N,a1=2*Pi*(i+1)/N;
                        const float x0=b.x+r*std::cos(a0),z0=b.z+r*std::sin(a0),x1=b.x+r*std::cos(a1),z1=b.z+r*std::sin(a1);
                        LightStrip(out,XMFLOAT3{x0*K,(c.Height(x0,z0)+0.15f)*K,z0*K},XMFLOAT3{x1*K,(c.Height(x1,z1)+0.15f)*K,z1*K},(ring==0?0.5f:0.9f)*K*(0.4f+fade),col,col,true);
                    }
                }
                const float t=state.blastAge,h=3+18*(1-std::exp(-t*4)),fade=std::max(0.0f,1-t/1.2f);
                const float core[3]={1.0f*fade,0.8f*fade,0.45f*fade},edge[3]={0.6f*fade,0.18f*fade,0.04f*fade};
                LightStrip(out,W(b),W(b,h),(2.2f+1.5f*t)*K,core,edge);
                LightStrip(out,W(b),W(b,h*0.7f),(0.9f+0.6f*t)*K,core,core);
            }
            // The trail: behind the ball all the way (bounces and rolling too), fading behind it,
            // as bright as the ball is fast. Its joints are on fixed frames of the path and its
            // head is the ball itself between frames, so nothing in it steps from frame to frame
            // (joints that moved with the newest frame made the head judder).
            if(state.phase==Phase::Flying&&state.flight.path.size()>1)
            {
                const auto& path=state.flight.path;
                const double nowT=std::min(state.flightTime*60,static_cast<double>(path.size()-1));
                constexpr double Long=150;   // frames of trail
                const double tailT=std::max(0.0,nowT-Long);
                const bool hers=state.turn==Her;
                float tint[3]={hers?1.0f:0.55f,hers?0.55f:0.8f,hers?0.8f:1.0f};
                const int trailLook=hers?0:state.progress.chosen[GolfProgress::Trail];
                {
                    static const float looks[4][3]={{0.55f,0.8f,1.0f},{1.0f,0.82f,0.35f},{0.35f,1.0f,0.6f},{0.8f,0.5f,1.0f}};
                    if(trailLook>0&&trailLook<4)for(int k=0;k<3;++k)tint[k]=looks[trailLook][k];
                }
                // The specials' trails: the boost gold, the meteor fire.
                if(state.shot.special==1){tint[0]=1.0f;tint[1]=0.85f;tint[2]=0.35f;}
                else if(state.shot.special==2){const auto fl=FlameColour(hers);tint[0]=fl[0];tint[1]=fl[1];tint[2]=fl[2];}
                const bool rainbow=trailLook==4&&state.shot.special==0;
                auto at=[&](double i){
                    const size_t a=static_cast<size_t>(i),b=std::min(a+1,path.size()-1);
                    return path[a]+(path[b]-path[a])*static_cast<float>(i-a);
                };
                auto speedAt=[&](double i){   // m/s, over 3 frames round i
                    const double a=std::max(0.0,i-1.5),b=std::min(static_cast<double>(path.size()-1),a+3);
                    return b>a?Golf::Length(at(b)-at(a))*60/static_cast<float>(b-a):0.0f;
                };
                double i=std::floor(tailT/3)*3;
                if(i<tailT)i=std::min(i+3,nowT);
                while(i<nowT)
                {
                    const double j=std::min(i+3,nowT);
                    const float age=static_cast<float>((i+j)/2-tailT)/static_cast<float>(Long);
                    const float k=std::clamp(speedAt((i+j)/2)/55.0f,0.0f,1.0f)*std::clamp(age,0.0f,1.0f)*0.9f;
                    if(k>=0.01f)
                    {
                        float tc[3]={tint[0],tint[1],tint[2]};
                        if(rainbow)
                        {
                            // The rainbow: the hue running along the trail.
                            const float hue=static_cast<float>(std::fmod(i/40.0,1.0))*6;
                            tc[0]=std::clamp(std::abs(hue-3)-1,0.0f,1.0f);tc[1]=std::clamp(2-std::abs(hue-2),0.0f,1.0f);tc[2]=std::clamp(2-std::abs(hue-4),0.0f,1.0f);
                        }
                        const float ca[3]={tc[0]*k,tc[1]*k,tc[2]*k};
                        LightStrip(out,W(at(i)),W(at(j)),(state.shot.special==2?0.12f+0.3f*age:0.03f+0.05f*age)*K,ca,ca);
                    }
                    i=j;
                }
            }
            // The rain: each drop a thin streak along its fall (a little light against the grey).
            for(const auto& d:state.drops)
            {
                if(!d.rain)continue;
                const float k=0.75f*std::min(1.0f,(d.life-d.age)*4);
                const float head[3]={0.20f*k,0.22f*k,0.25f*k},tail[3]={0,0,0};
                LightStrip(out,W(d.at),W(d.at-d.v*0.045f),0.010f*K,head,tail);
            }
            return out;
        }

        // The sparks this frame: rising round the light pillar, glittering along a square strike's
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
            // The meteor in the air breathes fire: flames streaming off behind it, rising as
            // they fade.
            if(state.phase==Phase::Flying&&state.shot.special==2&&state.flight.path.size()>2&&
               (state.flight.boomAt<0||state.flightTime*60<state.flight.boomAt))
            {
                const V3 p=BallNow(state.turn);
                const double i=std::min(state.flightTime*60,static_cast<double>(state.flight.path.size()-2));
                const V3 v=(state.flight.path[static_cast<size_t>(i)+1]-state.flight.path[static_cast<size_t>(i)])*60;
                std::uniform_real_distribution<float> u(0,1),pm(-1,1);
                // Its blazing core, white at the heart, and two ember spirals winding round its way.
                {
                    MascotRenderer::Particle q;
                    q.position={p.x*K,p.y*K,p.z*K};
                    const auto fl=FlameColour(state.turn==Her);
                    q.size=(0.9f+0.15f*std::sin(t*40))*K;q.color={std::min(1.0f,fl[0]+0.3f),std::min(1.0f,fl[1]+0.4f),std::min(1.0f,fl[2]+0.4f),0.9f};q.glow=true;
                    out.push_back(q);
                    q.size=0.45f*K;q.color={1.0f,1.0f,0.9f,1.0f};
                    out.push_back(q);
                    const V3 dir=Golf::Normalized(v);
                    V3 side=Golf::Normalized(Golf::Cross(dir,V3{0,1,0}));
                    if(Golf::Length(side)<0.5f)side={1,0,0};
                    const V3 up=Golf::Cross(side,dir);
                    for(int arm=0;arm<2;++arm)
                        for(int k=0;k<14;++k)
                        {
                            const float behind=k*0.35f,a=t*14+arm*Pi+k*0.55f,r=0.5f+0.05f*k;
                            const V3 at=p-dir*behind+side*(std::cos(a)*r)+up*(std::sin(a)*r);
                            MascotRenderer::Particle e;
                            e.position={at.x*K,at.y*K,at.z*K};
                            e.size=(0.22f-0.011f*k)*K;
                            e.color={arm==0?1.0f:0.55f,arm==0?0.55f:0.85f,arm==0?0.15f:1.0f,0.9f*(1-k/14.0f)};
                            e.glow=true;
                            out.push_back(e);
                        }
                }
                const int n=static_cast<int>(dt*360+u(state.random));
                for(int k=0;k<n;++k)
                {
                    Drop d;
                    const float back=u(state.random);
                    d.at=p-v*(back*0.02f)+V3{pm(state.random)*0.15f,pm(state.random)*0.15f,pm(state.random)*0.15f};
                    d.v=v*(-0.08f-0.1f*u(state.random))+V3{pm(state.random)*1.5f,1.0f+u(state.random)*1.5f,pm(state.random)*1.5f};
                    d.life=0.25f+0.35f*u(state.random);
                    d.size=0.14f+0.18f*u(state.random);
                    const float hot=u(state.random);
                    const auto fl=FlameColour(state.turn==Her);
                    d.rgb[0]=std::min(1.0f,fl[0]+0.3f*hot);d.rgb[1]=std::min(1.0f,fl[1]+0.4f*hot);d.rgb[2]=std::min(1.0f,fl[2]+0.3f*hot*hot);
                    d.fall=-2;   // flames rise
                    state.drops.push_back(d);
                }
            }
            // Rain: streaks falling round the camera.
            if(state.weather==Rain)
            {
                std::uniform_real_distribution<float> u(0,1);
                const int n=static_cast<int>(dt*1400+u(state.random));
                for(int k=0;k<n;++k)
                {
                    Drop d;
                    const float a=u(state.random)*2*Pi,r=1.5f+u(state.random)*22;
                    d.at=V3{state.eye.x/K+std::cos(a)*r,state.eye.y/K+4+u(state.random)*8,state.eye.z/K+std::sin(a)*r};
                    d.v={0.8f,-14,0.3f};
                    d.life=0.9f;d.size=0.022f;
                    d.rgb[0]=0.30f;d.rgb[1]=0.33f;d.rgb[2]=0.38f;d.fall=0;d.rain=true;
                    state.drops.push_back(d);
                }
            }
            // The splash: drops thrown up, falling back.
            for(auto& d:state.drops)
            {
                d.age+=static_cast<float>(dt);
                d.v.y-=d.fall*static_cast<float>(dt);
                d.at=d.at+d.v*static_cast<float>(dt);
            }
            state.drops.erase(std::remove_if(state.drops.begin(),state.drops.end(),[](const Drop& d){return d.age>d.life;}),state.drops.end());
            for(const auto& d:state.drops)
            {
                if(d.rain)continue;   // drawn as streaks (BuildGlow)
                MascotRenderer::Particle q;
                q.position={d.at.x*K,d.at.y*K,d.at.z*K};
                q.size=d.size*K;
                q.color={d.rgb[0],d.rgb[1],d.rgb[2],0.85f*(1-d.age/d.life)};
                q.glow=true;
                out.push_back(q);
            }
            return out;
        }

        // The meteor's fire, yours in the look you chose (hers orange).
        std::array<float,3> FlameColour(bool hers)
        {
            static const std::array<float,3> looks[4]={{1.0f,0.42f,0.12f},{0.30f,0.55f,1.0f},{0.75f,0.30f,1.0f},{0.30f,1.0f,0.45f}};
            return looks[hers?0:std::clamp(state.progress.chosen[GolfProgress::Flame],0,3)];
        }

        // Sparks thrown out from p (an effect): n of them in colour rgb, at up to speed m/s, lasting
        // about life seconds, size metres.
        void Burst(V3 p,int n,const std::array<float,3>& rgb,float speed,float life,float size)
        {
            std::uniform_real_distribution<float> u(0,1);
            for(int i=0;i<n;++i)
            {
                const float a=u(state.random)*2*Pi,up=u(state.random),out=std::sqrt(std::max(0.0f,1-up*up));
                const float sp=speed*(0.3f+0.7f*u(state.random));
                Drop d;
                d.at=p+V3{0,0.1f,0};
                d.v={std::cos(a)*out*sp,up*sp,std::sin(a)*out*sp};
                d.life=life*(0.6f+0.8f*u(state.random));
                d.size=size*(0.6f+0.8f*u(state.random));
                d.rgb[0]=rgb[0];d.rgb[1]=rgb[1];d.rgb[2]=rgb[2];
                d.fall=3;
                state.drops.push_back(d);
            }
        }

        // The meteor comes down at p: a white flash, a fireball, fire and sparks thrown up and
        // out, clods of earth flung high, smoke rolling up, the view shaken, rings racing out
        // (BuildGlow).
        void MeteorBlast(V3 p)
        {
            state.blastAt=p;state.blastAge=0;state.shake=0.9f;
            auto still=[&](float size,float life,const std::array<float,3>& rgb){
                Drop d;d.at=p+V3{0,0.6f,0};d.v={};d.life=life;d.size=size;d.rgb[0]=rgb[0];d.rgb[1]=rgb[1];d.rgb[2]=rgb[2];d.fall=0;
                state.drops.push_back(d);
            };
            still(9.0f,0.25f,{1.0f,1.0f,0.95f});    // the flash
            still(5.5f,0.6f,{1.0f,0.7f,0.3f});      // the fireball
            const auto fl=FlameColour(state.turn==Her);
            Burst(p,220,{fl[0],std::min(1.0f,fl[1]+0.1f),fl[2]},16.0f,1.2f,0.16f);
            Burst(p,160,{1.0f,0.9f,0.5f},10.0f,0.7f,0.07f);
            Burst(p,120,{0.55f,0.85f,1.0f},12.0f,0.9f,0.05f);
            std::uniform_real_distribution<float> u(0,1),pm(-1,1);
            for(int i=0;i<70;++i)   // clods, falling back hard
            {
                Drop d;d.at=p+V3{0,0.2f,0};
                d.v={pm(state.random)*7,9+u(state.random)*12,pm(state.random)*7};
                d.life=1.4f+u(state.random);d.size=0.12f+0.12f*u(state.random);
                d.rgb[0]=0.35f;d.rgb[1]=0.22f;d.rgb[2]=0.12f;d.fall=14;
                state.drops.push_back(d);
            }
            for(int i=0;i<60;++i)   // smoke, rolling up slowly
            {
                Drop d;d.at=p+V3{pm(state.random)*2,0.5f+u(state.random)*2,pm(state.random)*2};
                d.v={pm(state.random)*1.5f,2+u(state.random)*3,pm(state.random)*1.5f};
                d.life=2.0f+1.5f*u(state.random);d.size=1.0f+1.4f*u(state.random);
                const float grey=0.22f+0.12f*u(state.random);
                d.rgb[0]=grey;d.rgb[1]=grey*0.95f;d.rgb[2]=grey*0.9f;d.fall=-0.6f;
                state.drops.push_back(d);
            }
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
                // (as tall as the blue)
                rect(px0-1.5f*s,y0-2*s,px1+1.5f*s,y1+2*s,0.06f,0.10f,0.16f);
                rect(px0,y0,px1,y1,0.98f,0.42f,0.70f);
                const float jw=std::max(1.2f*s,GaugeX(bar,JustWidth(state.club))-GaugeX(bar,0));
                rect(GaugeX(bar,0)-jw-1.5f*s,y0-2*s,GaugeX(bar,0)+jw+1.5f*s,y1+2*s,0.06f,0.10f,0.16f);
                rect(GaugeX(bar,0)-jw,y0,GaugeX(bar,0)+jw,y1,1,1,1);
            }
            // The cup's plain distance on the bar (no slope, wind or lie): a red pin with its flag,
            // at the share of full power's reach it takes (at the end when it is further).
            {
                const Ball& yours=state.balls[You];
                const float toPin=Golf::Flat(yours.at,Course().Pin());
                const float full=putt?Golf::PutterRange(toPin,Course().Pin().y-yours.at.y):Golf::FlatCarry(state.club,state.spotX,state.spotY);
                if(full>0)
                {
                    const float x=GaugeX(bar,std::min(1.0f,toPin/full));
                    rect(x-1.2f*s,y0-10*s,x+1.2f*s,y1+3*s,0.95f,0.20f,0.18f);
                    const float col[4]={0.95f,0.20f,0.18f,Unlit};
                    D-=0.05f;
                    const Mesh::V a=at(x+1.2f*s,y0-10*s),b=at(x+9*s,y0-7*s),c=at(x+1.2f*s,y0-4*s);
                    m.Vertex(a,{n.x,n.y,n.z},col);m.Vertex(b,{n.x,n.y,n.z},col);m.Vertex(c,{n.x,n.y,n.z},col);
                }
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
                {
                    // Your ball in the look you chose.
                    static const float looks[5][4]={{0.97f,0.97f,0.97f,1},{1.0f,0.80f,0.30f,1},{1.0f,0.66f,0.78f,1},{0.55f,0.80f,1.0f,1},{0.10f,0.10f,0.14f,1}};
                    const int look=who==You?std::clamp(state.progress.chosen[GolfProgress::BallLook],0,4):0;
                    m.Sphere({p.x*K,bottom+r,p.z*K},r,looks[look],4,10,6);
                }
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
                // A mark over the other's ball, so it is found (pink hers, blue yours); none over
                // the ball being played (seen from behind it, it stood on the putt's line).
                if(who!=state.turn||state.phase==Phase::Settle||state.phase==Phase::HoleOver)
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
            // Diagnostics: her skeleton, joint to joint (left blue, right orange).
            if(state.bonesView)
            {
                const XMMATRIX world=HerWorld(model);
                for(const auto& b:model.BoneSegments())
                {
                    const float col[4]={b.side==0?0.30f:b.side==1?0.98f:0.35f,b.side==0?0.55f:b.side==1?0.58f:0.36f,b.side==0?0.98f:b.side==1?0.22f:0.40f,Unlit};
                    const XMVECTOR a=XMVector3Transform(XMVectorSet(b.from[0],b.from[1],b.from[2],1),world),c=XMVector3Transform(XMVectorSet(b.to[0],b.to[1],b.to[2],1),world);
                    const XMVECTOR ax=XMVectorSubtract(c,a);
                    if(XMVectorGetX(XMVector3LengthSq(ax))<1e-8f)continue;
                    const XMVECTOR axn=XMVector3Normalize(ax);
                    XMVECTOR u=XMVector3Cross(axn,XMVectorSet(0,1,0,0));
                    if(XMVectorGetX(XMVector3LengthSq(u))<1e-6f)u=XMVectorSet(1,0,0,0);
                    u=XMVector3Normalize(u);
                    const XMVECTOR v=XMVector3Cross(axn,u);
                    const float r=std::min(0.012f*K,0.18f*XMVectorGetX(XMVector3Length(ax)));
                    for(int i=0;i<6;++i)
                    {
                        const float a0=2*Pi*i/6,a1=2*Pi*(i+1)/6;
                        const XMVECTOR o0=XMVectorScale(XMVectorAdd(XMVectorScale(u,std::cos(a0)),XMVectorScale(v,std::sin(a0))),r);
                        const XMVECTOR o1=XMVectorScale(XMVectorAdd(XMVectorScale(u,std::cos(a1)),XMVectorScale(v,std::sin(a1))),r);
                        XMFLOAT3 p0,p1,q0,q1,nn;
                        XMStoreFloat3(&p0,XMVectorAdd(a,o0));XMStoreFloat3(&p1,XMVectorAdd(a,o1));
                        XMStoreFloat3(&q0,XMVectorAdd(c,o0));XMStoreFloat3(&q1,XMVectorAdd(c,o1));
                        XMStoreFloat3(&nn,XMVector3Normalize(XMVectorAdd(o0,o1)));
                        m.QuadN({p0.x,p0.y,p0.z},{p1.x,p1.y,p1.z},{q1.x,q1.y,q1.z},{q0.x,q0.y,q0.z},{nn.x,nn.y,nn.z},col);
                    }
                    const float joint[4]={0.92f,0.92f,0.95f,Unlit};
                    XMFLOAT3 j;XMStoreFloat3(&j,a);
                    m.Sphere({j.x,j.y,j.z},r*1.5f,joint,4,8,5);
                }
            }
            // The club in her hands: held in her left fist, so it goes as her wrist does. A
            // rubber grip, a tapering shaft (steel, graphite for the woods), and its head: a
            // lofted blade for an iron, a round head with a dark crown and a bright face for a
            // wood, a flat blade for the putter; the face turns with her left palm.
            std::array<float,3> middle{},club{},palm{};
            const size_t held=static_cast<size_t>(state.herClub);
            if(state.herSwinging&&model.ClubInHand(middle,club,state.herPutting?0.0f:state.clubTilt[held],&palm))
            {
                const XMMATRIX world=HerWorld(model);
                const XMVECTOR g=XMVector3Transform(XMVectorSet(middle[0],middle[1],middle[2],1),world);
                const XMVECTOR d=XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(club[0],club[1],club[2],0),world));
                const float len=state.herPutting?state.putterLength:state.clubLen[held];
                const XMVECTOR top=XMVectorSubtract(g,XMVectorScale(d,0.06f*K));
                const XMVECTOR end=XMVectorAdd(g,XMVectorScale(d,len));
                const bool putter=state.herPutting,wood=!putter&&state.herClub<=1;
                const float rubber[4]={0.08f,0.08f,0.09f,1},steel[4]={0.78f,0.79f,0.83f,1},graphite[4]={0.20f,0.21f,0.24f,1},
                            chrome[4]={0.80f,0.81f,0.85f,1},crown[4]={0.06f,0.06f,0.08f,1},face[4]={0.70f,0.72f,0.76f,1},ferrule[4]={0.85f,0.85f,0.85f,1};
                auto P=[](FXMVECTOR v){XMFLOAT3 f;XMStoreFloat3(&f,v);return Mesh::V{f.x,f.y,f.z};};
                // A tapering rod from a (radius ra) to b (radius rb), closed both sides round.
                auto rod=[&](FXMVECTOR a,FXMVECTOR b,float ra,float rb,const float (&col)[4]){
                    const XMVECTOR ax=XMVector3Normalize(XMVectorSubtract(b,a));
                    XMVECTOR u=XMVector3Cross(ax,XMVectorSet(0,1,0,0));
                    if(XMVectorGetX(XMVector3LengthSq(u))<1e-6f)u=XMVectorSet(1,0,0,0);
                    u=XMVector3Normalize(u);
                    const XMVECTOR v=XMVector3Cross(ax,u);
                    constexpr int Sides=8;
                    for(int i=0;i<Sides;++i)
                    {
                        const float a0=2*Pi*i/Sides,a1=2*Pi*(i+1)/Sides;
                        const XMVECTOR o0=XMVectorAdd(XMVectorScale(u,std::cos(a0)),XMVectorScale(v,std::sin(a0)));
                        const XMVECTOR o1=XMVectorAdd(XMVectorScale(u,std::cos(a1)),XMVectorScale(v,std::sin(a1)));
                        const auto p0=P(XMVectorAdd(a,XMVectorScale(o0,ra))),p1=P(XMVectorAdd(a,XMVectorScale(o1,ra)));
                        const auto q0=P(XMVectorAdd(b,XMVectorScale(o0,rb))),q1=P(XMVectorAdd(b,XMVectorScale(o1,rb)));
                        m.Quad(p0,p1,q1,q0,col);
                        m.Quad(q0,q1,p1,p0,col);
                    }
                };
                rod(top,XMVectorAdd(g,XMVectorScale(d,0.2f*K)),0.0125f*K,0.0098f*K,rubber);
                rod(XMVectorAdd(g,XMVectorScale(d,0.2f*K)),XMVectorAdd(g,XMVectorScale(d,0.215f*K)),0.0075f*K,0.0072f*K,ferrule);
                rod(XMVectorAdd(g,XMVectorScale(d,0.215f*K)),end,0.0068f*K,0.0042f*K,wood?graphite:steel);
                // The head's frame: N its face's way (the left palm's, square to the shaft), T
                // toward its toe (the shaft turned up through the club's lie), Y up its face.
                XMVECTOR N=XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(palm[0]*state.faceSign,palm[1]*state.faceSign,palm[2]*state.faceSign,0),world));
                const XMVECTOR U=XMVector3Normalize(XMVector3Cross(N,d));
                const float lie=(putter?72.0f:wood?57.0f:61.0f)*Deg;
                const XMVECTOR T=XMVector3Normalize(XMVectorAdd(XMVectorScale(d,std::cos(lie)),XMVectorScale(U,std::sin(lie))));
                const XMVECTOR Y=XMVector3Normalize(XMVector3Cross(N,T));
                const float loft=(putter?3.0f:Golf::ClubOf(state.herClub).loft)*Deg;
                // The sole sits a little under the shaft's end (the hosel rises from the heel).
                const XMVECTOR heel=XMVectorSubtract(end,XMVectorScale(Y,0.012f*K));
                auto at=[&](float t,float y,float n){return XMVectorAdd(heel,XMVectorAdd(XMVectorScale(T,t*K),XMVectorAdd(XMVectorScale(Y,y*K),XMVectorScale(N,n*K))));};
                // A six-sided piece from its eight corners (bottom face then top, each heel then toe,
                // face side then back).
                auto block=[&](const std::array<XMVECTOR,8>& c,const float (&col)[4]){
                    const int faces[6][4]={{0,1,3,2},{4,6,7,5},{0,4,5,1},{2,3,7,6},{0,2,6,4},{1,5,7,3}};
                    for(const auto& f:faces)
                    {
                        m.Quad(P(c[f[0]]),P(c[f[1]]),P(c[f[2]]),P(c[f[3]]),col);
                        m.Quad(P(c[f[3]]),P(c[f[2]]),P(c[f[1]]),P(c[f[0]]),col);
                    }
                };
                if(putter)
                {
                    block({at(-0.01f,0,0.006f),at(0.095f,0,0.006f),at(-0.01f,0,-0.022f),at(0.095f,0,-0.022f),
                           at(-0.01f,0.026f,0.006f),at(0.095f,0.026f,0.006f),at(-0.01f,0.026f,-0.016f),at(0.095f,0.026f,-0.016f)},chrome);
                }
                else if(!wood)
                {
                    // The blade: its face lofted back, its toe higher than its heel, thicker at the sole.
                    const float back=std::tan(loft);
                    const float hh=0.034f,ht=0.05f;
                    block({at(0,0,0.004f),at(0.078f,0,0.004f),at(0,0,-0.016f),at(0.078f,0,-0.016f),
                           at(0,hh,0.004f-hh*back),at(0.078f,ht,0.004f-ht*back),at(0,hh,-0.002f-hh*back),at(0.078f,ht,-0.002f-ht*back)},chrome);
                    rod(end,XMVectorSubtract(end,XMVectorScale(d,0.05f*K)),0.0075f*K,0.0068f*K,chrome);   // the hosel
                }
                else
                {
                    // The wood: a rounded head, its crown dark, its face bright; the driver's larger.
                    const float k=state.herClub==Golf::Driver?1.0f:0.78f;
                    const float rt=0.058f*k,ry=0.03f*k,rn=0.05f*k;
                    const XMVECTOR mid=at(rt*0.95f,ry,-rn*0.85f);
                    constexpr int Around=14,Up=8;
                    auto pt=[&](int i,int j,XMVECTOR& n){
                        const float a=2*Pi*i/Around,b=Pi*j/Up-Pi/2;
                        const float x=std::cos(b)*std::cos(a),z=std::cos(b)*std::sin(a),y=std::sin(b);
                        n=XMVector3Normalize(XMVectorAdd(XMVectorScale(T,x/rt),XMVectorAdd(XMVectorScale(N,z/rn),XMVectorScale(Y,y/ry))));
                        return XMVectorAdd(mid,XMVectorScale(XMVectorAdd(XMVectorScale(T,x*rt),XMVectorAdd(XMVectorScale(N,z*rn),XMVectorScale(Y,y*ry*(y<0?0.75f:1.0f)))),K));
                    };
                    for(int i=0;i<Around;++i)
                        for(int j=0;j<Up;++j)
                        {
                            XMVECTOR n0,n1,n2,n3;
                            const XMVECTOR p0=pt(i,j,n0),p1=pt(i+1,j,n1),p2=pt(i+1,j+1,n2),p3=pt(i,j+1,n3);
                            const float facing=XMVectorGetX(XMVector3Dot(XMVectorAdd(XMVectorAdd(n0,n1),XMVectorAdd(n2,n3)),N))*0.25f;
                            const float up=XMVectorGetX(XMVector3Dot(XMVectorAdd(XMVectorAdd(n0,n1),XMVectorAdd(n2,n3)),Y))*0.25f;
                            const float (&col)[4]=facing>0.72f?face:up>0.15f?crown:graphite;
                            m.Quad(P(p0),P(p1),P(p2),P(p3),col);
                            m.Quad(P(p3),P(p2),P(p1),P(p0),col);
                        }
                    rod(end,XMVectorSubtract(end,XMVectorScale(d,0.04f*K)),0.0072f*K,0.0066f*K,crown);
                }
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

        // The album (C): what you have achieved, your records, her rank, and the looks to choose.
        void DrawAlbum(Graphics& g,float W,float H,float s)
        {
            const Color white(255,240,244,248),soft(255,150,162,178),gold(255,246,212,120),pink(255,255,150,190);
            const auto& pr=state.progress;
            const float bw=std::min(W-30*s,1060*s),bh=std::min(H-30*s,760*s);
            const RectF box(W/2-bw/2,H/2-bh/2,bw,bh);
            Panel(g,box,s,230);
            const float x=box.X+20*s;
            float y=box.Y+12*s;
            Text(g,T(L"アルバム",L"Album"),RectF(x,y,bw-40*s,30*s),20*s,gold,true,StringAlignmentNear);
            Text(g,Fmt(T(L"ラウンド {0}　勝ち {1}　★ {2}　{3}の段位 {4}",L"Rounds {0}   Wins {1}   Stars {2}   {3}'s rank {4}"),
                       {std::to_wstring(pr.rounds),std::to_wstring(pr.wins),std::to_wstring(pr.stars),HerName(),std::to_wstring(pr.rank)}),
                 RectF(x+180*s,y+4*s,bw-220*s,24*s),13*s,white,false,StringAlignmentNear);
            y+=40*s;
            // Achievements in two columns.
            int got=0;
            for(bool b:pr.got)got+=b?1:0;
            Text(g,Fmt(T(L"実績 {0}/{1}",L"Achievements {0}/{1}"),{std::to_wstring(got),std::to_wstring(GolfProgress::AchievementCount)}),RectF(x,y,300*s,20*s),13*s,soft,true,StringAlignmentNear);
            y+=22*s;
            const float colW=(bw-40*s)/2,lineH=std::min(24*s,(bh*0.56f)/((GolfProgress::AchievementCount+1)/2));
            for(int a=0;a<GolfProgress::AchievementCount;++a)
            {
                const auto& info=GolfProgress::InfoOf(a);
                const bool has=pr.got[static_cast<size_t>(a)];
                const float cx=x+(a%2)*colW,cy=y+(a/2)*lineH;
                Text(g,std::wstring(has?L"✓ ":L"・ ")+T(info.jp,info.en),RectF(cx,cy,colW*0.42f,lineH),12*s,has?gold:soft,has,StringAlignmentNear);
                Text(g,T(info.howJp,info.howEn),RectF(cx+colW*0.42f,cy,colW*0.58f,lineH),10.5f*s,has?white:soft,false,StringAlignmentNear);
            }
            y+=((GolfProgress::AchievementCount+1)/2)*lineH+10*s;
            // Records.
            std::wstring holes=T(L"ホールのベスト　",L"Best per hole   ");
            for(int h=0;h<9;++h)holes+=std::to_wstring(h+1)+L":"+(pr.bestHole[static_cast<size_t>(h)]>0?std::to_wstring(pr.bestHole[static_cast<size_t>(h)]):std::wstring(L"-"))+L"  ";
            Text(g,holes,RectF(x,y,bw-40*s,20*s),12*s,white,false,StringAlignmentNear);
            y+=22*s;
            auto vs=[&](int v){if(v==0)return std::wstring(L"-");const int d=v-100;return d==0?std::wstring(L"±0"):(d>0?L"+":L"")+std::to_wstring(d);};
            Text(g,Fmt(T(L"ラウンドのベスト（パーとの差）　3ホール {0}　6ホール {1}　9ホール {2}",L"Best rounds (against par)   3 holes {0}   6 holes {1}   9 holes {2}"),
                       {vs(pr.bestRound[0]),vs(pr.bestRound[1]),vs(pr.bestRound[2])}),RectF(x,y,bw-40*s,20*s),12*s,white,false,StringAlignmentNear);
            y+=30*s;
            // Looks.
            const wchar_t* kinds[GolfProgress::LookKindCount]={T(L"軌跡",L"Trail"),T(L"魔法陣",L"Circle"),T(L"ボール",L"Ball"),T(L"メテオの炎",L"Meteor fire")};
            for(int k=0;k<GolfProgress::LookKindCount;++k)
            {
                const auto& list=GolfProgress::LooksOf(k);
                int open=0;
                for(int i=0;i<static_cast<int>(list.size());++i)open+=pr.Unlocked(k,i)?1:0;
                const auto& now=list[static_cast<size_t>(std::clamp(pr.chosen[static_cast<size_t>(k)],0,static_cast<int>(list.size())-1))];
                const RectF row(x,y,bw-40*s,24*s);
                if(k==state.albumRow){SolidBrush lit(Color(70,246,212,120));g.FillRectangle(&lit,row);}
                Text(g,kinds[k],RectF(x+8*s,y,140*s,24*s),13*s,k==state.albumRow?gold:white,k==state.albumRow,StringAlignmentNear);
                Text(g,std::wstring(L"◀  ")+T(now.jp,now.en)+L"  ▶",RectF(x+150*s,y,240*s,24*s),13*s,white,true,StringAlignmentNear);
                Text(g,Fmt(T(L"（{0}/{1} 解放）",L"({0}/{1} unlocked)"),{std::to_wstring(open),std::to_wstring(list.size())}),RectF(x+400*s,y,200*s,24*s),11*s,soft,false,StringAlignmentNear);
                y+=26*s;
            }
            Text(g,T(L"↑↓ 選ぶ　←→ 見た目を変える　C・Esc 閉じる",L"Up/down: choose   Left/right: change the look   C/Esc: close"),RectF(x,box.Y+bh-30*s,bw-40*s,22*s),12*s,pink,false,StringAlignmentNear);
        }

        void DrawHud(Graphics& g,int w,int h,float s)
        {
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAlias);
            const Color white(255,240,244,248),soft(255,176,190,206),gold(255,246,212,120),pink(255,255,150,190),blue(255,140,190,255);
            const auto& c=Course();
            const float W=static_cast<float>(w),H=static_cast<float>(h);
            // The day over everything: a grey veil for rain, a warm one at dusk.
            if(state.weather==Rain){SolidBrush veil(Color(46,40,48,60));g.FillRectangle(&veil,0.0f,0.0f,W,H);}
            else if(state.weather==Dusk){SolidBrush veil(Color(58,120,55,15));g.FillRectangle(&veil,0.0f,0.0f,W,H);}
            // The hole, the wind.
            {
                const RectF box(10*s,10*s,250*s,92*s);
                Panel(g,box,s);
                Text(g,Fmt(T(L"{0}番ホール　パー{1}　（{2}/{3}）",L"Hole {0}   Par {1}   ({2}/{3})"),{std::to_wstring(HoleNo()),std::to_wstring(c.Par()),std::to_wstring(state.hole+1),std::to_wstring(RoundLength())}),
                     RectF(box.X+12*s,box.Y+6*s,box.Width-20*s,24*s),15*s,white,true,StringAlignmentNear);
                Text(g,YardText(c.Length())+(state.weather!=Sunny?std::wstring(L"　")+WeatherName(state.weather):std::wstring()),RectF(box.X+12*s,box.Y+30*s,168*s,22*s),13*s,state.weather!=Sunny?gold:soft,false,StringAlignmentNear);
                // The wind's arrow, as the camera sees it (up: away from the camera).
                const PointF mid(box.X+box.Width-46*s,box.Y+52*s);
                const float a=state.wind.toward-ViewYaw();
                const float r=20*s;
                Pen ring(Color(120,200,210,220),1.5f*s);
                g.DrawEllipse(&ring,mid.X-r-4*s,mid.Y-r-4*s,2*r+8*s,2*r+8*s);
                const PointF tip(mid.X+r*std::sin(a),mid.Y-r*std::cos(a)),tail(mid.X-r*std::sin(a),mid.Y+r*std::cos(a));
                // Thicker the stronger the wind; a big head, so its way is plain.
                const float thick=(2.5f+std::min(state.wind.speed,10.0f)*0.7f)*s;
                const PointF neck(mid.X+r*0.25f*std::sin(a),mid.Y-r*0.25f*std::cos(a));
                Pen arrow(Color(255,120,200,255),thick);
                g.DrawLine(&arrow,tail,neck);
                const float hw=std::max(9*s,thick*1.6f);
                const PointF side(std::cos(a),std::sin(a));
                const PointF head[3]={PointF(tip.X+r*0.15f*std::sin(a),tip.Y-r*0.15f*std::cos(a)),PointF(neck.X+side.X*hw,neck.Y+side.Y*hw),PointF(neck.X-side.X*hw,neck.Y-side.Y*hw)};
                SolidBrush headFill(Color(255,120,200,255));
                g.FillPolygon(&headFill,head,3);
                wchar_t speed[32];
                swprintf_s(speed,L"%.1f m/s",state.wind.speed);
                Text(g,std::wstring(T(L"風 ",L"Wind "))+speed,RectF(box.X+12*s,box.Y+54*s,140*s,24*s),14*s,white,false,StringAlignmentNear);
            }
            // The special gauges: two bars each (the boost takes one, the meteor both), and what
            // you have chosen with S for this shot.
            {
                const RectF box(10*s,108*s,250*s,state.special>0?84*s:62*s);
                Panel(g,box,s);
                for(int who:{You,Her})
                {
                    const float y=box.Y+(who==You?10:34)*s;
                    Text(g,who==You?std::wstring(T(L"あなた",L"You")):HerName(),RectF(box.X+10*s,y-4*s,80*s,22*s),12*s,who==You?blue:pink,true,StringAlignmentNear);
                    const float m=state.meter[static_cast<size_t>(who)];
                    for(int bar=0;bar<2;++bar)
                    {
                        const RectF r(box.X+92*s+bar*74*s,y,70*s,12*s);
                        SolidBrush back(Color(255,40,52,64));
                        g.FillRectangle(&back,r);
                        const float fill=std::clamp((m-100.0f*bar)/100,0.0f,1.0f);
                        const bool full=fill>=1;
                        SolidBrush bright(full?Color(255,255,200,90):Color(255,120,170,210));
                        g.FillRectangle(&bright,RectF(r.X,r.Y,r.Width*fill,r.Height));
                        Pen edge(Color(160,220,230,240),1*s);
                        g.DrawRectangle(&edge,r);
                    }
                }
                if(state.special>0)
                    Text(g,state.special==2?T(L"メテオショット（S で選び直し）",L"Meteor shot (S to choose again)"):T(L"ブーストショット（S で選び直し）",L"Boost shot (S to choose again)"),
                         RectF(box.X+10*s,box.Y+56*s,box.Width-20*s,22*s),13*s,gold,true,StringAlignmentNear);
            }
            // This hole's mission.
            const float missionY=(state.special>0?200:178)*s;
            if(state.mission>=0&&!state.autoplay&&state.phase!=Phase::Over)
            {
                const RectF box(10*s,missionY,250*s,40*s);
                Panel(g,box,s);
                Text(g,std::wstring(state.missionDone?L"★ ":L"☆ ")+MissionText(state.mission),RectF(box.X+10*s,box.Y,box.Width-16*s,box.Height),12*s,state.missionDone?gold:white,state.missionDone,StringAlignmentNear);
            }
            // The special shots' menu.
            if(state.menuOpen)
            {
                const RectF box(10*s,missionY+48*s,250*s,150*s);
                Panel(g,box,s,215);
                Text(g,T(L"スペシャルショット（ゲージ1本）",L"Special shot (one bar)"),RectF(box.X+12*s,box.Y+8*s,box.Width-24*s,22*s),13*s,soft,false,StringAlignmentNear);
                const bool can=state.meter[You]>=100;
                const wchar_t* names[3]={T(L"ブースト：いつもより遠く",L"Boost: further"),T(L"メテオ：ずっと遠く、落ちて止まる",L"Meteor: much further, stops dead"),T(L"キャンセル",L"Cancel")};
                for(int i=0;i<3;++i)
                {
                    const RectF r(box.X+8*s,box.Y+(34+i*36)*s,box.Width-16*s,32*s);
                    state.menuBoxes[i]=r;
                    if(i==state.menuPick)
                    {
                        GraphicsPath path;
                        RoundRect(path,r,6*s);
                        SolidBrush lit(Color(150,246,212,120));
                        g.FillPath(&lit,&path);
                    }
                    const bool off=i<2&&!can;
                    Text(g,names[i],RectF(r.X+10*s,r.Y,r.Width-20*s,r.Height),14*s,off?Color(255,110,118,128):white,i==state.menuPick,StringAlignmentNear);
                }
                if(!can)Text(g,T(L"ゲージが足りません",L"Not enough gauge"),RectF(box.X+12*s,box.Y+box.Height+2*s,box.Width,20*s),12*s,pink,false,StringAlignmentNear);
            }
            // How far the ball has gone, while it flies and rolls (flat from where it was struck).
            if((state.phase==Phase::Flying||state.phase==Phase::Settle)&&!state.flight.path.empty())
            {
                const std::wstring gone=YardText(Golf::Flat(state.shotFrom,BallNow(state.turn)),true);
                const RectF box(W/2-110*s,64*s,220*s,56*s);
                Panel(g,box,s,150);
                Text(g,gone,box,30*s,white,true);
            }
            // The card: strokes per hole, the total against par.
            {
                const int n=RoundLength();
                const float cw=40*s,rw=110*s,bw=rw+cw*(n+1)+20*s,bh=104*s;
                const RectF box(W-bw-10*s,10*s,bw,bh);
                Panel(g,box,s);
                const float x0=box.X+10*s;
                Text(g,T(L"ホール",L"Hole"),RectF(x0,box.Y+4*s,rw,20*s),12*s,soft,false,StringAlignmentNear);
                Text(g,T(L"パー",L"Par"),RectF(x0,box.Y+22*s,rw,20*s),12*s,soft,false,StringAlignmentNear);
                int parTotal=0;
                for(int h2=0;h2<n;++h2)
                {
                    Text(g,std::to_wstring(HoleNo(h2)),RectF(x0+rw+cw*h2,box.Y+4*s,cw,20*s),12*s,h2==state.hole?white:soft,h2==state.hole);
                    const int par=Golf::HolePar(HoleNo(h2));
                    parTotal+=par;
                    Text(g,std::to_wstring(par),RectF(x0+rw+cw*h2,box.Y+22*s,cw,20*s),11*s,soft);
                }
                Text(g,T(L"計",L"Total"),RectF(x0+rw+cw*n,box.Y+4*s,cw,20*s),12*s,soft);
                Text(g,std::to_wstring(parTotal),RectF(x0+rw+cw*n,box.Y+22*s,cw,20*s),11*s,soft);
                for(int who:{You,Her})
                {
                    const float y=box.Y+(who==You?46:72)*s;
                    Text(g,who==You?std::wstring(T(L"あなた",L"You")):HerName(),RectF(x0,y,rw,24*s),14*s,who==You?blue:pink,true,StringAlignmentNear);
                    for(int h2=0;h2<n;++h2)
                    {
                        int strokes=state.card[who][h2];
                        if(h2==state.hole&&strokes==0&&state.phase!=Phase::Over)strokes=state.balls[who].strokes;
                        if(strokes>0||h2<=state.hole)Text(g,strokes>0?std::to_wstring(strokes):L"-",RectF(x0+rw+cw*h2,y,cw,24*s),15*s,white,h2==state.hole);
                    }
                    int total=0;
                    for(int h2=0;h2<n;++h2)total+=h2==state.hole&&state.card[who][h2]==0?state.balls[who].strokes:state.card[who][h2];
                    Text(g,std::to_wstring(total),RectF(x0+rw+cw*n,y,cw,24*s),15*s,gold,true);
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
                std::wstring info=std::wstring(Golf::LieName(b.lie))+T(L"　ピンまで ",L"   to the pin ")+YardText(toPin);
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
                    const std::wstring t=YardText(metres,Yards(metres)<20);
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
                swprintf_s(t,T(L"%.1fy　%ls%.1f m",L"%.1f yd   %ls%.1f m"),Yards(d),rise>=0?L"↑":L"↓",std::abs(rise));
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
                    T(L"パワー半分以上・白で：下打点で↓を押したまま→バックスピン、上打点で↑→トップスピン",L"Half power or more, struck in the white: low spot with ↓ held: backspin; high spot with ↑: topspin"),
                    T(L"S　スペシャルショットのメニュー（ブースト・メテオ、ゲージ1本。もう一度Sでやめる）",L"S   special shot menu (boost or meteor, one bar; S again: none)"),
                    T(L"0の視点で Shift＋↑↓　近づく・離れる",L"In the 0 view, Shift+↑↓   nearer, further"),
                    T(L"C　アルバム（実績・記録・見た目を選ぶ）",L"C   album (achievements, records, choose looks)"),
                    T(L"0／右クリック　黄色い輪を上から見る（もう一度・Spaceで戻る、中ドラッグで動かす）",L"0/right click   the yellow circle from above (again or Space: back; middle drag: move)"),
                    T(L"右ドラッグ・ホイール　カメラ（F1で戻す）",L"Right drag, wheel   camera (F1 puts it back)"),
                    T(L"Esc　やめる　　Tab　この説明を閉じる",L"Esc   stop      Tab   close this")};
                const float bw=std::min(W-40*s,720*s),lh=26*s,bh=lh*std::size(lines)+28*s;
                const RectF box(W/2-bw/2,H*0.45f-bh/2,bw,bh);
                Panel(g,box,s,215);
                for(size_t i=0;i<std::size(lines);++i)
                    Text(g,lines[i],RectF(box.X+18*s,box.Y+14*s+lh*i,box.Width-36*s,lh),14*s,white,false,StringAlignmentNear);
            }
            // A notice (achievement, mission), one at a time.
            if(!state.toasts.empty())
            {
                const float bw=std::min(W-40*s,640*s);
                const RectF box(W/2-bw/2,128*s,bw,40*s);
                Panel(g,box,s,200);
                Text(g,state.toasts.front(),box,15*s,gold,true);
            }
            // Her words beside her head.
            const bool herSeen=state.besideHead.x>=0&&state.besideHead.x<w&&state.headScreen.y>0&&state.headScreen.y<h&&state.phase!=Phase::Intro;
            if(!state.words.text.empty()&&herSeen)
            {
                const std::wstring& said=state.words.text;
                const float most=std::max(160*s,W*0.32f);
                const WordsBubble bubble(g,said,s,most);
                const float bw=bubble.Width(),bh=bubble.Height();
                // Beside her head on the right, or on the left when the right has no room for it
                // (pushed back over her face it hid her).
                const float right=static_cast<float>(state.besideHead.x);
                const float left=static_cast<float>(2*state.headScreen.x-state.besideHead.x);
                const bool onLeft=right+bw>W-14*s&&left-bw>=14*s;
                const float x=onLeft?left-bw:std::clamp(right,14*s,W-14*s-bw);
                const float headY=static_cast<float>(state.headScreen.y);
                const float y=std::clamp(headY-bh*0.6f,110*s,std::max(110*s,H-bh-140*s));
                bubble.Draw(g,x,y,std::clamp(headY,y+12*s,y+bh-12*s),ShownWords(),onLeft);
            }
            // Whose turn, when it is hers.
            if(state.turn==Her&&(state.phase==Phase::Aim||state.phase==Phase::Swing))
                Text(g,Fmt(T(L"{0}のショット",L"{0}'s shot"),{HerName()}),RectF(0,H-60*s,W,30*s),18*s,pink,true);
            // The banner.
            if(state.clock<state.bannerUntil&&!state.banner.empty())
            {
                const float bw=std::min(W-40*s,560*s),bh=state.bannerNote.empty()?64*s:96*s;
                // (Low during the look over the hole, which shows the clubhouse in the middle.)
                // (Low on the screen at the intro and at the result, clear of her face.)
                const RectF box(W/2-bw/2,(state.phase==Phase::Intro?H*0.62f:state.phase==Phase::Over?H*0.86f:H*0.36f)-bh/2,bw,bh);
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
                               L"|"+std::to_wstring(state.beamTop.x/6)+L","+std::to_wstring(state.beamTop.y/6)+
                               // (the special menu and gauges: without them, the menu came up only when something else changed)
                               L"|"+std::to_wstring(state.menuOpen?state.menuPick:-1)+L","+std::to_wstring(state.special)+L","+
                               std::to_wstring(static_cast<int>(state.meter[0]))+L","+std::to_wstring(static_cast<int>(state.meter[1]))+
                               L"|"+(state.toasts.empty()?std::wstring():state.toasts.front())+(state.album?L"A"+std::to_wstring(state.albumRow):L"")+
                               std::to_wstring(state.progress.chosen[0]*1000+state.progress.chosen[1]*100+state.progress.chosen[2]*10+state.progress.chosen[3])+
                               (state.missionDone?L"m":L"")+std::to_wstring(state.mission)+std::to_wstring(state.weather);
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
                if(state.album)DrawAlbum(gr,static_cast<float>(w),static_cast<float>(h),Scale());   // over everything
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
            case WM_MOUSEACTIVATE:
                // A click on an inactive window only brings it up (no shot, no camera).
                if(GetForegroundWindow()!=state.window)state.activating=true;
                return MA_ACTIVATE;
            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
                SetFocus(state.window);
                return Proc(state.window,WM_LBUTTONDOWN,wParam,lParam);
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
                SetFocus(state.window);
                if(state.activating){state.activating=false;return 0;}
                // The middle button in the 0 view moves it; otherwise either turns the camera.
                state.dragging=message==WM_MBUTTONDOWN&&state.overhead?2:1;
                state.dragFrom={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
                if(message==WM_RBUTTONDOWN)state.dragStart=state.dragFrom;
                SetCapture(hwnd);
                return 0;
            case WM_MOUSEMOVE:
                if(state.dragging)
                {
                    const float dx=static_cast<float>(GET_X_LPARAM(lParam)-state.dragFrom.x),dy=static_cast<float>(GET_Y_LPARAM(lParam)-state.dragFrom.y);
                    state.dragFrom={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
                    const float k=1/Scale();
                    if(state.dragging==2)
                    {
                        // The ground follows the mouse: across and along the aim, by how high the view is.
                        const float high=(state.club==Golf::Putter?9.0f:38.0f)*c.zoom;
                        const V3 dir{std::sin(state.aim),0,std::cos(state.aim)},side{std::cos(state.aim),0,-std::sin(state.aim)};
                        const float m=high*0.0016f*k;
                        state.overheadPan=state.overheadPan-side*(dx*m)+dir*(dy*m);
                    }
                    else
                    {
                        c.yaw+=dx*k*0.008f;
                        c.pitch=std::clamp(c.pitch+dy*k*0.004f,-0.3f,1.5f);
                    }
                }
                return 0;
            case WM_RBUTTONUP:
            case WM_MBUTTONUP:
                if(state.dragging&&message==WM_RBUTTONUP&&std::abs(GET_X_LPARAM(lParam)-state.dragStart.x)+std::abs(GET_Y_LPARAM(lParam)-state.dragStart.y)<5)
                {
                    // A right click without a drag: as the 0 key (the turn it made, undone).
                    ToggleOverhead();
                }
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
                                    {std::to_wstring(HoleNo()),std::to_wstring(Total(Her)+state.balls[Her].strokes),std::to_wstring(Total(You)+state.balls[You].strokes)});
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
                if(state.album)
                {
                    // The album: up and down pick a kind of look, left and right go through those
                    // unlocked; C or Esc closes it.
                    if(wParam=='C'||wParam==VK_ESCAPE)state.album=false;
                    else if(wParam==VK_UP)state.albumRow=(state.albumRow+GolfProgress::LookKindCount-1)%GolfProgress::LookKindCount;
                    else if(wParam==VK_DOWN)state.albumRow=(state.albumRow+1)%GolfProgress::LookKindCount;
                    else if(wParam==VK_LEFT||wParam==VK_RIGHT)
                    {
                        const int k=state.albumRow,n=static_cast<int>(GolfProgress::LooksOf(k).size());
                        int& now=state.progress.chosen[static_cast<size_t>(k)];
                        for(int step=1;step<=n;++step)
                        {
                            const int next=((now+(wParam==VK_RIGHT?step:-step))%n+n)%n;
                            if(state.progress.Unlocked(k,next)){now=next;break;}
                        }
                        SaveProgress();
                    }
                    return 0;
                }
                if(wParam=='C'&&!state.askingQuit&&!(lParam&0x40000000)){state.album=true;state.menuOpen=false;return 0;}
                if(state.menuOpen)
                {
                    // The special shots' menu: up and down pick, Space or Enter takes it, S again
                    // (or Esc) puts it away with no special.
                    if(wParam=='S'||wParam==VK_ESCAPE){state.menuOpen=false;state.special=0;UpdateLanding();}
                    else if(wParam==VK_UP)state.menuPick=(state.menuPick+2)%3;
                    else if(wParam==VK_DOWN)state.menuPick=(state.menuPick+1)%3;
                    else if((wParam==VK_SPACE||wParam==VK_RETURN)&&!(lParam&0x40000000))ChooseSpecial(state.menuPick);
                    return 0;
                }
                if(wParam==VK_ESCAPE)
                {
                    if(state.phase==Phase::Over)Finish();
                    else state.askingQuit=true;
                }
                else if(wParam==VK_RETURN&&state.phase==Phase::Over){state.firstSeed+=104729u;++state.round;NewRound();}
                else if(wParam==VK_F1)state.camera={};
                else if(wParam==VK_SPACE&&!(lParam&0x40000000))
                {
                    // From the 0 view, Space only comes back to the usual one.
                    if(state.overhead)LeaveOverhead();
                    else GaugeClick();
                }
                else if(wParam==VK_TAB)state.help=!state.help;
                else if(wParam=='S'&&!(lParam&0x40000000))OpenSpecialMenu();
                else if(state.overhead&&GetKeyState(VK_CONTROL)>=0&&(wParam==VK_UP||wParam==VK_DOWN))
                {
                    // In the 0 view up and down move the view (in the tick); with Shift, nearer or further.
                    if(GetKeyState(VK_SHIFT)<0)state.camera.zoom=std::clamp(state.camera.zoom*(wParam==VK_UP?0.85f:1.0f/0.85f),0.25f,4.0f);
                }
                else if(wParam=='0'||wParam==VK_NUMPAD0)ToggleOverhead();
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
                if(state.activating){state.activating=false;return 0;}   // the click that brought the window up
                if(state.menuOpen)
                {
                    const float x=static_cast<float>(GET_X_LPARAM(lParam)),y=static_cast<float>(GET_Y_LPARAM(lParam));
                    for(int i=0;i<3;++i)if(state.menuBoxes[i].Contains(x,y)){ChooseSpecial(i);return 0;}
                    return 0;
                }
                if(state.overhead){LeaveOverhead();return 0;}
                GaugeClick();
                return 0;
            case WM_MOUSEACTIVATE:
                if(GetForegroundWindow()!=hwnd)state.activating=true;
                return MA_ACTIVATE;
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
        state.progressFile=her.progressFile;
        GolfProgress::Load(state.progressFile,state.progress);
        state.onEnd=std::move(onEnd);
        state.ground=Golf::LoadGround(1);
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
        if(!state.swingVmd.empty())
        {
            // Half a second at the address, the swing at the game's pace, a second at the finish.
            // Timed for a 7 iron (the putter: a 10 m roll) at that power.
            Golf::Shot shot;
            shot.club=state.posePutter?Golf::Putter:3;
            shot.power=state.posePower;
            shot.range=10;
            const SwingPlan plan=MakePlan(model,state.posePower,state.posePutter,Golf::LaunchSpeed(shot,state.posePutter?Lie::Green:Lie::Fairway));
            // The address held long enough (half a second or a little more) that the strike falls
            // on a frame of its own.
            const double address=std::ceil((0.5+plan.Strike())*30-1e-6)/30-plan.Strike();
            std::vector<float> stages;
            for(int n=0;n*(1.0/30)<address+plan.Length()+1.0;++n)stages.push_back(SwingStage(plan,n/30.0,address));
            PlaceToSwing(state.balls[Her].at,PinAim(state.balls[Her].at),state.posePutter?Golf::Putter:3);
            std::wstring report;
            const bool ok=model.RecordSwing(state.swingVmd,stages,state.posePower,state.posePutter,report,state.clubLength);
            wchar_t timing[200];
            swprintf_s(timing,L"; back %.2f s, top %.2f s, down %.2f s, through %.2f s, head %.1f m/s at the strike (frame %d)",plan.back,plan.top,plan.down,plan.through,plan.speed,static_cast<int>((address+plan.Strike())*30+0.5));
            report+=timing;
            state.log+=L"swing vmd "+state.swingVmd+(ok?L": ":L": failed, ")+report+L"\n";
            Log(L"golf: swing vmd %s: %s",state.swingVmd.c_str(),report.c_str());
            state.swingVmd.clear();
            state.posing=0;
        }
        if(state.posing>=0)
        {
            PlaceToSwing(state.balls[Her].at,PinAim(state.balls[Her].at),state.posePutter?Golf::Putter:3);
            model.Swing(true,state.posing,state.posePower,state.posePutter);
        }
        else if(state.turn==Her&&(state.phase==Phase::Aim||state.phase==Phase::Swing||state.phase==Phase::Flying||state.phase==Phase::Settle))
        {
            const float stage=state.phase==Phase::Aim?0.0f:SwingStage(state.plan,state.swingTime,Address);
            const bool hold=state.swingTime<Address+state.plan.Length()+HoldFinish||state.phase==Phase::Aim;
            model.Swing(hold,stage,state.swingPower,state.herPutting);
            if(!hold)state.herSwinging=false;   // the club put down
        }
        else
        {
            model.Swing(false,0,1,false);
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
        state.renderer->SetModelHidden(state.bonesView);
        PlaceCamera(dt);
        {
            const float ease=1-std::exp(-14*static_cast<float>(dt));
            state.spotShownX+=(state.spotX-state.spotShownX)*ease;
            state.spotShownY+=(state.spotY-state.spotShownY)*ease;
        }
        state.renderer->SetMovingGeometry(BuildMoving(model));
        PlaceTrees(dt);
        state.renderer->SetGlowGeometry(BuildGlow());
        state.renderer->SetParticles(BuildSparks(dt));
        for(auto& r:state.ripples)r.age+=static_cast<float>(dt);
        if(state.blastAge>=0){state.blastAge+=static_cast<float>(dt);if(state.blastAge>3)state.blastAge=-1;}
        // The notices, one after another, 3.2 s each.
        if(!state.toasts.empty())
        {
            if(state.toastUntil==0)state.toastUntil=state.clock+3.2;
            else if(state.clock>state.toastUntil)
            {
                state.toasts.erase(state.toasts.begin());
                state.toastUntil=state.toasts.empty()?0:state.clock+3.2;
            }
        }
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
        // The top of the light pillar, for the distance written there.
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
    void SetGolfBones(bool on){state.bonesView=on;}
    void SetGolfSwingVmd(const std::wstring& path,float power,bool putter){state.swingVmd=path;state.posePower=power;state.posePutter=putter;state.posing=0;}
    void SetGolfSwingPose(float stage,float yaw,float power,bool putter){state.posing=stage;state.poseYaw=yaw;state.posePower=power;state.posePutter=putter;}
    void SetGolfSeed(std::uint32_t seed){state.firstSeed=seed;}
    void SetGolfHole(int hole){state.startHole=hole-1;}
    void SetGolfRound(int holes,bool random){state.roundHoles=std::clamp(holes,1,Holes);state.roundRandom=random;}
    void SetGolfSpecial(int special){state.diagSpecial=special;}
    void SetGolfAce(){state.diagAce=true;state.wind={0,0};}
    void SetGolfWeather(int weather)
    {
        // For this round too, if it has begun (the course's pins stay as they were placed).
        state.forcedWeather=weather;
        state.weather=weather;
        Golf::SetWet(weather==2);
        if(state.renderer)state.renderer->SetLight(35,weather==3?12.0f:50.0f);
    }
    void SetGolfGauge(float power,float at,const std::wstring& flags){state.frozenPower=power;state.frozenAt=at;state.frozenFlags=flags;}

    std::wstring DescribeGolf()
    {
        if(!state.course)return L"";
        std::wstring text;
        wchar_t line[300];
        const char* phases[]={"intro","aim","gauge","swing","flying","settle","hole over","over"};
        swprintf_s(line,L"golf: hole %d par %d (seed %u), phase %hs, turn %ls, you %d her %d this hole, totals you %d her %d, frames %d, shots %d\n",HoleNo(),Course().Par(),Course().Seed(),
                   phases[static_cast<int>(state.phase)],state.turn==You?L"you":L"her",state.balls[You].strokes,state.balls[Her].strokes,Total(You),Total(Her),state.frames,state.shots);
        text+=line;
        swprintf_s(line,L"golf: her hips %.1f upper %.1f, club %.2f m, ball %.2f,%.2f from her (m); at %.1f,%.1f yaw %.0f, swinging %d\n",state.hips,state.upper,state.clubLength/K,state.head[0]/K,state.head[2]/K,
                   state.herAt.x,state.herAt.z,state.herYaw/Deg,state.herSwinging?1:0);
        text+=line;
        text+=state.measured;
        swprintf_s(line,L"golf: ground shade: %d of %d vertices under trees\n",state.shadedVertices,state.groundVertices);
        text+=line;
        for(int c=0;c<Golf::Putter;++c)
        {
            swprintf_s(line,L"golf: %ls length %.2f m, tilt %.1f deg, ball %.2f,%.2f m from her\n",Golf::ClubOf(c).en,state.clubLen[static_cast<size_t>(c)]/K,
                       state.clubTilt[static_cast<size_t>(c)]/Deg,state.clubHead[static_cast<size_t>(c)][0]/K,state.clubHead[static_cast<size_t>(c)][2]/K);
            text+=line;
        }
        swprintf_s(line,L"golf: %zu trees, %zu drawn this frame\n",Course().Trees().size(),state.treesShown);
        text+=line;
        text+=L"golf play:\n"+state.log;
        if(!state.words.told.empty())text+=L"golf words:\n"+state.words.told;
        return text;
    }
}
