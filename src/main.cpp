#define NOMINMAX
#include "ChatWindows.hpp"
#include "LocalChat.hpp"
#include "Memory.hpp"
#include "Voice.hpp"
#include "MascotModel.hpp"
#include "MascotRenderer.hpp"
#include "Settings.hpp"
#include "Speech.hpp"
#include "SettingsWindow.hpp"
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
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>

using namespace MikuMikuDesktopPerch;

namespace
{
    constexpr wchar_t WindowClass[]=L"MikuMikuDesktopPerchWindow";
    constexpr UINT TrayMessage=WM_APP+1;
    enum MenuId:UINT {IdOpenModel=100,IdOpenMotion,IdCloseMotion,IdOpenIdleMotion,IdAddIdleMotion,IdCloseIdleMotion,
                      IdOpenClickMotion,IdCloseClickMotion,IdWindowMode,IdWalkLeft,IdWalkRight,IdSettings,IdExit,IdTalk,
                      IdOpenWalkMotion,IdOpenRunMotion,IdCloseGaitMotion};
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
        double lookAtViewerUntil=-1;   // while talking (and a moment after) she looks at the viewer

        // The window is a fixed transparent stage over the work area (stage). The drawn frame
        // ("window" below: windowX/Y, windowSize) moves inside it as a composition offset;
        // moving the real window could never be in step with the frame drawn for it.
        RECT stage{};
        // Frame placement in screen pixels, kept in doubles so slow walking accumulates.
        double windowX=0,windowY=0;
        SIZE windowSize{};
        float pixelsPerUnit=40;   // screen pixels per world unit at the model's depth
        float viewYaw=0;          // --view-yaw: camera orbit for diagnostic snapshots

        Place place=Place::Ground;
        bool pressed=false;           // left button down, not yet a grab
        POINT pressCursor{},dragOffset{};
        int wheelDelta=0;                           // mouse wheel travel not yet turned into a size step

        // Talking through a local LLM: the conversation so far (the last few turns), the reply
        // streaming in (raw, as received) and the request it belongs to.
        std::unique_ptr<LocalChat> chat;
        SpeechBubble bubble;
        ChatInput chatInput;
        CharacterProfile character;
        std::vector<ChatMessage> conversation;
        // Long-term memory: notes about the user, picked out of finished exchanges by a second
        // request to the LLM once the conversation pauses.
        CharacterMemory memory;
        std::unique_ptr<VoiceEngine> voice;   // while speaking aloud is on
        std::unique_ptr<LocalChat> memoryChat;
        std::vector<ChatMessage> unmemorized;   // exchanges not yet looked through for notes
        double lastReplyAt=0;
        int memoryRequest=0;
        std::wstring memoryReply;
        // Speaking first: when the next word of her own is due, and whether the user is away.
        double nextChatterAt=1500;
        double awaySince=-1;
        bool dozing=false;               // she has dozed off (the user has been away a while)
        bool forcedDoze=false;           // diagnostics (--doze): asleep whatever the input
        // The feeling the reply is said with (its tag), and when the face goes back to normal.
        bool replyFeelingSet=false;
        double feelingUntil=-1;
        std::wstring reply;
        size_t replyShown=0;      // characters of the cleaned reply already in the bubble
        Speech speech;            // the reply's syllables on a timeline, for the mouth
        // A click waits a double-click's time: a double-click opens the chat instead.
        std::optional<POINT> pendingClick;
        double pendingClickAt=0;
        int chatRequest=0;
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

        // The opened (one-shot) motion: requested right after opening, then now and then.
        bool motionRequested=false,motionWasPlaying=false,motionClosePending=false;
        double nextMotionAt=0,motionStartAt=0;   // motionStartAt: standing up before it plays
        bool behindSurface=false;     // hanging: the window sits just behind the surface window
        double hangEndedAt=-1;
        Activity forcedRestPose=Activity::Idle;   // --rest-pose sit|hang (diagnostics)
        int diagnosticWalk=0;                     // --walk <px>: walk that far 3 s after start
        int diagnosticClick=0;                    // --click head|body: 1 head, 2 body, 1.5 s after start

