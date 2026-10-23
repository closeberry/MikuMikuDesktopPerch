#define NOMINMAX
#include "MascotModel.hpp"
#include "MascotRenderer.hpp"
#include "Settings.hpp"
#include "Surfaces.hpp"
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>
#include <filesystem>
#include <algorithm>
#include <climits>
#include <crtdbg.h>
#include <dbghelp.h>
#pragma comment(lib,"dbghelp.lib")
#include <cstdlib>
#include <cmath>
#include <cstdarg>
#include <cwctype>
#include <optional>

using namespace DesktopMascot;

namespace
{
    constexpr wchar_t WindowClass[]=L"DesktopMascotWindow";
    constexpr UINT TrayMessage=WM_APP+1;
    enum MenuId:UINT {IdOpenModel=100,IdOpenMotion,IdPhysics,IdBlink,IdBreath,IdLookAt,IdTopmost,IdExit,
                      IdAuto,IdWalkLeft,IdWalkRight,IdWindowMode,
                      IdScaleBase=200};   // IdScaleBase + percent/10 for the 大きさ submenu
    constexpr double LookIdleSeconds=8.0;   // stop following a cursor that has not moved for this long
    constexpr int DragThreshold=4;          // pixels the cursor must move before a press becomes a grab
    constexpr float Gravity=98.0f;          // world units/s², the same as the physics world
    // Jumps and falls move the window at real gravity: 9.8 m/s² with 1 MMD unit = 8 cm.
    constexpr float FlightGravity=122.5f;

    // Where the mascot is: standing on a surface (the ground line or a window's top edge),
    // held by the mouse, falling, in the air on a planned jump, or warping to a window.
    enum class Place { Ground, Held, Falling, Jumping, Warping };

    // Active-window mode: crouching before a short hop to the target surface.
    enum class Travel { None, Crouching };

    // Warp: spin and shrink into a swirl of sparkles, then reappear at the destination the
    // same way in reverse. Only the drawing is spun and scaled; the pose is untouched.
    constexpr double WarpOutSeconds=0.5,WarpInSeconds=0.55;

    // One warp sparkle, moving on a spiral around the model's vertical axis: angle, radius and
    // height go from their start to their end values over its life (world units, relative to
    // the model centre and the ground).
    struct Spark
    {
        float angle0=0,turn=0,radius0=0,radius1=0,y0=0,y1=0;
        float age=0,life=1,size=0.1f,spin=0,spinRate=0;
        float twinkle=0,twinkleRate=0;   // phase and speed of the shimmer
        bool arriving=false;   // converging (ease in-out) rather than flying off (ease out)
        std::array<float,3> color{1,1,1};
    };

    // World region the window shows, relative to the model centre (x) and the ground (y=0).
    struct Extent {float halfWidth=1,top=1,bottom=0;};

    struct App
    {
        Settings settings;
        std::wstring settingsPath;
        MascotModel model;
        MascotRenderer renderer;
        HWND hwnd=nullptr;
        NOTIFYICONDATAW tray{};
        bool clickThrough=false;
        bool paused=false;
        bool restart=false;
        POINT lastCursor{LONG_MIN,LONG_MIN};
        double cursorIdle=0;

        // Window placement in screen pixels, kept in doubles so slow walking accumulates.
        double windowX=0,windowY=0;
        SIZE windowSize{};
        float pixelsPerUnit=40;   // screen pixels per world unit at the model's depth
        float viewYaw=0;          // --view-yaw: camera orbit for diagnostic snapshots

        Place place=Place::Ground;
        bool pressed=false;           // left button down, not yet a grab
        POINT pressCursor{},dragOffset{};
        DirectX::XMFLOAT2 carryVelocity{},carryAcceleration{};   // window motion, px/s and px/s²
        double previousWindowX=0,previousWindowY=0;
        double fallSpeed=0;           // px/s, downward

        Extent extent;
        double extentCheck=0,extentCalm=0;

        // The surface under the feet: a window's top edge, or the ground line when null.
        HWND surfaceWindow=nullptr;
        RECT surfaceRect{};

        // Active-window mode.
        HWND foreground=nullptr;      // last foreground window seen, debounced before acting
        double foregroundSince=0;
        HWND servedForeground=nullptr;   // foreground window already acted on
        HWND destinationWindow=nullptr;
        double destinationX=0,destinationLine=0;   // screen x of the model centre, y of the feet
        Travel travel=Travel::None;
        double travelTimer=0;
        struct Warp {double time=0;bool arrived=false;} warp;
        std::vector<Spark> sparks;
        struct Jump {double startX,startLine,vx,vy,duration,time;} jump{};
        Activity restPose=Activity::Idle; // pose to (re)take on the window once settled
        double restPoseAt=-1;             // clock time to take it, -1 for none
        double avoidCooldown=0;
        bool behindSurface=false;     // hanging: the window sits just behind the surface window
        double hangEndedAt=-1;
        Activity forcedRestPose=Activity::Idle;   // --rest-pose sit|hang (diagnostics)

        // Ground behaviour.
        double walkTargetX=0;         // screen x for the window centre
        double nextDecision=8,clock=0,topmostCheck=0;
        std::uint32_t random=0x2545F491u;
        float Random01(){random^=random<<13;random^=random>>17;random^=random<<5;return (random&0xFFFFFF)/16777216.0f;}
    } app;

    void SaveWindowPosition();

    // Behaviour trace for diagnosis: written only when DESKTOPMASCOT_LOG names a file.
    void Trace(const wchar_t* format,...)
    {
        static const std::wstring path=[]{wchar_t p[MAX_PATH]{};return GetEnvironmentVariableW(L"DESKTOPMASCOT_LOG",p,MAX_PATH)?std::wstring(p):std::wstring();}();
        if(path.empty())return;
        wchar_t text[512]{};
        va_list args;
        va_start(args,format);
        vswprintf_s(text,format,args);
        va_end(args);
        if(FILE* f=nullptr;_wfopen_s(&f,path.c_str(),L"a, ccs=UTF-8")==0&&f){fwprintf(f,L"%s\n",text);fclose(f);}
    }

    void ApplyProceduralSettings()
    {
        app.model.SetProcedural(app.settings.blink,app.settings.breath,app.settings.lookAtMouse);
    }

    // Rest-pose size and centre of the model; world y=0 is the ground.
    struct ModelFrame {float cx,cz,height,width,top;};
    ModelFrame ModelFrameOf()
    {
        const auto lo=app.model.BoundsMin(),hi=app.model.BoundsMax();
        return {(lo[0]+hi[0])*0.5f,(lo[2]+hi[2])*0.5f,std::max(hi[1]-lo[1],1.0f),std::max(hi[0]-lo[0],1.0f),hi[1]};
    }

    // The point placed on the surface line: centre of the feet, at the bottom front of the
    // soles (projecting the feet's centre would let perspective sink the toes below the line).
    std::array<float,3> GroundAnchor()
    {
        const ModelFrame m=ModelFrameOf();
        return {m.cx+app.model.RootOffset(),app.model.BoundsMin()[1],app.model.SoleFrontZ()};
    }

    // The rest pose with a little room; app.extent starts here and grows while hair spreads.
    Extent BaseExtent()
    {
        const ModelFrame m=ModelFrameOf();
        // Generous from the start (sitting legs, spread hair, warp sparkles): every resize moves
        // the window while the old frame is still on screen, which shows as a sideways jump.
        return {std::max(m.width*0.65f,0.8f*m.height),m.top+0.25f*m.height,-0.5f*m.height};
    }

    // Model size on screen follows Height/Scale; the window covers app.extent at that size.
    SIZE WindowSizeFor(int scalePercent)
    {
        const ModelFrame m=ModelFrameOf();
        const int modelPixels=MulDiv(MulDiv(app.settings.windowHeight,static_cast<int>(GetDpiForSystem()),96),scalePercent,100);
        app.pixelsPerUnit=modelPixels/(m.height*1.1f);
        return {static_cast<LONG>(std::lround(2*app.extent.halfWidth*app.pixelsPerUnit)),
                static_cast<LONG>(std::lround((app.extent.top-app.extent.bottom)*app.pixelsPerUnit))};
    }

