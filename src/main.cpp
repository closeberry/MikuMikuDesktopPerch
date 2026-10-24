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
    constexpr wchar_t ControlClass[]=L"MikuMikuDesktopPerchControl";
    constexpr UINT TrayMessage=WM_APP+1;
    enum MenuId:UINT {IdOpenModel=100,IdOpenMotion,IdCloseMotion,IdOpenIdleMotion,IdAddIdleMotion,IdCloseIdleMotion,
                      IdOpenClickMotion,IdCloseClickMotion,IdWindowMode,IdWalkLeft,IdWalkRight,IdSettings,IdExit,IdTalk,
                      IdOpenWalkMotion,IdOpenRunMotion,IdCloseGaitMotion,IdAddCharacter,IdRemoveCharacter};
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

    // One character on the desktop. Each has a window of its own: a transparent stage over the
    // work area, so its clicks, its place in the z-order (hanging behind a window) and its
    // click-through are its own, and characters overlap as windows do.
    struct Mascot
    {
        size_t slot=0;   // its settings: app.settings.characters[slot] (see Own)
        std::wstring modelPath;   // the model it was made with (its persona, memory and pending notes)
        MascotModel model;
        MascotRenderer renderer;
        HWND hwnd=nullptr;
        bool clickThrough=false;
        double lookAtViewerUntil=-1;   // while talking (and a moment after) she looks at the viewer
        std::uint32_t lastPhysicsResets=0;

        // The window is a fixed transparent stage over the work area (stage). The drawn frame
        // ("window" below: windowX/Y, windowSize) moves inside it as a composition offset;
        // moving the real window could never be in step with the frame drawn for it.
        RECT stage{};
        // Frame placement in screen pixels, kept in doubles so slow walking accumulates.
        double windowX=0,windowY=0;
        SIZE windowSize{};
        float pixelsPerUnit=40;   // screen pixels per world unit at the model's depth

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
        int diagnosticWalk=0;                     // --walk <px>: walk that far 3 s after start
        int diagnosticClick=0;                    // --click head|body: 1 head, 2 body, 1.5 s after start

        // Ground behaviour.
        double walkTargetX=0;         // screen x for the window centre
        double nextDecision=8;
    };

    // A character to bring on (added, or its model changed) or to take off, done between
    // frames: never while the characters are being updated, nor from inside one's own window.
    struct Change
    {
        enum Kind {Load,Remove} kind=Load;
        size_t slot=0;
        std::wstring previousModel;   // Load of a new model: what to go back to if it fails
    };

    // What the characters share: the settings, the tray icon (on a hidden window of its own),
    // the clock, and the timing of speaking first (one of them at a time).
    struct App
    {
        Settings settings;
        std::wstring settingsPath;
        HWND hwnd=nullptr;            // the hidden control window: tray icon, quitting
        NOTIFYICONDATAW tray{};
        bool paused=false;
        bool restart=false;
        POINT lastCursor{LONG_MIN,LONG_MIN};
        double cursorIdle=0;
        float viewYaw=0;          // --view-yaw: camera orbit for diagnostic snapshots
        // Speaking first: when the next word of her own is due, and whether the user is away.
        double nextChatterAt=1500;
        double awaySince=-1;
        Activity forcedRestPose=Activity::Idle;   // --rest-pose sit|hang (diagnostics)
        std::vector<std::unique_ptr<Mascot>> mascots;   // in the order of settings.characters
        std::vector<Change> changes;
        bool diagnostic=false;    // --snapshot: no dialogs, settings not saved over the user's
        double clock=0,topmostCheck=0;
        std::uint32_t random=0x2545F491u;
        float Random01(){random^=random<<13;random^=random>>17;random^=random<<5;return (random&0xFFFFFF)/16777216.0f;}
    } app;

    CharacterSettings& Own(const Mascot& m){return app.settings.characters[m.slot];}

    void SaveWindowPosition(Mascot& m);

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
    void ApplyVoiceSettings(Mascot& m)
    {
        if(!app.settings.voice){m.speech.SetVoice(nullptr);m.voice.reset();return;}
        if(!m.voice)m.voice=std::make_unique<VoiceEngine>(m.hwnd);
        m.speech.SetVoice([&m](const std::wstring& text){
            return m.voice->Speak({app.settings.voiceDirect,app.settings.voiceEndpoint,app.settings.voiceFolder},Own(m).voiceSpeaker,text);});
    }

    void ApplyProceduralSettings(Mascot& m)
    {
        m.model.SetProcedural(app.settings.blink,app.settings.breath,app.settings.lookAtMouse);
        m.model.SetPhysicsFilter(app.settings.physicsFilter);
        m.renderer.SetSelfShadow(app.settings.selfShadow);
        m.renderer.SetLight(static_cast<float>(app.settings.lightAzimuth),static_cast<float>(app.settings.lightElevation));
    }

    // Rest-pose size and centre of the model; world y=0 is the ground.
    struct ModelFrame {float cx,cz,height,width,top;};
    ModelFrame ModelFrameOf(Mascot& m)
    {
        const auto lo=m.model.BoundsMin(),hi=m.model.BoundsMax();
        return {(lo[0]+hi[0])*0.5f,(lo[2]+hi[2])*0.5f,std::max(hi[1]-lo[1],1.0f),std::max(hi[0]-lo[0],1.0f),hi[1]};
    }

    // The point placed on the surface line: centre of the feet, at the bottom front of the
    // soles (projecting the feet's centre would let perspective sink the toes below the line).
    std::array<float,3> GroundAnchor(Mascot& m)
    {
        const ModelFrame frame=ModelFrameOf(m);
        return {frame.cx+m.model.RootOffset(),m.model.BoundsMin()[1],m.model.SoleFrontZ()};
    }

    // The rest pose with a little room; m.extent starts here and grows while hair spreads.
    Extent BaseExtent(Mascot& m)
    {
        const ModelFrame frame=ModelFrameOf(m);
        // Generous from the start (sitting legs, spread hair, warp sparkles): every resize moves
        // the window while the old frame is still on screen, which shows as a sideways jump.
        return {std::max(frame.width*0.65f,0.8f*frame.height),frame.top+0.25f*frame.height,-0.5f*frame.height};
    }

    // Above this size the frame is drawn at this size's resolution and scaled up on screen, so
    // a very large mascot costs the GPU no more than this.
    constexpr int SharpScaleLimit=300;

    // The size on screen is in MMD's units, the same for every model: Height/Scale is what a
    // model of this height (a typical one, in MMD units) gets, with a little room. A taller or
    // shorter model shows taller or shorter, as in MMD, and accessories reaching past the head
    // (which widen the model's bounds) do not shrink it.
    constexpr float ReferenceModelHeight=20;

    // Model size on screen follows Height/Scale; the window covers m.extent at that size.
    SIZE WindowSizeFor(Mascot& m,int scalePercent)
    {
        m.renderer.SetResolutionScale(std::min(1.0f,static_cast<float>(SharpScaleLimit)/std::max(scalePercent,1)));
        const int modelPixels=MulDiv(MulDiv(app.settings.windowHeight,static_cast<int>(GetDpiForSystem()),96),scalePercent,100);
        m.pixelsPerUnit=modelPixels/(ReferenceModelHeight*1.1f);
        return {static_cast<LONG>(std::lround(2*m.extent.halfWidth*m.pixelsPerUnit)),
                static_cast<LONG>(std::lround((m.extent.top-m.extent.bottom)*m.pixelsPerUnit))};
    }

    // The camera follows センター's walking travel, so the window moves instead of the model leaving it.
    void UpdateCamera(Mascot& m)
    {
        const ModelFrame frame=ModelFrameOf(m);
        const float fovY=DirectX::XMConvertToRadians(30.0f);
        const float cx=frame.cx+m.model.RootOffset(),cy=(m.extent.top+m.extent.bottom)*0.5f;
        const float distance=(m.extent.top-m.extent.bottom)*0.5f/std::tan(fovY*0.5f);
        // app.viewYaw orbits the camera for diagnostic side views (always 0 in normal use).
        const float ex=cx-std::sin(app.viewYaw)*distance,ez=frame.cz-std::cos(app.viewYaw)*distance;
        m.renderer.SetCamera(DirectX::XMVectorSet(ex,cy,ez,1),DirectX::XMVectorSet(cx,cy,frame.cz,1),fovY);
    }

    // Screen position of the model's ground point (centre of the feet).
    POINT GroundPointOnScreen(Mascot& m)
    {
        const POINT p=m.renderer.Project(GroundAnchor(m));
        return {static_cast<LONG>(std::lround(m.windowX))+p.x,static_cast<LONG>(std::lround(m.windowY))+p.y};
    }

    // Resize the window for a scale and extent, keeping the model exactly where it is on screen.
    bool ApplyFrame(Mascot& m,int scalePercent,const Extent& extent)
    {
        const POINT before=GroundPointOnScreen(m);
        const Extent previous=m.extent;
        m.extent=extent;
        const SIZE size=WindowSizeFor(m,scalePercent);
        std::wstring error;
        if(!m.renderer.Resize(static_cast<unsigned>(size.cx),static_cast<unsigned>(size.cy),error))
        {
            m.extent=previous;
            WindowSizeFor(m,Own(m).scale);
            return false;
        }
        m.windowSize=size;
        UpdateCamera(m);
        const POINT inWindow=m.renderer.Project(GroundAnchor(m));
        const double x=before.x-inWindow.x,y=before.y-inWindow.y;
        // A held mascot must stay under the cursor: shift the grab offset with the window.
        m.dragOffset.x+=static_cast<LONG>(std::lround(m.windowX-x));
        m.dragOffset.y+=static_cast<LONG>(std::lround(m.windowY-y));
        m.windowX=x;m.windowY=y;
        return true;
    }

    // Every quarter second: grow the window when hair or limbs reach past it (physics can
    // spread long hair far sideways or upward), shrink back once the pose has settled.
    void UpdateExtent(Mascot& m,double dt)
    {
        m.extentCheck+=dt;
        if(m.extentCheck<0.25)return;
        const double elapsed=m.extentCheck;
        m.extentCheck=0;
        std::array<float,3> lo{},hi{};
        if(!m.model.BoneBounds(lo,hi))return;
        const ModelFrame frame=ModelFrameOf(m);
        const float c=frame.cx+m.model.RootOffset(),margin=0.07f*frame.height;
        Extent need{std::max(std::abs(lo[0]-c),std::abs(hi[0]-c))+margin,hi[1]+margin,lo[1]-margin};
        // Hanging, everything below the edge is hidden behind the window: no room needed there.
        if(m.model.CurrentActivity()==Activity::Hang)need.bottom=std::max(need.bottom,-0.3f*frame.height);
        const Extent base=BaseExtent(m);
        Extent& e=m.extent;
        if(need.halfWidth>e.halfWidth||need.top>e.top||need.bottom<e.bottom)
        {
            // Grow with headroom so a swinging ponytail does not resize every check.
            const Extent grown{std::min(std::max(e.halfWidth,need.halfWidth*1.15f),1.6f*frame.height),
                               std::min(std::max(e.top,need.top+0.05f*frame.height),frame.top+0.8f*frame.height),
                               std::max(std::min(e.bottom,need.bottom-0.05f*frame.height),-0.6f*frame.height)};
            ApplyFrame(m,Own(m).scale,grown);
            m.extentCalm=0;
        }
        else
        {
            // Shrink toward what the pose needs now (never below the rest framing) once the
            // window has been clearly too large for 4 seconds. Some models' hair bones sit
            // outside the rest framing even when still, so "fits the base" is not the test.
            const Extent desired{std::max(base.halfWidth,need.halfWidth*1.15f),std::max(base.top,need.top+0.05f*frame.height),
                                 std::min(base.bottom,need.bottom-0.05f*frame.height)};
            const bool tooLarge=e.halfWidth>desired.halfWidth*1.1f||e.top>desired.top+0.1f*frame.height||e.bottom<desired.bottom-0.1f*frame.height;
            m.extentCalm=tooLarge?m.extentCalm+elapsed:0;
            if(m.extentCalm>=4.0){ApplyFrame(m,Own(m).scale,desired);m.extentCalm=0;}
        }
    }

    double CenterX(Mascot& m){return m.windowX+m.windowSize.cx*0.5;}   // model centre on screen

    RECT WorkAreaOfWindow(Mascot& m)
    {
        return WorkAreaAt({static_cast<LONG>(CenterX(m)),static_cast<LONG>(m.windowY+m.windowSize.cy/2)});
    }

    // Screen y of the surface under the feet: a window's top edge, or the ground line (the
    // taskbar top when the taskbar is at the bottom, otherwise the bottom of the screen; both
    // are the work area's bottom edge).
    double SurfaceLine(Mascot& m)
    {
        return m.surfaceWindow?static_cast<double>(m.surfaceRect.top):static_cast<double>(WorkAreaOfWindow(m).bottom);
    }

    // Horizontal range the model centre may use on the current surface.
    void SurfaceRange(Mascot& m,double& left,double& right)
    {
        const double half=ModelFrameOf(m).width*0.5*m.pixelsPerUnit;
        const RECT r=m.surfaceWindow?m.surfaceRect:WorkAreaOfWindow(m);
        left=r.left+half;right=r.right-half;
        if(left>right)left=right=(r.left+r.right)*0.5;
    }

    // Window top that puts world y=0 (the feet) on a screen line.
    double WindowYForLine(Mascot& m,double line)
    {
        return line-m.renderer.Project(GroundAnchor(m)).y;
    }
    double GroundedWindowY(Mascot& m){return WindowYForLine(m,SurfaceLine(m));}
    double FeetLine(Mascot& m){return m.windowY+m.renderer.Project(GroundAnchor(m)).y;}

    // Takes effect with the next frame drawn (see PlaceFrame).
    void MoveWindowTo(Mascot& m,double x,double y)
    {
        m.windowX=x;m.windowY=y;
    }

    POINT WindowPixel(Mascot& m){return {static_cast<LONG>(std::lround(m.windowX)),static_cast<LONG>(std::lround(m.windowY))};}

    // Screen rectangle of the drawn frame.
    RECT FrameRect(Mascot& m)
    {
        const POINT p=WindowPixel(m);
        return {p.x,p.y,p.x+m.windowSize.cx,p.y+m.windowSize.cy};
    }

    // The stage covers the work area of the monitor under the frame's centre: not the whole
    // monitor, which would make the taskbar treat it as a full-screen window. Crossing to
    // another monitor moves the stage (the only window move left).
    void UpdateStage(Mascot& m)
    {
        const RECT frame=FrameRect(m);
        const RECT work=WorkAreaAt({(frame.left+frame.right)/2,(frame.top+frame.bottom)/2});
        if(EqualRect(&work,&m.stage))return;
        m.stage=work;
        SetWindowPos(m.hwnd,nullptr,work.left,work.top,work.right-work.left,work.bottom-work.top,SWP_NOZORDER|SWP_NOACTIVATE);
    }

    // Before each frame: where it goes in the stage, plus the fraction of a pixel rounded off.
    void PlaceFrame(Mascot& m)
    {
        UpdateStage(m);
        const POINT p=WindowPixel(m);
        m.renderer.SetVisualOffset(p.x-m.stage.left,p.y-m.stage.top);
        m.renderer.SetPixelOffset(static_cast<float>(m.windowX-p.x),static_cast<float>(m.windowY-p.y));
    }

    double ModelHeightPixels(Mascot& m){return ModelFrameOf(m).top*m.pixelsPerUnit;}

    // Scale around the feet: the model's ground point stays where it is on screen.
    void ApplyScale(Mascot& m,int percent)
    {
        if(!ApplyFrame(m,percent,m.extent))
        {
            ShowMessage(L"ウィンドウの大きさを変更できませんでした。",MB_ICONERROR);
            return;
        }
        Own(m).scale=percent;
        SaveWindowPosition(m);
    }

    double ModelHalfWidthPixels(Mascot& m){return ModelFrameOf(m).width*0.5*m.pixelsPerUnit;}

    // Walk (or run) so the model centre reaches targetX on the current surface.
    void StartWalk(Mascot& m,double targetX,Activity gait=Activity::Walk)
    {
        double left,right;
        SurfaceRange(m,left,right);
        m.walkTargetX=std::clamp(targetX,left,right);
        if(std::abs(m.walkTargetX-CenterX(m))<8)return;
        m.model.SetActivity(gait,m.walkTargetX>CenterX(m)?1:-1);
    }

    void StopGroundActivity(Mascot& m)
    {
        m.model.SetActivity(Activity::Idle);
        m.nextDecision=app.clock+10+app.Random01()*20;
        if(m.restPoseAt==-2)m.restPoseAt=app.clock+0.4;   // stepped aside: rest again right there
    }

    // The opened motion is playing, or about to (standing up for it): nothing else starts.
    // Also while it is due and on its way (standing up, warping): nothing else may start then.
    bool MotionBusy(Mascot& m)
    {
        if(m.model.Reacting())return true;   // answering a click
        // An idle motion plays to its end (or its loop's) before she walks off or sits down.
        if(m.model.CurrentActivity()==Activity::Idle&&!m.model.IdleMotionAtBreak())return true;
        const bool due=m.model.HasMotion()&&!m.motionClosePending&&(m.motionRequested||app.clock>=m.nextMotionAt);
        return m.model.MotionPlaying()||app.clock<m.motionStartAt||due;
    }

    void BeginWarp(Mascot& m);

    // A click (press and release without dragging) at a screen point: the mascot reacts,
    // differently on the head. Walking, it stops first; on a trip (warp, jump) it does not.
    void OnClick(Mascot& m,POINT p)
    {
        if(m.place!=Place::Ground||m.travel!=Travel::None)return;
        const Activity a=m.model.CurrentActivity();
        bool head=false;
        if(std::array<float,3> h{};m.model.HeadPosition(h))
        {
            // Above the 頭 bone (the top of the neck) and within about a head's width of it.
            const RECT r=FrameRect(m);
            const POINT s=m.renderer.Project(h);
            const double unit=ModelHeightPixels(m);
            head=p.y<r.top+s.y+0.02*unit&&std::abs(p.x-(r.left+s.x))<0.12*unit;
        }
        if(a==Activity::Walk||a==Activity::Run)StopGroundActivity(m);
        if(m.model.React(head))Trace(L"%.1f click on the %s",app.clock,head?L"head":L"body");
    }

    // Where the others stand on a surface line, or are on their way to: a spot there is taken.
    // Returns the x nearest to the wanted one (between left and right, model centres) that keeps
    // clear of them, or the wanted one when there is no such room.
    double FreeSpot(Mascot& m,double wanted,double line,double left,double right)
    {
        std::vector<std::pair<double,double>> taken;   // centre, room needed from it
        for(const auto& owned:app.mascots)
        {
            Mascot& other=*owned;
            if(&other==&m)continue;
            const bool going=other.place==Place::Warping||other.place==Place::Jumping||other.travel!=Travel::None;
            if(std::abs((going?other.destinationLine:FeetLine(other))-line)>4)continue;
            const Activity a=other.model.CurrentActivity();
            const double x=going?other.destinationX:a==Activity::Walk||a==Activity::Run?other.walkTargetX:CenterX(other);
            taken.push_back({x,ModelHalfWidthPixels(m)+ModelHalfWidthPixels(other)});
        }
        auto clear=[&](double x){
            return x>=left-0.5&&x<=right+0.5&&std::none_of(taken.begin(),taken.end(),[&](const auto& t){return std::abs(x-t.first)<t.second*0.9;});
        };
        std::vector<double> candidates{std::clamp(wanted,left,right)};
        for(const auto& [x,room]:taken){candidates.push_back(x-room);candidates.push_back(x+room);}
        double best=wanted,distance=1e30;
        for(const double x:candidates)
            if(clear(x)&&std::abs(x-wanted)<distance){best=x;distance=std::abs(x-wanted);}
        return best;
    }

    // The opened motion plays once right after opening, then now and then (every 1.5 to 4
    // minutes) while the mascot stands or rests on a surface. Resting, it stands up first; it
    // then warps to the bottom right of the screen and plays the motion on the ground there.
    void UpdateMotionPlayback(Mascot& m)
    {
        const bool playing=m.model.MotionPlaying();
        if(m.motionWasPlaying&&!playing)
        {
            Trace(L"%.1f motion finished",app.clock);
            m.nextMotionAt=app.clock+90+app.Random01()*150;
            if(m.motionClosePending){std::wstring error;m.model.SetMotion({},error);m.motionClosePending=false;}
            if(m.surfaceWindow&&m.restPose!=Activity::Idle)m.restPoseAt=app.clock+0.8;
        }
        m.motionWasPlaying=playing;
        if(playing)
        {
            if(m.place!=Place::Ground)m.model.StopMotion();
            return;
        }
        if(!m.model.HasMotion()||m.motionClosePending||(!m.motionRequested&&app.clock<m.nextMotionAt))return;
        // Now and then (not when just opened): between idle motions, not in the middle of one.
        if(!m.motionRequested&&m.model.CurrentActivity()==Activity::Idle&&!m.model.IdleMotionAtBreak())return;
        if(m.place!=Place::Ground||m.travel!=Travel::None)return;
        const Activity a=m.model.CurrentActivity();
        if(a==Activity::Walk||a==Activity::Run||a==Activity::Jump)return;
        if(a!=Activity::Idle)
        {
            m.restPose=a;
            m.model.SetActivity(Activity::Idle);
            m.motionStartAt=app.clock+0.8;
            return;
        }
        if(app.clock<m.motionStartAt)return;
        // Dance on the ground at the bottom right of the screen: warp there first.
        {
            const RECT work=WorkAreaOfWindow(m);
            const double edge=work.right-ModelHalfWidthPixels(m)-16;
            const double spotX=FreeSpot(m,edge,work.bottom,work.left+ModelHalfWidthPixels(m),edge);
            if(m.surfaceWindow||std::abs(CenterX(m)-spotX)>24)
            {
                m.destinationWindow=nullptr;
                m.destinationLine=work.bottom;
                m.destinationX=spotX;
                BeginWarp(m);
                return;
            }
        }
        Trace(L"%.1f motion starts",app.clock);
        m.motionRequested=false;
        m.restPoseAt=-1;
        m.model.PlayMotion();
        m.motionWasPlaying=true;
    }

    // Occasionally walk somewhere when left alone on the ground. Sitting is kept for window
    // edges: on the taskbar the legs would hang off the screen.
    void UpdateAutoBehavior(Mascot& m)
    {
        if(!app.settings.autoBehavior||m.place!=Place::Ground||m.surfaceWindow||m.travel!=Travel::None||MotionBusy(m))return;
        if(m.model.CurrentActivity()!=Activity::Idle||app.clock<m.nextDecision)return;
        if(app.Random01()<0.6f)
        {
            double left,right;
            SurfaceRange(m,left,right);
            const double distance=(120+app.Random01()*380)*(app.Random01()<0.5f?-1:1);
            double target=CenterX(m)+distance;
            if(target<left||target>right)target=CenterX(m)-distance;
            Trace(L"%.1f auto walk to %.0f",app.clock,target);
            StartWalk(m,target);
        }
        m.nextDecision=app.clock+15+app.Random01()*25;
    }

    // ---- Active-window mode -------------------------------------------------------------

    // Pick where to go for a window: its top edge, nearest to where the mascot is, or the
    // bottom-right of that screen when standing on the edge would push the head off the top.
    void PlanDestination(Mascot& m,HWND window)
    {
        RECT frame{};
        VisibleFrame(window,frame);
        const RECT work=WorkAreaAt({(frame.left+frame.right)/2,(frame.top+frame.bottom)/2});
        const double half=ModelFrameOf(m).width*0.5*m.pixelsPerUnit;
        if(frame.top-ModelHeightPixels(m)<work.top+8)
        {
            m.destinationWindow=nullptr;
            m.destinationLine=work.bottom;
            m.destinationX=FreeSpot(m,work.right-half-16,work.bottom,work.left+half,work.right-half-16);
            return;
        }
        m.destinationWindow=window;
        m.destinationLine=frame.top;
        const double left=frame.left+half,right=frame.right-half;
        m.destinationX=left<=right?FreeSpot(m,std::clamp(CenterX(m),left,right),frame.top,left,right):(frame.left+frame.right)*0.5;
    }

    void BeginCrouch(Mascot& m)
    {
        const double dx=m.destinationX-CenterX(m);
        m.model.SetActivity(Activity::Jump,std::abs(dx)<20?0:(dx>0?1:-1));
        m.travel=Travel::Crouching;
        m.travelTimer=JumpCrouchSeconds;
    }

    // Sparkles for leaving (spiralling out and up from the body) or arriving (spiralling in
    // to it); ring=true adds a flat ring spreading over the surface at the feet.
    void SpawnSparks(Mascot& m,bool arriving,bool ring=false)
    {
        static constexpr std::array<std::array<float,3>,4> palette{{{0.65f,0.95f,1.0f},{1,1,1},{1,0.75f,0.92f},{1,0.92f,0.55f}}};
        const ModelFrame frame=ModelFrameOf(m);
        const float H=frame.height;
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
                s.y1=r(0.05f,0.95f)*frame.top;s.y0=s.y1+r(-0.15f,0.35f)*H;
                s.life=r(0.35f,0.55f);
                s.age=-r(0.0f,0.12f);
            }
            else
            {
                s.radius0=r(0.0f,0.2f)*H;s.radius1=r(0.45f,0.75f)*H;
                s.y0=r(0.05f,0.95f)*frame.top;s.y1=std::min(s.y0+r(0.2f,0.5f)*H,frame.top+0.2f*H);
                s.life=r(0.4f,0.7f);
                s.age=-r(0.0f,0.25f);
            }
            m.sparks.push_back(s);
        }
    }

    void UpdateSparks(Mascot& m,double dt)
    {
        std::vector<MascotRenderer::Particle> particles;
        const ModelFrame frame=ModelFrameOf(m);
        const float cx=frame.cx+m.model.RootOffset();
        for(auto& s:m.sparks)
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
            particles.push_back({{cx+radius*std::cos(angle),s.y0+(s.y1-s.y0)*e,frame.cz+radius*std::sin(angle)},size,
                                 {s.color[0],s.color[1],s.color[2],alpha},s.spin});
        }
        std::erase_if(m.sparks,[](const Spark& s){return s.age>=s.life;});
        m.renderer.SetParticles(particles);
    }

    void BeginWarp(Mascot& m)
    {
        Trace(L"%.1f warp to x=%.0f line=%.0f window=%p",app.clock,m.destinationX,m.destinationLine,m.destinationWindow);
        m.place=Place::Warping;
        m.travel=Travel::None;
        m.warp={};
        m.model.SetActivity(Activity::Idle);
        SpawnSparks(m,false);
        // Hair and skirt ease back while the body spins away, and simulate afresh on arrival.
        m.model.ResetPhysics();
    }

    void Land(Mascot& m,HWND window,double impactPixelsPerSecond);

    // Spin up and shrink away, move the window, then grow and spin down at the destination.
    void UpdateWarp(Mascot& m,double dt)
    {
        auto& w=m.warp;
        w.time+=dt;
        if(!w.arrived&&w.time>=WarpOutSeconds)
        {
            w.arrived=true;
            m.sparks.clear();
            m.surfaceWindow=m.destinationWindow;
            if(m.surfaceWindow)VisibleFrame(m.surfaceWindow,m.surfaceRect);
            MoveWindowTo(m,m.destinationX-m.windowSize.cx*0.5,WindowYForLine(m,m.destinationLine));
            SpawnSparks(m,true);
        }
        // Spin about the vertical axis through the middle of the body.
        const ModelFrame frame=ModelFrameOf(m);
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
        const XMVECTOR pivot=XMVectorSet(frame.cx+m.model.RootOffset(),0.45f*frame.top,frame.cz,0);
        m.renderer.SetModelTransform(XMMatrixTranslationFromVector(XMVectorNegate(pivot))*
                                       XMMatrixScaling(std::max(scale,0.001f),std::max(scale,0.001f),std::max(scale,0.001f))*
                                       XMMatrixRotationY(angle)*XMMatrixTranslationFromVector(pivot));
        if(w.time>=WarpOutSeconds+WarpInSeconds)
        {
            m.renderer.SetModelTransform(XMMatrixIdentity());
            SpawnSparks(m,false,true);
            Land(m,m.destinationWindow,0);
        }
    }

    // Short hops (the target just above or anywhere below, close by) are jumped; walking or
    // running covers the same surface; everything else is a warp.
    void BeginTravel(Mascot& m,HWND window)
    {
        m.servedForeground=window;
        PlanDestination(m,window);
        Trace(L"%.1f travel to %p: x=%.0f line=%.0f window=%p (from x=%.0f line=%.0f)",app.clock,window,
              m.destinationX,m.destinationLine,m.destinationWindow,CenterX(m),SurfaceLine(m));
        m.restPoseAt=-1;
        const bool sameSurface=m.destinationWindow==m.surfaceWindow&&std::abs(m.destinationLine-SurfaceLine(m))<2;
        const double h=ModelHeightPixels(m),dx=m.destinationX-CenterX(m),rise=FeetLine(m)-m.destinationLine;
        if(sameSurface&&std::abs(dx)<2.5*h)
        {
            StartWalk(m,m.destinationX,std::abs(dx)>h?Activity::Run:Activity::Walk);
            m.travel=Travel::None;
            return;
        }
        if(!sameSurface&&std::abs(dx)<0.8*h&&rise<0.6*h)BeginCrouch(m);
        else BeginWarp(m);
    }

    // Ballistic arc from the feet now to the destination: up to an apex above both ends, at real
    // gravity, so the body slows toward the apex and speeds up on the way down.
    void Launch(Mascot& m)
    {
        const double g=FlightGravity*m.pixelsPerUnit,h=ModelHeightPixels(m);
        const double startLine=FeetLine(m),endLine=m.destinationLine,dx=m.destinationX-CenterX(m);
        const double apex=std::min(startLine,endLine)-std::clamp(0.25*h+0.1*std::abs(dx),0.15*h,0.8*h);
        const double up=std::sqrt(2*(startLine-apex)/g),down=std::sqrt(2*(endLine-apex)/g);
        m.jump={CenterX(m),startLine,dx/(up+down),-g*up,up+down,0};
        m.place=Place::Jumping;
        m.travel=Travel::None;
    }

    void Land(Mascot& m,HWND window,double impactPixelsPerSecond)
    {
        Trace(L"%.1f land on %p at x=%.0f",app.clock,window,CenterX(m));
        m.surfaceWindow=window;
        if(window)VisibleFrame(window,m.surfaceRect);
        m.place=Place::Ground;
        m.model.Land(static_cast<float>(impactPixelsPerSecond/m.pixelsPerUnit));
        StopGroundActivity(m);
        if(window)
        {
            // Settle, then sit or hang from the edge (--rest-pose fixes it for checks). Lying down
            // is left out: physics too often tangles hair and skirt on a body turned on its side.
            m.restPose=app.Random01()<0.55f?Activity::Sit:Activity::Hang;
            if(app.forcedRestPose!=Activity::Idle)m.restPose=app.forcedRestPose;
            m.restPoseAt=app.clock+1.2+app.Random01()*1.5;
        }
    }

    // Screen rectangle around the legs (下半身 to the toes), widened for the skirt and the mesh.
    RECT LegScreenRect(Mascot& m)
    {
        std::array<float,3> lo{},hi{};
        RECT r{};
        if(!m.model.LegBounds(lo,hi))return r;
        const ModelFrame frame=ModelFrameOf(m);
        const POINT a=m.renderer.Project({lo[0],lo[1],frame.cz}),b=m.renderer.Project({hi[0],hi[1],frame.cz});
        const LONG side=static_cast<LONG>(0.07*frame.height*m.pixelsPerUnit),end=static_cast<LONG>(0.03*frame.height*m.pixelsPerUnit);
        r={std::min(a.x,b.x)-side,std::min(a.y,b.y)-end,std::max(a.x,b.x)+side,std::max(a.y,b.y)+end};
        OffsetRect(&r,static_cast<int>(m.windowX),static_cast<int>(m.windowY));
        return r;
    }

    // While resting on a window, step aside when the cursor comes near the legs where they
    // cover the window: a short sidestep, then rest again where it stopped.
    // Parts above its top edge never trigger, so the mascot can still be grabbed or
    // right-clicked there. Hanging is behind the window, so nothing there is in the way.
    void AvoidCursor(Mascot& m,POINT cursor)
    {
        if(!m.surfaceWindow||m.place!=Place::Ground||m.travel!=Travel::None||app.clock<m.avoidCooldown||MotionBusy(m))return;
        const Activity a=m.model.CurrentActivity();
        if(a==Activity::Walk||a==Activity::Run||a==Activity::Jump||a==Activity::Hang)return;
        RECT legs=LegScreenRect(m),covered{};
        if(!IntersectRect(&covered,&legs,&m.surfaceRect))return;
        InflateRect(&covered,16,16);
        covered.top=std::max(covered.top,m.surfaceRect.top);
        if(!PtInRect(&covered,cursor))return;
        Trace(L"%.1f avoid cursor %ld,%ld (covered %ld,%ld-%ld,%ld)",app.clock,cursor.x,cursor.y,covered.left,covered.top,covered.right,covered.bottom);
        if(a!=Activity::Idle)m.restPose=a;
        double left,right;
        SurfaceRange(m,left,right);
        // A short sidestep away from the cursor (the other way at the end of the window); if
        // the cursor is still in the way after the cooldown, it steps again.
        const int away=cursor.x<CenterX(m)?1:-1;
        const double step=0.3*ModelHeightPixels(m);
        double target=std::clamp(CenterX(m)+away*step,left,right);
        if(std::abs(target-CenterX(m))<15)target=std::clamp(CenterX(m)-away*step,left,right);
        if(std::abs(target-CenterX(m))<15)return;
        StartWalk(m,target,Activity::Walk);
        m.restPoseAt=-2;   // rest again when the walk ends
        m.avoidCooldown=app.clock+1.5;
    }

    // Hanging puts the mascot's window right behind the window it hangs from, so the body is
    // hidden and only the face and hands show over the edge. Leaving the pose restores topmost a moment
    // later, once the body has come up over the edge.
    void UpdateHangOrder(Mascot& m)
    {
        const bool hanging=m.place==Place::Ground&&m.surfaceWindow&&m.model.CurrentActivity()==Activity::Hang;
        if(hanging)m.hangEndedAt=-1;
        else if(m.behindSurface&&m.hangEndedAt<0)m.hangEndedAt=app.clock;
        const bool behind=hanging||(m.behindSurface&&m.surfaceWindow&&m.place==Place::Ground&&app.clock-m.hangEndedAt<0.3);
        if(behind==m.behindSurface)return;
        m.behindSurface=behind;
        if(behind)
        {
            SetWindowPos(m.hwnd,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
            SetWindowPos(m.hwnd,m.surfaceWindow,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
        }
        else SetWindowPos(m.hwnd,app.settings.topmost?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
    }

    void UpdateWindowMode(Mascot& m,double dt)
    {
        if(!app.settings.windowMode)return;
        const HWND foreground=GetForegroundWindow();
        if(foreground!=m.foreground)
        {
            m.foreground=foreground;
            m.foregroundSince=app.clock;
            wchar_t title[96]{};
            GetWindowTextW(foreground,title,96);
            Trace(L"%.1f foreground %p '%s' standable=%d place=%d travel=%d",app.clock,foreground,title,
                  IsStandableWindow(foreground,m.hwnd,static_cast<int>(ModelFrameOf(m).width*m.pixelsPerUnit)),
                  static_cast<int>(m.place),static_cast<int>(m.travel));
        }

        if(m.place==Place::Ground)
        {
            // Ride along with the window under the feet; fall when it goes away.
            if(m.surfaceWindow)
            {
                RECT frame{};
                if(!IsStandableWindow(m.surfaceWindow,m.hwnd,0)||!VisibleFrame(m.surfaceWindow,frame))
                {
                    m.surfaceWindow=nullptr;
                    m.place=Place::Falling;
                    m.fallSpeed=0;
                    m.travel=Travel::None;
                    m.model.SetActivity(Activity::Fall);
                    return;
                }
                const LONG dx=frame.left-m.surfaceRect.left;
                m.surfaceRect=frame;
                if(dx)MoveWindowTo(m,m.windowX+dx,m.windowY);
                // Maximized or dragged to the top: the head would leave the screen.
                if(frame.top-ModelHeightPixels(m)<WorkAreaOfWindow(m).top+8&&m.travel==Travel::None&&!MotionBusy(m))
                {
                    BeginTravel(m,m.surfaceWindow);
                    return;
                }
            }

            // Once airborne the destination is fixed until landing.
            if(m.travel==Travel::Crouching&&(m.travelTimer-=dt)<=0){Launch(m);return;}

            // A new foreground window (settled for a moment): go there.
            // (Held back while the opened motion plays; it goes once that has finished.)
            if(m.travel==Travel::None&&!MotionBusy(m)&&app.clock-m.foregroundSince>0.6&&foreground!=m.surfaceWindow&&
               foreground!=m.servedForeground&&IsStandableWindow(foreground,m.hwnd,static_cast<int>(ModelFrameOf(m).width*m.pixelsPerUnit)))
            {
                BeginTravel(m,foreground);
                return;
            }

            // Settled on a window: take (or resume) the resting pose.
            if(m.surfaceWindow&&m.travel==Travel::None&&!MotionBusy(m)&&m.restPoseAt>=0&&app.clock>=m.restPoseAt&&
               m.model.CurrentActivity()==Activity::Idle)
            {
                // Sitting faces one side or the other at random (knees to that side).
                m.model.SetActivity(m.restPose,app.Random01()<0.5f?-1:1);
                m.restPoseAt=-1;
            }
        }
    }

    // Turn the cursor offset from the head on screen into a yaw/pitch for the head to face.
    void UpdateLookTarget(Mascot& m,POINT cursor,const RECT& window,double dt)
    {
        if(cursor.x!=app.lastCursor.x||cursor.y!=app.lastCursor.y){app.cursorIdle=0;app.lastCursor=cursor;}
        else app.cursorIdle+=dt;
        std::array<float,3> head{};
        const Activity a=m.model.CurrentActivity();
        // Talking (waiting for the reply, saying it, or with the input box open), she looks at
        // the viewer, who sits in front of the middle of the screen, instead of the cursor.
        if((m.chat&&m.chat->Busy())||m.speech.Speaking()||m.bubble.Talking()||m.chatInput.IsOpen())
            m.lookAtViewerUntil=app.clock+2.5;
        const bool atViewer=app.clock<m.lookAtViewerUntil;
        // The opened motion leads the head itself while it plays.
        if((!app.settings.lookAtMouse&&!atViewer)||m.model.Dozing()||a==Activity::Walk||a==Activity::Fall||m.model.MotionPlaying()||m.model.HeadBusy()||!m.model.HeadPosition(head))
        {
            m.model.SetLookTarget(0,0,false);
            return;
        }
        const POINT h=m.renderer.Project(head);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(m.hwnd,MONITOR_DEFAULTTONEAREST),&monitor);
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
        m.model.SetLookTarget(yaw,pitch,atViewer||app.cursorIdle<LookIdleSeconds);
    }

    void SetClickThrough(Mascot& m,bool enabled);

    // A modal dialog stops the frame loop, which keeps click-through up to date. Let clicks
    // through the stages first, or they would block every window under them while it is up.
    void LetClicksThrough()
    {
        for(const auto& m:app.mascots)if(m->hwnd)SetClickThrough(*m,true);
    }

    // Owned by the first character's window (topmost, so the message comes up over it).
    int ShowMessage(const std::wstring& text,UINT type)
    {
        LetClicksThrough();
        const HWND owner=!app.mascots.empty()&&app.mascots.front()->hwnd?app.mascots.front()->hwnd:app.hwnd;
        return MessageBoxW(owner,text.c_str(),L"MikuMikuDesktopPerch",type);
    }

    std::wstring OpenFileDialog(HWND owner,const wchar_t* filter,const std::wstring& current)
    {
        LetClicksThrough();
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
        LetClicksThrough();
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
    void SaveWindowPosition(Mascot& m)
    {
        if(m.hwnd)Own(m).x=static_cast<int>(m.windowX);
        app.settings.Save(app.settingsPath);
    }

    // Clicking the taskbar raises it above other topmost windows, mascot included. Put the
    // mascot back on top, but never over shell overlays (Start menu, flyouts, notifications),
    // which are topmost windows of their own when they take the foreground.
    bool IsMascotWindow(HWND window)
    {
        return std::any_of(app.mascots.begin(),app.mascots.end(),[&](const auto& m){return m->hwnd==window;});
    }

    void ReassertTopmost()
    {
        if(!app.settings.topmost||app.paused)return;
        if(HWND foreground=GetForegroundWindow();foreground&&!IsMascotWindow(foreground))
        {
            wchar_t name[64]{};
            GetClassNameW(foreground,name,static_cast<int>(std::size(name)));
            const bool taskbar=wcscmp(name,L"Shell_TrayWnd")==0||wcscmp(name,L"Shell_SecondaryTrayWnd")==0;
            const bool overlay=(GetWindowLongPtrW(foreground,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0;
            if(overlay&&!taskbar)return;
        }
        for(const auto& m:app.mascots)
            if(m->hwnd&&!m->behindSurface)
                SetWindowPos(m->hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
    }

    void CALLBACK OnForegroundChanged(HWINEVENTHOOK,DWORD,HWND,LONG,LONG,DWORD,DWORD)
    {
        ReassertTopmost();
    }

    void SetClickThrough(Mascot& m,bool enabled)
    {
        if(enabled==m.clickThrough)return;
        m.clickThrough=enabled;
        auto style=GetWindowLongPtrW(m.hwnd,GWL_EXSTYLE);
        style=enabled?(style|WS_EX_TRANSPARENT):(style&~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
        SetWindowLongPtrW(m.hwnd,GWL_EXSTYLE,style);
    }

    void ShowMenu(Mascot& m,POINT screen)
    {
        // Things used often stay here; switches and display options are in the settings window.
        HMENU menu=CreatePopupMenu();
        AppendMenuW(menu,MF_STRING,IdTalk,L"話しかける...");
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        // A character's window goes away when its model changes or it is taken off, and the
        // settings window belongs to one: not while that is open.
        const UINT settingsOpen=SettingsWindowHandle()?MF_GRAYED:0;
        AppendMenuW(menu,MF_STRING|settingsOpen,IdOpenModel,L"モデルを開く...");
        AppendMenuW(menu,MF_STRING|(app.mascots.size()>=Settings::MaxCharacters?MF_GRAYED:0),IdAddCharacter,
                    (L"キャラクターを追加...（"+std::to_wstring(app.mascots.size())+L"/"+std::to_wstring(Settings::MaxCharacters)+L"）").c_str());
        AppendMenuW(menu,MF_STRING|(app.mascots.size()<=1?MF_GRAYED:settingsOpen),IdRemoveCharacter,
                    (L"「"+m.character.name+L"」を外す").c_str());
        HMENU motions=CreatePopupMenu();
        AppendMenuW(motions,MF_STRING,IdOpenMotion,L"モーションを開く...");
        AppendMenuW(motions,MF_STRING|(Own(m).motionPath.empty()?MF_GRAYED:0),IdCloseMotion,L"モーションを閉じる");
        AppendMenuW(motions,MF_SEPARATOR,0,nullptr);
        const size_t idleCount=Own(m).idleMotionPaths.size();
        AppendMenuW(motions,MF_STRING,IdOpenIdleMotion,L"待機モーションを開く...（複数選択可）");
        AppendMenuW(motions,MF_STRING|(idleCount==0?MF_GRAYED:0),IdAddIdleMotion,L"待機モーションを追加...");
        AppendMenuW(motions,MF_STRING|(idleCount==0?MF_GRAYED:0),IdCloseIdleMotion,
                    idleCount>1?(L"待機モーションを閉じる（"+std::to_wstring(idleCount)+L"件）").c_str():L"待機モーションを閉じる");
        AppendMenuW(motions,MF_SEPARATOR,0,nullptr);
        const size_t clickCount=Own(m).clickMotionPaths.size();
        AppendMenuW(motions,MF_STRING,IdOpenClickMotion,L"クリック時のモーションを開く...（複数選択可）");
        AppendMenuW(motions,MF_STRING|(clickCount==0?MF_GRAYED:0),IdCloseClickMotion,
                    clickCount>1?(L"クリック時のモーションを閉じる（"+std::to_wstring(clickCount)+L"件）").c_str():L"クリック時のモーションを閉じる");
        AppendMenuW(motions,MF_SEPARATOR,0,nullptr);
        AppendMenuW(motions,MF_STRING|(Own(m).walkMotionPath.empty()?0:MF_CHECKED),IdOpenWalkMotion,L"歩くモーションを開く...");
        AppendMenuW(motions,MF_STRING|(Own(m).runMotionPath.empty()?0:MF_CHECKED),IdOpenRunMotion,L"走るモーションを開く...");
        AppendMenuW(motions,MF_STRING|(Own(m).walkMotionPath.empty()&&Own(m).runMotionPath.empty()?MF_GRAYED:0),IdCloseGaitMotion,
                    L"歩く・走るモーションを閉じる");
        AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(motions),L"モーション");
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        const bool grounded=m.place==Place::Ground;
        AppendMenuW(menu,MF_STRING|(app.settings.windowMode?MF_CHECKED:0),IdWindowMode,L"アクティブウィンドウに乗る");
        AppendMenuW(menu,MF_STRING|(grounded?0:MF_GRAYED),IdWalkLeft,L"左へ歩く");
        AppendMenuW(menu,MF_STRING|(grounded?0:MF_GRAYED),IdWalkRight,L"右へ歩く");
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        AppendMenuW(menu,MF_STRING,IdSettings,L"設定...");
        AppendMenuW(menu,MF_STRING,IdExit,L"終了");
        // The menu needs its owner in the foreground, which also raises it: while hanging, put
        // the mascot straight back behind the window it hangs from (before and after the menu).
        auto keepBehind=[&m]{
            if(m.behindSurface&&m.surfaceWindow)
                SetWindowPos(m.hwnd,m.surfaceWindow,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
        };
        SetForegroundWindow(m.hwnd);
        keepBehind();
        TrackPopupMenu(menu,TPM_RIGHTBUTTON,screen.x,screen.y,0,m.hwnd,nullptr);
        PostMessageW(m.hwnd,WM_NULL,0,0);
        keepBehind();
        DestroyMenu(menu);
    }

    void SetWindowMode(bool enabled)
    {
        app.settings.windowMode=enabled;
        for(const auto& owned:app.mascots)
        {
            Mascot& m=*owned;
            m.servedForeground=nullptr;
            m.foregroundSince=app.clock;
            if(!enabled&&m.surfaceWindow&&m.place==Place::Ground)
            {
                // Leaving the mode while on a window: drop down to the ground line.
                m.surfaceWindow=nullptr;
                m.travel=Travel::None;
                m.restPoseAt=-1;
                m.place=Place::Falling;
                m.fallSpeed=0;
                m.model.SetActivity(Activity::Fall);
            }
        }
    }

    // OK in the settings window: switches and display options take effect at once (for every
    // character; the size, the voice and the persona for the one the window was opened for);
    // the detailed ones (frame rate, antialiasing, textures, physics rate, toon folder) are read
    // at start-up, so offer a restart for them.
    void ApplySettings(Mascot& m,const SettingsChoice& choice)
    {
        const Settings& s=choice.settings;
        Settings& current=app.settings;
        if(s.windowMode!=current.windowMode)SetWindowMode(s.windowMode);
        if(s.autoBehavior!=current.autoBehavior)
        {
            current.autoBehavior=s.autoBehavior;
            for(const auto& other:app.mascots)other->nextDecision=app.clock+5+app.Random01()*5;
        }
        current.physics=s.physics;
        current.physicsFilter=s.physicsFilter;
        current.blink=s.blink;
        current.breath=s.breath;
        current.lookAtMouse=s.lookAtMouse;
        current.selfShadow=s.selfShadow;
        current.lightAzimuth=s.lightAzimuth;
        current.lightElevation=s.lightElevation;
        const bool topmostChanged=s.topmost!=current.topmost;
        current.topmost=s.topmost;
        current.edgeScale=s.edgeScale;
        current.chatEndpoint=s.chatEndpoint;
        const bool chatModelChanged=s.chatModel!=current.chatModel;
        current.chatModel=s.chatModel;
        current.chatMemory=s.chatMemory;
        current.voiceDirect=s.voiceDirect;
        current.voiceFolder=s.voiceFolder;
        const bool voiceChanged=s.voice!=current.voice;
        current.voice=s.voice;
        for(const auto& owned:app.mascots)
        {
            Mascot& each=*owned;
            if(s.physics!=each.model.PhysicsEnabled())each.model.SetPhysics(s.physics);
            ApplyProceduralSettings(each);
            if(topmostChanged&&!each.behindSurface)
                SetWindowPos(each.hwnd,current.topmost?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            if(chatModelChanged)each.conversation.clear();
            if(voiceChanged)ApplyVoiceSettings(each);
            if(!current.chatMemory)each.unmemorized.clear();
        }
        Own(m).voiceSpeaker=choice.mascot.voiceSpeaker;
        Own(m).voiceName=choice.mascot.voiceName;
        if(choice.character.name!=m.character.name||choice.character.persona!=m.character.persona)
        {
            // A new character starts a new conversation.
            m.character=choice.character;
            m.conversation.clear();
            if(!m.character.Save(Own(m).modelPath))ShowMessage(L"人物設定を保存できませんでした。",MB_ICONERROR);
        }
        if(choice.startWithWindows!=StartupRegistered()&&!SetStartup(choice.startWithWindows))
            ShowMessage(L"スタートアップの設定を変更できませんでした。",MB_ICONERROR);
        const bool restart=s.fps!=current.fps||s.motionBlur!=current.motionBlur||s.msaa!=current.msaa||s.maxTextureSize!=current.maxTextureSize||
                           s.physicsFps!=current.physicsFps||s.physicsIterations!=current.physicsIterations||s.toonDir!=current.toonDir;
        current.fps=s.fps;
        current.motionBlur=s.motionBlur;
        current.msaa=s.msaa;
        current.maxTextureSize=s.maxTextureSize;
        current.physicsFps=s.physicsFps;
        current.physicsIterations=s.physicsIterations;
        current.toonDir=s.toonDir;
        if(choice.mascot.scale!=Own(m).scale)ApplyScale(m,choice.mascot.scale);
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
    bool ChatReady(Mascot& m,bool tell)
    {
        if(app.settings.chatModel.empty())
        {
            if(tell)m.bubble.ShowNotice(L"設定の「会話」で、使うモデルを選んでください（Ollama などを起動してから「一覧を取得」）。");
            return false;
        }
        if(const auto problem=CheckLocalEndpoint(app.settings.chatEndpoint);!problem.empty())
        {
            if(tell)m.bubble.ShowNotice(problem);
            return false;
        }
        if(!m.chat)m.chat=std::make_unique<LocalChat>(m.hwnd);
        return true;
    }

    // The feelings a reply may start with, as the tags the LLM is asked to write.
    constexpr std::pair<const wchar_t*,Expression> Feelings[]={
        {L"笑顔",Expression::Smile},{L"驚き",Expression::Surprise},{L"困り",Expression::Troubled},{L"怒り",Expression::Angry},
        {L"悲しみ",Expression::Sad},{L"照れ",Expression::Shy},{L"普通",Expression::Neutral}};

    // Ask for a reply to the conversation as it stands; query is what the memory is searched for.
    void StartReply(Mascot& m,const std::wstring& query)
    {
        // The last six exchanges are enough for a chat at the desk.
        while(m.conversation.size()>12)m.conversation.erase(m.conversation.begin());
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
            if(const std::wstring notes=m.memory.Recall(text,1500);!notes.empty())
                remembered=L"\n以前の会話で覚えたこと（必要なときだけ自然に話に生かしてください。ここにないことを覚えているふりはしないでください）：\n"+notes+L"\n";
        std::vector<ChatMessage> messages;
        messages.push_back({L"system",
            L"あなたは「"+m.character.name+L"」です。\n"+m.character.persona+L"\n\n"+partner+remembered+
            L"いまは"+clock+L"です。\n"
            L"あなたの返事は、デスクトップの上にいるあなたの頭上の吹き出しに表示されます。"
            L"1〜3文くらいの短い話し言葉で答えてください。Markdown、箇条書き、絵文字、顔文字は使わないでください。\n"
            L"返事のいちばん最初に、そのときの気持ちを［笑顔］［驚き］［困り］［怒り］［悲しみ］［照れ］［普通］のどれか1つで書いてください（例：［笑顔］そうなんだ！）。"
            L"この印は最初の1つだけで、ほかの所には書かないでください。\n"
            L"話し相手の発言が（ ）で囲まれているときは、実際の発言ではなく状況の説明です。その状況に合わせて、あなたから話しかけてください。"});
        messages.insert(messages.end(),m.conversation.begin(),m.conversation.end());
        m.reply.clear();
        m.replyShown=0;
        m.speech.Reset();
        m.replyFeelingSet=false;
        if(m.voice)m.voice->CancelAll();   // what was left of the last reply is not said
        m.chatRequest=m.chat->Start(app.settings.chatEndpoint,app.settings.chatModel,std::move(messages));
        m.bubble.Begin(m.character.name);
        Trace(L"%.1f talk: request %d",app.clock,m.chatRequest);
    }

    void Talk(Mascot& m,const std::wstring& text)
    {
        if(!ChatReady(m,true))return;
        m.conversation.push_back({L"user",text});
        StartReply(m,text);
    }

    // She speaks first: situation says why (it goes in as a stage direction in brackets).
    void SpeakFirst(Mascot& m,const std::wstring& situation)
    {
        if(!ChatReady(m,false))return;
        Trace(L"%.1f speak first: %s",app.clock,situation.c_str());
        m.conversation.push_back({L"user",L"（"+situation+L"）"});
        StartReply(m,situation);
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
    void UpdateDozing(Mascot& m)
    {
        if(m.forcedDoze)return;
        const Activity a=m.model.CurrentActivity();
        const bool still=(a==Activity::Idle||a==Activity::Sit||a==Activity::Hang||a==Activity::Lie)&&m.travel==Travel::None&&
                         !m.model.MotionPlaying()&&!(m.chat&&m.chat->Busy())&&!m.bubble.Visible()&&!m.chatInput.IsOpen();
        const double idle=InputIdleSeconds();
        if(!m.dozing&&still&&idle>=5*60)
        {
            m.dozing=true;
            m.model.SetDozing(true);
            Trace(L"%.1f doze off",app.clock);
        }
        else if(m.dozing&&(idle<2||!still))
        {
            m.dozing=false;
            m.model.SetDozing(false);
            if(idle<2&&a==Activity::Idle&&app.Random01()<0.5f)m.model.Emote(L"yawn");
            Trace(L"%.1f wake up",app.clock);
        }
        if(m.dozing)m.nextDecision=std::max(m.nextDecision,app.clock+10);   // no wandering off asleep
    }

    // With several characters, one of them speaks: picked at random among those standing or
    // resting on a surface, and never while another one is talking (or being talked to).
    void MaybeSpeakFirst()
    {
        if(!app.settings.chatInitiative||app.settings.chatModel.empty()||app.clock<8)return;
        const double idle=InputIdleSeconds();
        if(idle>=15*60&&app.awaySince<0)app.awaySince=app.clock-idle;
        std::vector<Mascot*> free;
        for(const auto& owned:app.mascots)
        {
            const Mascot& each=*owned;
            if((each.chat&&each.chat->Busy())||each.bubble.Visible()||each.chatInput.IsOpen()||each.speech.Speaking())return;
            if(each.place==Place::Ground&&each.model.CurrentActivity()!=Activity::Grabbed&&each.travel==Travel::None)free.push_back(owned.get());
        }
        if(free.empty())return;
        auto anyone=[&]()->Mascot&{return *free[std::min(free.size()-1,static_cast<size_t>(app.Random01()*free.size()))];};
        if(app.awaySince>=0&&idle<3)
        {
            const int minutes=static_cast<int>((app.clock-app.awaySince)/60);
            app.awaySince=-1;
            SpeakFirst(anyone(),L"話し相手が"+std::to_wstring(minutes)+L"分ほど席を外していて、いまパソコンの前に戻ってきました。おかえりのひと言を");
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
                SpeakFirst(anyone(),s==L"morning"?L"朝です。話し相手がパソコンを使い始めました。朝のあいさつを":
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
            SpeakFirst(anyone(),L"特に用事はないけれど、話し相手に何かひと言話しかけたくなりました。覚えていることや、いまの時間帯・季節にちなんだ話題で");
        }
    }

    // The face keeps the reply's feeling while she speaks, and a few seconds after.
    void UpdateFeeling(Mascot& m)
    {
        if((m.chat&&m.chat->Busy())||m.speech.Speaking()||m.bubble.Talking()){m.feelingUntil=app.clock+3;return;}
        if(m.feelingUntil>=0&&app.clock>=m.feelingUntil)
        {
            m.feelingUntil=-1;
            m.model.SetExpression(Expression::Neutral,false);
        }
    }

    void OpenChatInput(Mascot& m)
    {
        // Just above the feet, over the legs, so it is close to her without hiding the face.
        const RECT r=FrameRect(m);
        const POINT ground=GroundPointOnScreen(m);
        m.chatInput.Open(m.hwnd,{(r.left+r.right)/2,ground.y-static_cast<LONG>(ModelHeightPixels(m)*0.12)},[&m](const std::wstring& text){Talk(m,text);});
    }

    void OnChatText(Mascot& m,std::wstring* text,int id)
    {
        std::unique_ptr<std::wstring> owned(text);
        if(id!=m.chatRequest)return;
        m.reply+=*owned;
        if(!m.replyFeelingSet)
            for(const auto& [tag,feeling]:Feelings)
                if(m.reply.find(tag)!=std::wstring::npos&&m.reply.find(tag)<16)
                {
                    m.replyFeelingSet=true;
                    m.model.SetExpression(feeling,true);
                    m.feelingUntil=-1;
                    Trace(L"%.1f talk: feeling %s",app.clock,tag);
                    break;
                }
        const std::wstring clean=CleanReply(m.reply);
        if(clean.size()>m.replyShown)
        {
            m.bubble.Append(clean.substr(m.replyShown));
            m.speech.Add(clean.substr(m.replyShown));
            m.replyShown=clean.size();
        }
    }

    void OnChatDone(Mascot& m,std::wstring* error,int id)
    {
        std::unique_ptr<std::wstring> owned(error);
        if(id!=m.chatRequest)return;
        Trace(L"%.1f talk: request %d done %s",app.clock,id,owned->c_str());
        if(!owned->empty())
        {
            // Not answered: forget the question too, and say why.
            if(!m.conversation.empty()&&m.conversation.back().role==L"user")m.conversation.pop_back();
            m.speech.Reset();
            m.bubble.ShowNotice(*owned);
            return;
        }
        std::wstring clean=CleanReply(m.reply+L"\n");
        while(!clean.empty()&&std::iswspace(clean.back()))clean.pop_back();
        const size_t start=clean.find_first_not_of(L" \t\r\n");
        clean=start==std::wstring::npos?std::wstring():clean.substr(start);
        // The last piece held back (a possible tag start) is shown and said now.
        if(const std::wstring all=CleanReply(m.reply+L"\n");all.size()>m.replyShown)
        {
            const std::wstring rest=all.substr(m.replyShown);
            m.bubble.Append(rest);
            m.speech.Add(rest);
            m.replyShown=all.size();
        }
        m.speech.Finish();
        if(!clean.empty())
        {
            if(app.settings.chatMemory&&!m.conversation.empty()&&m.conversation.back().role==L"user"&&
               m.conversation.back().content.rfind(L"（",0)!=0)
            {
                m.unmemorized.push_back(m.conversation.back());
                m.unmemorized.push_back({L"assistant",clean});
                while(m.unmemorized.size()>24)m.unmemorized.erase(m.unmemorized.begin());
            }
            m.lastReplyAt=app.clock;
            m.conversation.push_back({L"assistant",clean});
        }
        else if(!m.conversation.empty()&&m.conversation.back().role==L"user")m.conversation.pop_back();
        m.bubble.Finish();
    }

    // Once the conversation has paused (or grown long), ask the LLM what in it is worth
    // remembering. It runs in the background on its own request, never during a reply.
    void MaybeMemorize(Mascot& m)
    {
        if(!app.settings.chatMemory||m.unmemorized.empty()||app.settings.chatModel.empty())return;
        if((m.chat&&m.chat->Busy())||(m.memoryChat&&m.memoryChat->Busy()))return;
        if(app.clock-m.lastReplyAt<20&&m.unmemorized.size()<8)return;
        if(!CheckLocalEndpoint(app.settings.chatEndpoint).empty()){m.unmemorized.clear();return;}
        if(!m.memoryChat)m.memoryChat=std::make_unique<LocalChat>(m.hwnd);
        const std::wstring& name=m.character.name;
        const std::wstring partner=app.settings.userName.empty()?L"話し相手":L"話し相手（"+app.settings.userName+L"）";
        std::wstring dialogue;
        for(const auto& m:m.unmemorized)dialogue+=(m.role==L"user"?L"話し相手：":name+L"：")+m.content+L"\n";
        std::wstring known;
        for(const auto& e:m.memory.Entries())known+=L"- "+e.text+L"\n";
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
        m.unmemorized.clear();
        m.memoryReply.clear();
        m.memoryRequest=m.memoryChat->Start(app.settings.chatEndpoint,app.settings.chatModel,std::move(messages));
        Trace(L"%.1f memory: request %d",app.clock,m.memoryRequest);
    }

    void OnMemoryDone(Mascot& m,const std::wstring& error)
    {
        Trace(L"%.1f memory: done %s",app.clock,error.c_str());
        if(!error.empty())return;   // the exchanges are dropped: nothing to retry with later
        bool added=false;
        std::wstringstream lines(CleanReply(m.memoryReply+L"\n"));
        for(std::wstring line;std::getline(lines,line);)
        {
            // Bullets and numbering the model adds anyway.
            const size_t start=line.find_first_not_of(L" \t\r-*・•●0123456789.．)）、");
            if(start==std::wstring::npos)continue;
            line=line.substr(start);
            while(!line.empty()&&std::iswspace(line.back()))line.pop_back();
            if(line.empty()||line.find(L"なし")==0||line.size()>CharacterMemory::MaxLength)continue;
            if(line.back()==L'：'||line.back()==L':')continue;   // a heading
            if(m.memory.Add(line,TodayDate())){added=true;Trace(L"%.1f memory: + %s",app.clock,line.c_str());}
        }
        if(added)
        {
            m.memory.Save();
            RefreshMemoryWindow();
        }
    }

    // Per frame: the bubble follows the head (its tail points at the top of the head) and the
    // mouth moves while the text is being revealed.
    void UpdateChatBubble(Mascot& m,double dt)
    {
        if(!m.bubble.Visible())
        {
            if(m.speech.Active())m.speech.Reset();
            m.model.SetMouth({},false);
            return;
        }
        m.speech.Tick(dt);
        if(m.speech.Active())m.bubble.SetReveal(m.speech.RevealedChars());
        const RECT r=FrameRect(m);
        const double unit=ModelHeightPixels(m);
        POINT anchor{(r.left+r.right)/2,r.top};
        if(std::array<float,3> head{};m.model.HeadPosition(head))
        {
            const POINT h=m.renderer.Project(head);
            anchor={r.left+h.x,r.top+h.y-static_cast<LONG>(unit*0.13)};
        }
        m.bubble.Tick(dt,anchor,static_cast<int>(unit*0.08),WorkAreaOfWindow(m),app.paused);
        m.model.SetMouth(m.speech.Mouth(),m.speech.Speaking()&&!app.paused);
    }

    // A model can be on the desktop once (its persona and memory are kept per model), and must
    // load as a humanoid: checked before anything is replaced.
    bool ModelUsable(const std::wstring& path)
    {
        for(const auto& c:app.settings.characters)
            if(_wcsicmp(c.modelPath.c_str(),path.c_str())==0)
            {
                ShowMessage(L"このモデルは、すでにデスクトップにいます。",MB_ICONINFORMATION);
                return false;
            }
        MascotModel probe;
        std::wstring error;
        if(!probe.Load(path,{},false,app.settings.physicsFps,error))
        {
            ShowMessage(error,MB_ICONWARNING);
            return false;
        }
        return true;
    }

    void OnCommand(Mascot& m,UINT id)
    {
        switch(id)
        {
        case IdTalk:
            OpenChatInput(m);
            break;
        case IdOpenModel:
        {
            auto path=OpenFileDialog(m.hwnd,L"PMX モデル (*.pmx)\0*.pmx\0",Own(m).modelPath);
            if(path.empty()||!ModelUsable(path))break;
            // The window size follows the model's proportions: the character is made afresh
            // with the new model, where it stands now.
            SaveWindowPosition(m);
            app.changes.push_back({Change::Load,m.slot,Own(m).modelPath});
            Own(m).modelPath=path;
            break;
        }
        case IdAddCharacter:
        {
            if(app.settings.characters.size()>=Settings::MaxCharacters)break;
            auto path=OpenFileDialog(m.hwnd,L"PMX モデル (*.pmx)\0*.pmx\0",Own(m).modelPath);
            if(path.empty()||!ModelUsable(path))break;
            CharacterSettings added;
            added.modelPath=path;
            app.settings.characters.push_back(std::move(added));
            app.changes.push_back({Change::Load,app.settings.characters.size()-1});
            break;
        }
        case IdRemoveCharacter:
            if(app.mascots.size()>1)app.changes.push_back({Change::Remove,m.slot});
            break;
        case IdOpenMotion:
        {
            auto path=OpenFileDialog(m.hwnd,L"VMD モーション (*.vmd)\0*.vmd\0",Own(m).motionPath.empty()?Own(m).modelPath:Own(m).motionPath);
            if(path.empty())break;
            std::wstring error;
            if(!m.model.SetMotion(path,error))
            {
                ShowMessage(error,MB_ICONERROR);
                break;
            }
            Own(m).motionPath=path;
            app.settings.Save(app.settingsPath);
            m.motionRequested=true;   // play it once now
            m.motionClosePending=false;
            break;
        }
        case IdCloseMotion:
            // Blend out first if it is playing; dropped once it has.
            Own(m).motionPath.clear();
            app.settings.Save(app.settingsPath);
            m.motionRequested=false;
            if(m.model.MotionPlaying()){m.model.StopMotion();m.motionClosePending=true;}
            else {std::wstring error;m.model.SetMotion({},error);}
            break;
        case IdOpenWalkMotion:
        case IdOpenRunMotion:
        {
            // A walk or run in place (the kind moved by 全ての親) replaces the generated gait.
            const int which=id==IdOpenWalkMotion?0:1;
            std::wstring& setting=which==0?Own(m).walkMotionPath:Own(m).runMotionPath;
            const auto path=OpenFileDialog(m.hwnd,L"VMD モーション (*.vmd)\0*.vmd\0",setting.empty()?Own(m).modelPath:setting);
            if(path.empty())break;
            if(std::wstring error;!m.model.SetGaitMotion(which,path,error)){ShowMessage(error,MB_ICONERROR);break;}
            setting=path;
            app.settings.Save(app.settingsPath);
            break;
        }
        case IdCloseGaitMotion:
        {
            std::wstring error;
            m.model.SetGaitMotion(0,{},error);
            m.model.SetGaitMotion(1,{},error);
            Own(m).walkMotionPath.clear();
            Own(m).runMotionPath.clear();
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
                const auto& list=Own(m).clickMotionPaths;
                paths=OpenFilesDialog(m.hwnd,L"VMD モーション (*.vmd)\0*.vmd\0",list.empty()?Own(m).modelPath:list.back());
                if(paths.empty())break;
            }
            if(std::wstring error;!m.model.SetClickMotions(paths,error))
            {
                ShowMessage(error,MB_ICONERROR);
                break;
            }
            Own(m).clickMotionPaths=paths;
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
                const auto& list=Own(m).idleMotionPaths;
                auto picked=OpenFilesDialog(m.hwnd,L"VMD モーション (*.vmd)\0*.vmd\0",list.empty()?Own(m).modelPath:list.back());
                if(picked.empty())break;
                if(id==IdAddIdleMotion)paths=list;
                for(auto& p:picked)
                    if(std::find(paths.begin(),paths.end(),p)==paths.end())paths.push_back(std::move(p));
            }
            if(std::wstring error;!m.model.SetIdleMotions(paths,error))
            {
                ShowMessage(error,MB_ICONERROR);
                break;
            }
            Own(m).idleMotionPaths=paths;
            app.settings.Save(app.settingsPath);
            break;
        }
        case IdWindowMode:
            SetWindowMode(!app.settings.windowMode);
            app.settings.Save(app.settingsPath);
            break;
        case IdWalkLeft:
        case IdWalkRight:
            if(m.place==Place::Ground)
            {
                m.model.StopMotion();
                StartWalk(m,m.windowX+m.windowSize.cx*0.5+(id==IdWalkLeft?-300:300));
            }
            break;
        case IdSettings:
            ShowSettingsWindow(m.hwnd,{app.settings,StartupRegistered(),Own(m),m.character,&m.memory},
                               [&m](const SettingsChoice& choice){ApplySettings(m,choice);},
                               [](int azimuth,int elevation){
                                   for(const auto& each:app.mascots)each->renderer.SetLight(static_cast<float>(azimuth),static_cast<float>(elevation));});
            break;
        case IdExit:
            DestroyWindow(app.hwnd);
            break;
        }
    }

    // A character's window: the Mascot is its user data (set from CreateWindowEx's parameter,
    // cleared just before the window goes).
    LRESULT CALLBACK WindowProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
    {
        if(message==WM_NCCREATE)
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        auto* mascot=reinterpret_cast<Mascot*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(!mascot)return DefWindowProcW(hwnd,message,wParam,lParam);
        Mascot& m=*mascot;
        switch(message)
        {
        case WM_LBUTTONDOWN:
        {
            // A press only becomes a grab once the cursor moves a little.
            GetCursorPos(&m.pressCursor);
            m.dragOffset={static_cast<LONG>(m.pressCursor.x-m.windowX),static_cast<LONG>(m.pressCursor.y-m.windowY)};
            m.pressed=true;
            SetCapture(hwnd);
            return 0;
        }
        case WM_MOUSEMOVE:
            // The stage is large: let clicks through at once when the cursor leaves the frame,
            // without waiting for the next alpha sample.
            if(!m.pressed&&m.place!=Place::Held)
            {
                POINT cursor{};
                GetCursorPos(&cursor);
                const RECT frame=FrameRect(m);
                if(!PtInRect(&frame,cursor))SetClickThrough(m,true);
            }
            if(m.pressed||m.place==Place::Held)
            {
                POINT cursor{};
                GetCursorPos(&cursor);
                if(m.place!=Place::Held&&(std::abs(cursor.x-m.pressCursor.x)>DragThreshold||std::abs(cursor.y-m.pressCursor.y)>DragThreshold))
                {
                    m.place=Place::Held;
                    m.pressed=false;
                    // Lifted off whatever it stood on; any trip in progress is abandoned.
                    m.surfaceWindow=nullptr;
                    m.travel=Travel::None;
                    m.restPoseAt=-1;
                    m.renderer.SetModelTransform(DirectX::XMMatrixIdentity());
                    m.previousWindowX=m.windowX;m.previousWindowY=m.windowY;
                    m.carryVelocity=m.carryAcceleration={};
                    m.model.StopMotion();
                    m.model.SetActivity(Activity::Grabbed);
                    // Picked up: over the other characters.
                    if(app.settings.topmost)SetWindowPos(m.hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
                }
                if(m.place==Place::Held)MoveWindowTo(m,cursor.x-m.dragOffset.x,cursor.y-m.dragOffset.y);
            }
            return 0;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if(m.place==Place::Held)
            {
                // Let go: fall to the ground line (or settle at once if already there or below).
                m.place=Place::Falling;
                m.fallSpeed=std::max(0.0f,m.carryVelocity.y*0.5f);
                m.model.SetActivity(Activity::Fall);
            }
            if(m.pressed||message==WM_LBUTTONUP)
            {
                if(m.pressed&&message==WM_LBUTTONUP&&m.place!=Place::Held)
                {
                    m.pendingClick=m.pressCursor;
                    m.pendingClickAt=app.clock+GetDoubleClickTime()/1000.0;
                }
                m.pressed=false;
                if(GetCapture()==hwnd)ReleaseCapture();
            }
            return 0;
        case WM_LBUTTONDBLCLK:
            // Double-click: talk to her (the first click's reaction is called off).
            m.pendingClick.reset();
            OpenChatInput(m);
            return 0;
        case WM_MOUSEACTIVATE:
            // Hanging behind a window: a click must not raise the mascot over it.
            if(m.behindSurface)return MA_NOACTIVATE;
            break;
        case WM_MOUSEWHEEL:
        {
            // Over the mascot (clicks elsewhere pass through, so the wheel does too): one notch
            // is one 10% size step, up to grow. Smooth-scrolling wheels send smaller deltas.
            if(m.place==Place::Held)return 0;
            m.wheelDelta+=GET_WHEEL_DELTA_WPARAM(wParam);
            int steps=0;
            while(m.wheelDelta>=WHEEL_DELTA){m.wheelDelta-=WHEEL_DELTA;++steps;}
            while(m.wheelDelta<=-WHEEL_DELTA){m.wheelDelta+=WHEEL_DELTA;--steps;}
            if(steps!=0)
            {
                const int percent=std::clamp(Own(m).scale+steps*10,Settings::MinScale,Settings::MaxScale);
                if(percent!=Own(m).scale)ApplyScale(m,percent);
            }
            return 0;
        }
        case WM_RBUTTONUP:
        {
            POINT p{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd,&p);
            ShowMenu(m,p);
            return 0;
        }
        case LocalChat::TextMessage:
            if(m.memoryRequest&&static_cast<int>(lParam)==m.memoryRequest)
            {
                std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(wParam));
                if(m.memoryReply.size()<8000)m.memoryReply+=*text;
                return 0;
            }
            OnChatText(m,reinterpret_cast<std::wstring*>(wParam),static_cast<int>(lParam));
            return 0;
        case VoiceEngine::DoneMessage:
            Trace(L"%.1f voice: %d %s",app.clock,static_cast<int>(lParam),wParam?L"made":L"failed");
            m.speech.Voiced(static_cast<int>(lParam),reinterpret_cast<VoiceClip*>(wParam));
            return 0;
        case LocalChat::DoneMessage:
            if(m.memoryRequest&&static_cast<int>(lParam)==m.memoryRequest)
            {
                std::unique_ptr<std::wstring> error(reinterpret_cast<std::wstring*>(wParam));
                m.memoryRequest=0;
                OnMemoryDone(m,*error);
                return 0;
            }
            OnChatDone(m,reinterpret_cast<std::wstring*>(wParam),static_cast<int>(lParam));
            return 0;
        case WM_COMMAND:
            OnCommand(m,LOWORD(wParam));
            return 0;
        }
        return DefWindowProcW(hwnd,message,wParam,lParam);
    }

    // Takes a character off the desktop: what it was told since its last notes is kept for the
    // next start to look through, and its window goes.
    void Retire(Mascot& m)
    {
        if(app.settings.chatMemory)
        {
            std::vector<std::pair<bool,std::wstring>> pending;
            for(const auto& said:m.unmemorized)pending.push_back({said.role==L"user",said.content});
            SavePendingExchanges(m.modelPath,pending);
        }
        m.chat.reset();
        m.memoryChat.reset();
        m.speech.SetVoice(nullptr);
        m.voice.reset();
        m.chatInput.Close();
        m.bubble.Hide();
        m.renderer.WaitIdle();
        if(m.hwnd)
        {
            SetWindowLongPtrW(m.hwnd,GWLP_USERDATA,0);
            DestroyWindow(m.hwnd);
            m.hwnd=nullptr;
        }
    }

    // The hidden control window: the tray icon (it stands for the first character) and quitting.
    LRESULT CALLBACK ControlProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
    {
        switch(message)
        {
        case TrayMessage:
            if(app.mascots.empty())return 0;
            if(LOWORD(lParam)==WM_LBUTTONDBLCLK)OpenChatInput(*app.mascots.front());
            if(LOWORD(lParam)==WM_RBUTTONUP||LOWORD(lParam)==WM_CONTEXTMENU)
            {
                POINT p{};
                GetCursorPos(&p);
                ShowMenu(*app.mascots.front(),p);
            }
            return 0;
        case WM_DESTROY:
            for(const auto& m:app.mascots)
            {
                Own(*m).x=static_cast<int>(m->windowX);
                Retire(*m);
            }
            if(!app.diagnostic)app.settings.Save(app.settingsPath);
            Shell_NotifyIconW(NIM_DELETE,&app.tray);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd,message,wParam,lParam);
    }

    // Brings the character in settings.characters[slot] onto the desktop: its model and
    // motions, its window and renderer, its persona and memory. At start-up, when one is added,
    // and when its model changes (the window size follows the model's proportions): the
    // character in that slot, if any, is replaced, and left as it was on failure.
    bool CreateMascot(size_t slot,std::wstring& error)
    {
        CharacterSettings& own=app.settings.characters[slot];
        auto mascot=std::make_unique<Mascot>();
        Mascot& m=*mascot;
        m.slot=slot;
        m.modelPath=own.modelPath;
        m.model.SetFrameRate(app.settings.fps);
        m.model.SetPhysicsIterations(app.settings.physicsIterations);
        // A missing idle motion is dropped rather than blocking the model.
        std::erase_if(own.idleMotionPaths,[](const std::wstring& p){return !std::filesystem::exists(p);});
        if(!m.model.Load(own.modelPath,own.idleMotionPaths,app.settings.physics,app.settings.physicsFps,error))
        {
            // So is one that does not load: the model comes up on the built-in idle instead.
            std::wstring plainError;
            if(own.idleMotionPaths.empty()||!m.model.Load(own.modelPath,{},app.settings.physics,app.settings.physicsFps,plainError))return false;
            if(!app.diagnostic)ShowMessage(L"待機モーションを読み込めなかったため、外しました。\n\n"+error,MB_ICONWARNING);
            own.idleMotionPaths.clear();
            error.clear();
        }

        m.extent=BaseExtent(m);
        const SIZE size=WindowSizeFor(m,own.scale);
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
        // Placed where it was; a new one at the bottom right, to the left of those before it.
        int x=own.x,y=work.bottom-size.cy;
        if(x==-1)x=work.right-size.cx-static_cast<int>(slot*ModelFrameOf(m).width*1.6f*m.pixelsPerUnit);
        // Keep the window reachable if the saved position is off every monitor.
        RECT wanted{x,y,x+size.cx,y+size.cy};
        if(!MonitorFromRect(&wanted,MONITOR_DEFAULTTONULL))x=work.right-size.cx;
        m.windowX=x;m.windowY=y;m.windowSize=size;

        // NOREDIRECTIONBITMAP + DirectComposition gives per-pixel alpha;
        // LAYERED is only there so WS_EX_TRANSPARENT can pass clicks through.
        const DWORD exStyle=WS_EX_NOREDIRECTIONBITMAP|WS_EX_LAYERED|WS_EX_TOOLWINDOW|(app.settings.topmost?WS_EX_TOPMOST:0);
        m.stage=WorkAreaAt({x+size.cx/2,y+size.cy/2});
        const RECT& st=m.stage;
        m.hwnd=CreateWindowExW(exStyle,WindowClass,L"MikuMikuDesktopPerch",WS_POPUP,st.left,st.top,st.right-st.left,st.bottom-st.top,
                               nullptr,nullptr,GetModuleHandleW(nullptr),&m);
        if(!m.hwnd){error=L"ウィンドウを作成できませんでした。";return false;}
        // Alpha 254, not 255: Chromium-based apps treat an opaque window covering theirs as occluding
        // and stop painting; a not-quite-opaque layered window is ignored (and looks the same).
        SetLayeredWindowAttributes(m.hwnd,0,254,LWA_ALPHA);

        m.renderer.SetFrameRate(app.settings.fps);
        if(!m.renderer.Initialize(m.hwnd,size.cx,size.cy,app.settings.msaa,app.settings.motionBlur,error)||
           !m.renderer.LoadModel(m.model,app.settings.toonDir,static_cast<unsigned>(app.settings.maxTextureSize),error))
        {
            Retire(m);
            return false;
        }
        ApplyProceduralSettings(m);
        UpdateCamera(m);
        MoveWindowTo(m,m.windowX,GroundedWindowY(m));
        m.nextDecision=app.clock+8+app.Random01()*10;
        m.nextMotionAt=app.clock+60+app.Random01()*120;
        m.foregroundSince=app.clock;
        if(std::wstring motionError;!m.model.SetMotion(own.motionPath,motionError))own.motionPath.clear();
        std::erase_if(own.clickMotionPaths,[](const std::wstring& p){return !std::filesystem::exists(p);});
        if(std::wstring clickError;!m.model.SetClickMotions(own.clickMotionPaths,clickError))own.clickMotionPaths.clear();
        for(int which=0;which<2;++which)
        {
            std::wstring& path=which==0?own.walkMotionPath:own.runMotionPath;
            if(std::wstring gaitError;!path.empty()&&(!std::filesystem::exists(path)||!m.model.SetGaitMotion(which,path,gaitError)))path.clear();
        }

        m.bubble.Create(m.hwnd);
        m.character=CharacterProfile::Load(own.modelPath,m.model.ModelName());
        m.memory.Load(own.modelPath);
        for(const auto& [user,text]:TakePendingExchanges(own.modelPath))
            m.unmemorized.push_back({user?L"user":L"assistant",text});
        ApplyVoiceSettings(m);
        ShowWindow(m.hwnd,app.paused?SW_HIDE:SW_SHOWNOACTIVATE);

        if(slot<app.mascots.size())
        {
            Retire(*app.mascots[slot]);
            app.mascots[slot]=std::move(mascot);
        }
        else app.mascots.push_back(std::move(mascot));
        Trace(L"%.1f character %zu: %s",app.clock,slot,own.modelPath.c_str());
        return true;
    }

    // Between frames: bring on and take off the characters asked for.
    void ApplyChanges()
    {
        for(const Change& change:std::exchange(app.changes,{}))
        {
            if(change.kind==Change::Remove)
            {
                if(change.slot>=app.mascots.size()||app.mascots.size()<=1)continue;
                Retire(*app.mascots[change.slot]);
                app.mascots.erase(app.mascots.begin()+static_cast<std::ptrdiff_t>(change.slot));
                app.settings.characters.erase(app.settings.characters.begin()+static_cast<std::ptrdiff_t>(change.slot));
                for(size_t i=0;i<app.mascots.size();++i)app.mascots[i]->slot=i;
            }
            else if(std::wstring error;change.slot<app.settings.characters.size()&&!CreateMascot(change.slot,error))
            {
                ShowMessage(error,MB_ICONERROR);
                // A new character that did not come up is dropped; a changed model goes back.
                if(change.slot>=app.mascots.size())app.settings.characters.erase(app.settings.characters.begin()+static_cast<std::ptrdiff_t>(change.slot));
                else app.settings.characters[change.slot].modelPath=change.previousModel;
            }
            if(!app.diagnostic)app.settings.Save(app.settingsPath);
        }
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
    std::optional<int> snapshotFps;           // --fps <n>: drawing rate for this run
    std::optional<bool> snapshotBlur;         // --motion-blur 0|1
    std::optional<int> snapshotIterations;    // --physics-iterations <n>
    std::optional<int> snapshotFilter;        // --physics-filter 0-3
    bool snapshotPlayMotion=false;            // --play-motion: play the opened motion at once
    int snapshotWalk=0;                       // --walk <px>: walk that far 3 s after start
    int snapshotClick=0;                      // --click head|body: 1 head, 2 body, 1.5 s after start
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
                snapshotPlayMotion=true;
            }
            else if(arg==L"--walk"&&i+1<argc)
            {
                snapshotWalk=_wtoi(argv[++i]);
            }
            else if(arg==L"--click"&&i+1<argc)
            {
                snapshotClick=std::wstring(argv[++i])==L"head"?1:2;
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
            else if(arg==L"--motion-blur"&&i+1<argc)
            {
                snapshotBlur=_wtoi(argv[++i])!=0;
            }
            else if(arg==L"--physics-iterations"&&i+1<argc)
            {
                snapshotIterations=std::clamp(_wtoi(argv[++i]),1,100);
            }
            else if(arg==L"--physics-filter"&&i+1<argc)
            {
                snapshotFilter=std::clamp(_wtoi(argv[++i]),0,3);
            }
            else if(arg==L"--fps"&&i+1<argc)
            {
                snapshotFps=std::clamp(_wtoi(argv[++i]),5,120);
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
    app.diagnostic=!snapshotPath.empty();
    if(app.diagnostic)
    {
        // Diagnostic runs must not overwrite the user's settings, and show the first character only.
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH,temp);
        app.settingsPath=std::wstring(temp)+L"MikuMikuDesktopPerch_snapshot.ini";
        app.settings.characters.resize(1);
        if(!snapshotModel.empty())app.settings.characters[0].modelPath=snapshotModel;
        if(snapshotFps)app.settings.fps=*snapshotFps;
        if(snapshotBlur)app.settings.motionBlur=*snapshotBlur;
        if(snapshotIterations)app.settings.physicsIterations=*snapshotIterations;
        if(snapshotFilter)app.settings.physicsFilter=*snapshotFilter;
        app.settings.characters[0].x=-1;
        app.settings.autoBehavior=false;app.settings.windowMode=false;app.settings.chatInitiative=false;
    }

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
    WNDCLASSEXW control=wc;
    control.lpfnWndProc=ControlProc;
    control.style=0;
    control.lpszClassName=ControlClass;
    RegisterClassExW(&control);
    // Never shown: it only holds the tray icon and outlives the characters' windows.
    app.hwnd=CreateWindowExW(WS_EX_TOOLWINDOW,ControlClass,L"MikuMikuDesktopPerch",WS_POPUP,0,0,0,0,nullptr,nullptr,instance,nullptr);
    if(!app.hwnd)return 1;

    // Keep asking for the first character's model until one loads (or the user cancels).
    std::wstring error;
    for(;;)
    {
        std::wstring& modelPath=app.settings.characters[0].modelPath;
        if(modelPath.empty()||!std::filesystem::exists(modelPath))
        {
            modelPath=OpenFileDialog(nullptr,L"PMX モデル (*.pmx)\0*.pmx\0",{});
            if(modelPath.empty())return 0;
        }
        if(CreateMascot(0,error))break;
        if(app.diagnostic)
        {
            // Diagnostic runs report instead of blocking on a dialog.
            if(FILE* info=nullptr;_wfopen_s(&info,(snapshotPath+L".txt").c_str(),L"w, ccs=UTF-8")==0&&info)
            {fwprintf(info,L"error=%s\n",error.c_str());fclose(info);}
            return 2;
        }
        MessageBoxW(nullptr,error.c_str(),L"MikuMikuDesktopPerch",MB_ICONERROR);
        modelPath.clear();
    }
    // The others: one that does not come up is taken off, saying why.
    for(size_t slot=1;slot<app.settings.characters.size();)
    {
        if(CreateMascot(slot,error)){++slot;continue;}
        ShowMessage(L"「"+app.settings.characters[slot].modelPath+L"」を読み込めなかったため、外しました。\n\n"+error,MB_ICONWARNING);
        app.settings.characters.erase(app.settings.characters.begin()+static_cast<std::ptrdiff_t>(slot));
    }
    app.settings.Save(app.settingsPath);

    // Diagnostics act on the first character.
    Mascot& first=*app.mascots.front();
    std::optional<Activity> forcedActivity;   // --activity for snapshot checks
    if(snapshotActivity==L"walk")forcedActivity=Activity::Walk;
    else if(snapshotActivity==L"run")forcedActivity=Activity::Run;
    else if(snapshotActivity==L"jump")forcedActivity=Activity::Jump;
    else if(snapshotActivity==L"lie")forcedActivity=Activity::Lie;
    else if(snapshotActivity==L"hang")forcedActivity=Activity::Hang;
    else if(snapshotActivity==L"sit")forcedActivity=Activity::Sit;
    else if(snapshotActivity==L"grab")forcedActivity=Activity::Grabbed;
    else if(snapshotActivity==L"fall")forcedActivity=Activity::Fall;
    if(forcedActivity)first.model.SetActivity(*forcedActivity,1);
    if(!snapshotGesture.empty())first.model.ForceGesture(snapshotGesture);
    if(snapshotDoze){first.dozing=first.forcedDoze=true;first.model.SetDozing(true);}
    first.motionRequested=snapshotPlayMotion;
    first.diagnosticWalk=snapshotWalk;
    first.diagnosticClick=snapshotClick;
    for(int which=0;which<2;++which)
        if(std::wstring gaitError;!snapshotGait[which].empty()&&!first.model.SetGaitMotion(which,snapshotGait[which],gaitError))
            Trace(L"gait motion: %s",gaitError.c_str());

    app.tray.cbSize=sizeof(app.tray);
    app.tray.hWnd=app.hwnd;
    app.tray.uID=1;
    app.tray.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;
    app.tray.uCallbackMessage=TrayMessage;
    app.tray.hIcon=wc.hIconSm?wc.hIconSm:wc.hIcon;
    wcscpy_s(app.tray.szTip,L"MikuMikuDesktopPerch");
    Shell_NotifyIconW(NIM_ADD,&app.tray);

    // Event-driven: called on the message loop whenever another window takes the foreground.
    HWINEVENTHOOK foregroundHook=SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,nullptr,OnForegroundChanged,
                                                 0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
    if(snapshotScale>0)ApplyScale(first,std::clamp(snapshotScale/10*10,Settings::MinScale,Settings::MaxScale));

    // Frame pacing: a periodic high-resolution timer wakes the loop; messages wake it too.
    HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
    if(!timer)timer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
    LARGE_INTEGER due{};
    due.QuadPart=-1;
    SetWaitableTimer(timer,&due,std::max(1,1000/app.settings.fps),nullptr,nullptr,FALSE);

    LARGE_INTEGER frequency{},last{},lastFullscreenCheck{};
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
        if(!running)continue;
        if(!app.changes.empty())
        {
            ApplyChanges();
            QueryPerformanceCounter(&last);   // loading a model is not time that passed for the others
        }
        if(wait!=WAIT_OBJECT_0||app.mascots.empty())continue;

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if(Seconds(lastFullscreenCheck,now,frequency)>2.0)
        {
            lastFullscreenCheck=now;
            const bool fullscreen=IsFullscreenAppActive();
            if(fullscreen!=app.paused)
            {
                app.paused=fullscreen;
                for(const auto& m:app.mascots)ShowWindow(m->hwnd,fullscreen?SW_HIDE:SW_SHOWNOACTIVATE);
            }
        }
        const double dt=Seconds(last,now,frequency);
        last=now;
        if(app.paused)continue;

        app.clock+=dt;
        if(!forcedActivity)MaybeSpeakFirst();
        // The taskbar can also end up above the characters without taking the foreground.
        if((app.topmostCheck+=dt)>=2.0){app.topmostCheck=0;ReassertTopmost();}
        for(const auto& owned:app.mascots)
        {
            Mascot& m=*owned;
            const float ppu=m.pixelsPerUnit;

            // Window motion while held or falling: velocity for the dangling sway, acceleration
            // as inertia for the physics (free fall makes hair and skirt float).
            if(m.place==Place::Falling)
            {
                // Land on the ground line, or (in active-window mode) on the foreground window's
                // top edge when it lies below the feet within reach.
                HWND landing=nullptr;
                double line=WorkAreaOfWindow(m).bottom;
                const double feet=FeetLine(m);
                if(app.settings.windowMode&&IsStandableWindow(GetForegroundWindow(),m.hwnd,0))
                {
                    RECT frame{};
                    VisibleFrame(GetForegroundWindow(),frame);
                    if(CenterX(m)>=frame.left&&CenterX(m)<=frame.right&&frame.top>=feet-1&&frame.top<line&&
                       frame.top-ModelHeightPixels(m)>=WorkAreaOfWindow(m).top+8)
                    {landing=GetForegroundWindow();line=frame.top;}
                }
                const double target=WindowYForLine(m,line);
                m.fallSpeed+=FlightGravity*ppu*dt;
                double y=m.windowY+m.fallSpeed*dt;
                if(y>=target)
                {
                    y=target;
                    MoveWindowTo(m,m.windowX,y);
                    Land(m,landing,m.fallSpeed);
                    m.servedForeground=landing?landing:m.servedForeground;
                }
                else MoveWindowTo(m,m.windowX,y);
            }
            else if(m.place==Place::Warping)UpdateWarp(m,dt);
            else if(m.place==Place::Jumping)
            {
                auto& j=m.jump;
                j.time=std::min(j.time+dt,j.duration);
                const double g=FlightGravity*ppu;
                const double x=j.startX+j.vx*j.time,line=j.startLine+j.vy*j.time+0.5*g*j.time*j.time;
                MoveWindowTo(m,x-m.windowSize.cx*0.5,WindowYForLine(m,line));
                if(j.time>=j.duration)Land(m,m.destinationWindow,j.vy+g*j.duration);
            }
            if(m.place!=Place::Ground&&m.place!=Place::Warping&&dt>0)
            {
                const float vx=static_cast<float>((m.windowX-m.previousWindowX)/dt),vy=static_cast<float>((m.windowY-m.previousWindowY)/dt);
                const float k=static_cast<float>(1.0-std::exp(-dt/0.08));
                const DirectX::XMFLOAT2 previous=m.carryVelocity;
                m.carryVelocity.x+=(vx-m.carryVelocity.x)*k;
                m.carryVelocity.y+=(vy-m.carryVelocity.y)*k;
                m.carryAcceleration={static_cast<float>((m.carryVelocity.x-previous.x)/dt),static_cast<float>((m.carryVelocity.y-previous.y)/dt)};
                // Ballistic flight: the body falls with the window, so it feels weightless (the
                // physics world's own gravity is cancelled exactly).
                if(m.place==Place::Falling||m.place==Place::Jumping)m.carryAcceleration={0,static_cast<float>(Gravity*ppu)};
                // Screen y grows downward, world y upward.
                m.model.SetCarryMotion(m.carryVelocity.x/ppu,-m.carryVelocity.y/ppu,m.carryAcceleration.x/ppu,-m.carryAcceleration.y/ppu);
            }
            else m.model.SetCarryMotion(0,0,0,0);
            m.previousWindowX=m.windowX;m.previousWindowY=m.windowY;

            if(m.place==Place::Ground&&!forcedActivity)
            {
                // Walking (or running on the same surface): stop at the target or the surface's end.
                const Activity a=m.model.CurrentActivity();
                if(m.travel==Travel::None&&(a==Activity::Walk||a==Activity::Run))
                {
                    double left,right;
                    SurfaceRange(m,left,right);
                    const double centerX=CenterX(m),margin=a==Activity::Run?16:6;
                    const bool reached=std::abs(centerX-m.walkTargetX)<margin||centerX<left-1||centerX>right+1;
                    if(reached)StopGroundActivity(m);
                }
                UpdateAutoBehavior(m);
                MaybeMemorize(m);
                UpdateDozing(m);
                UpdateFeeling(m);
                if(m.pendingClick&&app.clock>=m.pendingClickAt)
                {
                    OnClick(m,*m.pendingClick);
                    m.pendingClick.reset();
                }
                if(m.diagnosticClick&&app.clock>1.5)
                {
                    const bool reacted=m.model.React(m.diagnosticClick==1);
                    Trace(L"%.1f diagnostic click: %s (activity %d)",app.clock,reacted?L"reacts":L"no reaction",static_cast<int>(m.model.CurrentActivity()));
                    m.diagnosticClick=0;
                }
                if(m.diagnosticWalk&&app.clock>3.0)
                {
                    StartWalk(m,CenterX(m)+m.diagnosticWalk);
                    m.diagnosticWalk=0;
                }
            }

            // Click-through wherever the mascot is not drawn.
            POINT cursor{};
            GetCursorPos(&cursor);
            const RECT r=FrameRect(m);
            if(m.place!=Place::Held)
            {
                if(PtInRect(&r,cursor))
                {
                    const int alpha=m.renderer.AlphaAt({cursor.x-r.left,cursor.y-r.top});
                    SetClickThrough(m,alpha>=0&&alpha<16);
                }
                else SetClickThrough(m,true);
            }
            if(!forcedActivity)
            {
                UpdateMotionPlayback(m);
                UpdateWindowMode(m,dt);
                AvoidCursor(m,cursor);
                UpdateHangOrder(m);
            }
            if(snapshotLook)cursor={r.left+snapshotLook->x,r.top+snapshotLook->y};
            UpdateLookTarget(m,cursor,r,dt);

            m.model.Update(dt);
            // センター's walking travel moves the window by the same amount; the camera follows it.
            if(const float travel=m.model.TakeRootDelta();travel!=0.0f)
            {
                UpdateCamera(m);
                if(!forcedActivity)MoveWindowTo(m,m.windowX+travel*ppu,m.windowY);
            }
            if(m.place==Place::Ground&&std::abs(m.windowY-GroundedWindowY(m))>0.5)MoveWindowTo(m,m.windowX,GroundedWindowY(m));
            // After the walking step above: resizing measures where the feet are on screen, which
            // must already include this frame's step or the step is applied twice.
            UpdateExtent(m,dt);
            if(const std::uint32_t resets=m.model.PhysicsResets();resets!=m.lastPhysicsResets)
            {
                Trace(L"%.1f physics reset (%u so far) %s",app.clock,resets,m.model.PhysicsResetReason().c_str());
                m.lastPhysicsResets=resets;
            }
            UpdateSparks(m,dt);
            PlaceFrame(m);
            UpdateChatBubble(m,dt);
            m.renderer.Render(m.model,app.settings.edgeScale);
        }
        if(app.diagnostic&&--snapshotFrames<=0)
        {
            Mascot& m=*app.mascots.front();
            m.renderer.SaveLastFrame(snapshotPath);
            if(FILE* info=nullptr;_wfopen_s(&info,(snapshotPath+L".txt").c_str(),L"w, ccs=UTF-8")==0&&info)
            {
                const auto rest=m.model.ArmRestAngles(),fix=m.model.ArmCorrections();
                fwprintf(info,L"model=%s\narmRest L=%.1f R=%.1f\narmCorrection L=%.1f R=%.1f\nwindow=%dx%d\n",
                         Own(m).modelPath.c_str(),rest[0],rest[1],fix[0],fix[1],m.windowSize.cx,m.windowSize.cy);
                fwprintf(info,L"%s",m.model.DescribeRig().c_str());
                fclose(info);
            }
            DestroyWindow(app.hwnd);
        }
    }
    if(foregroundHook)UnhookWinEvent(foregroundHook);
    CloseHandle(timer);
    app.mascots.clear();

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