        // Ground behaviour.
        double walkTargetX=0;         // screen x for the window centre
        double nextDecision=8,clock=0,topmostCheck=0;
        std::uint32_t random=0x2545F491u;
        float Random01(){random^=random<<13;random^=random>>17;random^=random<<5;return (random&0xFFFFFF)/16777216.0f;}
    } app;

    void SaveWindowPosition();

    // Behaviour trace for diagnosis: written only when MIKUMIKUDESKTOPPERCH_LOG names a file.
    void Trace(const wchar_t* format,...)
    {
        static const std::wstring path=[]{wchar_t p[MAX_PATH]{};return GetEnvironmentVariableW(L"MIKUMIKUDESKTOPPERCH_LOG",p,MAX_PATH)?std::wstring(p):std::wstring();}();
        if(path.empty())return;
        wchar_t text[512]{};
        va_list args;
        va_start(args,format);
        vswprintf_s(text,format,args);
        va_end(args);
        if(FILE* f=nullptr;_wfopen_s(&f,path.c_str(),L"a, ccs=UTF-8")==0&&f){fwprintf(f,L"%s\n",text);fclose(f);}
    }

    int ShowMessage(const std::wstring& text,UINT type);

    // Speaking aloud: each phrase of a reply goes to the voice engine (in the chosen voice).
    void ApplyVoiceSettings()
    {
        if(!app.settings.voice){app.speech.SetVoice(nullptr);app.voice.reset();return;}
        if(!app.voice)app.voice=std::make_unique<VoiceEngine>(app.hwnd);
        app.speech.SetVoice([](const std::wstring& text){
            return app.voice->Speak({app.settings.voiceDirect,app.settings.voiceEndpoint,app.settings.voiceFolder},app.settings.voiceSpeaker,text);});
    }

    void ApplyProceduralSettings()
    {
        app.model.SetProcedural(app.settings.blink,app.settings.breath,app.settings.lookAtMouse);
        app.renderer.SetSelfShadow(app.settings.selfShadow);
        app.renderer.SetLight(static_cast<float>(app.settings.lightAzimuth),static_cast<float>(app.settings.lightElevation));
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

    // Takes effect with the next frame drawn (see PlaceFrame).
    void MoveWindowTo(double x,double y)
    {
        app.windowX=x;app.windowY=y;
    }

    POINT WindowPixel(){return {static_cast<LONG>(std::lround(app.windowX)),static_cast<LONG>(std::lround(app.windowY))};}

    // Screen rectangle of the drawn frame.
    RECT FrameRect()
    {
        const POINT p=WindowPixel();
        return {p.x,p.y,p.x+app.windowSize.cx,p.y+app.windowSize.cy};
    }

    // The stage covers the work area of the monitor under the frame's centre: not the whole
    // monitor, which would make the taskbar treat it as a full-screen window. Crossing to
    // another monitor moves the stage (the only window move left).
    void UpdateStage()
    {
        const RECT frame=FrameRect();
        const RECT work=WorkAreaAt({(frame.left+frame.right)/2,(frame.top+frame.bottom)/2});
        if(EqualRect(&work,&app.stage))return;
        app.stage=work;
        SetWindowPos(app.hwnd,nullptr,work.left,work.top,work.right-work.left,work.bottom-work.top,SWP_NOZORDER|SWP_NOACTIVATE);
    }

    // Before each frame: where it goes in the stage, plus the fraction of a pixel rounded off.
    void PlaceFrame()
    {
        UpdateStage();
        const POINT p=WindowPixel();
        app.renderer.SetVisualOffset(p.x-app.stage.left,p.y-app.stage.top);
        app.renderer.SetPixelOffset(static_cast<float>(app.windowX-p.x),static_cast<float>(app.windowY-p.y));
    }

    double ModelHeightPixels(){return ModelFrameOf().top*app.pixelsPerUnit;}

    // Scale around the feet: the model's ground point stays where it is on screen.
    void ApplyScale(int percent)
    {
        if(!ApplyFrame(percent,app.extent))
        {
            ShowMessage(L"ウィンドウの大きさを変更できませんでした。",MB_ICONERROR);
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

    // The opened motion is playing, or about to (standing up for it): nothing else starts.
    // Also while it is due and on its way (standing up, warping): nothing else may start then.
    bool MotionBusy()
    {
        if(app.model.Reacting())return true;   // answering a click
        // An idle motion plays to its end (or its loop's) before she walks off or sits down.
        if(app.model.CurrentActivity()==Activity::Idle&&!app.model.IdleMotionAtBreak())return true;
        const bool due=app.model.HasMotion()&&!app.motionClosePending&&(app.motionRequested||app.clock>=app.nextMotionAt);
        return app.model.MotionPlaying()||app.clock<app.motionStartAt||due;
    }

    void BeginWarp();

    // A click (press and release without dragging) at a screen point: the mascot reacts,
    // differently on the head. Walking, it stops first; on a trip (warp, jump) it does not.
    void OnClick(POINT p)
    {
        if(app.place!=Place::Ground||app.travel!=Travel::None)return;
        const Activity a=app.model.CurrentActivity();
        bool head=false;
        if(std::array<float,3> h{};app.model.HeadPosition(h))
        {
            // Above the 頭 bone (the top of the neck) and within about a head's width of it.
            const RECT r=FrameRect();
            const POINT s=app.renderer.Project(h);
            const double unit=ModelHeightPixels();
            head=p.y<r.top+s.y+0.02*unit&&std::abs(p.x-(r.left+s.x))<0.12*unit;
        }
        if(a==Activity::Walk||a==Activity::Run)StopGroundActivity();
        if(app.model.React(head))Trace(L"%.1f click on the %s",app.clock,head?L"head":L"body");
    }

    // The opened motion plays once right after opening, then now and then (every 1.5 to 4
    // minutes) while the mascot stands or rests on a surface. Resting, it stands up first; it
    // then warps to the bottom right of the screen and plays the motion on the ground there.
    void UpdateMotionPlayback()
    {
        const bool playing=app.model.MotionPlaying();
        if(app.motionWasPlaying&&!playing)
        {
            Trace(L"%.1f motion finished",app.clock);
            app.nextMotionAt=app.clock+90+app.Random01()*150;
            if(app.motionClosePending){std::wstring error;app.model.SetMotion({},error);app.motionClosePending=false;}
            if(app.surfaceWindow&&app.restPose!=Activity::Idle)app.restPoseAt=app.clock+0.8;
        }
        app.motionWasPlaying=playing;
        if(playing)
        {
            if(app.place!=Place::Ground)app.model.StopMotion();
            return;
        }
        if(!app.model.HasMotion()||app.motionClosePending||(!app.motionRequested&&app.clock<app.nextMotionAt))return;
        // Now and then (not when just opened): between idle motions, not in the middle of one.
        if(!app.motionRequested&&app.model.CurrentActivity()==Activity::Idle&&!app.model.IdleMotionAtBreak())return;
        if(app.place!=Place::Ground||app.travel!=Travel::None)return;
        const Activity a=app.model.CurrentActivity();
        if(a==Activity::Walk||a==Activity::Run||a==Activity::Jump)return;
        if(a!=Activity::Idle)
        {
            app.restPose=a;
            app.model.SetActivity(Activity::Idle);
            app.motionStartAt=app.clock+0.8;
            return;
        }
        if(app.clock<app.motionStartAt)return;
        // Dance on the ground at the bottom right of the screen: warp there first.
        {
            const RECT work=WorkAreaOfWindow();
            const double spotX=work.right-ModelHalfWidthPixels()-16;
            if(app.surfaceWindow||std::abs(CenterX()-spotX)>24)
            {
                app.destinationWindow=nullptr;
                app.destinationLine=work.bottom;
                app.destinationX=spotX;
                BeginWarp();
                return;
            }
        }
        Trace(L"%.1f motion starts",app.clock);
        app.motionRequested=false;
        app.restPoseAt=-1;
        app.model.PlayMotion();
        app.motionWasPlaying=true;
    }

    // Occasionally walk somewhere when left alone on the ground. Sitting is kept for window
    // edges: on the taskbar the legs would hang off the screen.
    void UpdateAutoBehavior()
    {
        if(!app.settings.autoBehavior||app.place!=Place::Ground||app.surfaceWindow||app.travel!=Travel::None||MotionBusy())return;
        if(app.model.CurrentActivity()!=Activity::Idle||app.clock<app.nextDecision)return;
        if(app.Random01()<0.6f)
        {
            double left,right;
            SurfaceRange(left,right);
            const double distance=(120+app.Random01()*380)*(app.Random01()<0.5f?-1:1);
            double target=CenterX()+distance;
            if(target<left||target>right)target=CenterX()-distance;
            Trace(L"%.1f auto walk to %.0f",app.clock,target);
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
        if(!app.surfaceWindow||app.place!=Place::Ground||app.travel!=Travel::None||app.clock<app.avoidCooldown||MotionBusy())return;
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
                if(frame.top-ModelHeightPixels()<WorkAreaOfWindow().top+8&&app.travel==Travel::None&&!MotionBusy())
                {
                    BeginTravel(app.surfaceWindow);
                    return;
                }
            }

            // Once airborne the destination is fixed until landing.
            if(app.travel==Travel::Crouching&&(app.travelTimer-=dt)<=0){Launch();return;}

            // A new foreground window (settled for a moment): go there.
            // (Held back while the opened motion plays; it goes once that has finished.)
            if(app.travel==Travel::None&&!MotionBusy()&&app.clock-app.foregroundSince>0.6&&foreground!=app.surfaceWindow&&
               foreground!=app.servedForeground&&IsStandableWindow(foreground,app.hwnd,static_cast<int>(ModelFrameOf().width*app.pixelsPerUnit)))
            {
                BeginTravel(foreground);
                return;
            }

            // Settled on a window: take (or resume) the resting pose.
            if(app.surfaceWindow&&app.travel==Travel::None&&!MotionBusy()&&app.restPoseAt>=0&&app.clock>=app.restPoseAt&&
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
        // Talking (waiting for the reply, saying it, or with the input box open), she looks at
        // the viewer, who sits in front of the middle of the screen, instead of the cursor.
        if((app.chat&&app.chat->Busy())||app.speech.Speaking()||app.bubble.Talking()||app.chatInput.IsOpen())
            app.lookAtViewerUntil=app.clock+2.5;
        const bool atViewer=app.clock<app.lookAtViewerUntil;
        // The opened motion leads the head itself while it plays.
        if((!app.settings.lookAtMouse&&!atViewer)||app.model.Dozing()||a==Activity::Walk||a==Activity::Fall||app.model.MotionPlaying()||app.model.HeadBusy()||!app.model.HeadPosition(head))
        {
            app.model.SetLookTarget(0,0,false);
            return;
        }
        const POINT h=app.renderer.Project(head);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(app.hwnd,MONITOR_DEFAULTTONEAREST),&monitor);
        const RECT& screen=monitor.rcMonitor;
        const POINT viewer{(screen.left+screen.right)/2,screen.top+(screen.bottom-screen.top)*2/5};
        const POINT at=atViewer?viewer:cursor;
        const float dx=static_cast<float>(at.x-(window.left+h.x));
        const float dy=static_cast<float>(at.y-(window.top+h.y));
        // Treat the viewer as sitting about one window height away from the screen (the
        // face in front of the screen: about its width away).
        const float distance=static_cast<float>(atViewer?(screen.right-screen.left)*0.8:window.bottom-window.top);
        const float yaw=std::clamp(std::atan2(dx,distance),DirectX::XMConvertToRadians(-40.0f),DirectX::XMConvertToRadians(40.0f));
        const float pitch=std::clamp(std::atan2(-dy,distance),DirectX::XMConvertToRadians(-20.0f),DirectX::XMConvertToRadians(25.0f));
        app.model.SetLookTarget(yaw,pitch,atViewer||app.cursorIdle<LookIdleSeconds);
    }

    void SetClickThrough(bool enabled);

    // A modal dialog stops the frame loop, which keeps click-through up to date. Let clicks
    // through the stage first, or it would block every window under it while the dialog is up.
    int ShowMessage(const std::wstring& text,UINT type)
    {
        if(app.hwnd)SetClickThrough(true);
        return MessageBoxW(app.hwnd,text.c_str(),L"MikuMikuDesktopPerch",type);
    }

    std::wstring OpenFileDialog(HWND owner,const wchar_t* filter,const std::wstring& current)
    {
        if(app.hwnd)SetClickThrough(true);
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

    // Several files at once (empty when cancelled).
    std::vector<std::wstring> OpenFilesDialog(HWND owner,const wchar_t* filter,const std::wstring& current)
    {
        if(app.hwnd)SetClickThrough(true);
        std::vector<wchar_t> buffer(32768,L'\0');
        std::wstring initialDir;
        if(!current.empty())initialDir=std::filesystem::path(current).parent_path().wstring();
        OPENFILENAMEW ofn{};
        ofn.lStructSize=sizeof(ofn);
        ofn.hwndOwner=owner;
        ofn.lpstrFilter=filter;
        ofn.lpstrFile=buffer.data();
        ofn.nMaxFile=static_cast<DWORD>(buffer.size());
        ofn.lpstrInitialDir=initialDir.empty()?nullptr:initialDir.c_str();
        ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_ALLOWMULTISELECT|OFN_EXPLORER;
        if(!GetOpenFileNameW(&ofn))return {};
        // One file: its full path. Several: the folder, then each name, each NUL-terminated.
        std::vector<std::wstring> parts;
        for(const wchar_t* p=buffer.data();*p;p+=std::wcslen(p)+1)parts.emplace_back(p);
        if(parts.size()==1)return parts;
        std::vector<std::wstring> paths;
        for(size_t i=1;i<parts.size();++i)paths.push_back((std::filesystem::path(parts[0])/parts[i]).wstring());
        return paths;
    }

    // Start with Windows: a value under the current user's Run key pointing at this exe.
    constexpr wchar_t RunKey[]=L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t RunValue[]=L"MikuMikuDesktopPerch";

    std::wstring StartupCommand()
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr,exe,MAX_PATH);
        return L"\""+std::wstring(exe)+L"\"";
    }

    // Registered for this exe (an entry left by a copy elsewhere does not count).
    bool StartupRegistered()
    {
        wchar_t value[MAX_PATH+8]{};
        DWORD size=sizeof(value);
        if(RegGetValueW(HKEY_CURRENT_USER,RunKey,RunValue,RRF_RT_REG_SZ,nullptr,value,&size)!=ERROR_SUCCESS)return false;
        return _wcsicmp(value,StartupCommand().c_str())==0;
    }

    bool SetStartup(bool enabled)
    {
        if(!enabled)
        {
            const LSTATUS status=RegDeleteKeyValueW(HKEY_CURRENT_USER,RunKey,RunValue);
            return status==ERROR_SUCCESS||status==ERROR_FILE_NOT_FOUND;
        }
        const std::wstring command=StartupCommand();
        return RegSetKeyValueW(HKEY_CURRENT_USER,RunKey,RunValue,REG_SZ,command.c_str(),
                               static_cast<DWORD>((command.size()+1)*sizeof(wchar_t)))==ERROR_SUCCESS;
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
        // Things used often stay here; switches and display options are in the settings window.
        HMENU menu=CreatePopupMenu();
        AppendMenuW(menu,MF_STRING,IdTalk,L"話しかける...");
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        AppendMenuW(menu,MF_STRING,IdOpenModel,L"モデルを開く...");
        HMENU motions=CreatePopupMenu();
        AppendMenuW(motions,MF_STRING,IdOpenMotion,L"モーションを開く...");
        AppendMenuW(motions,MF_STRING|(app.settings.motionPath.empty()?MF_GRAYED:0),IdCloseMotion,L"モーションを閉じる");
        AppendMenuW(motions,MF_SEPARATOR,0,nullptr);
        const size_t idleCount=app.settings.idleMotionPaths.size();
        AppendMenuW(motions,MF_STRING,IdOpenIdleMotion,L"待機モーションを開く...（複数選択可）");
        AppendMenuW(motions,MF_STRING|(idleCount==0?MF_GRAYED:0),IdAddIdleMotion,L"待機モーションを追加...");
        AppendMenuW(motions,MF_STRING|(idleCount==0?MF_GRAYED:0),IdCloseIdleMotion,
                    idleCount>1?(L"待機モーションを閉じる（"+std::to_wstring(idleCount)+L"件）").c_str():L"待機モーションを閉じる");
        AppendMenuW(motions,MF_SEPARATOR,0,nullptr);
        const size_t clickCount=app.settings.clickMotionPaths.size();
        AppendMenuW(motions,MF_STRING,IdOpenClickMotion,L"クリック時のモーションを開く...（複数選択可）");
        AppendMenuW(motions,MF_STRING|(clickCount==0?MF_GRAYED:0),IdCloseClickMotion,
                    clickCount>1?(L"クリック時のモーションを閉じる（"+std::to_wstring(clickCount)+L"件）").c_str():L"クリック時のモーションを閉じる");
        AppendMenuW(motions,MF_SEPARATOR,0,nullptr);
        AppendMenuW(motions,MF_STRING|(app.settings.walkMotionPath.empty()?0:MF_CHECKED),IdOpenWalkMotion,L"歩くモーションを開く...");
        AppendMenuW(motions,MF_STRING|(app.settings.runMotionPath.empty()?0:MF_CHECKED),IdOpenRunMotion,L"走るモーションを開く...");
        AppendMenuW(motions,MF_STRING|(app.settings.walkMotionPath.empty()&&app.settings.runMotionPath.empty()?MF_GRAYED:0),IdCloseGaitMotion,
                    L"歩く・走るモーションを閉じる");
        AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(motions),L"モーション");
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        const bool grounded=app.place==Place::Ground;
        AppendMenuW(menu,MF_STRING|(app.settings.windowMode?MF_CHECKED:0),IdWindowMode,L"アクティブウィンドウに乗る");
        AppendMenuW(menu,MF_STRING|(grounded?0:MF_GRAYED),IdWalkLeft,L"左へ歩く");
        AppendMenuW(menu,MF_STRING|(grounded?0:MF_GRAYED),IdWalkRight,L"右へ歩く");
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        AppendMenuW(menu,MF_STRING,IdSettings,L"設定...");
        AppendMenuW(menu,MF_STRING,IdExit,L"終了");
        // The menu needs its owner in the foreground, which also raises it: while hanging, put
        // the mascot straight back behind the window it hangs from (before and after the menu).
        auto keepBehind=[]{
            if(app.behindSurface&&app.surfaceWindow)
                SetWindowPos(app.hwnd,app.surfaceWindow,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
        };
        SetForegroundWindow(app.hwnd);
        keepBehind();
        TrackPopupMenu(menu,TPM_RIGHTBUTTON,screen.x,screen.y,0,app.hwnd,nullptr);
        PostMessageW(app.hwnd,WM_NULL,0,0);
        keepBehind();
        DestroyMenu(menu);
    }

    void SetWindowMode(bool enabled)
    {
        app.settings.windowMode=enabled;
        app.servedForeground=nullptr;
        app.foregroundSince=app.clock;
        if(!enabled&&app.surfaceWindow&&app.place==Place::Ground)
        {
            // Leaving the mode while on a window: drop down to the ground line.
            app.surfaceWindow=nullptr;
            app.travel=Travel::None;
            app.restPoseAt=-1;
            app.place=Place::Falling;
            app.fallSpeed=0;
            app.model.SetActivity(Activity::Fall);
        }
    }

    // OK in the settings window: switches and display options take effect at once; the
    // detailed ones (frame rate, antialiasing, textures, physics rate, toon folder) are read at
    // start-up, so offer a restart for them.
    void ApplySettings(const SettingsChoice& choice)
    {
        const Settings& s=choice.settings;
        Settings& current=app.settings;
        if(s.windowMode!=current.windowMode)SetWindowMode(s.windowMode);
        if(s.autoBehavior!=current.autoBehavior){current.autoBehavior=s.autoBehavior;app.nextDecision=app.clock+5;}
        current.physics=s.physics;
        if(s.physics!=app.model.PhysicsEnabled())app.model.SetPhysics(s.physics);
        current.blink=s.blink;
        current.breath=s.breath;
        current.lookAtMouse=s.lookAtMouse;
        current.selfShadow=s.selfShadow;
        current.lightAzimuth=s.lightAzimuth;
        current.lightElevation=s.lightElevation;
        ApplyProceduralSettings();
        if(s.topmost!=current.topmost)
        {
            current.topmost=s.topmost;
            if(!app.behindSurface)SetWindowPos(app.hwnd,current.topmost?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        }
        current.edgeScale=s.edgeScale;
        current.chatEndpoint=s.chatEndpoint;
        if(s.chatModel!=current.chatModel){current.chatModel=s.chatModel;app.conversation.clear();}
        current.chatMemory=s.chatMemory;
        current.voiceSpeaker=s.voiceSpeaker;
        current.voiceName=s.voiceName;
        current.voiceDirect=s.voiceDirect;
        current.voiceFolder=s.voiceFolder;
        if(s.voice!=current.voice){current.voice=s.voice;ApplyVoiceSettings();}
        if(!current.chatMemory)app.unmemorized.clear();
        if(choice.character.name!=app.character.name||choice.character.persona!=app.character.persona)
        {
            // A new character starts a new conversation.
            app.character=choice.character;
            app.conversation.clear();
            if(!app.character.Save(current.modelPath))ShowMessage(L"人物設定を保存できませんでした。",MB_ICONERROR);
        }
        if(choice.startWithWindows!=StartupRegistered()&&!SetStartup(choice.startWithWindows))
            ShowMessage(L"スタートアップの設定を変更できませんでした。",MB_ICONERROR);
        const bool restart=s.fps!=current.fps||s.msaa!=current.msaa||s.maxTextureSize!=current.maxTextureSize||
                           s.physicsFps!=current.physicsFps||s.toonDir!=current.toonDir;
        current.fps=s.fps;
        current.msaa=s.msaa;
        current.maxTextureSize=s.maxTextureSize;
        current.physicsFps=s.physicsFps;
        current.toonDir=s.toonDir;
        if(s.scale!=current.scale)ApplyScale(s.scale);
        current.Save(app.settingsPath);
        if(restart&&ShowMessage(L"詳細の設定は、再起動すると反映されます。\n今すぐ再起動しますか？",MB_ICONQUESTION|MB_YESNO)==IDYES)
        {
            app.restart=true;
            DestroyWindow(app.hwnd);
        }
    }

    // The reply without what reasoning models think aloud (<think>...</think>), holding back a
    // tail that may be the start of such a tag.
    std::wstring CleanReply(const std::wstring& raw)
    {
        std::wstring out;
        size_t at=0;
        for(;;)
        {
            const size_t open=raw.find(L"<think>",at);
            if(open==std::wstring::npos){out+=raw.substr(at);break;}
            out+=raw.substr(at,open-at);
            const size_t close=raw.find(L"</think>",open);
            if(close==std::wstring::npos)return out;   // still thinking
            at=close+8;
        }
        for(size_t keep=std::min<size_t>(out.size(),7);keep>0;--keep)
            if(std::wstring_view(L"<think>").substr(0,keep)==std::wstring_view(out).substr(out.size()-keep)){out.resize(out.size()-keep);break;}
        // The feeling tag ([笑顔] and the like, in any of the brackets models use) is for the
        // face, not the bubble. One not closed yet waits.
        {
            std::wstring kept;
            for(size_t i=0;i<out.size();++i)
            {
                const wchar_t c=out[i];
                const wchar_t closing=c==L'['?L']':c==L'［'?L'］':c==L'【'?L'】':0;
                if(closing)
                {
                    const size_t end=out.find(closing,i+1);
                    if(end!=std::wstring::npos&&end-i<=7){i=end;continue;}
                    if(end==std::wstring::npos&&out.size()-i<=7)break;
                }
                kept+=c;
            }
            out=kept;
            const size_t start=out.find_first_not_of(L" 　\t\r\n");
            out.erase(0,start==std::wstring::npos?out.size():start);
        }
        // Emoji: the bubble's font cannot draw them (they come out as boxes), and models add
        // them however they are asked not to. Pictographs, dingbats, their variation selectors
        // and joiners go.
        // (♪ and ★ ☆ stay: the Japanese fonts have them.) A pair split across two pieces waits.
        if(!out.empty()&&IS_HIGH_SURROGATE(out.back()))out.pop_back();
        std::wstring plain;
        for(size_t i=0;i<out.size();++i)
        {
            unsigned code=out[i];
            size_t units=1;
            if(IS_HIGH_SURROGATE(out[i])&&i+1<out.size()&&IS_LOW_SURROGATE(out[i+1]))
            {
                code=0x10000+((code-0xD800)<<10)+(out[i+1]-0xDC00);
                units=2;
            }
            const bool kept=code==0x266A||code==0x266B||code==0x2605||code==0x2606;
            const bool emoji=!kept&&((code>=0x1F000&&code<=0x1FAFF)||(code>=0x2600&&code<=0x27BF)||(code>=0x2B00&&code<=0x2BFF)||
                             code==0xFE0F||code==0x200D||code==0x20E3||(code>=0xE0020&&code<=0xE007F));
            if(!emoji)plain.append(out,i,units);
            i+=units-1;
        }
        return plain;
    }

    // Say something to the mascot: the reply streams into the bubble over her head.
    // The LLM is set up and allowed (tell: say what is missing in the bubble).
    bool ChatReady(bool tell)
    {
        if(app.settings.chatModel.empty())
        {
            if(tell)app.bubble.ShowNotice(L"設定の「会話」で、使うモデルを選んでください（Ollama などを起動してから「一覧を取得」）。");
            return false;
        }
        if(const auto problem=CheckLocalEndpoint(app.settings.chatEndpoint);!problem.empty())
        {
            if(tell)app.bubble.ShowNotice(problem);
            return false;
        }
        if(!app.chat)app.chat=std::make_unique<LocalChat>(app.hwnd);
        return true;
    }

    // The feelings a reply may start with, as the tags the LLM is asked to write.
    constexpr std::pair<const wchar_t*,Expression> Feelings[]={
        {L"笑顔",Expression::Smile},{L"驚き",Expression::Surprise},{L"困り",Expression::Troubled},{L"怒り",Expression::Angry},
        {L"悲しみ",Expression::Sad},{L"照れ",Expression::Shy},{L"普通",Expression::Neutral}};

    // Ask for a reply to the conversation as it stands; query is what the memory is searched for.
    void StartReply(const std::wstring& query)
    {
        // The last six exchanges are enough for a chat at the desk.
        while(app.conversation.size()>12)app.conversation.erase(app.conversation.begin());
        const std::wstring& text=query;
        SYSTEMTIME now{};
        GetLocalTime(&now);
        static constexpr const wchar_t* Days[]={L"日",L"月",L"火",L"水",L"木",L"金",L"土"};
        wchar_t clock[64]{};
        swprintf_s(clock,L"%d年%d月%d日（%s）%d時%02d分",now.wYear,now.wMonth,now.wDay,Days[now.wDayOfWeek%7],now.wHour,now.wMinute);
        const std::wstring& user=app.settings.userName;
        const std::wstring partner=user.empty()
            ?L"話し相手の名前は分からないので、名前では呼ばないでください（「◯◯さん」のような書き方もしないでください）。\n"
            :L"話し相手（あなたに話しかけている人）の名前は「"+user+L"」です。\n";
        std::wstring remembered;
        if(app.settings.chatMemory)
            if(const std::wstring notes=app.memory.Recall(text,1500);!notes.empty())
                remembered=L"\n以前の会話で覚えたこと（必要なときだけ自然に話に生かしてください。ここにないことを覚えているふりはしないでください）：\n"+notes+L"\n";
        std::vector<ChatMessage> messages;
        messages.push_back({L"system",
            L"あなたは「"+app.character.name+L"」です。\n"+app.character.persona+L"\n\n"+partner+remembered+
            L"いまは"+clock+L"です。\n"
            L"あなたの返事は、デスクトップの上にいるあなたの頭上の吹き出しに表示されます。"
            L"1〜3文くらいの短い話し言葉で答えてください。Markdown、箇条書き、絵文字、顔文字は使わないでください。\n"
            L"返事のいちばん最初に、そのときの気持ちを［笑顔］［驚き］［困り］［怒り］［悲しみ］［照れ］［普通］のどれか1つで書いてください（例：［笑顔］そうなんだ！）。"
            L"この印は最初の1つだけで、ほかの所には書かないでください。\n"
            L"話し相手の発言が（ ）で囲まれているときは、実際の発言ではなく状況の説明です。その状況に合わせて、あなたから話しかけてください。"});
        messages.insert(messages.end(),app.conversation.begin(),app.conversation.end());
        app.reply.clear();
        app.replyShown=0;
        app.speech.Reset();
        app.replyFeelingSet=false;
        if(app.voice)app.voice->CancelAll();   // what was left of the last reply is not said
        app.chatRequest=app.chat->Start(app.settings.chatEndpoint,app.settings.chatModel,std::move(messages));
        app.bubble.Begin(app.character.name);
        Trace(L"%.1f talk: request %d",app.clock,app.chatRequest);
    }

    void Talk(const std::wstring& text)
    {
        if(!ChatReady(true))return;
        app.conversation.push_back({L"user",text});
        StartReply(text);
    }

    // She speaks first: situation says why (it goes in as a stage direction in brackets).
    void SpeakFirst(const std::wstring& situation)
    {
        if(!ChatReady(false))return;
        Trace(L"%.1f speak first: %s",app.clock,situation.c_str());
        app.conversation.push_back({L"user",L"（"+situation+L"）"});
        StartReply(situation);
    }

    // Greetings for the time of day (once per part of the day, restarts included), a welcome
    // back after a while away, and now and then a word of her own. Never over a reply, the
    // input box, or while the user is away.
    // Seconds since the last keyboard or mouse input anywhere.
    double InputIdleSeconds()
    {
        LASTINPUTINFO input{sizeof(input)};
        return GetLastInputInfo(&input)?(GetTickCount()-input.dwTime)/1000.0:0.0;
    }

    // With nobody at the computer for five minutes she dozes off where she is (standing,
    // sitting or hanging, not in the middle of anything); any input wakes her, now and then
    // with a yawn.
    void UpdateDozing()
    {
        if(app.forcedDoze)return;
        const Activity a=app.model.CurrentActivity();
        const bool still=(a==Activity::Idle||a==Activity::Sit||a==Activity::Hang||a==Activity::Lie)&&app.travel==Travel::None&&
                         !app.model.MotionPlaying()&&!(app.chat&&app.chat->Busy())&&!app.bubble.Visible()&&!app.chatInput.IsOpen();
        const double idle=InputIdleSeconds();
        if(!app.dozing&&still&&idle>=5*60)
        {
            app.dozing=true;
            app.model.SetDozing(true);
            Trace(L"%.1f doze off",app.clock);
        }
        else if(app.dozing&&(idle<2||!still))
        {
            app.dozing=false;
            app.model.SetDozing(false);
            if(idle<2&&a==Activity::Idle&&app.Random01()<0.5f)app.model.Emote(L"yawn");
            Trace(L"%.1f wake up",app.clock);
        }
        if(app.dozing)app.nextDecision=std::max(app.nextDecision,app.clock+10);   // no wandering off asleep
    }

    void MaybeSpeakFirst()
    {
        if(!app.settings.chatInitiative||app.settings.chatModel.empty()||app.clock<8)return;
        const double idle=InputIdleSeconds();
        if(idle>=15*60&&app.awaySince<0)app.awaySince=app.clock-idle;
        if((app.chat&&app.chat->Busy())||app.bubble.Visible()||app.chatInput.IsOpen()||app.speech.Speaking())return;
        if(app.model.CurrentActivity()==Activity::Grabbed||app.travel!=Travel::None)return;
        if(app.awaySince>=0&&idle<3)
        {
            const int minutes=static_cast<int>((app.clock-app.awaySince)/60);
            app.awaySince=-1;
            SpeakFirst(L"話し相手が"+std::to_wstring(minutes)+L"分ほど席を外していて、いまパソコンの前に戻ってきました。おかえりのひと言を");
            app.nextChatterAt=std::max(app.nextChatterAt,app.clock+20*60);
            return;
        }
        if(idle>90)return;
        SYSTEMTIME now{};
        GetLocalTime(&now);
        const int h=now.wHour;
        const wchar_t* slot=h>=5&&h<10?L"morning":h>=11&&h<14?L"noon":h>=18&&h<22?L"evening":(h>=23||h<3)?L"late":nullptr;
        if(slot)
        {
            wchar_t key[40]{};
            swprintf_s(key,L"%04d-%02d-%02d %s",now.wYear,now.wMonth,now.wDay,slot);
            if(app.settings.lastGreeting!=key)
            {
                app.settings.lastGreeting=key;
                app.settings.Save(app.settingsPath);
                const std::wstring s=slot;
                SpeakFirst(s==L"morning"?L"朝です。話し相手がパソコンを使い始めました。朝のあいさつを":
                           s==L"noon"?L"お昼どきです。お昼ごはんのことなどで、軽く声をかけて":
                           s==L"evening"?L"夕方から夜の時間です。今日一日おつかれさまの気持ちで、ひと言":
                                          L"夜遅い時間です。夜ふかしを気づかうひと言を");
                app.nextChatterAt=std::max(app.nextChatterAt,app.clock+20*60);
                return;
            }
        }
        if(app.clock>=app.nextChatterAt)
        {
            app.nextChatterAt=app.clock+(30+app.Random01()*30)*60;
            SpeakFirst(L"特に用事はないけれど、話し相手に何かひと言話しかけたくなりました。覚えていることや、いまの時間帯・季節にちなんだ話題で");
        }
    }

    // The face keeps the reply's feeling while she speaks, and a few seconds after.
    void UpdateFeeling()
    {
        if((app.chat&&app.chat->Busy())||app.speech.Speaking()||app.bubble.Talking()){app.feelingUntil=app.clock+3;return;}
        if(app.feelingUntil>=0&&app.clock>=app.feelingUntil)
        {
            app.feelingUntil=-1;
            app.model.SetExpression(Expression::Neutral,false);
        }
    }

    void OpenChatInput()
    {
        // Just above the feet, over the legs, so it is close to her without hiding the face.
        const RECT r=FrameRect();
        const POINT ground=GroundPointOnScreen();
        app.chatInput.Open(app.hwnd,{(r.left+r.right)/2,ground.y-static_cast<LONG>(ModelHeightPixels()*0.12)},Talk);
    }

    void OnChatText(std::wstring* text,int id)
    {
        std::unique_ptr<std::wstring> owned(text);
        if(id!=app.chatRequest)return;
        app.reply+=*owned;
        if(!app.replyFeelingSet)
            for(const auto& [tag,feeling]:Feelings)
                if(app.reply.find(tag)!=std::wstring::npos&&app.reply.find(tag)<16)
                {
                    app.replyFeelingSet=true;
                    app.model.SetExpression(feeling,true);
                    app.feelingUntil=-1;
                    Trace(L"%.1f talk: feeling %s",app.clock,tag);
                    break;
                }
        const std::wstring clean=CleanReply(app.reply);
        if(clean.size()>app.replyShown)
        {
            app.bubble.Append(clean.substr(app.replyShown));
            app.speech.Add(clean.substr(app.replyShown));
            app.replyShown=clean.size();
        }
    }

    void OnChatDone(std::wstring* error,int id)
    {
        std::unique_ptr<std::wstring> owned(error);
        if(id!=app.chatRequest)return;
        Trace(L"%.1f talk: request %d done %s",app.clock,id,owned->c_str());
        if(!owned->empty())
        {
            // Not answered: forget the question too, and say why.
            if(!app.conversation.empty()&&app.conversation.back().role==L"user")app.conversation.pop_back();
            app.speech.Reset();
            app.bubble.ShowNotice(*owned);
            return;
        }
        std::wstring clean=CleanReply(app.reply+L"\n");
        while(!clean.empty()&&std::iswspace(clean.back()))clean.pop_back();
        const size_t start=clean.find_first_not_of(L" \t\r\n");
        clean=start==std::wstring::npos?std::wstring():clean.substr(start);
        // The last piece held back (a possible tag start) is shown and said now.
        if(const std::wstring all=CleanReply(app.reply+L"\n");all.size()>app.replyShown)
        {
            const std::wstring rest=all.substr(app.replyShown);
            app.bubble.Append(rest);
            app.speech.Add(rest);
            app.replyShown=all.size();
        }
        app.speech.Finish();
        if(!clean.empty())
        {
            if(app.settings.chatMemory&&!app.conversation.empty()&&app.conversation.back().role==L"user"&&
               app.conversation.back().content.rfind(L"（",0)!=0)
            {
                app.unmemorized.push_back(app.conversation.back());
                app.unmemorized.push_back({L"assistant",clean});
                while(app.unmemorized.size()>24)app.unmemorized.erase(app.unmemorized.begin());
            }
            app.lastReplyAt=app.clock;
            app.conversation.push_back({L"assistant",clean});
        }
        else if(!app.conversation.empty()&&app.conversation.back().role==L"user")app.conversation.pop_back();
        app.bubble.Finish();
    }

    // Once the conversation has paused (or grown long), ask the LLM what in it is worth
    // remembering. It runs in the background on its own request, never during a reply.
    void MaybeMemorize()
    {
        if(!app.settings.chatMemory||app.unmemorized.empty()||app.settings.chatModel.empty())return;
        if((app.chat&&app.chat->Busy())||(app.memoryChat&&app.memoryChat->Busy()))return;
        if(app.clock-app.lastReplyAt<20&&app.unmemorized.size()<8)return;
        if(!CheckLocalEndpoint(app.settings.chatEndpoint).empty()){app.unmemorized.clear();return;}
        if(!app.memoryChat)app.memoryChat=std::make_unique<LocalChat>(app.hwnd);
        const std::wstring& name=app.character.name;
        const std::wstring partner=app.settings.userName.empty()?L"話し相手":L"話し相手（"+app.settings.userName+L"）";
        std::wstring dialogue;
        for(const auto& m:app.unmemorized)dialogue+=(m.role==L"user"?L"話し相手：":name+L"：")+m.content+L"\n";
        std::wstring known;
        for(const auto& e:app.memory.Entries())known+=L"- "+e.text+L"\n";
        if(known.size()>3000)known=known.substr(known.size()-3000);
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t today[32]{};
        swprintf_s(today,L"%d年%d月%d日",now.wYear,now.wMonth,now.wDay);
        std::vector<ChatMessage> messages;
        messages.push_back({L"system",L"あなたは会話の記録係です。指示されたことだけを、指示された形で書きます。"});
        messages.push_back({L"user",
            L"次は「"+name+L"」と"+partner+L"の会話です。今日は"+today+L"です。\n"
            L"この先の会話で「"+name+L"」が覚えておくとよい、話し相手についての新しいことを書き出してください。\n"
            L"覚えること：話し相手の好きなもの・苦手なもの、予定や出来事、家族やペット、仕事や学校や趣味、二人の約束、話し相手が「覚えておいて」と言ったこと。\n"
            L"書き方：\n"
            L"・1行に1つ、「話し相手は〜」のように主語をつけた短い一文で、多くても3つまで。\n"
            L"・「明日」「来週」などは今日の日付をもとに日付に直す。\n"
            L"・あいさつ、その場かぎりの雑談、"+name+L"自身の発言は書かない。\n"
            L"・すでに覚えていることと同じ内容は書かない。\n"
            L"・書くことがなければ「なし」とだけ書く。\n\n"
            L"すでに覚えていること：\n"+(known.empty()?std::wstring(L"（まだない）\n"):known)+L"\n"
            L"会話：\n"+dialogue});
        app.unmemorized.clear();
        app.memoryReply.clear();
        app.memoryRequest=app.memoryChat->Start(app.settings.chatEndpoint,app.settings.chatModel,std::move(messages));
        Trace(L"%.1f memory: request %d",app.clock,app.memoryRequest);
    }

    void OnMemoryDone(const std::wstring& error)
    {
        Trace(L"%.1f memory: done %s",app.clock,error.c_str());
        if(!error.empty())return;   // the exchanges are dropped: nothing to retry with later
        bool added=false;
        std::wstringstream lines(CleanReply(app.memoryReply+L"\n"));
        for(std::wstring line;std::getline(lines,line);)
        {
            // Bullets and numbering the model adds anyway.
            const size_t start=line.find_first_not_of(L" \t\r-*・•●0123456789.．)）、");
            if(start==std::wstring::npos)continue;
            line=line.substr(start);
            while(!line.empty()&&std::iswspace(line.back()))line.pop_back();
            if(line.empty()||line.find(L"なし")==0||line.size()>CharacterMemory::MaxLength)continue;
            if(line.back()==L'：'||line.back()==L':')continue;   // a heading
            if(app.memory.Add(line,TodayDate())){added=true;Trace(L"%.1f memory: + %s",app.clock,line.c_str());}
        }
        if(added)
        {
            app.memory.Save();
            RefreshMemoryWindow();
        }
    }

    // Per frame: the bubble follows the head (its tail points at the top of the head) and the
    // mouth moves while the text is being revealed.
    void UpdateChatBubble(double dt)
    {
        if(!app.bubble.Visible())
        {
            if(app.speech.Active())app.speech.Reset();
            app.model.SetMouth({},false);
            return;
        }
        app.speech.Tick(dt);
        if(app.speech.Active())app.bubble.SetReveal(app.speech.RevealedChars());
        const RECT r=FrameRect();
        const double unit=ModelHeightPixels();
        POINT anchor{(r.left+r.right)/2,r.top};
        if(std::array<float,3> head{};app.model.HeadPosition(head))
        {
            const POINT h=app.renderer.Project(head);
            anchor={r.left+h.x,r.top+h.y-static_cast<LONG>(unit*0.13)};
        }
        app.bubble.Tick(dt,anchor,static_cast<int>(unit*0.08),WorkAreaOfWindow(),app.paused);
        app.model.SetMouth(app.speech.Mouth(),app.speech.Speaking()&&!app.paused);
    }

    void OnCommand(UINT id)
    {
        switch(id)
        {
        case IdTalk:
            OpenChatInput();
            break;
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
                    ShowMessage(error,MB_ICONWARNING);
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
            if(!app.model.SetMotion(path,error))
            {
                ShowMessage(error,MB_ICONERROR);
                break;
            }
            app.settings.motionPath=path;
            app.settings.Save(app.settingsPath);
            app.motionRequested=true;   // play it once now
            app.motionClosePending=false;
            break;
        }
        case IdCloseMotion:
            // Blend out first if it is playing; dropped once it has.
            app.settings.motionPath.clear();
            app.settings.Save(app.settingsPath);
            app.motionRequested=false;
            if(app.model.MotionPlaying()){app.model.StopMotion();app.motionClosePending=true;}
            else {std::wstring error;app.model.SetMotion({},error);}
            break;
        case IdOpenWalkMotion:
        case IdOpenRunMotion:
        {
            // A walk or run in place (the kind moved by 全ての親) replaces the generated gait.
            const int which=id==IdOpenWalkMotion?0:1;
            std::wstring& setting=which==0?app.settings.walkMotionPath:app.settings.runMotionPath;
            const auto path=OpenFileDialog(app.hwnd,L"VMD モーション (*.vmd)\0*.vmd\0",setting.empty()?app.settings.modelPath:setting);
            if(path.empty())break;
            if(std::wstring error;!app.model.SetGaitMotion(which,path,error)){ShowMessage(error,MB_ICONERROR);break;}
            setting=path;
            app.settings.Save(app.settingsPath);
            break;
        }
        case IdCloseGaitMotion:
        {
            std::wstring error;
            app.model.SetGaitMotion(0,{},error);
            app.model.SetGaitMotion(1,{},error);
            app.settings.walkMotionPath.clear();
            app.settings.runMotionPath.clear();
            app.settings.Save(app.settingsPath);
            break;
        }
        case IdOpenClickMotion:
        case IdCloseClickMotion:
        {
            // Open replaces the list; Close empties it (back to the built-in reactions).
            std::vector<std::wstring> paths;
            if(id==IdOpenClickMotion)
            {
                const auto& list=app.settings.clickMotionPaths;
                paths=OpenFilesDialog(app.hwnd,L"VMD モーション (*.vmd)\0*.vmd\0",list.empty()?app.settings.modelPath:list.back());
                if(paths.empty())break;
            }
            if(std::wstring error;!app.model.SetClickMotions(paths,error))
            {
                ShowMessage(error,MB_ICONERROR);
                break;
            }
            app.settings.clickMotionPaths=paths;
            app.settings.Save(app.settingsPath);
            break;
        }
        case IdOpenIdleMotion:
        case IdAddIdleMotion:
        case IdCloseIdleMotion:
        {
            // Open replaces the list, Add appends to it (skipping ones already there), Close
            // empties it (back to the generated standing pose).
            std::vector<std::wstring> paths;
            if(id!=IdCloseIdleMotion)
            {
                const auto& list=app.settings.idleMotionPaths;
                auto picked=OpenFilesDialog(app.hwnd,L"VMD モーション (*.vmd)\0*.vmd\0",list.empty()?app.settings.modelPath:list.back());
                if(picked.empty())break;
                if(id==IdAddIdleMotion)paths=list;
                for(auto& p:picked)
                    if(std::find(paths.begin(),paths.end(),p)==paths.end())paths.push_back(std::move(p));
            }
            if(std::wstring error;!app.model.SetIdleMotions(paths,error))
            {
                ShowMessage(error,MB_ICONERROR);
                break;
            }
            app.settings.idleMotionPaths=paths;
            app.settings.Save(app.settingsPath);
            break;
        }
        case IdWindowMode:
            SetWindowMode(!app.settings.windowMode);
            app.settings.Save(app.settingsPath);
            break;
        case IdWalkLeft:
        case IdWalkRight:
            if(app.place==Place::Ground)
            {
                app.model.StopMotion();
                StartWalk(app.windowX+app.windowSize.cx*0.5+(id==IdWalkLeft?-300:300));
            }
            break;
        case IdSettings:
            ShowSettingsWindow(app.hwnd,{app.settings,StartupRegistered(),app.character,&app.memory},ApplySettings,
                               [](int azimuth,int elevation){app.renderer.SetLight(static_cast<float>(azimuth),static_cast<float>(elevation));});
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
            // The stage is large: let clicks through at once when the cursor leaves the frame,
            // without waiting for the next alpha sample.
            if(!app.pressed&&app.place!=Place::Held)
            {
                POINT cursor{};
                GetCursorPos(&cursor);
                const RECT frame=FrameRect();
                if(!PtInRect(&frame,cursor))SetClickThrough(true);
            }
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
                    app.model.StopMotion();
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
                if(app.pressed&&message==WM_LBUTTONUP&&app.place!=Place::Held)
                {
                    app.pendingClick=app.pressCursor;
                    app.pendingClickAt=app.clock+GetDoubleClickTime()/1000.0;
                }
                app.pressed=false;
                if(GetCapture()==hwnd)ReleaseCapture();
            }
            return 0;
        case WM_LBUTTONDBLCLK:
            // Double-click: talk to her (the first click's reaction is called off).
            app.pendingClick.reset();
            OpenChatInput();
            return 0;
        case WM_MOUSEACTIVATE:
            // Hanging behind a window: a click must not raise the mascot over it.
            if(app.behindSurface)return MA_NOACTIVATE;
            break;
        case WM_MOUSEWHEEL:
        {
            // Over the mascot (clicks elsewhere pass through, so the wheel does too): one notch
            // is one 10% size step, up to grow. Smooth-scrolling wheels send smaller deltas.
            if(app.place==Place::Held)return 0;
            app.wheelDelta+=GET_WHEEL_DELTA_WPARAM(wParam);
            int steps=0;
            while(app.wheelDelta>=WHEEL_DELTA){app.wheelDelta-=WHEEL_DELTA;++steps;}
            while(app.wheelDelta<=-WHEEL_DELTA){app.wheelDelta+=WHEEL_DELTA;--steps;}
            if(steps!=0)
            {
                const int percent=std::clamp(app.settings.scale+steps*10,Settings::MinScale,Settings::MaxScale);
                if(percent!=app.settings.scale)ApplyScale(percent);
            }
            return 0;
        }
        case WM_RBUTTONUP:
        {
            POINT p{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd,&p);
            ShowMenu(p);
            return 0;
        }
        case LocalChat::TextMessage:
            if(app.memoryRequest&&static_cast<int>(lParam)==app.memoryRequest)
            {
                std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(wParam));
                if(app.memoryReply.size()<8000)app.memoryReply+=*text;
                return 0;
            }
            OnChatText(reinterpret_cast<std::wstring*>(wParam),static_cast<int>(lParam));
            return 0;
        case VoiceEngine::DoneMessage:
            Trace(L"%.1f voice: %d %s",app.clock,static_cast<int>(lParam),wParam?L"made":L"failed");
            app.speech.Voiced(static_cast<int>(lParam),reinterpret_cast<VoiceClip*>(wParam));
            return 0;
        case LocalChat::DoneMessage:
            if(app.memoryRequest&&static_cast<int>(lParam)==app.memoryRequest)
            {
                std::unique_ptr<std::wstring> error(reinterpret_cast<std::wstring*>(wParam));
                app.memoryRequest=0;
                OnMemoryDone(*error);
                return 0;
            }
            OnChatDone(reinterpret_cast<std::wstring*>(wParam),static_cast<int>(lParam));
            return 0;
        case TrayMessage:
            if(LOWORD(lParam)==WM_LBUTTONDBLCLK)OpenChatInput();
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
            // What was said since the last notes are kept for the next start to look through.
            if(app.settings.chatMemory)
            {
                std::vector<std::pair<bool,std::wstring>> pending;
                for(const auto& m:app.unmemorized)pending.push_back({m.role==L"user",m.content});
                SavePendingExchanges(app.settings.modelPath,pending);
            }
            app.chat.reset();
            app.memoryChat.reset();
            app.speech.SetVoice(nullptr);
            app.voice.reset();
            app.chatInput.Close();
            app.bubble.Hide();
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

    // Diagnostics: MikuMikuDesktopPerch.exe --snapshot <out.png> [frames] renders, saves one frame and exits.
    // --look <x> <y> pins the look-at cursor to that client-area point (for snapshot checks).
    std::wstring snapshotPath;
    int snapshotFrames=60;
    std::optional<POINT> snapshotLook;
    std::wstring snapshotModel;   // --model <pmx>: model for this run only
    int snapshotScale=0;          // --scale <percent>: resize through the menu path after start-up
    std::wstring snapshotActivity;   // --activity walk|sit|grab|fall: hold that generated activity
    std::wstring snapshotGesture;    // --gesture hip|wave|peace|...: hold that standing gesture
    bool snapshotDoze=false;         // --doze: doze off from the start
    std::array<std::wstring,2> snapshotGait;   // --walk-motion / --run-motion <vmd>: use that gait motion
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
            else if(arg==L"--gesture"&&i+1<argc)
            {
                snapshotGesture=argv[++i];
            }
            else if(arg==L"--play-motion")
            {
                app.motionRequested=true;
            }
            else if(arg==L"--walk"&&i+1<argc)
            {
                app.diagnosticWalk=_wtoi(argv[++i]);
            }
            else if(arg==L"--click"&&i+1<argc)
            {
                app.diagnosticClick=std::wstring(argv[++i])==L"head"?1:2;
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
            else if((arg==L"--walk-motion"||arg==L"--run-motion")&&i+1<argc)
            {
                snapshotGait[arg==L"--walk-motion"?0:1]=argv[++i];
            }
            else if(arg==L"--doze")
            {
                snapshotDoze=true;
            }
            else if(arg==L"--light"&&i+2<argc)
            {
                app.settings.lightAzimuth=_wtoi(argv[i+1]);
                app.settings.lightElevation=_wtoi(argv[i+2]);
                i+=2;
            }
            else if(arg==L"--self-shadow"&&i+1<argc)
            {
                app.settings.selfShadow=_wtoi(argv[++i])!=0;
            }
            else if(arg==L"--look"&&i+2<argc)
            {
                snapshotLook=POINT{_wtoi(argv[i+1]),_wtoi(argv[i+2])};
                i+=2;
            }
        }
        LocalFree(argv);
    }

    // DLLs loaded by name from here on are not looked for in the current folder (a model or
    // motion folder could carry a planted copy). GPU drivers keep their own search paths.
    SetDllDirectoryW(L"");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);

    // Diagnostics: --reading <text> <out.txt> writes the hiragana reading the lip-sync uses.
    {
        int argc=0;
        LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
        // --voice-test <VOICEVOX folder> <style id> <out.wav>: say a line with VOICEVOX's files
        // directly (no engine), writing the sound or the error (to out.wav.txt).
        if(argc>=5&&std::wstring(argv[1])==L"--voice-test")
        {
            std::string wav;
            const std::wstring error=SynthesizeVoice({true,{},argv[2]},_wtoi(argv[3]),L"こんにちは。今日もいっしょに、がんばろうね。",wav);
            if(error.empty()){std::ofstream out(std::filesystem::path(argv[4]),std::ios::binary);out.write(wav.data(),static_cast<std::streamsize>(wav.size()));}
            const std::wstring report=error.empty()?L"ok "+std::to_wstring(wav.size())+L" bytes":error;
            const int n=WideCharToMultiByte(CP_UTF8,0,report.c_str(),static_cast<int>(report.size()),nullptr,0,nullptr,nullptr);
            std::string text(static_cast<size_t>(n),'\0');
            WideCharToMultiByte(CP_UTF8,0,report.c_str(),static_cast<int>(report.size()),text.data(),n,nullptr,nullptr);
            std::ofstream(std::filesystem::path(std::wstring(argv[4])+L".txt"),std::ios::binary)<<text;
            LocalFree(argv);
            return 0;
        }
        const bool reading=argc>=4&&std::wstring(argv[1])==L"--reading";
        const std::wstring text=reading?argv[2]:L"",outPath=reading?argv[3]:L"";
        LocalFree(argv);
        if(reading)
        {
            if(FILE* out=nullptr;_wfopen_s(&out,outPath.c_str(),L"w, ccs=UTF-8")==0&&out)
            {
                fwprintf(out,L"%s\n",HiraganaReading(text).c_str());
                fclose(out);
            }
            CoUninitialize();
            return 0;
        }
    }

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

    // One mascot at a time (diagnostic runs excepted). The mutex goes away with the process,
    // or just before a restart starts the next one.
    HANDLE instanceMutex=nullptr;
    if(snapshotPath.empty())
    {
        instanceMutex=CreateMutexW(nullptr,FALSE,L"Local\\MikuMikuDesktopPerch");
        if(instanceMutex&&GetLastError()==ERROR_ALREADY_EXISTS)
        {
            MessageBoxW(nullptr,L"MikuMikuDesktopPerch はすでに起動しています。\n\nタスクトレイのアイコンを右クリックすると、メニューを開けます。",
                        L"MikuMikuDesktopPerch",MB_ICONINFORMATION);
            CloseHandle(instanceMutex);
            CoUninitialize();
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
        app.settingsPath=std::wstring(temp)+L"MikuMikuDesktopPerch_snapshot.ini";
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
        // A missing idle motion is dropped rather than blocking the model.
        std::erase_if(app.settings.idleMotionPaths,[](const std::wstring& p){return !std::filesystem::exists(p);});
        if(app.model.Load(app.settings.modelPath,app.settings.idleMotionPaths,app.settings.physics,app.settings.physicsFps,error))break;
        // So is one that does not load: the model comes up on the built-in idle instead.
        if(std::wstring plainError;!app.settings.idleMotionPaths.empty()&&
           app.model.Load(app.settings.modelPath,{},app.settings.physics,app.settings.physicsFps,plainError))
        {
            if(snapshotPath.empty())
                MessageBoxW(nullptr,(L"待機モーションを読み込めなかったため、外しました。\n\n"+error).c_str(),L"MikuMikuDesktopPerch",MB_ICONWARNING);
            app.settings.idleMotionPaths.clear();
            break;
        }
        if(!snapshotPath.empty())
        {
            // Diagnostic runs report instead of blocking on a dialog.
            if(FILE* info=nullptr;_wfopen_s(&info,(snapshotPath+L".txt").c_str(),L"w, ccs=UTF-8")==0&&info)
            {fwprintf(info,L"error=%s\n",error.c_str());fclose(info);}
            return 2;
        }
        MessageBoxW(nullptr,error.c_str(),L"MikuMikuDesktopPerch",MB_ICONERROR);
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
    wc.style=CS_DBLCLKS;   // a double-click on her opens the chat
    wc.hInstance=instance;
    wc.hCursor=LoadCursorW(nullptr,IDC_HAND);
    // res/MikuMikuDesktopPerch.rc: icon 1, at the sizes the shell asks for.
    wc.hIcon=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(1),IMAGE_ICON,GetSystemMetrics(SM_CXICON),GetSystemMetrics(SM_CYICON),LR_DEFAULTCOLOR));
    wc.hIconSm=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(1),IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_DEFAULTCOLOR));
    wc.lpszClassName=WindowClass;
    RegisterClassExW(&wc);
    // NOREDIRECTIONBITMAP + DirectComposition gives per-pixel alpha;
    // LAYERED is only there so WS_EX_TRANSPARENT can pass clicks through.
    const DWORD exStyle=WS_EX_NOREDIRECTIONBITMAP|WS_EX_LAYERED|WS_EX_TOOLWINDOW|(app.settings.topmost?WS_EX_TOPMOST:0);
    app.stage=WorkAreaAt({x+width/2,y+height/2});
    const RECT& st=app.stage;
    app.hwnd=CreateWindowExW(exStyle,WindowClass,L"MikuMikuDesktopPerch",WS_POPUP,st.left,st.top,st.right-st.left,st.bottom-st.top,nullptr,nullptr,instance,nullptr);
    if(!app.hwnd)return 1;
    // Alpha 254, not 255: Chromium-based apps treat an opaque window covering theirs as occluding
    // and stop painting; a not-quite-opaque layered window is ignored (and looks the same).
    SetLayeredWindowAttributes(app.hwnd,0,254,LWA_ALPHA);

    if(!app.renderer.Initialize(app.hwnd,width,height,app.settings.msaa,error)||
       !app.renderer.LoadModel(app.model,app.settings.toonDir,static_cast<unsigned>(app.settings.maxTextureSize),error))
    {
        MessageBoxW(nullptr,error.c_str(),L"MikuMikuDesktopPerch",MB_ICONERROR);
        DestroyWindow(app.hwnd);
        return 1;
    }
    UpdateCamera();
    MoveWindowTo(app.windowX,GroundedWindowY());
    app.nextDecision=8+app.Random01()*10;
    app.nextMotionAt=60+app.Random01()*120;
    if(std::wstring motionError;!app.model.SetMotion(app.settings.motionPath,motionError))app.settings.motionPath.clear();
    std::erase_if(app.settings.clickMotionPaths,[](const std::wstring& p){return !std::filesystem::exists(p);});
    if(std::wstring clickError;!app.model.SetClickMotions(app.settings.clickMotionPaths,clickError))app.settings.clickMotionPaths.clear();
    for(int which=0;which<2;++which)
    {
        std::wstring& path=which==0?app.settings.walkMotionPath:app.settings.runMotionPath;
        if(std::wstring gaitError;!path.empty()&&(!std::filesystem::exists(path)||!app.model.SetGaitMotion(which,path,gaitError)))path.clear();
    }
    std::optional<Activity> forcedActivity;   // --activity for snapshot checks
    if(snapshotActivity==L"walk")forcedActivity=Activity::Walk;
    else if(snapshotActivity==L"run")forcedActivity=Activity::Run;
    else if(snapshotActivity==L"jump")forcedActivity=Activity::Jump;
    else if(snapshotActivity==L"lie")forcedActivity=Activity::Lie;
    else if(snapshotActivity==L"hang")forcedActivity=Activity::Hang;
    else if(snapshotActivity==L"sit")forcedActivity=Activity::Sit;
    else if(snapshotActivity==L"grab")forcedActivity=Activity::Grabbed;
    else if(snapshotActivity==L"fall")forcedActivity=Activity::Fall;
    if(!snapshotPath.empty()){app.settings.autoBehavior=false;app.settings.windowMode=false;app.settings.chatInitiative=false;}
    if(forcedActivity)app.model.SetActivity(*forcedActivity,1);
    if(!snapshotGesture.empty())app.model.ForceGesture(snapshotGesture);
    if(snapshotDoze){app.dozing=app.forcedDoze=true;app.model.SetDozing(true);}
    for(int which=0;which<2;++which)
        if(std::wstring gaitError;!snapshotGait[which].empty()&&!app.model.SetGaitMotion(which,snapshotGait[which],gaitError))
            Trace(L"gait motion: %s",gaitError.c_str());

    app.tray.cbSize=sizeof(app.tray);
    app.tray.hWnd=app.hwnd;
    app.tray.uID=1;
    app.tray.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;
    app.tray.uCallbackMessage=TrayMessage;
    app.tray.hIcon=wc.hIconSm?wc.hIconSm:wc.hIcon;
    wcscpy_s(app.tray.szTip,L"MikuMikuDesktopPerch");
    Shell_NotifyIconW(NIM_ADD,&app.tray);
    app.bubble.Create(app.hwnd);
    app.character=CharacterProfile::Load(app.settings.modelPath,app.model.ModelName());
    app.memory.Load(app.settings.modelPath);
    for(const auto& [user,text]:TakePendingExchanges(app.settings.modelPath))
        app.unmemorized.push_back({user?L"user":L"assistant",text});
    ApplyVoiceSettings();

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
            if(const HWND settings=SettingsWindowHandle();settings&&IsDialogMessageW(settings,&msg))continue;
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
            MaybeMemorize();
            UpdateDozing();
            MaybeSpeakFirst();
            UpdateFeeling();
            if(app.pendingClick&&app.clock>=app.pendingClickAt)
            {
                OnClick(*app.pendingClick);
                app.pendingClick.reset();
            }
            if(app.diagnosticClick&&app.clock>1.5)
            {
                const bool reacted=app.model.React(app.diagnosticClick==1);
                Trace(L"%.1f diagnostic click: %s (activity %d)",app.clock,reacted?L"reacts":L"no reaction",static_cast<int>(app.model.CurrentActivity()));
                app.diagnosticClick=0;
            }
            if(app.diagnosticWalk&&app.clock>3.0)
            {
                StartWalk(CenterX()+app.diagnosticWalk);
                app.diagnosticWalk=0;
            }
        }

        // Click-through wherever the mascot is not drawn.
        POINT cursor{};
        GetCursorPos(&cursor);
        const RECT r=FrameRect();
        if(app.place!=Place::Held)
        {
            if(PtInRect(&r,cursor))
            {
                const int alpha=app.renderer.AlphaAt({cursor.x-r.left,cursor.y-r.top});
                SetClickThrough(alpha>=0&&alpha<16);
            }
            else SetClickThrough(true);
        }
        if(!forcedActivity)
        {
            UpdateMotionPlayback();
            UpdateWindowMode(dt);
            AvoidCursor(cursor);
            UpdateHangOrder();
        }
        if(snapshotLook)cursor={r.left+snapshotLook->x,r.top+snapshotLook->y};
        UpdateLookTarget(cursor,r,dt);

        app.model.Update(dt);
        // The taskbar can also end up above the mascot without taking the foreground.
        if((app.topmostCheck+=dt)>=2.0){app.topmostCheck=0;ReassertTopmost();}
        // センター's walking travel moves the window by the same amount; the camera follows it.
        if(const float travel=app.model.TakeRootDelta();travel!=0.0f)
        {
            UpdateCamera();
            if(!forcedActivity)MoveWindowTo(app.windowX+travel*ppu,app.windowY);
        }
        if(app.place==Place::Ground&&std::abs(app.windowY-GroundedWindowY())>0.5)MoveWindowTo(app.windowX,GroundedWindowY());
        // After the walking step above: resizing measures where the feet are on screen, which
        // must already include this frame's step or the step is applied twice.
        UpdateExtent(dt);
        if(const std::uint32_t resets=app.model.PhysicsResets();resets!=lastPhysicsResets)
        {
            Trace(L"%.1f physics reset (%u so far) %s",app.clock,resets,app.model.PhysicsResetReason().c_str());
            lastPhysicsResets=resets;
        }
        UpdateSparks(dt);
        PlaceFrame();
        UpdateChatBubble(dt);
        app.renderer.Render(app.model,app.settings.edgeScale);
        if(!snapshotPath.empty()&&--snapshotFrames<=0)
        {
            app.renderer.SaveLastFrame(snapshotPath);
            if(FILE* info=nullptr;_wfopen_s(&info,(snapshotPath+L".txt").c_str(),L"w, ccs=UTF-8")==0&&info)
            {
                const auto rest=app.model.ArmRestAngles(),fix=app.model.ArmCorrections();
                fwprintf(info,L"model=%s\narmRest L=%.1f R=%.1f\narmCorrection L=%.1f R=%.1f\nwindow=%dx%d\n",
                         app.settings.modelPath.c_str(),rest[0],rest[1],fix[0],fix[1],app.windowSize.cx,app.windowSize.cy);
                fwprintf(info,L"%s",app.model.DescribeRig().c_str());
                fclose(info);
            }
            DestroyWindow(app.hwnd);
        }
    }
    if(foregroundHook)UnhookWinEvent(foregroundHook);
    CloseHandle(timer);
    app.renderer.WaitIdle();

    if(instanceMutex){CloseHandle(instanceMutex);instanceMutex=nullptr;}
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