    // The camera follows センター's walking travel, so the window moves instead of the model leaving it.
    void UpdateCamera()
    {
        const ModelFrame m=ModelFrameOf();
        const float fovY=DirectX::XMConvertToRadians(30.0f);
        const float cx=m.cx+app.model.RootOffset(),cy=(app.extent.top+app.extent.bottom)*0.5f;
        const float distance=(app.extent.top-app.extent.bottom)*0.5f/std::tan(fovY*0.5f);
        // app.viewYaw orbits the camera for diagnostic side views (always 0 in normal use).
        const float ex=cx-std::sin(app.viewYaw)*distance,ez=m.cz-std::cos(app.viewYaw)*distance;
        app.renderer.SetCamera(DirectX::XMVectorSet(ex,cy,ez,1),DirectX::XMVectorSet(cx,cy,m.cz,1),fovY);
    }

    // Screen position of the model's ground point (centre of the feet).
    POINT GroundPointOnScreen()
    {
        const POINT p=app.renderer.Project(GroundAnchor());
        return {static_cast<LONG>(std::lround(app.windowX))+p.x,static_cast<LONG>(std::lround(app.windowY))+p.y};
    }

    // Resize the window for a scale and extent, keeping the model exactly where it is on screen.
    bool ApplyFrame(int scalePercent,const Extent& extent)
    {
        const POINT before=GroundPointOnScreen();
        const Extent previous=app.extent;
        app.extent=extent;
        const SIZE size=WindowSizeFor(scalePercent);
        std::wstring error;
        if(!app.renderer.Resize(static_cast<unsigned>(size.cx),static_cast<unsigned>(size.cy),error))
        {
            app.extent=previous;
            WindowSizeFor(app.settings.scale);
            return false;
        }
        app.windowSize=size;
        UpdateCamera();
        const POINT inWindow=app.renderer.Project(GroundAnchor());
        const double x=before.x-inWindow.x,y=before.y-inWindow.y;
        // A held mascot must stay under the cursor: shift the grab offset with the window.
        app.dragOffset.x+=static_cast<LONG>(std::lround(app.windowX-x));
        app.dragOffset.y+=static_cast<LONG>(std::lround(app.windowY-y));
        app.windowX=x;app.windowY=y;
        SetWindowPos(app.hwnd,nullptr,static_cast<int>(std::lround(x)),static_cast<int>(std::lround(y)),size.cx,size.cy,SWP_NOZORDER|SWP_NOACTIVATE);
        return true;
    }

    // Every quarter second: grow the window when hair or limbs reach past it (physics can
    // spread long hair far sideways or upward), shrink back once the pose has settled.
    void UpdateExtent(double dt)
    {
        app.extentCheck+=dt;
        if(app.extentCheck<0.25)return;
        const double elapsed=app.extentCheck;
        app.extentCheck=0;
        std::array<float,3> lo{},hi{};
        if(!app.model.BoneBounds(lo,hi))return;
        const ModelFrame m=ModelFrameOf();
        const float c=m.cx+app.model.RootOffset(),margin=0.07f*m.height;
        Extent need{std::max(std::abs(lo[0]-c),std::abs(hi[0]-c))+margin,hi[1]+margin,lo[1]-margin};
        // Hanging, everything below the edge is hidden behind the window: no room needed there.
        if(app.model.CurrentActivity()==Activity::Hang)need.bottom=std::max(need.bottom,-0.3f*m.height);
        const Extent base=BaseExtent();
        Extent& e=app.extent;
        if(need.halfWidth>e.halfWidth||need.top>e.top||need.bottom<e.bottom)
        {
            // Grow with headroom so a swinging ponytail does not resize every check.
            const Extent grown{std::min(std::max(e.halfWidth,need.halfWidth*1.15f),1.6f*m.height),
                               std::min(std::max(e.top,need.top+0.05f*m.height),m.top+0.8f*m.height),
                               std::max(std::min(e.bottom,need.bottom-0.05f*m.height),-0.6f*m.height)};
            ApplyFrame(app.settings.scale,grown);
            app.extentCalm=0;
        }
        else
        {
            // Shrink toward what the pose needs now (never below the rest framing) once the
            // window has been clearly too large for 4 seconds. Some models' hair bones sit
            // outside the rest framing even when still, so "fits the base" is not the test.
            const Extent desired{std::max(base.halfWidth,need.halfWidth*1.15f),std::max(base.top,need.top+0.05f*m.height),
                                 std::min(base.bottom,need.bottom-0.05f*m.height)};
            const bool tooLarge=e.halfWidth>desired.halfWidth*1.1f||e.top>desired.top+0.1f*m.height||e.bottom<desired.bottom-0.1f*m.height;
            app.extentCalm=tooLarge?app.extentCalm+elapsed:0;
            if(app.extentCalm>=4.0){ApplyFrame(app.settings.scale,desired);app.extentCalm=0;}
        }
    }

    double CenterX(){return app.windowX+app.windowSize.cx*0.5;}   // model centre on screen

    RECT WorkAreaOfWindow()
    {
        return WorkAreaAt({static_cast<LONG>(CenterX()),static_cast<LONG>(app.windowY+app.windowSize.cy/2)});
    }

    // Screen y of the surface under the feet: a window's top edge, or the ground line (the
    // taskbar top when the taskbar is at the bottom, otherwise the bottom of the screen; both
    // are the work area's bottom edge).
    double SurfaceLine()
    {
        return app.surfaceWindow?static_cast<double>(app.surfaceRect.top):static_cast<double>(WorkAreaOfWindow().bottom);
    }

    // Horizontal range the model centre may use on the current surface.
    void SurfaceRange(double& left,double& right)
    {
        const double half=ModelFrameOf().width*0.5*app.pixelsPerUnit;
        const RECT r=app.surfaceWindow?app.surfaceRect:WorkAreaOfWindow();
        left=r.left+half;right=r.right-half;
        if(left>right)left=right=(r.left+r.right)*0.5;
    }

    // Window top that puts world y=0 (the feet) on a screen line.
    double WindowYForLine(double line)
    {
        return line-app.renderer.Project(GroundAnchor()).y;
    }
    double GroundedWindowY(){return WindowYForLine(SurfaceLine());}
    double FeetLine(){return app.windowY+app.renderer.Project(GroundAnchor()).y;}

    void MoveWindowTo(double x,double y)
    {
        app.windowX=x;app.windowY=y;
        SetWindowPos(app.hwnd,nullptr,static_cast<int>(std::lround(x)),static_cast<int>(std::lround(y)),0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    }

    double ModelHeightPixels(){return ModelFrameOf().top*app.pixelsPerUnit;}

    // Scale around the feet: the model's ground point stays where it is on screen.
    void ApplyScale(int percent)
    {
        if(!ApplyFrame(percent,app.extent))
        {
            MessageBoxW(app.hwnd,L"ウィンドウの大きさを変更できませんでした。",L"DesktopMascot",MB_ICONERROR);
            return;
        }
        app.settings.scale=percent;
        SaveWindowPosition();
    }

    double ModelHalfWidthPixels(){return ModelFrameOf().width*0.5*app.pixelsPerUnit;}

    // Walk (or run) so the model centre reaches targetX on the current surface.
    void StartWalk(double targetX,Activity gait=Activity::Walk)
    {
        double left,right;
        SurfaceRange(left,right);
        app.walkTargetX=std::clamp(targetX,left,right);
        if(std::abs(app.walkTargetX-CenterX())<8)return;
        app.model.SetActivity(gait,app.walkTargetX>CenterX()?1:-1);
    }

    void StopGroundActivity()
    {
        app.model.SetActivity(Activity::Idle);
        app.nextDecision=app.clock+10+app.Random01()*20;
        if(app.restPoseAt==-2)app.restPoseAt=app.clock+0.4;   // stepped aside: rest again right there
    }

    // Occasionally walk somewhere when left alone on the ground. Sitting is kept for window
    // edges: on the taskbar the legs would hang off the screen.
    void UpdateAutoBehavior()
    {
        if(!app.settings.autoBehavior||app.place!=Place::Ground||app.surfaceWindow||app.travel!=Travel::None)return;
        if(app.model.CurrentActivity()!=Activity::Idle||app.clock<app.nextDecision)return;
        if(app.Random01()<0.6f)
        {
            double left,right;
            SurfaceRange(left,right);
            const double distance=(120+app.Random01()*380)*(app.Random01()<0.5f?-1:1);
            double target=CenterX()+distance;
            if(target<left||target>right)target=CenterX()-distance;
            StartWalk(target);
        }
        app.nextDecision=app.clock+15+app.Random01()*25;
    }

    // ---- Active-window mode -------------------------------------------------------------

    // Pick where to go for a window: its top edge, nearest to where the mascot is, or the
    // bottom-right of that screen when standing on the edge would push the head off the top.
    void PlanDestination(HWND window)
    {
        RECT frame{};
        VisibleFrame(window,frame);
        const RECT work=WorkAreaAt({(frame.left+frame.right)/2,(frame.top+frame.bottom)/2});
        const double half=ModelFrameOf().width*0.5*app.pixelsPerUnit;
        if(frame.top-ModelHeightPixels()<work.top+8)
        {
            app.destinationWindow=nullptr;
            app.destinationLine=work.bottom;
            app.destinationX=work.right-half-16;
            return;
        }
        app.destinationWindow=window;
        app.destinationLine=frame.top;
        const double left=frame.left+half,right=frame.right-half;
        app.destinationX=left<=right?std::clamp(CenterX(),left,right):(frame.left+frame.right)*0.5;
    }

    void BeginCrouch()
    {
        const double dx=app.destinationX-CenterX();
        app.model.SetActivity(Activity::Jump,std::abs(dx)<20?0:(dx>0?1:-1));
        app.travel=Travel::Crouching;
        app.travelTimer=JumpCrouchSeconds;
    }

    // Sparkles for leaving (spiralling out and up from the body) or arriving (spiralling in
    // to it); ring=true adds a flat ring spreading over the surface at the feet.
    void SpawnSparks(bool arriving,bool ring=false)
    {
        static constexpr std::array<std::array<float,3>,4> palette{{{0.65f,0.95f,1.0f},{1,1,1},{1,0.75f,0.92f},{1,0.92f,0.55f}}};
        const ModelFrame m=ModelFrameOf();
        const float H=m.height;
        auto r=[&](float lo,float hi){return lo+(hi-lo)*app.Random01();};
        // Mostly small sparkles, some fine glitter, and a few large glints with long rays.
        const int count=ring?60:170;
        for(int i=0;i<count;++i)
        {
            Spark s;
            s.angle0=r(0,DirectX::XM_2PI);
            s.turn=r(1.0f,2.2f)*DirectX::XM_PI*(arriving?-1.0f:1.0f);
            const float kind=app.Random01();
            s.size=(kind<0.35f?r(0.007f,0.014f):kind<0.9f?r(0.018f,0.04f):r(0.06f,0.09f))*H;
            s.spinRate=r(-6.0f,6.0f);
            s.twinkle=r(0,DirectX::XM_2PI);
            s.twinkleRate=r(14.0f,30.0f);
            s.color=palette[static_cast<size_t>(app.Random01()*palette.size())%palette.size()];
            s.arriving=arriving;
            if(ring)
            {
                s.radius0=r(0.05f,0.15f)*H;s.radius1=r(0.45f,0.7f)*H;
                s.y0=s.y1=r(0.0f,0.04f)*H;
                s.turn*=0.25f;
                s.life=r(0.5f,0.8f);
                s.arriving=false;
            }
            else if(arriving)
            {
                s.radius0=r(0.55f,0.8f)*H;s.radius1=r(0.0f,0.15f)*H;
                s.y1=r(0.05f,0.95f)*m.top;s.y0=s.y1+r(-0.15f,0.35f)*H;
                s.life=r(0.35f,0.55f);
                s.age=-r(0.0f,0.12f);
            }
            else
            {
                s.radius0=r(0.0f,0.2f)*H;s.radius1=r(0.45f,0.75f)*H;
                s.y0=r(0.05f,0.95f)*m.top;s.y1=std::min(s.y0+r(0.2f,0.5f)*H,m.top+0.2f*H);
                s.life=r(0.4f,0.7f);
                s.age=-r(0.0f,0.25f);
            }
            app.sparks.push_back(s);
        }
    }

    void UpdateSparks(double dt)
    {
        std::vector<MascotRenderer::Particle> particles;
        const ModelFrame m=ModelFrameOf();
        const float cx=m.cx+app.model.RootOffset();
        for(auto& s:app.sparks)
        {
            s.age+=static_cast<float>(dt);
            s.spin+=s.spinRate*static_cast<float>(dt);
            if(s.age<0||s.age>=s.life)continue;
            const float k=s.age/s.life;
            const float e=s.arriving?k*k*(3-2*k):1-(1-k)*(1-k);
            const float angle=s.angle0+s.turn*e,radius=s.radius0+(s.radius1-s.radius0)*e;
            // Quick fade in, fade out over the last part (arriving sparks brighten as they gather),
            // with a fast shimmer: each spark flares and dims a few times over its life.
            const float shimmer=0.5f+0.5f*std::sin(s.twinkle+s.age*s.twinkleRate);
            const float flare=shimmer*shimmer*shimmer;
            const float alpha=std::min(1.0f,k*8)*(s.arriving?1.0f:std::min(1.0f,(1-k)*2.5f))*(0.35f+0.65f*shimmer);
            const float size=s.size*(s.arriving?1.0f-0.4f*k:1.0f-0.6f*k)*(0.8f+0.5f*flare);
            particles.push_back({{cx+radius*std::cos(angle),s.y0+(s.y1-s.y0)*e,m.cz+radius*std::sin(angle)},size,
                                 {s.color[0],s.color[1],s.color[2],alpha},s.spin});
        }
        std::erase_if(app.sparks,[](const Spark& s){return s.age>=s.life;});
        app.renderer.SetParticles(particles);
    }

    void BeginWarp()
    {
        Trace(L"%.1f warp to x=%.0f line=%.0f window=%p",app.clock,app.destinationX,app.destinationLine,app.destinationWindow);
        app.place=Place::Warping;
        app.travel=Travel::None;
        app.warp={};
        app.model.SetActivity(Activity::Idle);
        SpawnSparks(false);
        // Hair and skirt ease back while the body spins away, and simulate afresh on arrival.
        app.model.ResetPhysics();
    }

    void Land(HWND window,double impactPixelsPerSecond);

    // Spin up and shrink away, move the window, then grow and spin down at the destination.
    void UpdateWarp(double dt)
    {
        auto& w=app.warp;
        w.time+=dt;
        if(!w.arrived&&w.time>=WarpOutSeconds)
        {
            w.arrived=true;
            app.sparks.clear();
            app.surfaceWindow=app.destinationWindow;
            if(app.surfaceWindow)VisibleFrame(app.surfaceWindow,app.surfaceRect);
            MoveWindowTo(app.destinationX-app.windowSize.cx*0.5,WindowYForLine(app.destinationLine));
            SpawnSparks(true);
        }
        // Spin about the vertical axis through the middle of the body.
        const ModelFrame m=ModelFrameOf();
        float scale,angle;
        if(!w.arrived)
        {
            const float k=static_cast<float>(w.time/WarpOutSeconds);
            scale=1-k*k*(3-2*k);
            angle=4*DirectX::XM_PI*k*k;
        }
        else
        {
            const float k=std::min(1.0f,static_cast<float>((w.time-WarpOutSeconds)/WarpInSeconds));
            scale=k*k*(3-2*k);
            angle=-4*DirectX::XM_PI*(1-k)*(1-k);
        }
        using namespace DirectX;
        const XMVECTOR pivot=XMVectorSet(m.cx+app.model.RootOffset(),0.45f*m.top,m.cz,0);
        app.renderer.SetModelTransform(XMMatrixTranslationFromVector(XMVectorNegate(pivot))*
                                       XMMatrixScaling(std::max(scale,0.001f),std::max(scale,0.001f),std::max(scale,0.001f))*
                                       XMMatrixRotationY(angle)*XMMatrixTranslationFromVector(pivot));
        if(w.time>=WarpOutSeconds+WarpInSeconds)
        {
            app.renderer.SetModelTransform(XMMatrixIdentity());
            SpawnSparks(false,true);
            Land(app.destinationWindow,0);
        }
    }

    // Short hops (the target just above or anywhere below, close by) are jumped; walking or
    // running covers the same surface; everything else is a warp.
    void BeginTravel(HWND window)
    {
        app.servedForeground=window;
        PlanDestination(window);
        Trace(L"%.1f travel to %p: x=%.0f line=%.0f window=%p (from x=%.0f line=%.0f)",app.clock,window,
              app.destinationX,app.destinationLine,app.destinationWindow,CenterX(),SurfaceLine());
        app.restPoseAt=-1;
        const bool sameSurface=app.destinationWindow==app.surfaceWindow&&std::abs(app.destinationLine-SurfaceLine())<2;
        const double h=ModelHeightPixels(),dx=app.destinationX-CenterX(),rise=FeetLine()-app.destinationLine;
        if(sameSurface&&std::abs(dx)<2.5*h)
        {
            StartWalk(app.destinationX,std::abs(dx)>h?Activity::Run:Activity::Walk);
            app.travel=Travel::None;
            return;
        }
        if(!sameSurface&&std::abs(dx)<0.8*h&&rise<0.6*h)BeginCrouch();
        else BeginWarp();
    }

    // Ballistic arc from the feet now to the destination: up to an apex above both ends, at real
    // gravity, so the body slows toward the apex and speeds up on the way down.
    void Launch()
    {
        const double g=FlightGravity*app.pixelsPerUnit,h=ModelHeightPixels();
        const double startLine=FeetLine(),endLine=app.destinationLine,dx=app.destinationX-CenterX();
        const double apex=std::min(startLine,endLine)-std::clamp(0.25*h+0.1*std::abs(dx),0.15*h,0.8*h);
        const double up=std::sqrt(2*(startLine-apex)/g),down=std::sqrt(2*(endLine-apex)/g);
        app.jump={CenterX(),startLine,dx/(up+down),-g*up,up+down,0};
        app.place=Place::Jumping;
        app.travel=Travel::None;
    }

    void Land(HWND window,double impactPixelsPerSecond)
    {
        Trace(L"%.1f land on %p at x=%.0f",app.clock,window,CenterX());
        app.surfaceWindow=window;
        if(window)VisibleFrame(window,app.surfaceRect);
        app.place=Place::Ground;
        app.model.Land(static_cast<float>(impactPixelsPerSecond/app.pixelsPerUnit));
        StopGroundActivity();
        if(window)
        {
            // Settle, then sit or hang from the edge (--rest-pose fixes it for checks). Lying down
            // is left out: physics too often tangles hair and skirt on a body turned on its side.
            app.restPose=app.Random01()<0.55f?Activity::Sit:Activity::Hang;
            if(app.forcedRestPose!=Activity::Idle)app.restPose=app.forcedRestPose;
            app.restPoseAt=app.clock+1.2+app.Random01()*1.5;
        }
    }

    // Screen rectangle around the legs (下半身 to the toes), widened for the skirt and the mesh.
    RECT LegScreenRect()
    {
        std::array<float,3> lo{},hi{};
        RECT r{};
        if(!app.model.LegBounds(lo,hi))return r;
        const ModelFrame m=ModelFrameOf();
        const POINT a=app.renderer.Project({lo[0],lo[1],m.cz}),b=app.renderer.Project({hi[0],hi[1],m.cz});
        const LONG side=static_cast<LONG>(0.07*m.height*app.pixelsPerUnit),end=static_cast<LONG>(0.03*m.height*app.pixelsPerUnit);
        r={std::min(a.x,b.x)-side,std::min(a.y,b.y)-end,std::max(a.x,b.x)+side,std::max(a.y,b.y)+end};
        OffsetRect(&r,static_cast<int>(app.windowX),static_cast<int>(app.windowY));
        return r;
    }

    // While resting on a window, step aside when the cursor comes near the legs where they
    // cover the window: a short sidestep, then rest again where it stopped.
    // Parts above its top edge never trigger, so the mascot can still be grabbed or
    // right-clicked there. Hanging is behind the window, so nothing there is in the way.
    void AvoidCursor(POINT cursor)
    {
        if(!app.surfaceWindow||app.place!=Place::Ground||app.travel!=Travel::None||app.clock<app.avoidCooldown)return;
        const Activity a=app.model.CurrentActivity();
        if(a==Activity::Walk||a==Activity::Run||a==Activity::Jump||a==Activity::Hang)return;
        RECT legs=LegScreenRect(),covered{};
        if(!IntersectRect(&covered,&legs,&app.surfaceRect))return;
        InflateRect(&covered,16,16);
        covered.top=std::max(covered.top,app.surfaceRect.top);
        if(!PtInRect(&covered,cursor))return;
        Trace(L"%.1f avoid cursor %ld,%ld (covered %ld,%ld-%ld,%ld)",app.clock,cursor.x,cursor.y,covered.left,covered.top,covered.right,covered.bottom);
        if(a!=Activity::Idle)app.restPose=a;
        double left,right;
        SurfaceRange(left,right);
        // A short sidestep away from the cursor (the other way at the end of the window); if
        // the cursor is still in the way after the cooldown, it steps again.
        const int away=cursor.x<CenterX()?1:-1;
        const double step=0.3*ModelHeightPixels();
        double target=std::clamp(CenterX()+away*step,left,right);
        if(std::abs(target-CenterX())<15)target=std::clamp(CenterX()-away*step,left,right);
        if(std::abs(target-CenterX())<15)return;
        StartWalk(target,Activity::Walk);
        app.restPoseAt=-2;   // rest again when the walk ends
        app.avoidCooldown=app.clock+1.5;
    }

    // Hanging puts the mascot's window right behind the window it hangs from, so the body is
    // hidden and only the face and hands show over the edge. Leaving the pose restores topmost a moment
    // later, once the body has come up over the edge.
    void UpdateHangOrder()
    {
        const bool hanging=app.place==Place::Ground&&app.surfaceWindow&&app.model.CurrentActivity()==Activity::Hang;
        if(hanging)app.hangEndedAt=-1;
        else if(app.behindSurface&&app.hangEndedAt<0)app.hangEndedAt=app.clock;
        const bool behind=hanging||(app.behindSurface&&app.surfaceWindow&&app.place==Place::Ground&&app.clock-app.hangEndedAt<0.3);
        if(behind==app.behindSurface)return;
        app.behindSurface=behind;
        if(behind)
        {
            SetWindowPos(app.hwnd,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
            SetWindowPos(app.hwnd,app.surfaceWindow,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
        }
        else SetWindowPos(app.hwnd,app.settings.topmost?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
    }

    void UpdateWindowMode(double dt)
    {
        if(!app.settings.windowMode)return;
        const HWND foreground=GetForegroundWindow();
        if(foreground!=app.foreground)
        {
            app.foreground=foreground;
            app.foregroundSince=app.clock;
            wchar_t title[96]{};
            GetWindowTextW(foreground,title,96);
            Trace(L"%.1f foreground %p '%s' standable=%d place=%d travel=%d",app.clock,foreground,title,
                  IsStandableWindow(foreground,app.hwnd,static_cast<int>(ModelFrameOf().width*app.pixelsPerUnit)),
                  static_cast<int>(app.place),static_cast<int>(app.travel));
        }

        if(app.place==Place::Ground)
        {
            // Ride along with the window under the feet; fall when it goes away.
            if(app.surfaceWindow)
            {
                RECT frame{};
                if(!IsStandableWindow(app.surfaceWindow,app.hwnd,0)||!VisibleFrame(app.surfaceWindow,frame))
                {
                    app.surfaceWindow=nullptr;
                    app.place=Place::Falling;
                    app.fallSpeed=0;
                    app.travel=Travel::None;
                    app.model.SetActivity(Activity::Fall);
                    return;
                }
                const LONG dx=frame.left-app.surfaceRect.left;
                app.surfaceRect=frame;
                if(dx)MoveWindowTo(app.windowX+dx,app.windowY);
                // Maximized or dragged to the top: the head would leave the screen.
                if(frame.top-ModelHeightPixels()<WorkAreaOfWindow().top+8&&app.travel==Travel::None)
                {
                    BeginTravel(app.surfaceWindow);
                    return;
                }
            }

            // Once airborne the destination is fixed until landing.
            if(app.travel==Travel::Crouching&&(app.travelTimer-=dt)<=0){Launch();return;}

            // A new foreground window (settled for a moment): go there.
            if(app.travel==Travel::None&&app.clock-app.foregroundSince>0.6&&foreground!=app.surfaceWindow&&
               foreground!=app.servedForeground&&IsStandableWindow(foreground,app.hwnd,static_cast<int>(ModelFrameOf().width*app.pixelsPerUnit)))
            {
                BeginTravel(foreground);
                return;
            }

            // Settled on a window: take (or resume) the resting pose.
            if(app.surfaceWindow&&app.travel==Travel::None&&app.restPoseAt>=0&&app.clock>=app.restPoseAt&&
               app.model.CurrentActivity()==Activity::Idle)
            {
                // Sitting faces one side or the other at random (knees to that side).
                app.model.SetActivity(app.restPose,app.Random01()<0.5f?-1:1);
                app.restPoseAt=-1;
            }
        }
    }

    // Turn the cursor offset from the head on screen into a yaw/pitch for the head to face.
    void UpdateLookTarget(POINT cursor,const RECT& window,double dt)
    {
        if(cursor.x!=app.lastCursor.x||cursor.y!=app.lastCursor.y){app.cursorIdle=0;app.lastCursor=cursor;}
        else app.cursorIdle+=dt;
        std::array<float,3> head{};
        const Activity a=app.model.CurrentActivity();
        if(!app.settings.lookAtMouse||a==Activity::Walk||a==Activity::Fall||!app.model.HeadPosition(head))
        {
            app.model.SetLookTarget(0,0,false);
            return;
        }
        const POINT h=app.renderer.Project(head);
        const float dx=static_cast<float>(cursor.x-(window.left+h.x));
        const float dy=static_cast<float>(cursor.y-(window.top+h.y));
        // Treat the viewer as sitting about one window height away from the screen.
        const float distance=static_cast<float>(window.bottom-window.top);
        const float yaw=std::clamp(std::atan2(dx,distance),DirectX::XMConvertToRadians(-40.0f),DirectX::XMConvertToRadians(40.0f));
        const float pitch=std::clamp(std::atan2(-dy,distance),DirectX::XMConvertToRadians(-20.0f),DirectX::XMConvertToRadians(25.0f));
        app.model.SetLookTarget(yaw,pitch,app.cursorIdle<LookIdleSeconds);
    }

    std::wstring OpenFileDialog(HWND owner,const wchar_t* filter,const std::wstring& current)
    {
        wchar_t path[MAX_PATH]{};
        std::wstring initialDir;
        if(!current.empty())initialDir=std::filesystem::path(current).parent_path().wstring();
        OPENFILENAMEW ofn{};
        ofn.lStructSize=sizeof(ofn);
        ofn.hwndOwner=owner;
        ofn.lpstrFilter=filter;
        ofn.lpstrFile=path;
        ofn.nMaxFile=MAX_PATH;
        ofn.lpstrInitialDir=initialDir.empty()?nullptr:initialDir.c_str();
        ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
        return GetOpenFileNameW(&ofn)?std::wstring(path):std::wstring();
    }

    // Only X is remembered: the mascot always starts on the ground.
    void SaveWindowPosition()
    {
        if(app.hwnd)app.settings.x=static_cast<int>(app.windowX);
        app.settings.Save(app.settingsPath);
    }

    // Clicking the taskbar raises it above other topmost windows, mascot included. Put the
    // mascot back on top, but never over shell overlays (Start menu, flyouts, notifications),
    // which are topmost windows of their own when they take the foreground.
    void ReassertTopmost()
    {
        if(!app.settings.topmost||app.paused||!app.hwnd||app.behindSurface)return;
        if(HWND foreground=GetForegroundWindow();foreground&&foreground!=app.hwnd)
        {
            wchar_t name[64]{};
            GetClassNameW(foreground,name,static_cast<int>(std::size(name)));
            const bool taskbar=wcscmp(name,L"Shell_TrayWnd")==0||wcscmp(name,L"Shell_SecondaryTrayWnd")==0;
            const bool overlay=(GetWindowLongPtrW(foreground,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0;
            if(overlay&&!taskbar)return;
        }
        SetWindowPos(app.hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
    }

    void CALLBACK OnForegroundChanged(HWINEVENTHOOK,DWORD,HWND,LONG,LONG,DWORD,DWORD)
    {
        ReassertTopmost();
    }

    void SetClickThrough(bool enabled)
    {
        if(enabled==app.clickThrough)return;
        app.clickThrough=enabled;
        auto style=GetWindowLongPtrW(app.hwnd,GWL_EXSTYLE);
        style=enabled?(style|WS_EX_TRANSPARENT):(style&~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
        SetWindowLongPtrW(app.hwnd,GWL_EXSTYLE,style);
    }

    void ShowMenu(POINT screen)
    {
        HMENU menu=CreatePopupMenu();
        AppendMenuW(menu,MF_STRING,IdOpenModel,L"モデルを開く...");
        AppendMenuW(menu,MF_STRING,IdOpenMotion,L"モーションを開く...");
        HMENU scaleMenu=CreatePopupMenu();
        for(int percent=Settings::MinScale;percent<=Settings::MaxScale;percent+=10)
        {
            const std::wstring label=std::to_wstring(percent)+L"%";
            AppendMenuW(scaleMenu,MF_STRING|(percent==app.settings.scale?MF_CHECKED:0),IdScaleBase+percent/10,label.c_str());
        }
        AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(scaleMenu),L"大きさ");
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        const bool grounded=app.place==Place::Ground;
        AppendMenuW(menu,MF_STRING|(app.settings.windowMode?MF_CHECKED:0),IdWindowMode,L"アクティブウィンドウに乗る");
        AppendMenuW(menu,MF_STRING|(app.settings.autoBehavior?MF_CHECKED:0),IdAuto,L"おまかせ行動");
        AppendMenuW(menu,MF_STRING|(grounded?0:MF_GRAYED),IdWalkLeft,L"左へ歩く");
        AppendMenuW(menu,MF_STRING|(grounded?0:MF_GRAYED),IdWalkRight,L"右へ歩く");
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        AppendMenuW(menu,MF_STRING|(app.model.PhysicsEnabled()?MF_CHECKED:0),IdPhysics,L"物理演算");
        AppendMenuW(menu,MF_STRING|(app.settings.blink?MF_CHECKED:0),IdBlink,L"まばたき");
        AppendMenuW(menu,MF_STRING|(app.settings.breath?MF_CHECKED:0),IdBreath,L"呼吸");
        AppendMenuW(menu,MF_STRING|(app.settings.lookAtMouse?MF_CHECKED:0),IdLookAt,L"マウスの方を見る");
        AppendMenuW(menu,MF_STRING|(app.settings.topmost?MF_CHECKED:0),IdTopmost,L"最前面に表示");
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        AppendMenuW(menu,MF_STRING,IdExit,L"終了");
        SetForegroundWindow(app.hwnd);
        TrackPopupMenu(menu,TPM_RIGHTBUTTON,screen.x,screen.y,0,app.hwnd,nullptr);
        PostMessageW(app.hwnd,WM_NULL,0,0);
        DestroyMenu(menu);
    }

    void OnCommand(UINT id)
    {
        if(id>=IdScaleBase+Settings::MinScale/10&&id<=IdScaleBase+Settings::MaxScale/10)
        {
            ApplyScale(static_cast<int>(id-IdScaleBase)*10);
            return;
        }
        switch(id)
        {
        case IdOpenModel:
        {
            auto path=OpenFileDialog(app.hwnd,L"PMX モデル (*.pmx)\0*.pmx\0",app.settings.modelPath);
            if(path.empty())break;
            // Validate before restarting so a non-humanoid model never replaces the current one.
            {
                MascotModel probe;
                std::wstring error;
                if(!probe.Load(path,{},false,app.settings.physicsFps,error))
                {
                    MessageBoxW(app.hwnd,error.c_str(),L"DesktopMascot",MB_ICONWARNING);
                    break;
                }
            }
            // The window size follows the model's proportions, so restart with the new model.
            app.settings.modelPath=path;
            app.restart=true;
            DestroyWindow(app.hwnd);
            break;
        }
        case IdOpenMotion:
        {
            auto path=OpenFileDialog(app.hwnd,L"VMD モーション (*.vmd)\0*.vmd\0",app.settings.motionPath.empty()?app.settings.modelPath:app.settings.motionPath);
            if(path.empty())break;
            std::wstring error;
            app.renderer.WaitIdle();
            if(!app.model.Load(app.settings.modelPath,path,app.settings.physics,app.settings.physicsFps,error)||
               !app.renderer.LoadModel(app.model,app.settings.toonDir,static_cast<unsigned>(app.settings.maxTextureSize),error))
            {
                MessageBoxW(app.hwnd,error.c_str(),L"DesktopMascot",MB_ICONERROR);
                break;
            }
            ApplyProceduralSettings();
            app.settings.motionPath=path;
            app.settings.Save(app.settingsPath);
            break;
        }
        case IdWindowMode:
            app.settings.windowMode=!app.settings.windowMode;
            app.servedForeground=nullptr;
            app.foregroundSince=app.clock;
            if(!app.settings.windowMode&&app.surfaceWindow&&app.place==Place::Ground)
            {
                // Leaving the mode while on a window: drop down to the ground line.
                app.surfaceWindow=nullptr;
                app.travel=Travel::None;
                app.restPoseAt=-1;
                app.place=Place::Falling;
                app.fallSpeed=0;
                app.model.SetActivity(Activity::Fall);
            }
            app.settings.Save(app.settingsPath);
            break;
        case IdAuto:
            app.settings.autoBehavior=!app.settings.autoBehavior;
            app.nextDecision=app.clock+5;
            app.settings.Save(app.settingsPath);
            break;
        case IdWalkLeft:
        case IdWalkRight:
            if(app.place==Place::Ground)StartWalk(app.windowX+app.windowSize.cx*0.5+(id==IdWalkLeft?-300:300));
            break;
        case IdBlink:
        case IdBreath:
        case IdLookAt:
        {
            bool& flag=id==IdBlink?app.settings.blink:(id==IdBreath?app.settings.breath:app.settings.lookAtMouse);
            flag=!flag;
            ApplyProceduralSettings();
            app.settings.Save(app.settingsPath);
            break;
        }
        case IdPhysics:
            app.settings.physics=!app.model.PhysicsEnabled();
            app.model.SetPhysics(app.settings.physics);
            app.settings.Save(app.settingsPath);
            break;
        case IdTopmost:
            app.settings.topmost=!app.settings.topmost;
            if(!app.behindSurface)SetWindowPos(app.hwnd,app.settings.topmost?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            app.settings.Save(app.settingsPath);
            break;
        case IdExit:
            DestroyWindow(app.hwnd);
            break;
        }
    }

    LRESULT CALLBACK WindowProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
    {
        switch(message)
        {
        case WM_LBUTTONDOWN:
        {
            // A press only becomes a grab once the cursor moves a little.
            GetCursorPos(&app.pressCursor);
            app.dragOffset={static_cast<LONG>(app.pressCursor.x-app.windowX),static_cast<LONG>(app.pressCursor.y-app.windowY)};
            app.pressed=true;
            SetCapture(hwnd);
            return 0;
        }
        case WM_MOUSEMOVE:
            if(app.pressed||app.place==Place::Held)
            {
                POINT cursor{};
                GetCursorPos(&cursor);
                if(app.place!=Place::Held&&(std::abs(cursor.x-app.pressCursor.x)>DragThreshold||std::abs(cursor.y-app.pressCursor.y)>DragThreshold))
                {
                    app.place=Place::Held;
                    app.pressed=false;
                    // Lifted off whatever it stood on; any trip in progress is abandoned.
                    app.surfaceWindow=nullptr;
                    app.travel=Travel::None;
                    app.restPoseAt=-1;
                    app.renderer.SetModelTransform(DirectX::XMMatrixIdentity());
                    app.previousWindowX=app.windowX;app.previousWindowY=app.windowY;
                    app.carryVelocity=app.carryAcceleration={};
                    app.model.SetActivity(Activity::Grabbed);
                }
                if(app.place==Place::Held)MoveWindowTo(cursor.x-app.dragOffset.x,cursor.y-app.dragOffset.y);
            }
            return 0;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if(app.place==Place::Held)
            {
                // Let go: fall to the ground line (or settle at once if already there or below).
                app.place=Place::Falling;
                app.fallSpeed=std::max(0.0f,app.carryVelocity.y*0.5f);
                app.model.SetActivity(Activity::Fall);
            }
            if(app.pressed||message==WM_LBUTTONUP)
            {
                app.pressed=false;
                if(GetCapture()==hwnd)ReleaseCapture();
            }
            return 0;
        case WM_RBUTTONUP:
        {
            POINT p{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd,&p);
            ShowMenu(p);
            return 0;
        }
        case TrayMessage:
            if(LOWORD(lParam)==WM_RBUTTONUP||LOWORD(lParam)==WM_CONTEXTMENU)
            {
                POINT p{};
                GetCursorPos(&p);
                ShowMenu(p);
            }
            return 0;
        case WM_COMMAND:
            OnCommand(LOWORD(wParam));
            return 0;
        case WM_DESTROY:
            SaveWindowPosition();
            Shell_NotifyIconW(NIM_DELETE,&app.tray);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd,message,wParam,lParam);
    }

    bool IsFullscreenAppActive()
    {
        QUERY_USER_NOTIFICATION_STATE state{};
        if(FAILED(SHQueryUserNotificationState(&state)))return false;
        return state==QUNS_BUSY||state==QUNS_RUNNING_D3D_FULL_SCREEN||state==QUNS_PRESENTATION_MODE;
    }

    double Seconds(LARGE_INTEGER a,LARGE_INTEGER b,LARGE_INTEGER frequency)
    {
        return static_cast<double>(b.QuadPart-a.QuadPart)/frequency.QuadPart;
    }
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int)
{
    // Diagnostic runs must never block on a CRT assertion or abort dialog.
    if(const std::wstring line=GetCommandLineW();line.find(L"--snapshot")!=std::wstring::npos||line.find(L"--check-list")!=std::wstring::npos)
    {
        _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL,[](int,char* message,int*)->int{
            // Print the call stack of the failing assertion, then stop.
            fputs(message,stderr);
            HANDLE process=GetCurrentProcess();
            SymSetOptions(SYMOPT_LOAD_LINES|SYMOPT_UNDNAME);
            SymInitialize(process,nullptr,TRUE);
            void* frames[48]{};
            const USHORT count=CaptureStackBackTrace(0,48,frames,nullptr);
            for(USHORT i=0;i<count;++i)
            {
                alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO)+256]{};
                auto* symbol=reinterpret_cast<SYMBOL_INFO*>(buffer);
                symbol->SizeOfStruct=sizeof(SYMBOL_INFO);symbol->MaxNameLen=255;
                IMAGEHLP_LINE64 source{sizeof(source)};
                DWORD64 offset=0;DWORD column=0;
                const auto address=reinterpret_cast<DWORD64>(frames[i]);
                const bool named=SymFromAddr(process,address,&offset,symbol)!=FALSE;
                const bool lined=SymGetLineFromAddr64(process,address,&column,&source)!=FALSE;
                fprintf(stderr,"  %s  %s:%lu\n",named?symbol->Name:"?",lined?source.FileName:"?",lined?source.LineNumber:0ul);
            }
            fflush(stderr);
            TerminateProcess(process,99);
            return TRUE;
        });
        _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR,_CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR,_CRTDBG_FILE_STDERR);
        _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
        SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    }

    // Diagnostics: DesktopMascot.exe --snapshot <out.png> [frames] renders, saves one frame and exits.
    // --look <x> <y> pins the look-at cursor to that client-area point (for snapshot checks).
    std::wstring snapshotPath;
    int snapshotFrames=60;
    std::optional<POINT> snapshotLook;
    std::wstring snapshotModel;   // --model <pmx>: model for this run only
    int snapshotScale=0;          // --scale <percent>: resize through the menu path after start-up
    std::wstring snapshotActivity;   // --activity walk|sit|grab|fall: hold that generated activity
    {
        int argc=0;
        LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
        for(int i=1;i<argc;++i)
        {
            const std::wstring arg=argv[i];
            if(arg==L"--snapshot"&&i+1<argc)
            {
                snapshotPath=argv[++i];
                if(i+1<argc&&iswdigit(argv[i+1][0]))snapshotFrames=std::max(1,_wtoi(argv[++i]));
            }
            else if(arg==L"--model"&&i+1<argc)
            {
                snapshotModel=argv[++i];
            }
            else if(arg==L"--scale"&&i+1<argc)
            {
                snapshotScale=_wtoi(argv[++i]);
            }
            else if(arg==L"--activity"&&i+1<argc)
            {
                snapshotActivity=argv[++i];
            }
            else if(arg==L"--rest-pose"&&i+1<argc)
            {
                const std::wstring pose=argv[++i];
                app.forcedRestPose=pose==L"sit"?Activity::Sit:pose==L"hang"?Activity::Hang:Activity::Idle;
            }
            else if(arg==L"--view-yaw"&&i+1<argc)
            {
                app.viewYaw=DirectX::XMConvertToRadians(static_cast<float>(_wtof(argv[++i])));
            }
            else if(arg==L"--look"&&i+2<argc)
            {
                snapshotLook=POINT{_wtoi(argv[i+1]),_wtoi(argv[i+2])};
                i+=2;
            }
        }
        LocalFree(argv);
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);

    // Diagnostics: --check-list <in.txt> <out.tsv> runs the humanoid / T-stance checks on
    // every PMX path listed (UTF-8, one per line) without opening a window.
    {
        int argc=0;
        LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
        const bool check=argc>=4&&std::wstring(argv[1])==L"--check-list";
        const std::wstring listPath=check?argv[2]:L"",outPath=check?argv[3]:L"";
        LocalFree(argv);
        if(check)
        {
            FILE* in=nullptr;FILE* out=nullptr;
            if(_wfopen_s(&in,listPath.c_str(),L"r, ccs=UTF-8")!=0||!in)return 1;
            if(_wfopen_s(&out,outPath.c_str(),L"w, ccs=UTF-8")!=0||!out){fclose(in);return 1;}
            wchar_t line[2048];
            while(fgetws(line,2048,in))
            {
                std::wstring path=line;
                while(!path.empty()&&(path.back()==L'\n'||path.back()==L'\r'))path.pop_back();
                if(path.empty())continue;
                MascotModel probe;
                std::wstring error;
                const bool ok=probe.Load(path,{},false,60,error);
                for(auto& c:error)if(c==L'\n'||c==L'\t')c=L' ';
                const auto rest=probe.ArmRestAngles(),fix=probe.ArmCorrections();
                fwprintf(out,L"%s\t%.1f\t%.1f\t%.1f\t%s\n",ok?L"ok":L"ng",ok?rest[0]:0.0f,ok?fix[0]:0.0f,ok?fix[1]:0.0f,ok?path.c_str():error.c_str());
                fflush(out);
            }
            fclose(in);fclose(out);
            return 0;
        }
    }

    app.settingsPath=Settings::DefaultPath();
    app.settings.Load(app.settingsPath);
    if(!snapshotPath.empty())
    {
        // Diagnostic runs must not overwrite the user's settings.
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH,temp);
        app.settingsPath=std::wstring(temp)+L"DesktopMascot_snapshot.ini";
        if(!snapshotModel.empty())app.settings.modelPath=snapshotModel;
        app.settings.x=app.settings.y=-1;
    }

    // Keep asking for a model until one loads (or the user cancels).
    std::wstring error;
    for(;;)
    {
        if(app.settings.modelPath.empty()||!std::filesystem::exists(app.settings.modelPath))
        {
            app.settings.modelPath=OpenFileDialog(nullptr,L"PMX モデル (*.pmx)\0*.pmx\0",{});
            if(app.settings.modelPath.empty())return 0;
        }
        if(app.model.Load(app.settings.modelPath,app.settings.motionPath,app.settings.physics,app.settings.physicsFps,error))break;
        if(!snapshotPath.empty())
        {
            // Diagnostic runs report instead of blocking on a dialog.
            if(FILE* info=nullptr;_wfopen_s(&info,(snapshotPath+L".txt").c_str(),L"w, ccs=UTF-8")==0&&info)
            {fwprintf(info,L"error=%s\n",error.c_str());fclose(info);}
            return 2;
        }
        MessageBoxW(nullptr,error.c_str(),L"DesktopMascot",MB_ICONERROR);
        app.settings.modelPath.clear();
    }
    app.settings.Save(app.settingsPath);
    ApplyProceduralSettings();

    app.extent=BaseExtent();
    const SIZE size=WindowSizeFor(app.settings.scale);
    const int width=size.cx,height=size.cy;

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
    int x=app.settings.x,y=work.bottom-height;
    if(x==-1)x=work.right-width;
    // Keep the window reachable if the saved position is off every monitor.
    RECT wanted{x,y,x+width,y+height};
    if(!MonitorFromRect(&wanted,MONITOR_DEFAULTTONULL))x=work.right-width;
    app.windowX=x;app.windowY=y;app.windowSize=size;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc=WindowProc;
    wc.hInstance=instance;
    wc.hCursor=LoadCursorW(nullptr,IDC_HAND);
    wc.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
    wc.lpszClassName=WindowClass;
    RegisterClassExW(&wc);
    // NOREDIRECTIONBITMAP + DirectComposition gives per-pixel alpha;
    // LAYERED is only there so WS_EX_TRANSPARENT can pass clicks through.
    const DWORD exStyle=WS_EX_NOREDIRECTIONBITMAP|WS_EX_LAYERED|WS_EX_TOOLWINDOW|(app.settings.topmost?WS_EX_TOPMOST:0);
    app.hwnd=CreateWindowExW(exStyle,WindowClass,L"DesktopMascot",WS_POPUP,x,y,width,height,nullptr,nullptr,instance,nullptr);
    if(!app.hwnd)return 1;
    SetLayeredWindowAttributes(app.hwnd,0,255,LWA_ALPHA);

    if(!app.renderer.Initialize(app.hwnd,width,height,app.settings.msaa,error)||
       !app.renderer.LoadModel(app.model,app.settings.toonDir,static_cast<unsigned>(app.settings.maxTextureSize),error))
    {
        MessageBoxW(nullptr,error.c_str(),L"DesktopMascot",MB_ICONERROR);
        DestroyWindow(app.hwnd);
        return 1;
    }
    UpdateCamera();
    MoveWindowTo(app.windowX,GroundedWindowY());
    app.nextDecision=8+app.Random01()*10;
    std::optional<Activity> forcedActivity;   // --activity for snapshot checks
    if(snapshotActivity==L"walk")forcedActivity=Activity::Walk;
    else if(snapshotActivity==L"run")forcedActivity=Activity::Run;
    else if(snapshotActivity==L"jump")forcedActivity=Activity::Jump;
    else if(snapshotActivity==L"lie")forcedActivity=Activity::Lie;
    else if(snapshotActivity==L"hang")forcedActivity=Activity::Hang;
    else if(snapshotActivity==L"sit")forcedActivity=Activity::Sit;
    else if(snapshotActivity==L"grab")forcedActivity=Activity::Grabbed;
    else if(snapshotActivity==L"fall")forcedActivity=Activity::Fall;
    if(!snapshotPath.empty())app.settings.autoBehavior=false;
    if(forcedActivity)app.model.SetActivity(*forcedActivity,1);

    app.tray.cbSize=sizeof(app.tray);
    app.tray.hWnd=app.hwnd;
    app.tray.uID=1;
    app.tray.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;
    app.tray.uCallbackMessage=TrayMessage;
    app.tray.hIcon=wc.hIcon;
    wcscpy_s(app.tray.szTip,L"DesktopMascot");
    Shell_NotifyIconW(NIM_ADD,&app.tray);

    ShowWindow(app.hwnd,SW_SHOWNOACTIVATE);
    // Event-driven: called on the message loop whenever another window takes the foreground.
    HWINEVENTHOOK foregroundHook=SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,nullptr,OnForegroundChanged,
                                                 0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
    if(snapshotScale>0)ApplyScale(std::clamp(snapshotScale/10*10,Settings::MinScale,Settings::MaxScale));

    // Frame pacing: a periodic high-resolution timer wakes the loop; messages wake it too.
    HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
    if(!timer)timer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
    LARGE_INTEGER due{};
    due.QuadPart=-1;
    SetWaitableTimer(timer,&due,std::max(1,1000/app.settings.fps),nullptr,nullptr,FALSE);

    LARGE_INTEGER frequency{},last{},lastFullscreenCheck{};
    std::uint32_t lastPhysicsResets=0;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&last);
    lastFullscreenCheck=last;

    bool running=true;
    while(running)
    {
        const DWORD wait=MsgWaitForMultipleObjectsEx(1,&timer,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        MSG msg;
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE))
        {
            if(msg.message==WM_QUIT){running=false;break;}
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if(!running||wait!=WAIT_OBJECT_0)continue;

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if(Seconds(lastFullscreenCheck,now,frequency)>2.0)
        {
            lastFullscreenCheck=now;
            const bool fullscreen=IsFullscreenAppActive();
            if(fullscreen!=app.paused)
            {
                app.paused=fullscreen;
                ShowWindow(app.hwnd,fullscreen?SW_HIDE:SW_SHOWNOACTIVATE);
            }
        }
        const double dt=Seconds(last,now,frequency);
        last=now;
        if(app.paused)continue;

        app.clock+=dt;
        const float ppu=app.pixelsPerUnit;

        // Window motion while held or falling: velocity for the dangling sway, acceleration
        // as inertia for the physics (free fall makes hair and skirt float).
        if(app.place==Place::Falling)
        {
            // Land on the ground line, or (in active-window mode) on the foreground window's
            // top edge when it lies below the feet within reach.
            HWND landing=nullptr;
            double line=WorkAreaOfWindow().bottom;
            const double feet=FeetLine();
            if(app.settings.windowMode&&IsStandableWindow(GetForegroundWindow(),app.hwnd,0))
            {
                RECT frame{};
                VisibleFrame(GetForegroundWindow(),frame);
                if(CenterX()>=frame.left&&CenterX()<=frame.right&&frame.top>=feet-1&&frame.top<line&&
                   frame.top-ModelHeightPixels()>=WorkAreaOfWindow().top+8)
                {landing=GetForegroundWindow();line=frame.top;}
            }
            const double target=WindowYForLine(line);
            app.fallSpeed+=FlightGravity*ppu*dt;
            double y=app.windowY+app.fallSpeed*dt;
            if(y>=target)
            {
                y=target;
                MoveWindowTo(app.windowX,y);
                Land(landing,app.fallSpeed);
                app.servedForeground=landing?landing:app.servedForeground;
            }
            else MoveWindowTo(app.windowX,y);
        }
        else if(app.place==Place::Warping)UpdateWarp(dt);
        else if(app.place==Place::Jumping)
        {
            auto& j=app.jump;
            j.time=std::min(j.time+dt,j.duration);
            const double g=FlightGravity*ppu;
            const double x=j.startX+j.vx*j.time,line=j.startLine+j.vy*j.time+0.5*g*j.time*j.time;
            MoveWindowTo(x-app.windowSize.cx*0.5,WindowYForLine(line));
            if(j.time>=j.duration)Land(app.destinationWindow,j.vy+g*j.duration);
        }
        if(app.place!=Place::Ground&&app.place!=Place::Warping&&dt>0)
        {
            const float vx=static_cast<float>((app.windowX-app.previousWindowX)/dt),vy=static_cast<float>((app.windowY-app.previousWindowY)/dt);
            const float k=static_cast<float>(1.0-std::exp(-dt/0.08));
            const DirectX::XMFLOAT2 previous=app.carryVelocity;
            app.carryVelocity.x+=(vx-app.carryVelocity.x)*k;
            app.carryVelocity.y+=(vy-app.carryVelocity.y)*k;
            app.carryAcceleration={static_cast<float>((app.carryVelocity.x-previous.x)/dt),static_cast<float>((app.carryVelocity.y-previous.y)/dt)};
            // Ballistic flight: the body falls with the window, so it feels weightless (the
            // physics world's own gravity is cancelled exactly).
            if(app.place==Place::Falling||app.place==Place::Jumping)app.carryAcceleration={0,static_cast<float>(Gravity*ppu)};
            // Screen y grows downward, world y upward.
            app.model.SetCarryMotion(app.carryVelocity.x/ppu,-app.carryVelocity.y/ppu,app.carryAcceleration.x/ppu,-app.carryAcceleration.y/ppu);
        }
        else app.model.SetCarryMotion(0,0,0,0);
        app.previousWindowX=app.windowX;app.previousWindowY=app.windowY;

        if(app.place==Place::Ground&&!forcedActivity)
        {
            // Walking (or running on the same surface): stop at the target or the surface's end.
            const Activity a=app.model.CurrentActivity();
            if(app.travel==Travel::None&&(a==Activity::Walk||a==Activity::Run))
            {
                double left,right;
                SurfaceRange(left,right);
                const double centerX=CenterX(),margin=a==Activity::Run?16:6;
                const bool reached=std::abs(centerX-app.walkTargetX)<margin||centerX<left-1||centerX>right+1;
                if(reached)StopGroundActivity();
            }
            UpdateAutoBehavior();
        }

        // Click-through wherever the mascot is not drawn.
        POINT cursor{};
        RECT r{};
        GetCursorPos(&cursor);
        GetWindowRect(app.hwnd,&r);
        if(app.place!=Place::Held&&PtInRect(&r,cursor))
        {
            const int alpha=app.renderer.AlphaAt({cursor.x-r.left,cursor.y-r.top});
            SetClickThrough(alpha>=0&&alpha<16);
        }
        if(!forcedActivity)
        {
            UpdateWindowMode(dt);
            AvoidCursor(cursor);
            UpdateHangOrder();
        }
        if(snapshotLook)cursor={r.left+snapshotLook->x,r.top+snapshotLook->y};
        UpdateLookTarget(cursor,r,dt);

        app.model.Update(dt);
        UpdateExtent(dt);
        // The taskbar can also end up above the mascot without taking the foreground.
        if((app.topmostCheck+=dt)>=2.0){app.topmostCheck=0;ReassertTopmost();}
        // センター's walking travel moves the window by the same amount; the camera follows it.
        if(const float travel=app.model.TakeRootDelta();travel!=0.0f)
        {
            UpdateCamera();
            if(!forcedActivity)MoveWindowTo(app.windowX+travel*ppu,app.windowY);
        }
        if(app.place==Place::Ground&&std::abs(app.windowY-GroundedWindowY())>0.5)MoveWindowTo(app.windowX,GroundedWindowY());
        if(const std::uint32_t resets=app.model.PhysicsResets();resets!=lastPhysicsResets)
        {
            Trace(L"%.1f physics reset (%u so far) %s",app.clock,resets,app.model.PhysicsResetReason().c_str());
            lastPhysicsResets=resets;
        }
        UpdateSparks(dt);
        app.renderer.Render(app.model,app.settings.edgeScale);
        if(!snapshotPath.empty()&&--snapshotFrames<=0)
        {
            app.renderer.SaveLastFrame(snapshotPath);
            if(FILE* info=nullptr;_wfopen_s(&info,(snapshotPath+L".txt").c_str(),L"w, ccs=UTF-8")==0&&info)
            {
                const auto rest=app.model.ArmRestAngles(),fix=app.model.ArmCorrections();
                RECT wr{};
                GetWindowRect(app.hwnd,&wr);
                fwprintf(info,L"model=%s\narmRest L=%.1f R=%.1f\narmCorrection L=%.1f R=%.1f\nwindow=%dx%d\n",
                         app.settings.modelPath.c_str(),rest[0],rest[1],fix[0],fix[1],wr.right-wr.left,wr.bottom-wr.top);
                fwprintf(info,L"%s",app.model.DescribeRig().c_str());
                fclose(info);
            }
            DestroyWindow(app.hwnd);
        }
    }
    if(foregroundHook)UnhookWinEvent(foregroundHook);
    CloseHandle(timer);
    app.renderer.WaitIdle();

    if(app.restart)
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr,exe,MAX_PATH);
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if(CreateProcessW(exe,nullptr,nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi))
        {CloseHandle(pi.hThread);CloseHandle(pi.hProcess);}
    }
    CoUninitialize();
    return 0;
}
