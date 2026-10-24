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
#include "Calendar.hpp"
#include "Planner.hpp"
#include "PlannerWindow.hpp"
#include "Updater.hpp"
#include "Text.hpp"
#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
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
#include <set>
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
                      IdOpenWalkMotion,IdOpenRunMotion,IdCloseGaitMotion,IdAddCharacter,IdRemoveCharacter,IdPlanner,IdCancelTimers,IdFocus};
    constexpr UINT IdTimerMinutes=300;      // + minutes: the menu's quick timers
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
        double roomCheckAt=0;         // next look at whether she stands on top of another character
        // Talking with another character: the request now running is a line of that conversation.
        bool banterLine=false;
        // A timer, reminder or to-do request the words alone did not make clear, being read by
        // the LLM (planSaid: what the user said).
        std::unique_ptr<LocalChat> planChat;
        int planRequest=0;
        std::wstring planReply,planSaid;
        // What she says instead if the LLM cannot put it in her own words (a request done, an
        // alarm): the news must reach the user either way.
        std::wstring fallbackLine;
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
        double nextChatterAt=-1;   // set from the chosen interval at start
        double awaySince=-1;
        Activity forcedRestPose=Activity::Idle;   // --rest-pose sit|hang (diagnostics)
        std::vector<std::unique_ptr<Mascot>> mascots;   // in the order of settings.characters
        std::vector<Change> changes;
        // Two characters talking with each other (see UpdateBanter): the second comes over to
        // the first, then they take turns, one line each, the first starting.
        struct Banter
        {
            enum class Stage {None,Gathering,Talking} stage=Stage::None;
            Mascot* first=nullptr;
            Mascot* second=nullptr;
            Mascot* speaker=nullptr;   // whose line is being said, or comes next
            bool lineStarted=false;
            int linesLeft=0;
            std::wstring topic;
            std::vector<std::pair<std::wstring,std::wstring>> lines;   // who said it, the line
            double since=0,nextLineAt=0;
        } banter;
        double nextBanterAt=-1;       // set from the chosen interval at start
        bool banterRequested=false;   // --banter: one a few seconds after start (diagnostics)
        // Updates (with UpdateCheck on): a newer release found and not yet answered, the one
        // asking the user about it (she stays put until answered), and the answer box.
        std::unique_ptr<Updater> updater;
        double nextUpdateCheck=20;
        std::optional<Release> update;
        std::wstring declinedVersion;   // answered いいえ: not asked again this run
        Mascot* asker=nullptr;
        ChoiceBox updateChoice;
        bool updating=false;
        // Timers, reminders and to-dos: those come due and not yet told, the one telling them
        // (she stays put until answered) with わかった／5分後に under her.
        Planner planner;
        std::vector<Alarm> ringing,told;
        double ringingSince=-1,nextPlannerCheck=0;
        Mascot* alarmTeller=nullptr;
        ChoiceBox alarmChoice;
        // Focus mode: work and breaks in turn (rounds: works finished; phaseEnd in Now() time).
        struct Focus {bool on=false,resting=false;std::int64_t phaseEnd=0;int rounds=0;} focus;
        // What a character has to tell that is not an alarm (focus mode's turns, a break due):
        // told like one, without the answer box.
        struct Notice {std::wstring line,facts;};
        std::vector<Notice> notices;
        double noticesSince=-1;
        // Using the computer without a break (five minutes away counts as one).
        double activeSince=-1,nextRestAt=-1;
        // Google Calendar (with its address set): today's and the next two days' events, read
        // every half hour, and those already told about before they start.
        std::unique_ptr<CalendarFeed> calendar;
        std::vector<CalendarEvent> events;
        std::set<std::wstring> eventsTold;
        double nextCalendarFetch=15;
        bool diagnostic=false;    // --snapshot: no dialogs, settings not saved over the user's
        double clock=0,topmostCheck=0;
        std::uint32_t random=0x2545F491u;
        float Random01(){random^=random<<13;random^=random>>17;random^=random<<5;return (random&0xFFFFFF)/16777216.0f;}
    } app;

    CharacterSettings& Own(const Mascot& m){return app.settings.characters[m.slot];}

    // Now and then, about every so many minutes: a third sooner or later at random.
    double Interval(int minutes){return minutes*60.0*(2.0/3.0+app.Random01()*2.0/3.0);}

    // Taking part in a conversation between characters (getting together for it, or talking).
    bool InBanter(const Mascot& m)
    {
        return app.banter.stage!=App::Banter::Stage::None&&(&m==app.banter.first||&m==app.banter.second);
    }

    // The one she is talking with, while they talk.
    Mascot* BanterPartner(const Mascot& m)
    {
        const auto& b=app.banter;
        if(b.stage!=App::Banter::Stage::Talking)return nullptr;
        return &m==b.first?b.second:&m==b.second?b.first:nullptr;
    }

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
            WindowSizeFor(m,app.settings.scale);
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
            ApplyFrame(m,app.settings.scale,grown);
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
            if(m.extentCalm>=4.0){ApplyFrame(m,app.settings.scale,desired);m.extentCalm=0;}
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

    // Scale around the feet: each model's ground point stays where it is on screen. The size
    // is the same for every character (models of different heights keep their proportions).
    void ApplyScale(int percent)
    {
        const int previous=app.settings.scale;
        for(size_t i=0;i<app.mascots.size();++i)
            if(Mascot& m=*app.mascots[i];!ApplyFrame(m,percent,m.extent))
            {
                for(size_t j=0;j<i;++j)ApplyFrame(*app.mascots[j],previous,app.mascots[j]->extent);
                ShowMessage(T(L"ウィンドウの大きさを変更できませんでした。",L"Could not resize the window."),MB_ICONERROR);
                return;
            }
        app.settings.scale=percent;
        for(const auto& m:app.mascots)Own(*m).x=static_cast<int>(m->windowX);
        app.settings.Save(app.settingsPath);
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
        if(InBanter(m)||&m==app.asker||&m==app.alarmTeller)return true;   // staying for the conversation, or for an answer
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
        if(InBanter(m)&&!m.motionRequested)return;   // not in the middle of a conversation
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
            target=FreeSpot(m,std::clamp(target,left,right),FeetLine(m),left,right);
            Trace(L"%.1f auto walk to %.0f",app.clock,target);
            StartWalk(m,target);
        }
        m.nextDecision=app.clock+15+app.Random01()*25;
    }

    // Standing still on top of another character (dropped there, or both came to the same
    // place): step aside to free room on the same surface. When both could move, the one added
    // later does; one in the middle of something moves once it is done.
    void MakeRoom(Mascot& m)
    {
        if(app.clock<m.roomCheckAt)return;
        m.roomCheckAt=app.clock+0.5;
        auto still=[](Mascot& c){
            const Activity a=c.model.CurrentActivity();
            return c.place==Place::Ground&&c.travel==Travel::None&&a!=Activity::Walk&&a!=Activity::Run&&a!=Activity::Jump;
        };
        auto movable=[](Mascot& c){
            return c.place==Place::Ground&&c.travel==Travel::None&&c.model.CurrentActivity()==Activity::Idle&&
                   !MotionBusy(c)&&!c.dozing&&!c.chatInput.IsOpen();
        };
        if(!movable(m))return;
        const double line=FeetLine(m);
        for(const auto& owned:app.mascots)
        {
            Mascot& other=*owned;
            if(&other==&m||!still(other)||std::abs(FeetLine(other)-line)>4)continue;
            if(std::abs(CenterX(other)-CenterX(m))>=(ModelHalfWidthPixels(m)+ModelHalfWidthPixels(other))*0.6)continue;
            if(movable(other)&&other.slot>m.slot)continue;   // that one steps aside instead
            double left,right;
            SurfaceRange(m,left,right);
            const double target=FreeSpot(m,CenterX(m),line,left,right);
            if(std::abs(target-CenterX(m))<8)return;   // no room on this surface
            Trace(L"%.1f character %zu makes room: to %.0f",app.clock,m.slot,target);
            StartWalk(m,target,std::abs(target-CenterX(m))>ModelHeightPixels(m)?Activity::Run:Activity::Walk);
            if(m.surfaceWindow)m.restPoseAt=-2;   // rest again where the walk ends
            return;
        }
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
        Mascot* partner=BanterPartner(m);
        // The opened motion leads the head itself while it plays.
        if((!app.settings.lookAtMouse&&!atViewer&&!partner)||m.model.Dozing()||a==Activity::Walk||a==Activity::Fall||m.model.MotionPlaying()||m.model.HeadBusy()||!m.model.HeadPosition(head))
        {
            m.model.SetLookTarget(0,0,false);
            return;
        }
        const POINT h=m.renderer.Project(head);
        // Talking with another character: eye to eye. The body is turned toward her (see
        // UpdateFacing), so the head turns only part of the way sideways; up or down, neck and
        // head tilt toward her eyes from one's own, so a taller one looks down at a smaller.
        if(std::array<float,3> mine{},theirs{};partner&&m.model.EyePosition(mine)&&partner->model.EyePosition(theirs))
        {
            const POINT a=m.renderer.Project(mine),b=partner->renderer.Project(theirs);
            const RECT r=FrameRect(*partner);
            const float dx=static_cast<float>(r.left+b.x-(window.left+a.x)),dy=static_cast<float>(r.top+b.y-(window.top+a.y));
            const float pitch=std::clamp(std::atan2(-dy,std::max(std::abs(dx),1.0f)),DirectX::XMConvertToRadians(-30.0f),DirectX::XMConvertToRadians(30.0f));
            m.model.SetLookTarget(DirectX::XMConvertToRadians(dx>0?20.0f:-20.0f),pitch,true);
            return;
        }
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

    // Standing with a partner in a conversation, she turns in place a little toward her (hips,
    // upper body and feet: see MascotModel::SetTurn); otherwise she turns back.
    void UpdateFacing(Mascot& m)
    {
        float target=0;
        if(Mascot* partner=BanterPartner(m);partner&&m.place==Place::Ground)
            target=DirectX::XMConvertToRadians(CenterX(*partner)>CenterX(m)?-25.0f:25.0f);
        m.model.SetTurn(target);
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
    // The update question's answer box stays over the characters (topmost as well, they are
    // put back on top now and then).
    void RaiseAnswerBox()
    {
        for(HWND box:{app.updateChoice.Handle(),app.alarmChoice.Handle()})
            if(box)SetWindowPos(box,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    }

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
        RaiseAnswerBox();
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
        AppendMenuW(menu,MF_STRING,IdTalk,T(L"話しかける...",L"Talk..."));
        HMENU timers=CreatePopupMenu();
        for(const int minutes:{3,5,10,15,25,30,60})
            AppendMenuW(timers,MF_STRING,IdTimerMinutes+minutes,Fmt(T(L"{0}分",L"{0} minutes"),{std::to_wstring(minutes)}).c_str());
        AppendMenuW(timers,MF_SEPARATOR,0,nullptr);
        const bool timing=std::any_of(app.planner.Alarms().begin(),app.planner.Alarms().end(),[](const Alarm& a){return a.timerSeconds>0;});
        AppendMenuW(timers,MF_STRING|(timing?0:MF_GRAYED),IdCancelTimers,T(L"タイマーを止める",L"Stop timers"));
        AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(timers),T(L"タイマー",L"Timer"));
        AppendMenuW(menu,MF_STRING|(app.focus.on?MF_CHECKED:0),IdFocus,
                    Fmt(T(L"集中モード（作業{0}分・休憩{1}分）",L"Focus mode ({0} min work, {1} min break)"),
                        {std::to_wstring(app.settings.focusMinutes),std::to_wstring(app.settings.focusBreakMinutes)}).c_str());
        AppendMenuW(menu,MF_STRING,IdPlanner,T(L"予定とやること...",L"Reminders and to-dos..."));
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        // A character's window goes away when its model changes or it is taken off, and the
        // settings window belongs to one: not while that is open.
        const UINT settingsOpen=SettingsWindowHandle()?MF_GRAYED:0;
        AppendMenuW(menu,MF_STRING|settingsOpen,IdOpenModel,T(L"モデルを開く...",L"Open model..."));
        AppendMenuW(menu,MF_STRING|(app.mascots.size()>=Settings::MaxCharacters?MF_GRAYED:0),IdAddCharacter,
                    Fmt(T(L"キャラクターを追加...（{0}/{1}）",L"Add character... ({0}/{1})"),{std::to_wstring(app.mascots.size()),std::to_wstring(Settings::MaxCharacters)}).c_str());
        AppendMenuW(menu,MF_STRING|(app.mascots.size()<=1?MF_GRAYED:settingsOpen),IdRemoveCharacter,
                    Fmt(T(L"「{0}」を外す",L"Take {0} off"),{m.character.name}).c_str());
        HMENU motions=CreatePopupMenu();
        AppendMenuW(motions,MF_STRING,IdOpenMotion,T(L"モーションを開く...",L"Open motion..."));
        AppendMenuW(motions,MF_STRING|(Own(m).motionPath.empty()?MF_GRAYED:0),IdCloseMotion,T(L"モーションを閉じる",L"Close motion"));
        AppendMenuW(motions,MF_SEPARATOR,0,nullptr);
        const size_t idleCount=Own(m).idleMotionPaths.size();
        AppendMenuW(motions,MF_STRING,IdOpenIdleMotion,T(L"待機モーションを開く...（複数選択可）",L"Open idle motions... (one or more)"));
        AppendMenuW(motions,MF_STRING|(idleCount==0?MF_GRAYED:0),IdAddIdleMotion,T(L"待機モーションを追加...",L"Add idle motions..."));
        AppendMenuW(motions,MF_STRING|(idleCount==0?MF_GRAYED:0),IdCloseIdleMotion,
                    idleCount>1?Fmt(T(L"待機モーションを閉じる（{0}件）",L"Close idle motions ({0})"),{std::to_wstring(idleCount)}).c_str():T(L"待機モーションを閉じる",L"Close idle motion"));
        AppendMenuW(motions,MF_SEPARATOR,0,nullptr);
        const size_t clickCount=Own(m).clickMotionPaths.size();
        AppendMenuW(motions,MF_STRING,IdOpenClickMotion,T(L"クリック時のモーションを開く...（複数選択可）",L"Open click motions... (one or more)"));
        AppendMenuW(motions,MF_STRING|(clickCount==0?MF_GRAYED:0),IdCloseClickMotion,
                    clickCount>1?Fmt(T(L"クリック時のモーションを閉じる（{0}件）",L"Close click motions ({0})"),{std::to_wstring(clickCount)}).c_str():T(L"クリック時のモーションを閉じる",L"Close click motion"));
        AppendMenuW(motions,MF_SEPARATOR,0,nullptr);
        AppendMenuW(motions,MF_STRING|(Own(m).walkMotionPath.empty()?0:MF_CHECKED),IdOpenWalkMotion,T(L"歩くモーションを開く...",L"Open walk motion..."));
        AppendMenuW(motions,MF_STRING|(Own(m).runMotionPath.empty()?0:MF_CHECKED),IdOpenRunMotion,T(L"走るモーションを開く...",L"Open run motion..."));
        AppendMenuW(motions,MF_STRING|(Own(m).walkMotionPath.empty()&&Own(m).runMotionPath.empty()?MF_GRAYED:0),IdCloseGaitMotion,
                    T(L"歩く・走るモーションを閉じる",L"Close walk and run motions"));
        AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(motions),T(L"モーション",L"Motion"));
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        const bool grounded=m.place==Place::Ground;
        AppendMenuW(menu,MF_STRING|(app.settings.windowMode?MF_CHECKED:0),IdWindowMode,T(L"アクティブウィンドウに乗る",L"Ride on the active window"));
        AppendMenuW(menu,MF_STRING|(grounded?0:MF_GRAYED),IdWalkLeft,T(L"左へ歩く",L"Walk left"));
        AppendMenuW(menu,MF_STRING|(grounded?0:MF_GRAYED),IdWalkRight,T(L"右へ歩く",L"Walk right"));
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        AppendMenuW(menu,MF_STRING,IdSettings,T(L"設定...",L"Settings..."));
        AppendMenuW(menu,MF_STRING,IdExit,T(L"終了",L"Exit"));
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
        current.userName=s.userName;
        current.chatInitiative=s.chatInitiative;
        // A shorter interval takes effect now, not after the old wait.
        if(s.chatterMinutes!=current.chatterMinutes)
        {
            current.chatterMinutes=s.chatterMinutes;
            app.nextChatterAt=std::min(app.nextChatterAt,app.clock+Interval(s.chatterMinutes));
        }
        if(s.banterMinutes!=current.banterMinutes)
        {
            current.banterMinutes=s.banterMinutes;
            if(s.banterMinutes>0)app.nextBanterAt=std::min(app.nextBanterAt,app.clock+Interval(s.banterMinutes));
        }
        if(s.updateCheck&&!current.updateCheck)app.nextUpdateCheck=app.clock+5;
        current.updateCheck=s.updateCheck;
        current.morningSummary=s.morningSummary;
        current.restMinutes=s.restMinutes;
        // Focus mode's lengths take effect from its next turn.
        current.focusMinutes=s.focusMinutes;
        current.focusBreakMinutes=s.focusBreakMinutes;
        current.calendarLeadMinutes=s.calendarLeadMinutes;
        if(s.calendarAddress!=current.calendarAddress)
        {
            if(const std::wstring problem=s.calendarAddress.empty()?std::wstring():CheckCalendarAddress(s.calendarAddress);!problem.empty())
                ShowMessage(problem,MB_ICONWARNING);   // the old address stays
            else
            {
                current.calendarAddress=s.calendarAddress;
                app.events.clear();
                if(!current.calendarAddress.empty())
                {
                    if(!app.calendar)app.calendar=std::make_unique<CalendarFeed>(app.hwnd);
                    app.calendar->Fetch(current.calendarAddress,true);
                    app.nextCalendarFetch=app.clock+30*60;
                }
            }
        }
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
            if(!m.character.Save(Own(m).modelPath))ShowMessage(T(L"人物設定を保存できませんでした。",L"Could not save the persona."),MB_ICONERROR);
        }
        if(s.language!=current.language)
        {
            // Everything follows at once; a persona left as the default becomes the new
            // language's default (and the conversation starts again in that language).
            const std::wstring oldDefault=CharacterProfile::DefaultPersona();
            current.language=s.language;
            SetLanguage(current.language);
            for(const auto& owned:app.mascots)
                if(owned->character.persona==oldDefault)
                {
                    owned->character.persona=CharacterProfile::DefaultPersona();
                    owned->conversation.clear();
                }
        }
        if(choice.startWithWindows!=StartupRegistered()&&!SetStartup(choice.startWithWindows))
            ShowMessage(T(L"スタートアップの設定を変更できませんでした。",L"Could not change starting with Windows."),MB_ICONERROR);
        const bool restart=s.fps!=current.fps||s.motionBlur!=current.motionBlur||s.msaa!=current.msaa||s.maxTextureSize!=current.maxTextureSize||
                           s.physicsFps!=current.physicsFps||s.physicsIterations!=current.physicsIterations||s.toonDir!=current.toonDir;
        current.fps=s.fps;
        current.motionBlur=s.motionBlur;
        current.msaa=s.msaa;
        current.maxTextureSize=s.maxTextureSize;
        current.physicsFps=s.physicsFps;
        current.physicsIterations=s.physicsIterations;
        current.toonDir=s.toonDir;
        if(s.scale!=current.scale)ApplyScale(s.scale);
        current.Save(app.settingsPath);
        if(restart&&ShowMessage(T(L"詳細の設定は、再起動すると反映されます。\n今すぐ再起動しますか？",L"The advanced settings take effect after a restart.\nRestart now?"),MB_ICONQUESTION|MB_YESNO)==IDYES)
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
                    if(end!=std::wstring::npos&&end-i<=11){i=end;continue;}
                    if(end==std::wstring::npos&&out.size()-i<=11)break;
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
            if(tell)m.bubble.ShowNotice(T(L"設定の「会話」で、使うモデルを選んでください（Ollama などを起動してから「一覧を取得」）。\n"
                                          L"タイマーやリマインダー、やることは、LLMなしでも頼めます（例：「5分後に教えて」「15時に会議って教えて」「やることに牛乳を追加」）。",
                                          L"Choose a model under Talking in the settings (start Ollama or the like, then press Get list).\n"
                                          L"Timers, reminders and to-dos work without it (e.g. \"remind me in 5 minutes\", \"remind me at 3pm to call mom\", \"add milk to my todo list\")."));
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
    // (English replies are asked for the English tag in square brackets.)
    struct Feeling {const wchar_t* japanese;const wchar_t* english;Expression expression;};
    constexpr Feeling Feelings[]={
        {L"笑顔",L"smile",Expression::Smile},{L"驚き",L"surprise",Expression::Surprise},{L"困り",L"troubled",Expression::Troubled},
        {L"怒り",L"angry",Expression::Angry},{L"悲しみ",L"sad",Expression::Sad},{L"照れ",L"shy",Expression::Shy},{L"普通",L"neutral",Expression::Neutral}};

    // How the LLM is asked to mark the feeling at the start of a reply, in the language in use.
    std::wstring FeelingTags()
    {
        std::wstring tags;
        for(const auto& f:Feelings)tags+=English()?L"["+std::wstring(f.english)+L"] ":L"［"+std::wstring(f.japanese)+L"］";
        return tags;
    }

    // The feeling a reply starts with, if its tag has come in (near the start, in either language).
    const Feeling* ReplyFeeling(const std::wstring& reply)
    {
        std::wstring head=reply.substr(0,24);
        std::transform(head.begin(),head.end(),head.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});
        for(const auto& f:Feelings)
            if(head.find(f.japanese)!=std::wstring::npos||head.find(L"["+std::wstring(f.english))!=std::wstring::npos||
               head.find(L"［"+std::wstring(f.english))!=std::wstring::npos)return &f;
        return nullptr;
    }

    void BeginReply(Mascot& m,std::vector<ChatMessage> messages);
    void EndBanter(const wchar_t* why);
    void SayFixed(Mascot& m,const std::wstring& text);

    // The date and time for the LLM, in the language in use.
    std::wstring NowText()
    {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t text[96]{};
        if(English())
        {
            static constexpr const wchar_t* Days[]={L"Sunday",L"Monday",L"Tuesday",L"Wednesday",L"Thursday",L"Friday",L"Saturday"};
            static constexpr const wchar_t* Months[]={L"January",L"February",L"March",L"April",L"May",L"June",L"July",L"August",L"September",L"October",L"November",L"December"};
            swprintf_s(text,L"%s, %s %d, %d, %d:%02d",Days[now.wDayOfWeek%7],Months[(now.wMonth+11)%12],now.wDay,now.wYear,now.wHour,now.wMinute);
        }
        else
        {
            static constexpr const wchar_t* Days[]={L"日",L"月",L"火",L"水",L"木",L"金",L"土"};
            swprintf_s(text,L"%d年%d月%d日（%s）%d時%02d分",now.wYear,now.wMonth,now.wDay,Days[now.wDayOfWeek%7],now.wHour,now.wMinute);
        }
        return text;
    }

    // Ask for a reply to the conversation as it stands; query is what the memory is searched for.
    void StartReply(Mascot& m,const std::wstring& query)
    {
        // The last six exchanges are enough for a chat at the desk.
        while(m.conversation.size()>12)m.conversation.erase(m.conversation.begin());
        const std::wstring& text=query;
        const std::wstring clock=NowText();
        const std::wstring& user=app.settings.userName;
        const std::wstring partner=user.empty()
            ?std::wstring(T(L"話し相手の名前は分からないので、名前では呼ばないでください（「◯◯さん」のような書き方もしないでください）。\n",
                            L"You do not know the name of the person you talk with, so do not call them by any name.\n"))
            :Fmt(T(L"話し相手（あなたに話しかけている人）の名前は「{0}」です。\n",L"The person talking with you is called \"{0}\".\n"),{user});
        std::wstring remembered;
        if(app.settings.chatMemory)
            if(const std::wstring notes=m.memory.Recall(text,1500);!notes.empty())
                remembered=T(L"\n以前の会話で覚えたこと（必要なときだけ自然に話に生かしてください。ここにないことを覚えているふりはしないでください）：\n",
                             L"\nWhat you remember from earlier conversations (bring it in naturally, only when it fits; do not pretend to remember anything not listed here):\n")+notes+L"\n";
        std::vector<ChatMessage> messages;
        if(English())
            messages.push_back({L"system",
                L"You are \""+m.character.name+L"\".\n"+m.character.persona+L"\n\n"+partner+remembered+
                L"It is now "+clock+L".\n"
                L"Your replies appear in a speech bubble over your head on the desktop. "
                L"Always answer in English, in one to three short, casual spoken sentences. Do not use Markdown, lists, emoji or emoticons.\n"
                L"Begin every reply with how you feel, as exactly one of "+FeelingTags()+L"(for example: [smile] Oh, really!). "
                L"Use that tag only once, at the very start.\n"
                L"When what the other person says is in ( ), it is not something they said but a description of the situation: speak to them first, in a way that suits it."});
        else
            messages.push_back({L"system",
                L"あなたは「"+m.character.name+L"」です。\n"+m.character.persona+L"\n\n"+partner+remembered+
                L"いまは"+clock+L"です。\n"
                L"あなたの返事は、デスクトップの上にいるあなたの頭上の吹き出しに表示されます。"
                L"1〜3文くらいの短い話し言葉で答えてください。Markdown、箇条書き、絵文字、顔文字は使わないでください。\n"
                L"返事のいちばん最初に、そのときの気持ちを"+FeelingTags()+L"のどれか1つで書いてください（例：［笑顔］そうなんだ！）。"
                L"この印は最初の1つだけで、ほかの所には書かないでください。\n"
                L"話し相手の発言が（ ）で囲まれているときは、実際の発言ではなく状況の説明です。その状況に合わせて、あなたから話しかけてください。"});
        messages.insert(messages.end(),m.conversation.begin(),m.conversation.end());
        BeginReply(m,std::move(messages));
    }

    // A reply (or a line said to another character) starts streaming into her bubble.
    void BeginReply(Mascot& m,std::vector<ChatMessage> messages)
    {
        m.reply.clear();
        m.replyShown=0;
        m.speech.Reset();
        m.replyFeelingSet=false;
        if(m.voice)m.voice->CancelAll();   // what was left of the last reply is not said
        m.chatRequest=m.chat->Start(app.settings.chatEndpoint,app.settings.chatModel,std::move(messages));
        m.bubble.Begin(m.character.name);
        Trace(L"%.1f talk: request %d",app.clock,m.chatRequest);
    }

    bool HandlePlan(Mascot& m,const std::wstring& said);

    void Talk(Mascot& m,const std::wstring& text)
    {
        if(InBanter(m))EndBanter(L"the user talks to one of them");
        m.fallbackLine.clear();
        if(HandlePlan(m,text))return;
        if(!ChatReady(m,true))return;
        m.conversation.push_back({L"user",text});
        StartReply(m,text);
    }

    // She speaks first: situation says why (it goes in as a stage direction in brackets).
    // fallback: said as it is if the LLM does not answer.
    void SpeakFirst(Mascot& m,const std::wstring& situation,const std::wstring& fallback={})
    {
        if(!ChatReady(m,false))return;
        m.fallbackLine=fallback;
        Trace(L"%.1f speak first: %s",app.clock,situation.c_str());
        m.conversation.push_back({L"user",English()?L"("+situation+L")":L"（"+situation+L"）"});
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
    bool TodaySummary(std::wstring& line,std::wstring& facts);

    void MaybeSpeakFirst()
    {
        if(!app.settings.chatInitiative||app.settings.chatModel.empty()||app.clock<8)return;
        if(app.focus.on&&!app.focus.resting)return;   // focus mode: the user is working
        if(app.banter.stage!=App::Banter::Stage::None)return;
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
            SpeakFirst(anyone(),Fmt(T(L"話し相手が{0}分ほど席を外していて、いまパソコンの前に戻ってきました。おかえりのひと言を",
                                     L"The person you talk with was away from the computer for about {0} minutes and has just come back. Say a word of welcome back"),
                                   {std::to_wstring(minutes)}));
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
            // The morning's is today's plans, when there are any (MaybeMorningSummary).
            std::wstring line,facts;
            const bool summary=std::wstring(slot)==L"morning"&&app.settings.morningSummary&&TodaySummary(line,facts);
            if(app.settings.lastGreeting!=key&&!summary)
            {
                app.settings.lastGreeting=key;
                app.settings.Save(app.settingsPath);
                const std::wstring s=slot;
                SpeakFirst(anyone(),s==L"morning"?T(L"朝です。話し相手がパソコンを使い始めました。朝のあいさつを",L"It is morning. The person you talk with has started using the computer. Say good morning"):
                           s==L"noon"?T(L"お昼どきです。お昼ごはんのことなどで、軽く声をかけて",L"It is around lunchtime. Say something light, about lunch or the like"):
                           s==L"evening"?T(L"夕方から夜の時間です。今日一日おつかれさまの気持ちで、ひと言",L"It is evening. Say a word of thanks for the day's work"):
                                          T(L"夜遅い時間です。夜ふかしを気づかうひと言を",L"It is late at night. Say a word of concern about staying up late"));
                app.nextChatterAt=std::max(app.nextChatterAt,app.clock+20*60);
                return;
            }
        }
        if(app.clock>=app.nextChatterAt)
        {
            app.nextChatterAt=app.clock+Interval(app.settings.chatterMinutes);
            SpeakFirst(anyone(),T(L"特に用事はないけれど、話し相手に何かひと言話しかけたくなりました。覚えていることや、いまの時間帯・季節にちなんだ話題で",
                                  L"Nothing in particular, but you feel like saying a word to the person you talk with: about something you remember, or the time of day or the season"));
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

    // Show and say the reply as far as text goes (it only ever grows).
    void Reveal(Mascot& m,const std::wstring& text)
    {
        if(text.size()<=m.replyShown)return;
        m.bubble.Append(text.substr(m.replyShown));
        m.speech.Add(text.substr(m.replyShown));
        m.replyShown=text.size();
    }

    // A line said to another character is that character's line only. A name in front of it
    // ("空：") and quotes around it go, and it ends at the first line break: small models go on
    // to write the other one's line too. complete: the line has ended before the reply has.
    std::wstring BanterLine(const Mascot& m,const std::wstring& clean,bool& complete)
    {
        complete=false;
        std::wstring line=clean;
        for(const wchar_t* separator:{L"：",L":"})
        {
            const std::wstring prefix=m.character.name+separator;
            if(line.rfind(prefix,0)==0){line.erase(0,prefix.size());break;}
            if(line.size()<prefix.size()&&prefix.rfind(line,0)==0)return {};   // may become one
        }
        line.erase(0,std::min(line.size(),line.find_first_not_of(L" 　")));
        if(!line.empty()&&line[0]==L'「')
        {
            line.erase(0,1);
            if(const size_t close=line.find(L'」');close!=std::wstring::npos){line.resize(close);complete=true;}
        }
        if(const size_t end=line.find_first_of(L"\r\n");end!=std::wstring::npos){line.resize(end);complete=true;}
        return line;
    }

    void FinishBanterLine(Mascot& m,std::wstring line);

    void OnChatText(Mascot& m,std::wstring* text,int id)
    {
        std::unique_ptr<std::wstring> owned(text);
        if(id!=m.chatRequest)return;
        m.reply+=*owned;
        if(!m.replyFeelingSet)
            if(const Feeling* feeling=ReplyFeeling(m.reply))
            {
                m.replyFeelingSet=true;
                m.model.SetExpression(feeling->expression,true);
                m.feelingUntil=-1;
                Trace(L"%.1f talk: feeling %s",app.clock,feeling->english);
            }
        if(m.banterLine)
        {
            bool complete=false;
            const std::wstring line=BanterLine(m,CleanReply(m.reply),complete);
            Reveal(m,line);
            if(complete)
            {
                // The rest would be more than her line: stop the request here.
                if(m.chat)m.chat->Cancel();
                m.chatRequest=0;
                FinishBanterLine(m,line);
            }
            return;
        }
        Reveal(m,CleanReply(m.reply));
    }

    void OnChatDone(Mascot& m,std::wstring* error,int id)
    {
        std::unique_ptr<std::wstring> owned(error);
        if(id!=m.chatRequest)return;
        Trace(L"%.1f talk: request %d done %s",app.clock,id,owned->c_str());
        if(m.banterLine)
        {
            if(!owned->empty())
            {
                m.banterLine=false;
                m.speech.Reset();
                m.bubble.ShowNotice(*owned);
                EndBanter(L"no reply");
                return;
            }
            bool complete=false;
            FinishBanterLine(m,BanterLine(m,CleanReply(m.reply+L"\n"),complete));
            return;
        }
        if(!owned->empty())
        {
            // Not answered: forget the question too, and say why (or what she had to say).
            if(!m.conversation.empty()&&m.conversation.back().role==L"user")m.conversation.pop_back();
            m.speech.Reset();
            if(!m.fallbackLine.empty()){SayFixed(m,std::wstring(m.fallbackLine));return;}
            m.bubble.ShowNotice(*owned);
            return;
        }
        m.fallbackLine.clear();
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
               m.conversation.back().content.rfind(L"（",0)!=0&&m.conversation.back().content.rfind(L"(",0)!=0)
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

    // ---- Characters talking with each other -----------------------------------------------

    // What two characters may start talking about (the prompt adds the date and time).
    constexpr const wchar_t* BanterTopics[]={
        L"いまの季節や天気のこと",L"好きな食べ物やおやつのこと",L"最近気になっていること",L"休みの日にしてみたいこと",
        L"デスクトップの上での暮らしのこと",L"このパソコンの持ち主（いつも話しかけてくれる人）のこと",
        L"相手に前から聞いてみたかったこと",L"いまの時間帯にちなんだこと",L"好きな音楽や歌のこと",L"ちょっとした思い出話"};
    constexpr const wchar_t* EnglishBanterTopics[]={
        L"the season and the weather",L"favorite foods and snacks",L"something on your mind lately",L"what you would like to do on a day off",
        L"life on the desktop",L"the owner of this computer (who always talks to you)",
        L"something you have long wanted to ask the other",L"something about this time of day",L"favorite music and songs",L"a little memory"};
    static_assert(std::size(BanterTopics)==std::size(EnglishBanterTopics));

    // Someone is saying something, about to, or being talked to: nobody else starts talking.
    bool AnyoneTalking()
    {
        return std::any_of(app.mascots.begin(),app.mascots.end(),[](const auto& m){
            return (m->chat&&m->chat->Busy())||m->bubble.Visible()||m->chatInput.IsOpen()||m->speech.Speaking();});
    }

    void EndBanter(const wchar_t* why)
    {
        auto& b=app.banter;
        if(b.stage==App::Banter::Stage::None)return;
        Trace(L"%.1f banter ends: %s",app.clock,why);
        for(Mascot* m:{b.first,b.second})
            if(m&&m->banterLine)
            {
                // A line still coming is dropped; what was shown of it fades out.
                if(m->chat)m->chat->Cancel();
                m->chatRequest=0;
                m->banterLine=false;
                m->speech.Finish();
                m->bubble.Finish();
            }
        b=App::Banter{};
    }

    // The line has ended: the rest is shown and said, and it joins what they have said.
    void FinishBanterLine(Mascot& m,std::wstring line)
    {
        Reveal(m,line);
        m.speech.Finish();
        m.bubble.Finish();
        m.banterLine=false;
        while(!line.empty()&&std::iswspace(line.back()))line.pop_back();
        if(line.empty()){EndBanter(L"an empty line");return;}
        app.banter.lines.push_back({m.character.name,line});
        --app.banter.linesLeft;
        Trace(L"%.1f banter: %s: %s",app.clock,m.character.name.c_str(),line.c_str());
    }

    // Free to talk: standing on a surface with nothing else to do.
    bool FreeToTalk(Mascot& c)
    {
        return c.place==Place::Ground&&c.travel==Travel::None&&c.model.CurrentActivity()==Activity::Idle&&
               !MotionBusy(c)&&!c.dozing&&!c.chatInput.IsOpen();
    }

    // Two characters free to talk get together: the second walks over to stand beside the
    // first (on the side she comes from, if there is room), or warps there from another
    // surface. False when no two can.
    bool StartBanter()
    {
        std::vector<Mascot*> free;
        for(const auto& owned:app.mascots)if(FreeToTalk(*owned))free.push_back(owned.get());
        if(free.size()<2)return false;
        auto pick=[&]{
            const size_t i=std::min(free.size()-1,static_cast<size_t>(app.Random01()*free.size()));
            Mascot* c=free[i];
            free.erase(free.begin()+static_cast<std::ptrdiff_t>(i));
            return c;
        };
        Mascot& first=*pick();
        Mascot& second=*pick();
        if(!ChatReady(first,false)||!ChatReady(second,false))return false;
        double left,right;
        SurfaceRange(first,left,right);
        const double gap=(ModelHalfWidthPixels(first)+ModelHalfWidthPixels(second))*0.8;
        const double side=CenterX(second)>=CenterX(first)?1:-1;
        double x=CenterX(first)+side*gap;
        if(x<left||x>right)x=CenterX(first)-side*gap;
        if(x<left||x>right)return false;

        auto& b=app.banter;
        b=App::Banter{};
        b.stage=App::Banter::Stage::Gathering;
        b.first=&first;b.second=&second;
        b.since=app.clock;
        const size_t topic=std::min(std::size(BanterTopics)-1,static_cast<size_t>(app.Random01()*std::size(BanterTopics)));
        b.topic=English()?EnglishBanterTopics[topic]:BanterTopics[topic];
        b.linesLeft=4+static_cast<int>(app.Random01()*3);   // 4 to 6 lines
        Trace(L"%.1f banter: %s and %s, %d lines about %s",app.clock,first.character.name.c_str(),second.character.name.c_str(),b.linesLeft,b.topic.c_str());
        const bool sameSurface=second.surfaceWindow==first.surfaceWindow&&std::abs(FeetLine(second)-FeetLine(first))<4;
        if(sameSurface)
        {
            const double distance=std::abs(x-CenterX(second));
            if(distance>8)StartWalk(second,x,distance>ModelHeightPixels(second)?Activity::Run:Activity::Walk);
        }
        else
        {
            second.destinationWindow=first.surfaceWindow;
            second.destinationLine=FeetLine(first);
            second.destinationX=x;
            second.restPoseAt=-1;
            BeginWarp(second);
        }
        return true;
    }

    // One line of the conversation, by m: her persona and the partner's, what they have said
    // so far, and what they talk about.
    void SayBanterLine(Mascot& m)
    {
        auto& b=app.banter;
        Mascot& partner=&m==b.first?*b.second:*b.first;
        if(!ChatReady(m,false)){EndBanter(L"the LLM is not set up");return;}
        const std::wstring& name=m.character.name;
        const std::wstring& other=partner.character.name;
        const std::wstring clock=NowText();
        std::wstring otherPersona=partner.character.persona;
        if(otherPersona.size()>400)otherPersona=otherPersona.substr(0,400)+L"…";
        const std::wstring& user=app.settings.userName;
        std::wstring remembered;
        if(app.settings.chatMemory)
            if(const std::wstring notes=m.memory.Recall(b.topic,600);!notes.empty())
                remembered=T(L"\nこのパソコンの持ち主について、あなたが覚えていること（話に合うときだけ使ってください）：\n",
                             L"\nWhat you remember about the owner of this computer (use it only when it fits the talk):\n")+notes+L"\n";
        std::vector<ChatMessage> messages;
        std::wstring ask;
        if(English())
        {
            messages.push_back({L"system",
                L"You are \""+name+L"\".\n"+m.character.persona+L"\n\n"
                L"You are now chatting with \""+other+L"\", who is beside you on the desktop. \""+other+L"\" is like this:\n"+otherPersona+L"\n\n"+
                (user.empty()?std::wstring(L"The owner of this computer (who always talks to you both) is watching you two.\n")
                             :L"The owner of this computer (who always talks to you both) is called \""+user+L"\" and is watching you two.\n")+
                remembered+
                L"It is now "+clock+L". What got the chat going: "+b.topic+L"\n"
                L"Your line appears in a speech bubble over your head. Write only your own (\""+name+L"\") line, in English: one short, casual spoken line of one or two sentences. "
                L"Do not write \""+other+L"\"'s lines, names, stage directions or anything in ( ). Do not use Markdown, lists, emoji or emoticons.\n"
                L"Begin your line with how you feel, as exactly one of "+FeelingTags()+L"(for example: [smile] Hey, guess what!)."});
            if(b.lines.empty())ask=L"Speak to \""+other+L"\" and start the chat.";
            else
            {
                ask=L"The chat so far:\n";
                for(const auto& [who,said]:b.lines)ask+=who+L": "+said+L"\n";
                ask+=L"\nFollowing \""+other+L"\"'s last line, say \""+name+L"\"'s next line.";
                if(b.linesLeft<=1)ask+=L" This is the last line: bring the chat to a natural close.";
            }
            messages.push_back({L"user",ask});
        }
        else
        {
            messages.push_back({L"system",
                L"あなたは「"+name+L"」です。\n"+m.character.persona+L"\n\n"
                L"いま、デスクトップの上で、となりにいる「"+other+L"」とおしゃべりしています。「"+other+L"」は、こんな人です：\n"+otherPersona+L"\n\n"+
                (user.empty()?std::wstring(L"このパソコンの持ち主（いつも二人に話しかけてくれる人）が、二人の様子を見ています。\n")
                             :L"このパソコンの持ち主（いつも二人に話しかけてくれる人）の名前は「"+user+L"」で、二人の様子を見ています。\n")+
                remembered+
                L"いまは"+clock+L"です。おしゃべりのきっかけ："+b.topic+L"\n"
                L"あなたのセリフは、あなたの頭上の吹き出しに表示されます。あなた（「"+name+L"」）のセリフを1つだけ、1〜2文の短い話し言葉で書いてください。"
                L"「"+other+L"」のセリフ、名前、ト書き、（ ）書きは書かないでください。Markdown、箇条書き、絵文字、顔文字は使わないでください。\n"
                L"セリフのいちばん最初に、そのときの気持ちを"+FeelingTags()+L"のどれか1つで書いてください（例：［笑顔］ねえ、聞いて！）。"});
            if(b.lines.empty())ask=L"あなたから「"+other+L"」に話しかけて、おしゃべりを始めてください。";
            else
            {
                ask=L"ここまでの二人の会話：\n";
                for(const auto& [who,said]:b.lines)ask+=who+L"："+said+L"\n";
                ask+=L"\n「"+other+L"」の最後のセリフに続けて、「"+name+L"」の次のセリフを言ってください。";
                if(b.linesLeft<=1)ask+=L"これで最後のひと言です。おしゃべりが自然に終わるように締めくくってください。";
            }
            messages.push_back({L"user",ask});
        }
        b.lineStarted=true;
        m.banterLine=true;
        BeginReply(m,std::move(messages));
    }

    // Now and then (every 20 to 40 minutes, with "speak first" on, while the user is at the
    // computer) two characters talk with each other: they get together, then take turns, one
    // line each, each said to its end before the reply. Anything that needs either of them
    // (picking one up, talking to one, the input box) ends it.
    void UpdateBanter()
    {
        using Stage=App::Banter::Stage;
        auto& b=app.banter;
        if(b.stage==Stage::None)
        {
            const bool due=app.banterRequested?app.clock>5:
                           app.settings.chatInitiative&&app.settings.banterMinutes>0&&app.clock>=app.nextBanterAt&&InputIdleSeconds()<90;
            // Not while focus mode has the user working.
            if(!due||app.mascots.size()<2||app.settings.chatModel.empty()||AnyoneTalking()||(app.focus.on&&!app.focus.resting))return;
            app.banterRequested=false;
            app.nextBanterAt=app.clock+(StartBanter()?Interval(std::max(app.settings.banterMinutes,1)):60);
            return;
        }
        Mascot& first=*b.first;
        Mascot& second=*b.second;
        for(Mascot* m:{&first,&second})
        {
            const bool coming=b.stage==Stage::Gathering&&m==&second&&(m->place==Place::Warping||m->place==Place::Jumping);
            if(m->chatInput.IsOpen()||m->dozing||(m->place!=Place::Ground&&!coming)){EndBanter(L"one of them is needed elsewhere");return;}
        }
        if(b.stage==Stage::Gathering)
        {
            if(app.clock-b.since>20){EndBanter(L"they could not get together");return;}
            auto settled=[](Mascot& c){
                const Activity a=c.model.CurrentActivity();
                return c.place==Place::Ground&&c.travel==Travel::None&&a!=Activity::Walk&&a!=Activity::Run;
            };
            if(!settled(first)||!settled(second))return;
            b.stage=Stage::Talking;
            b.speaker=&first;
            b.nextLineAt=app.clock+0.6;
            return;
        }
        // Walking off (the menu's walk) ends it too.
        for(Mascot* m:{&first,&second})
            if(const Activity a=m->model.CurrentActivity();a==Activity::Walk||a==Activity::Run){EndBanter(L"one of them walks off");return;}
        Mascot& speaker=*b.speaker;
        if(b.lineStarted)
        {
            if(speaker.banterLine||(speaker.chat&&speaker.chat->Busy())||speaker.speech.Speaking()||speaker.bubble.Talking())return;
            b.lineStarted=false;
            if(b.linesLeft<=0){EndBanter(L"they have finished");return;}
            b.speaker=&speaker==&first?&second:&first;
            b.nextLineAt=app.clock+0.7;
            return;
        }
        if(app.clock>=b.nextLineAt)SayBanterLine(speaker);
    }

    // ---- Updates -------------------------------------------------------------------------

    // Something of the app's own, said in her bubble (and voice) like a reply, without the LLM.
    void SayFixed(Mascot& m,const std::wstring& text)
    {
        m.fallbackLine.clear();
        if(m.chat)m.chat->Cancel();
        m.chatRequest=0;
        m.banterLine=false;
        m.reply.clear();
        m.replyShown=0;
        m.speech.Reset();
        m.replyFeelingSet=false;
        if(m.voice)m.voice->CancelAll();
        m.bubble.Begin(m.character.name);
        Reveal(m,text);
        m.speech.Finish();
        m.bubble.Finish();
    }

    void OnUpdateAnswer(bool yes)
    {
        if(!app.asker||!app.update)return;
        Mascot& m=*app.asker;
        if(!yes)
        {
            Trace(L"%.1f update %s: declined",app.clock,app.update->version.c_str());
            SayFixed(m,T(L"わかった。また今度ね。",L"Okay. Maybe next time."));
            app.declinedVersion=app.update->version;
            app.update.reset();
            app.asker=nullptr;
            return;
        }
        Trace(L"%.1f update %s: downloading",app.clock,app.update->version.c_str());
        SayFixed(m,T(L"じゃあ、更新するね。終わったら、すぐに戻ってくるよ。",L"Then I'll update. I'll be right back when it's done."));
        app.updating=true;
        app.updater->Download(*app.update);
    }

    // A newer release, not yet answered: one of the characters free to talk tells the user and
    // asks whether to update now (in her own words when the LLM is set up), with はい／いいえ
    // under her. Not over someone talking, nor while the user is away.
    void MaybeAskUpdate()
    {
        if(!app.update||app.asker||app.updating||app.banter.stage!=App::Banter::Stage::None)return;
        if(AnyoneTalking()||InputIdleSeconds()>=90)return;
        std::vector<Mascot*> free;
        for(const auto& owned:app.mascots)if(FreeToTalk(*owned))free.push_back(owned.get());
        if(free.empty())return;
        Mascot& m=*free[std::min(free.size()-1,static_cast<size_t>(app.Random01()*free.size()))];
        app.asker=&m;
        const std::wstring& version=app.update->version;
        Trace(L"%.1f update %s: asking",app.clock,version.c_str());
        if(ChatReady(m,false))
            SpeakFirst(m,Fmt(T(L"このアプリ（MikuMikuDesktopPerch）の新しいバージョン{0}が出ました。話し相手にそのことを伝えて、いま更新するかどうかを聞いて",
                              L"A new version ({0}) of this app, MikuMikuDesktopPerch, is out. Tell the person you talk with, and ask whether to update now"),{version}));
        else SayFixed(m,Fmt(T(L"MikuMikuDesktopPerch の新しいバージョン（{0}）が出てるよ。いま更新する？",L"A new version of MikuMikuDesktopPerch ({0}) is out. Shall I update now?"),{version}));
        const RECT r=FrameRect(m);
        const POINT ground=GroundPointOnScreen(m);
        app.updateChoice.Open(m.hwnd,{(r.left+r.right)/2,ground.y-static_cast<LONG>(ModelHeightPixels(m)*0.12)},
                              T(L"はい（更新する）",L"Yes (update)"),T(L"いいえ",L"No"),OnUpdateAnswer);
    }

    // Now and then (shortly after start, then once a day) ask GitHub for a newer release.
    void MaybeCheckForUpdate()
    {
        if(!app.settings.updateCheck||app.diagnostic||app.updating||app.update||app.clock<app.nextUpdateCheck)return;
        app.nextUpdateCheck=app.clock+24*60*60;
        if(!app.updater)app.updater=std::make_unique<Updater>(app.hwnd);
        app.updater->Check();
    }

    void OnUpdateChecked(Release* found)
    {
        std::unique_ptr<Release> owned(found);
        Trace(L"%.1f update check: %s (this is %s)",app.clock,owned?owned->version.c_str():L"nothing newer",CurrentVersion().c_str());
        if(owned&&owned->version!=app.declinedVersion&&!app.updating)app.update=*owned;
    }

    // The new files are unpacked: put them in place and start again on the new exe.
    void OnUpdateDownloaded(std::wstring* error)
    {
        std::unique_ptr<std::wstring> owned(error);
        std::wstring problem=*owned;
        if(problem.empty())problem=Updater::Install();
        Trace(L"%.1f update: %s",app.clock,problem.empty()?L"installed":problem.c_str());
        if(problem.empty())
        {
            app.restart=true;
            DestroyWindow(app.hwnd);
            return;
        }
        if(app.asker)SayFixed(*app.asker,T(L"ごめんね、更新できなかったみたい……",L"Sorry, the update didn't work..."));
        ShowMessage(T(L"更新できませんでした。\n\n",L"Could not update.\n\n")+problem,MB_ICONWARNING);
        app.declinedVersion=app.update?app.update->version:std::wstring();
        app.update.reset();
        app.asker=nullptr;
        app.updating=false;
    }

    // ---- Timers, reminders and to-dos -----------------------------------------------------

    void SavePlanner()
    {
        if(!app.diagnostic)app.planner.Save();
        RefreshPlannerWindow();
    }

    std::wstring Quoted(const std::wstring& text){return English()?L"\""+text+L"\"":L"「"+text+L"」";}

    std::wstring Listing(const std::vector<std::wstring>& items,const wchar_t* japaneseSeparator=L"、")
    {
        std::wstring out;
        for(size_t i=0;i<items.size();++i){if(i)out+=English()?L", ":japaneseSeparator;out+=items[i];}
        return out;
    }

    // A reminder's time to go after a verb: "15時" (に added by the sentence) / "at 3:00 PM",
    // "tomorrow at 9:00 AM", "on Fri, Oct 30 at 6:00 PM".
    std::wstring AtTime(std::int64_t at,std::int64_t now,bool daily)
    {
        const std::wstring when=TimeText(at,now,daily);
        if(!English())return when;
        if(!when.empty()&&std::iswdigit(when[0]))return L"at "+when;
        if(when.find(L", ")!=std::wstring::npos)return L"on "+when;
        return when;
    }

    // A calendar event as she says it: 10時から「打ち合わせ」, 明日の10時から…, 終日「誕生日」 /
    // "meeting" at 10:00 AM, "birthday" (all day).
    std::wstring EventLabel(const CalendarEvent& e,std::int64_t now)
    {
        const bool today=e.start<DayEnd(now);
        if(e.allDay)
            return English()?Quoted(e.title)+(today?L" (all day)":L" (all day, "+DayText(DateOf(e.start),now)+L")")
                            :(today?std::wstring():DayText(DateOf(e.start),now)+L"、")+L"終日"+Quoted(e.title);
        return English()?Quoted(e.title)+L" "+AtTime(e.start,now,false):TimeText(e.start,now)+L"から"+Quoted(e.title);
    }

    // The calendar's events not yet over, starting before until.
    std::vector<std::wstring> EventLabels(std::int64_t now,std::int64_t until,size_t most)
    {
        std::vector<std::wstring> labels;
        for(const CalendarEvent& e:app.events)
            if(e.end>now&&e.start<until&&labels.size()<most)labels.push_back(EventLabel(e,now));
        return labels;
    }

    // The to-dos, those due soonest first (then the rest in the order they came).
    std::vector<const TodoItem*> TodosBySoonest()
    {
        std::vector<const TodoItem*> items;
        for(const auto& t:app.planner.Todos())items.push_back(&t);
        std::stable_sort(items.begin(),items.end(),[](const TodoItem* a,const TodoItem* b){
            if(a->due.empty()!=b->due.empty())return b->due.empty();
            return a->due<b->due;});
        return items;
    }

    // A to-do item as she says it: 「レポート」（金曜まで） / "report" (due Fri, Oct 30).
    std::wstring TodoLabel(const TodoItem& t,std::int64_t now)
    {
        std::wstring label=Quoted(t.text);
        if(t.due.empty())return label;
        if(t.due<DateOf(now))return label+T(L"（期限切れ）",L" (overdue)");
        return label+Fmt(T(L"（{0}まで）",L" (due {0})"),{DayText(t.due,now)});
    }

    // The to-dos due by tomorrow (or overdue) not yet told about today.
    std::vector<const TodoItem*> DeadlinesToTell(std::int64_t now)
    {
        const std::wstring today=DateOf(now),tomorrow=DateOf(DayEnd(now)+3600);
        std::vector<const TodoItem*> items;
        for(const TodoItem* t:TodosBySoonest())
            if(!t->due.empty()&&t->due<=tomorrow&&t->told!=today)items.push_back(t);
        return items;
    }

    void MarkDeadlinesTold(std::int64_t now)
    {
        const std::wstring today=DateOf(now);
        std::vector<int> ids;
        for(const TodoItem* t:DeadlinesToTell(now))ids.push_back(t->id);
        for(const int id:ids)app.planner.SetTodoTold(id,today);
        if(!ids.empty())SavePlanner();
    }

    // Today's reminders (timers aside) and the to-dos, as she says them and as facts for the
    // LLM. False when there are none.
    bool TodaySummary(std::wstring& line,std::wstring& facts)
    {
        const std::int64_t now=Now(),end=DayEnd(now);
        std::vector<std::wstring> reminders,todos;
        for(const Alarm& a:app.planner.Alarms())
        {
            if(a.timerSeconds>0||a.due>=end||reminders.size()>=6)continue;
            const std::wstring when=TimeText(a.due,now);
            if(English())reminders.push_back((a.text.empty()?std::wstring(L"a reminder"):Quoted(a.text))+L" at "+when);
            else reminders.push_back(when+L"に"+(a.text.empty()?std::wstring(L"お知らせ"):Quoted(a.text)));
        }
        const auto items=TodosBySoonest();
        for(size_t i=0;i<items.size()&&i<5;++i)todos.push_back(TodoLabel(*items[i],now));
        if(items.size()>5)todos.push_back(Fmt(T(L"ほか{0}件",L"and {0} more"),{std::to_wstring(items.size()-5)}));
        const std::vector<std::wstring> calendar=EventLabels(now,end,6);
        if(reminders.empty()&&todos.empty()&&calendar.empty())return false;
        line.clear();
        facts.clear();
        if(!calendar.empty())
        {
            line+=Fmt(T(L"カレンダーには、{0}があるよ。",L"On your calendar today: {0}."),{Listing(calendar)});
            facts+=Fmt(T(L"今日のカレンダーの予定：{0}。",L"Today's calendar: {0}. "),{Listing(calendar)});
        }
        if(!reminders.empty())
        {
            line+=(line.empty()?L"":(English()?L" ":L""))+Fmt(T(L"今日は、{0}があるよ。",L"Today you have {0}."),{Listing(reminders)});
            facts+=Fmt(T(L"今日のリマインダー：{0}。",L"Today's reminders: {0}. "),{Listing(reminders)});
        }
        if(!todos.empty())
        {
            const std::wstring count=std::to_wstring(items.size());
            line+=(line.empty()?L"":(English()?L" ":L""))+Fmt(T(L"やることは{0}件、{1}だよ。",L"You have {0} to-dos: {1}."),{count,Listing(todos)});
            facts+=Fmt(T(L"やることリスト（{0}件）：{1}。",L"To-do list ({0}): {1}."),{count,Listing(todos)});
        }
        return true;
    }

    // Focus mode, as she says where it stands: "集中モードの作業中、あと12分".
    std::wstring FocusState()
    {
        const auto& f=app.focus;
        const std::int64_t left=std::max<std::int64_t>(60,(f.phaseEnd-Now()+59)/60*60);
        return Fmt(f.resting?T(L"集中モードの休憩中、あと{0}",L"focus mode break, {0} left"):T(L"集中モードの作業中、あと{0}",L"focus mode work, {0} left"),
                   {DurationText(static_cast<int>(left))});
    }

    // Do what the user asked (said: their words; empty for the menu's quick timers). She says
    // what was done, in her own words when the LLM is there (the facts go to it in brackets);
    // lists are read out as they are.
    void DoPlan(Mascot& m,const PlanRequest& r,const std::wstring& said)
    {
        using Kind=PlanRequest::Kind;
        const std::int64_t now=Now();
        std::wstring line,news;
        bool own=true;
        switch(r.kind)
        {
        case Kind::Timer:
        case Kind::Remind:
        {
            Alarm a;
            a.due=r.due;
            a.timerSeconds=r.kind==Kind::Timer?r.seconds:0;
            a.daily=r.daily;
            a.text=r.text;
            if(!app.planner.AddAlarm(a)){line=T(L"ごめんね、もうこれ以上は覚えきれないよ。",L"Sorry, I can't keep track of any more.");own=false;break;}
            const std::wstring what=r.text.empty()?std::wstring():Quoted(r.text);
            if(r.kind==Kind::Timer)
            {
                const std::wstring length=DurationText(r.seconds);
                line=what.empty()?Fmt(T(L"{0}たったら知らせるね。",L"Okay, I'll let you know in {0}."),{length})
                                 :Fmt(T(L"{0}たったら、{1}って知らせるね。",L"Okay, in {0} I'll remind you: {1}."),{length,what});
                news=Fmt(T(L"頼まれて、{0}のタイマーをセットしました",L"You were asked to, and set a {0} timer"),{length});
            }
            else
            {
                const std::wstring when=AtTime(r.due,now,r.daily);
                line=what.empty()?Fmt(T(L"{0}になったら知らせるね。",L"Okay, I'll let you know {0}."),{when})
                                 :Fmt(T(L"{0}に、{1}って知らせるね。",L"Okay, I'll remind you {0}: {1}."),{when,what});
                news=Fmt(T(L"頼まれて、{0}に知らせるリマインダーをセットしました",L"You were asked to, and set a reminder for {0}"),{when});
            }
            if(!what.empty())news+=Fmt(T(L"（知らせる内容：{0}）",L" (about: {0})"),{what});
            break;
        }
        case Kind::TodoAdd:
        {
            if(!app.planner.AddTodo(r.text,TodayDate(),r.deadline)){line=T(L"ごめんね、やることがいっぱいで、もう入らないよ。",L"Sorry, the to-do list is full.");own=false;break;}
            const std::wstring count=std::to_wstring(app.planner.Todos().size());
            if(r.deadline.empty())
            {
                line=Fmt(T(L"{0}を、やることに入れておいたよ（いま{1}件）。",L"Added {0} to your to-do list ({1} in all)."),{Quoted(r.text),count});
                news=Fmt(T(L"頼まれて、やることリストに{0}を追加しました（全部で{1}件）",L"You were asked to, and added {0} to the to-do list ({1} in all)"),{Quoted(r.text),count});
            }
            else
            {
                const std::wstring day=DayText(r.deadline,now);
                line=Fmt(T(L"{0}を、{1}までのやることに入れておいたよ。近くなったら知らせるね（いま{2}件）。",L"Added {0} to your to-do list, due {1}. I'll remind you when it's close ({2} in all)."),{Quoted(r.text),day,count});
                news=Fmt(T(L"頼まれて、やることリストに{0}を、期限{1}で追加しました（全部で{2}件）。期限の前日と当日に知らせます",
                           L"You were asked to, and added {0} to the to-do list, due {1} ({2} in all). You will remind them the day before and on the day"),{Quoted(r.text),day,count});
            }
            break;
        }
        case Kind::TodoDone:
        {
            const int id=app.planner.FindTodo(r.text);
            const auto& todos=app.planner.Todos();
            const auto item=std::find_if(todos.begin(),todos.end(),[id](const TodoItem& t){return t.id==id;});
            if(item==todos.end()){line=Fmt(T(L"やることに{0}は見つからなかったよ。",L"I couldn't find {0} on your to-do list."),{Quoted(r.text)});own=false;break;}
            const std::wstring text=item->text;
            app.planner.RemoveTodo(id);
            line=Fmt(T(L"{0}、済みにしたよ。おつかれさま！",L"Checked off {0}. Nice work!"),{Quoted(text)});
            news=Fmt(T(L"話し相手がやることリストの{0}を済ませたので、リストから外しました",L"They finished {0} on their to-do list, so you took it off the list"),{Quoted(text)});
            break;
        }
        case Kind::TodoList:
        {
            own=false;
            const auto todos=TodosBySoonest();
            if(todos.empty()){line=T(L"いま、やることは入ってないよ。",L"Your to-do list is empty.");break;}
            std::vector<std::wstring> items;
            for(size_t i=0;i<todos.size()&&i<10;++i)items.push_back(TodoLabel(*todos[i],now));
            if(todos.size()>10)items.push_back(Fmt(T(L"ほか{0}件",L"and {0} more"),{std::to_wstring(todos.size()-10)}));
            line=Fmt(T(L"やることは{0}件。{1}だよ。",L"You have {0} to-dos: {1}."),{std::to_wstring(todos.size()),Listing(items)});
            break;
        }
        case Kind::AlarmList:
        {
            own=false;
            const auto& alarms=app.planner.Alarms();
            const std::vector<std::wstring> calendar=EventLabels(now,DayEnd(DayEnd(now)+3600),6);
            const std::wstring onCalendar=calendar.empty()?std::wstring():Fmt(T(L"カレンダーには、{0}があるよ。",L"On your calendar: {0}."),{Listing(calendar)});
            if(alarms.empty())
            {
                line=onCalendar.empty()?T(L"いまセットしているタイマーやリマインダーはないよ。",L"No timers or reminders are set."):onCalendar;
                if(app.focus.on)line=FocusState()+T(L"。",L". ")+line;
                break;
            }
            std::vector<std::wstring> items;
            for(size_t i=0;i<alarms.size()&&i<6;++i)
            {
                const Alarm& a=alarms[i];
                const std::wstring what=a.text.empty()?std::wstring():(English()?L" ":L"の")+Quoted(a.text);
                if(a.timerSeconds>0)
                {
                    const std::int64_t left=std::max<std::int64_t>(1,a.due-now);
                    const int shown=static_cast<int>(left>=60?(left+59)/60*60:left);
                    items.push_back(Fmt(T(L"{0}のタイマー{1}（あと{2}）",L"a {0} timer{1} ({2} left)"),{DurationText(a.timerSeconds),what,DurationText(shown)}));
                }
                else items.push_back(Fmt(T(L"{0}{1}",L"{0}{1}"),{TimeText(a.due,now,a.daily),what}));
            }
            if(alarms.size()>6)items.push_back(Fmt(T(L"ほか{0}件",L"and {0} more"),{std::to_wstring(alarms.size()-6)}));
            line=Fmt(T(L"セットしているのは、{0}だよ。",L"Set: {0}."),{Listing(items,L"、")});
            if(!onCalendar.empty())line+=(English()?L" ":L"")+onCalendar;
            if(app.focus.on)line=FocusState()+T(L"。",L". ")+line;
            break;
        }
        case Kind::Today:
        {
            own=false;
            std::wstring facts;
            if(!TodaySummary(line,facts))line=T(L"今日は、リマインダーもやることも入ってないよ。",L"Nothing for today: no reminders and no to-dos.");
            MarkDeadlinesTold(now);
            if(app.focus.on)line=FocusState()+T(L"。",L". ")+line;
            break;
        }
        case Kind::FocusStart:
        {
            auto& f=app.focus;
            if(f.on){line=Fmt(T(L"もう集中モード中だよ（{0}）。",L"Focus mode is already on ({0})."),{FocusState()});own=false;break;}
            f={true,false,now+app.settings.focusMinutes*60,0};
            const std::wstring work=std::to_wstring(app.settings.focusMinutes),rest=std::to_wstring(app.settings.focusBreakMinutes);
            line=Fmt(T(L"集中モード、始めるね。{0}分たったら声をかけるよ。がんばって！",L"Focus mode on. I'll tell you when {0} minutes are up. You can do it!"),{work});
            news=Fmt(T(L"頼まれて、集中モード（{0}分の作業と{1}分の休憩をくり返す）を始めました。作業の間は話しかけません",
                       L"You were asked to, and started focus mode ({0} minutes of work and {1} of break, in turn). You will not chat during work"),{work,rest});
            break;
        }
        case Kind::FocusStop:
        {
            auto& f=app.focus;
            if(!f.on){line=T(L"いまは集中モードじゃないよ。",L"Focus mode isn't on.");own=false;break;}
            const int rounds=f.rounds;
            f=App::Focus{};
            app.activeSince=-1;   // its breaks were rests: the count of time at the computer starts again
            line=rounds>0?Fmt(T(L"集中モード、おしまい。{0}回がんばったね。おつかれさま！",L"Focus mode off. {0} rounds done. Nice work!"),{std::to_wstring(rounds)})
                         :T(L"集中モード、おしまい。おつかれさま！",L"Focus mode off. Nice work!");
            news=Fmt(T(L"頼まれて、集中モードを終えました（作業{0}回）",L"You were asked to, and ended focus mode ({0} rounds of work)"),{std::to_wstring(rounds)});
            break;
        }
        case Kind::CancelTimers:
        {
            const int stopped=app.planner.RemoveTimers();
            const bool ringing=app.alarmChoice.IsOpen();
            if(ringing){app.alarmChoice.Close();app.told.clear();app.alarmTeller=nullptr;}
            line=stopped?T(L"タイマーを止めたよ。",L"I stopped the timer."):ringing?T(L"わかった、止めたよ。",L"Okay, stopped."):T(L"動いているタイマーはないよ。",L"There's no timer running.");
            news=T(L"頼まれて、タイマーを止めました",L"You were asked to, and stopped the timer");
            own=stopped>0;
            break;
        }
        case Kind::None:
            return;
        }
        SavePlanner();
        Trace(L"%.1f plan: %d %s",app.clock,static_cast<int>(r.kind),line.c_str());
        if(said.empty()||!own||!ChatReady(m,false)){SayFixed(m,line);return;}
        m.fallbackLine=line;
        m.conversation.push_back({L"user",said+L"\n"+(English()?L"(":L"（")+news+
                                  T(L"。引き受けたことを、あなたの言葉で短く伝えてください）",L". Tell them so, briefly, in your own words.)")});
        StartReply(m,said);
    }

    // A timer, reminder or to-do request in what the user said: read without the LLM, or,
    // when the words might be one put another way, by the LLM first (the bubble's dots run
    // meanwhile). False: just talk.
    bool HandlePlan(Mascot& m,const std::wstring& said)
    {
        PlanRequest r=ReadPlanRequest(said,Now());
        if(r.kind==PlanRequest::Kind::TodoDone&&!r.sure&&!app.planner.FindTodo(r.text))r.kind=PlanRequest::Kind::None;
        if(r.kind!=PlanRequest::Kind::None){DoPlan(m,r,said);return true;}
        if(!MightBePlanRequest(said)||!ChatReady(m,false))return false;
        if(!m.planChat)m.planChat=std::make_unique<LocalChat>(m.hwnd);
        if(m.chat)m.chat->Cancel();
        m.chatRequest=0;
        m.speech.Reset();
        if(m.voice)m.voice->CancelAll();
        m.planSaid=said;
        m.planReply.clear();
        m.planRequest=m.planChat->Start(app.settings.chatEndpoint,app.settings.chatModel,{
            {L"system",T(L"あなたは依頼を命令に直す係です。指示された形だけで答えます。",L"You turn requests into commands. You answer only in the form asked.")},
            {L"user",PlanCommandPrompt(said,Now())}});
        m.bubble.Begin(m.character.name);
        Trace(L"%.1f plan: asking the LLM (request %d)",app.clock,m.planRequest);
        return true;
    }

    void OnPlanDone(Mascot& m,const std::wstring& error)
    {
        const std::wstring said=std::exchange(m.planSaid,{});
        const PlanRequest r=error.empty()?ReadPlanCommand(CleanReply(m.planReply+L"\n"),Now()):PlanRequest{};
        Trace(L"%.1f plan: the LLM says %s",app.clock,error.empty()?m.planReply.c_str():error.c_str());
        if(r.kind!=PlanRequest::Kind::None){DoPlan(m,r,said);return;}
        if(!ChatReady(m,true))return;
        m.conversation.push_back({L"user",said});
        StartReply(m,said);
    }

    void OnAlarmAnswer(bool gotIt)
    {
        Mascot* m=std::exchange(app.alarmTeller,nullptr);
        const std::vector<Alarm> told=std::exchange(app.told,{});
        if(gotIt||!m)return;
        // Again in five minutes: each comes back as a reminder at that time.
        for(Alarm a:told)
        {
            a.due=Now()+5*60;
            a.daily=false;
            a.timerSeconds=0;
            app.planner.AddAlarm(a);
        }
        SavePlanner();
        SayFixed(*m,T(L"じゃあ、5分後にもう一度言うね。",L"Okay, I'll tell you again in 5 minutes."));
    }

    // Who tells what has come due: a character free to talk, or, after a few seconds of
    // waiting (waited), one busy too (she cuts in). Null for nobody yet.
    Mascot* PickTeller(double waited)
    {
        if(waited<8&&AnyoneTalking())return nullptr;   // not over someone else at first
        std::vector<Mascot*> free;
        for(const auto& owned:app.mascots)
        {
            Mascot& c=*owned;
            if((&c==app.asker||&c==app.alarmTeller)&&waited<30)continue;
            const bool busy=(c.chat&&c.chat->Busy())||c.bubble.Talking()||c.speech.Speaking()||(c.planChat&&c.planChat->Busy());
            if(waited>=30||(waited>=8&&c.place==Place::Ground)||(FreeToTalk(c)&&!busy))free.push_back(&c);
        }
        if(free.empty())return nullptr;
        return free[std::min(free.size()-1,static_cast<size_t>(app.Random01()*free.size()))];
    }

    // It is time: she says so (in her own words when the LLM is there), with a chime, and
    // わかった／5分後に under her until answered.
    void TellAlarms(Mascot& m,std::vector<Alarm> alarms)
    {
        const std::int64_t now=Now();
        std::vector<std::wstring> lines,facts;
        for(const Alarm& a:alarms)
        {
            const std::wstring clock=TimeText(a.due,a.due);
            const std::wstring what=a.text.empty()?std::wstring():Quoted(a.text);
            if(now-a.due>120)
            {
                lines.push_back(what.empty()?Fmt(T(L"{0}の時間、過ぎちゃってたよ。",L"It's past {0}, the time you asked me about."),{clock})
                                            :Fmt(T(L"{0}の{1}、時間を過ぎちゃってたよ。",L"It's past {0}: {1}."),{clock,what}));
                facts.push_back(Fmt(T(L"頼まれていた{0}の知らせの時間を、アプリが起動していなかったので過ぎてしまいました",L"The {0} reminder you were asked for went by while the app was not running"),{clock}));
            }
            else if(a.timerSeconds>0)
            {
                const std::wstring length=DurationText(a.timerSeconds);
                lines.push_back(what.empty()?Fmt(T(L"{0}たったよ！時間だよ。",L"Time's up! It's been {0}."),{length})
                                            :Fmt(T(L"時間だよ！{1}（{0}たったよ）",L"Time's up! {1} ({0} have passed)."),{length,what}));
                facts.push_back(Fmt(T(L"頼まれていた{0}のタイマーの時間になりました",L"The {0} timer you were asked to set is up"),{length}));
            }
            else
            {
                lines.push_back(what.empty()?Fmt(T(L"{0}になったよ！",L"It's {0}!"),{clock})
                                            :Fmt(T(L"{0}だよ。{1}の時間だよ！",L"It's {0}. Time for {1}!"),{clock,what}));
                facts.push_back(Fmt(T(L"頼まれていた{0}になりました",L"It is {0}, the time you were asked to tell them"),{clock}));
            }
            if(!what.empty())facts.back()+=Fmt(T(L"（知らせる内容：{0}）",L" (about: {0})"),{what});
        }
        std::wstring line;
        for(const auto& each:lines)line+=(line.empty()||!English()?L"":L" ")+each;
        Trace(L"%.1f alarm: %s",app.clock,line.c_str());
        if(m.dozing){m.dozing=false;m.model.SetDozing(false);}
        if(ChatReady(m,false))
            SpeakFirst(m,Listing(facts,L"。")+T(L"。そのことを、あなたの言葉で知らせて",L". Tell them so, in your own words"),line);
        else SayFixed(m,line);
        PlaySoundW(L"SystemNotification",nullptr,SND_ALIAS|SND_ASYNC|SND_NODEFAULT);
        app.alarmChoice.Close();
        app.told=std::move(alarms);
        app.alarmTeller=&m;
        const RECT r=FrameRect(m);
        const POINT ground=GroundPointOnScreen(m);
        app.alarmChoice.Open(m.hwnd,{(r.left+r.right)/2,ground.y-static_cast<LONG>(ModelHeightPixels(m)*0.12)},
                             T(L"わかった",L"Got it"),T(L"5分後にもう一度",L"Again in 5 min"),OnAlarmAnswer);
    }

    // Once a second: what has come due is told by a character free to talk; one busy is
    // interrupted after a few seconds of waiting (a talk between two ends at once).
    void UpdatePlanner()
    {
        if(app.diagnostic||app.mascots.empty())return;
        if(app.clock>=app.nextPlannerCheck)
        {
            app.nextPlannerCheck=app.clock+1;
            std::vector<Alarm> due=app.planner.TakeDue(Now());
            if(!due.empty())
            {
                app.ringing.insert(app.ringing.end(),due.begin(),due.end());
                if(app.ringingSince<0)app.ringingSince=app.clock;
                SavePlanner();
            }
        }
        if(!app.notices.empty()&&app.noticesSince<0)app.noticesSince=app.clock;
        // Just started: let the characters come on first.
        if(app.updating||app.clock<5)return;
        if(!app.ringing.empty())
        {
            if(app.banter.stage!=App::Banter::Stage::None)EndBanter(L"an alarm");
            if(Mascot* m=PickTeller(app.clock-app.ringingSince))
            {
                TellAlarms(*m,std::exchange(app.ringing,{}));
                app.ringingSince=-1;
            }
            return;
        }
        if(!app.notices.empty())
        {
            if(app.banter.stage!=App::Banter::Stage::None)EndBanter(L"a notice");
            if(Mascot* m=PickTeller(app.clock-app.noticesSince))
            {
                const App::Notice notice=app.notices.front();
                app.notices.erase(app.notices.begin());
                app.noticesSince=app.notices.empty()?-1:app.clock;
                Trace(L"%.1f notice: %s",app.clock,notice.line.c_str());
                if(m->dozing){m->dozing=false;m->model.SetDozing(false);}
                if(ChatReady(*m,false))SpeakFirst(*m,notice.facts+T(L"。そのことを、あなたの言葉で伝えて",L". Tell them so, in your own words"),notice.line);
                else SayFixed(*m,notice.line);
                PlaySoundW(L"SystemNotification",nullptr,SND_ALIAS|SND_ASYNC|SND_NODEFAULT);
            }
        }
    }

    // Focus mode's turns: when a work ends, a break is suggested (every fourth one three times
    // as long), and when the break ends, the next work starts. Away for a quarter of an hour
    // in the middle of a work, it ends by itself.
    void UpdateFocus()
    {
        auto& f=app.focus;
        if(!f.on||app.diagnostic)return;
        if(!f.resting&&InputIdleSeconds()>=15*60)
        {
            Trace(L"%.1f focus: ended (the user is away)",app.clock);
            f=App::Focus{};
            app.activeSince=-1;
            return;
        }
        const std::int64_t now=Now();
        if(now<f.phaseEnd)return;
        const std::wstring work=std::to_wstring(app.settings.focusMinutes);
        if(!f.resting)
        {
            ++f.rounds;
            const bool longBreak=f.rounds%4==0;
            const int minutes=app.settings.focusBreakMinutes*(longBreak?3:1);
            f.resting=true;
            f.phaseEnd=now+minutes*60;
            const std::wstring rest=std::to_wstring(minutes),rounds=std::to_wstring(f.rounds);
            app.notices.push_back({
                longBreak?Fmt(T(L"{0}回がんばったね！{1}分、ゆっくり休憩しよう。",L"That's {0} rounds! Take a good {1}-minute break."),{rounds,rest})
                         :Fmt(T(L"{0}分たったよ。{1}分休憩しよう！",L"{0} minutes are up. Time for a {1}-minute break!"),{work,rest}),
                Fmt(T(L"集中モードで、{0}分の作業が終わりました（{1}回目）。{2}分の休憩をすすめて",L"In focus mode, {0} minutes of work are done (round {1}). Suggest a {2}-minute break"),{work,rounds,rest})});
        }
        else
        {
            f.resting=false;
            f.phaseEnd=now+app.settings.focusMinutes*60;
            app.notices.push_back({
                Fmt(T(L"休憩おわり！また{0}分がんばろう。",L"Break's over! Another {0} minutes, let's go."),{work}),
                Fmt(T(L"集中モードの休憩が終わりました。次の{0}分の作業を始めるよう、はげまして",L"The focus mode break is over. Cheer them on to start the next {0} minutes of work"),{work})});
        }
    }

    // Long at the computer without a break (five minutes away counts as one): a break is
    // suggested, then again after as long once more. Not in focus mode, which has its own.
    void UpdateRest()
    {
        if(app.diagnostic)return;
        const double idle=InputIdleSeconds();
        if(idle>=5*60){app.activeSince=-1;return;}
        if(app.activeSince<0){app.activeSince=app.clock-idle;app.nextRestAt=-1;}
        const int every=app.settings.restMinutes;
        if(every<=0||app.focus.on||app.paused)return;
        const double due=app.nextRestAt>=0?app.nextRestAt:app.activeSince+every*60.0;
        if(app.clock<due)return;
        app.nextRestAt=app.clock+every*60.0;
        const int used=static_cast<int>((app.clock-app.activeSince)/60+0.5)*60;
        app.notices.push_back({
            Fmt(T(L"{0}、休まずにパソコンを使ってるよ。少し休憩しない？目を休めたり、体を伸ばしたりしよう。",
                  L"You've been at the computer for {0} without a break. How about a short rest? Rest your eyes, stretch a little."),{DurationText(used)}),
            Fmt(T(L"話し相手が{0}ほど、休まずにパソコンを使い続けています。少し休憩するよう、やさしくすすめて",
                  L"The person you talk with has been using the computer for about {0} without a break. Gently suggest a short rest"),{DurationText(used)})});
    }

    // To-dos due tomorrow, today or already: told once a day, when the user is at the computer
    // (in the morning, the morning summary tells them; in focus mode, at a break).
    void UpdateDeadlines()
    {
        if(app.diagnostic||app.clock<20||!app.notices.empty())return;
        SYSTEMTIME local{};
        GetLocalTime(&local);
        if(local.wHour<7||(app.settings.morningSummary&&local.wHour<10)||InputIdleSeconds()>90||(app.focus.on&&!app.focus.resting))return;
        const std::int64_t now=Now();
        const auto items=DeadlinesToTell(now);
        if(items.empty())return;
        std::vector<std::wstring> labels;
        for(const TodoItem* t:items)labels.push_back(TodoLabel(*t,now));
        MarkDeadlinesTold(now);
        app.notices.push_back({
            Fmt(T(L"やることの期限が近いよ。{0}。",L"Deadlines coming up: {0}."),{Listing(labels)}),
            Fmt(T(L"やることリストの期限が近づいています：{0}。忘れないよう知らせて",L"Deadlines on the to-do list are close: {0}. Remind them so they don't forget"),{Listing(labels)})});
    }

    // Every half hour (with its address set), the calendar is read again; a few minutes before
    // an event (not all-day ones), she says so.
    void UpdateCalendar()
    {
        if(app.diagnostic||app.settings.calendarAddress.empty())return;
        if(app.clock>=app.nextCalendarFetch)
        {
            app.nextCalendarFetch=app.clock+30*60;
            if(!app.calendar)app.calendar=std::make_unique<CalendarFeed>(app.hwnd);
            app.calendar->Fetch(app.settings.calendarAddress,false);
        }
        const int lead=app.settings.calendarLeadMinutes;
        if(lead<=0)return;
        const std::int64_t now=Now();
        for(const CalendarEvent& e:app.events)
        {
            if(e.allDay||e.start<=now||e.start-now>lead*60||app.eventsTold.count(e.key))continue;
            app.eventsTold.insert(e.key);
            const std::wstring minutes=std::to_wstring(std::max<std::int64_t>(1,(e.start-now+30)/60));
            app.notices.push_back({
                Fmt(T(L"あと{0}分で{1}だよ（{2}から）。",L"{1} starts in {0} minutes ({2})."),{minutes,Quoted(e.title),TimeText(e.start,now)}),
                Fmt(T(L"カレンダーの予定{0}が、あと{1}分（{2}）で始まります。そのことを知らせて",L"The calendar event {0} starts in {1} minutes ({2}). Let them know"),{Quoted(e.title),minutes,TimeText(e.start,now)})});
        }
    }

    void OnCalendarFetched(CalendarResult* fetched)
    {
        std::unique_ptr<CalendarResult> result(fetched);
        if(app.settings.calendarAddress.empty())return;   // taken off meanwhile
        // Never the address in the log: only what came of it.
        Trace(L"%.1f calendar: %s",app.clock,result->error.empty()?(std::to_wstring(result->events.size())+L" events").c_str():result->error.c_str());
        if(!result->error.empty())
        {
            if(result->report)ShowMessage(T(L"Googleカレンダーを読み込めませんでした。\n\n",L"Could not read Google Calendar.\n\n")+result->error,MB_ICONWARNING);
            return;   // the events read last time stay
        }
        app.events=std::move(result->events);
        if(result->report)
        {
            const std::int64_t now=Now();
            const size_t soon=EventLabels(now,DayEnd(DayEnd(now)+3600),1000).size();
            app.notices.push_back({
                Fmt(T(L"Googleカレンダーを読み込んだよ。今日と明日の予定は{0}件だね。",L"I've read your Google Calendar: {0} events today and tomorrow."),{std::to_wstring(soon)}),
                Fmt(T(L"話し相手のGoogleカレンダーを読み込めるようになりました（今日と明日の予定は{0}件）。これからは予定の前に知らせます",
                      L"You can now read their Google Calendar ({0} events today and tomorrow). From now on you will tell them before events"),{std::to_wstring(soon)})});
        }
    }

    // The first morning greeting tells today's reminders and to-dos (with them, the greeting
    // is this, in place of the usual one; without the LLM, in fixed words).
    void MaybeMorningSummary()
    {
        if(!app.settings.morningSummary||app.clock<8||app.diagnostic||app.banter.stage!=App::Banter::Stage::None)return;
        SYSTEMTIME now{};
        GetLocalTime(&now);
        if(now.wHour<5||now.wHour>=10||InputIdleSeconds()>90)return;
        wchar_t key[40]{};
        swprintf_s(key,L"%04d-%02d-%02d morning",now.wYear,now.wMonth,now.wDay);
        if(app.settings.lastGreeting==key)return;
        std::wstring line,facts;
        if(!TodaySummary(line,facts))return;   // the usual greeting, if any
        Mascot* m=PickTeller(0);
        if(!m)return;
        app.settings.lastGreeting=key;
        app.settings.Save(app.settingsPath);
        app.nextChatterAt=std::max(app.nextChatterAt,app.clock+20*60);
        Trace(L"%.1f morning: %s",app.clock,line.c_str());
        MarkDeadlinesTold(Now());
        if(ChatReady(*m,false))
            SpeakFirst(*m,T(L"朝です。話し相手がパソコンを使い始めました。朝のあいさつをして、今日の予定を伝えて。",
                            L"It is morning. The person you talk with has started using the computer. Say good morning, and tell them today's plans. ")+facts,
                       T(L"おはよう！",L"Good morning! ")+line);
        else SayFixed(*m,T(L"おはよう！",L"Good morning! ")+line);
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
        const std::wstring partner=app.settings.userName.empty()?std::wstring(T(L"話し相手",L"the user"))
                                                                 :Fmt(T(L"話し相手（{0}）",L"the user ({0})"),{app.settings.userName});
        std::wstring dialogue;
        for(const auto& said:m.unmemorized)dialogue+=(said.role==L"user"?std::wstring(T(L"話し相手：",L"User: ")):name+T(L"：",L": "))+said.content+L"\n";
        std::wstring known;
        for(const auto& e:m.memory.Entries())known+=L"- "+e.text+L"\n";
        if(known.size()>3000)known=known.substr(known.size()-3000);
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t today[32]{};
        swprintf_s(today,English()?L"%04d-%02d-%02d":L"%d年%d月%d日",now.wYear,now.wMonth,now.wDay);
        std::vector<ChatMessage> messages;
        messages.push_back({L"system",T(L"あなたは会話の記録係です。指示されたことだけを、指示された形で書きます。",
                                        L"You keep the record of conversations. You write only what you are asked, in the form asked.")});
        if(English())messages.push_back({L"user",
            L"Below is a conversation between \""+name+L"\" and "+partner+L". Today is "+today+L".\n"
            L"Write down new things about the user that \""+name+L"\" should remember in future conversations.\n"
            L"What to remember: what the user likes and dislikes, plans and events, family and pets, work, school and hobbies, promises between the two, "
            L"and anything the user asked to be remembered.\n"
            L"How to write:\n"
            L"- One per line, a short sentence with its subject, like \"The user ...\", three at most.\n"
            L"- Turn \"tomorrow\", \"next week\" and the like into dates, from today's date.\n"
            L"- Do not write greetings, passing small talk, or what "+name+L" said.\n"
            L"- Do not repeat what is already remembered.\n"
            L"- If there is nothing to write, write only \"none\".\n\n"
            L"Already remembered:\n"+(known.empty()?std::wstring(L"(nothing yet)\n"):known)+L"\n"
            L"Conversation:\n"+dialogue});
        else messages.push_back({L"user",
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
            if(line.empty()||line.find(L"なし")==0||_wcsnicmp(line.c_str(),L"none",4)==0||line.size()>CharacterMemory::MaxLength)continue;
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
        // Clear of the other characters (head to feet, about a shoulder's width each way) and
        // of their bubbles that came up earlier (a later one makes room, never both).
        std::vector<RECT> avoid;
        for(const auto& owned:app.mascots)
        {
            Mascot& other=*owned;
            if(&other==&m)continue;
            const RECT f=FrameRect(other);
            const double unit=ModelHeightPixels(other),cx=CenterX(other);
            LONG top=f.top;
            if(std::array<float,3> head{};other.model.HeadPosition(head))top=f.top+other.renderer.Project(head).y-static_cast<LONG>(unit*0.12);
            avoid.push_back({static_cast<LONG>(cx-unit*0.18),top,static_cast<LONG>(cx+unit*0.18),static_cast<LONG>(FeetLine(other))});
            if(RECT bubble{};other.bubble.Placed(bubble)&&other.bubble.Age()>m.bubble.Age())avoid.push_back(bubble);
        }
        m.bubble.SetAvoid(std::move(avoid));
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
                ShowMessage(T(L"このモデルは、すでにデスクトップにいます。",L"This model is already on the desktop."),MB_ICONINFORMATION);
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
        if(id>IdTimerMinutes&&id<=IdTimerMinutes+120)
        {
            PlanRequest r;
            r.kind=PlanRequest::Kind::Timer;
            r.seconds=static_cast<int>(id-IdTimerMinutes)*60;
            r.due=Now()+r.seconds;
            DoPlan(m,r,{});
            return;
        }
        switch(id)
        {
        case IdTalk:
            OpenChatInput(m);
            break;
        case IdPlanner:
            ShowPlannerWindow(m.hwnd,app.planner);
            break;
        case IdCancelTimers:
        case IdFocus:
        {
            PlanRequest r;
            r.kind=id==IdCancelTimers?PlanRequest::Kind::CancelTimers:app.focus.on?PlanRequest::Kind::FocusStop:PlanRequest::Kind::FocusStart;
            DoPlan(m,r,{});
            break;
        }
        case IdOpenModel:
        {
            auto path=OpenFileDialog(m.hwnd,T(L"PMX モデル (*.pmx)\0*.pmx\0",L"PMX model (*.pmx)\0*.pmx\0"),Own(m).modelPath);
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
            auto path=OpenFileDialog(m.hwnd,T(L"PMX モデル (*.pmx)\0*.pmx\0",L"PMX model (*.pmx)\0*.pmx\0"),Own(m).modelPath);
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
            auto path=OpenFileDialog(m.hwnd,T(L"VMD モーション (*.vmd)\0*.vmd\0",L"VMD motion (*.vmd)\0*.vmd\0"),Own(m).motionPath.empty()?Own(m).modelPath:Own(m).motionPath);
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
            const auto path=OpenFileDialog(m.hwnd,T(L"VMD モーション (*.vmd)\0*.vmd\0",L"VMD motion (*.vmd)\0*.vmd\0"),setting.empty()?Own(m).modelPath:setting);
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
                paths=OpenFilesDialog(m.hwnd,T(L"VMD モーション (*.vmd)\0*.vmd\0",L"VMD motion (*.vmd)\0*.vmd\0"),list.empty()?Own(m).modelPath:list.back());
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
                auto picked=OpenFilesDialog(m.hwnd,T(L"VMD モーション (*.vmd)\0*.vmd\0",L"VMD motion (*.vmd)\0*.vmd\0"),list.empty()?Own(m).modelPath:list.back());
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
                    RaiseAnswerBox();
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
                const int percent=std::clamp(app.settings.scale+steps*10,Settings::MinScale,Settings::MaxScale);
                if(percent!=app.settings.scale)ApplyScale(percent);
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
            if(m.planRequest&&static_cast<int>(lParam)==m.planRequest)
            {
                std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(wParam));
                if(m.planReply.size()<2000)m.planReply+=*text;
                return 0;
            }
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
            if(m.planRequest&&static_cast<int>(lParam)==m.planRequest)
            {
                std::unique_ptr<std::wstring> error(reinterpret_cast<std::wstring*>(wParam));
                m.planRequest=0;
                OnPlanDone(m,*error);
                return 0;
            }
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
        m.planChat.reset();
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
        case Updater::CheckedMessage:
            OnUpdateChecked(reinterpret_cast<Release*>(wParam));
            return 0;
        case Updater::DownloadedMessage:
            OnUpdateDownloaded(reinterpret_cast<std::wstring*>(wParam));
            return 0;
        case CalendarFeed::FetchedMessage:
            OnCalendarFetched(reinterpret_cast<CalendarResult*>(wParam));
            return 0;
        case WM_DESTROY:
            EndBanter(L"quitting");
            app.updateChoice.Close();
            app.alarmChoice.Close();
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
            if(!app.diagnostic)ShowMessage(T(L"待機モーションを読み込めなかったため、外しました。\n\n",L"The idle motions could not be loaded, so they were taken off.\n\n")+error,MB_ICONWARNING);
            own.idleMotionPaths.clear();
            error.clear();
        }

        m.extent=BaseExtent(m);
        const SIZE size=WindowSizeFor(m,app.settings.scale);
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
        if(!m.hwnd){error=T(L"ウィンドウを作成できませんでした。",L"Could not create the window.");return false;}
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
        if(!app.changes.empty())
        {
            EndBanter(L"the characters change");
            // The one asking about the update may go: ask again later.
            if(app.asker&&!app.updating){app.updateChoice.Close();app.asker=nullptr;}
            // So may the one telling the time: taken as heard.
            if(app.alarmTeller){app.alarmChoice.Close();app.alarmTeller=nullptr;app.told.clear();}
        }
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
            else if(arg==L"--pretend-version"&&i+1<argc)
            {
                PretendVersion(argv[++i]);   // an update to the latest release is offered (and checked at once)
                app.nextUpdateCheck=5;
            }
            else if(arg==L"--banter")
            {
                app.banterRequested=true;   // two characters start talking a few seconds after start
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
        // --calendar-test <in.ics> <out.txt>: the events an iCal file has from today for three
        // days, as they would be read from Google Calendar.
        if(argc>=4&&std::wstring(argv[1])==L"--calendar-test")
        {
            std::ifstream in(std::filesystem::path(argv[2]),std::ios::binary);
            std::stringstream buffer;
            buffer<<in.rdbuf();
            const std::int64_t now=Now(),from=DayEnd(now)-24*60*60;
            SetLanguage(L"ja");
            if(FILE* out=nullptr;_wfopen_s(&out,argv[3],L"w, ccs=UTF-8")==0&&out)
            {
                for(const CalendarEvent& e:ReadCalendar(buffer.str(),from,DayEnd(DayEnd(DayEnd(now)+3600)+3600)))
                    fwprintf(out,L"%s\t%s\t%s\n",(DayText(DateOf(e.start),now)+L" "+TimeText(e.start,e.start)).c_str(),e.allDay?L"all-day":L"",e.title.c_str());
                fclose(out);
            }
            LocalFree(argv);
            return 0;
        }
        // --plan-test <in.txt> <out.txt>: how each line (UTF-8) is read as a timer, reminder or
        // to-do request, without the LLM.
        if(argc>=4&&std::wstring(argv[1])==L"--plan-test")
        {
            std::ifstream in(std::filesystem::path(argv[2]),std::ios::binary);
            std::stringstream buffer;
            buffer<<in.rdbuf();
            const std::string bytes=buffer.str();
            std::wstring all(static_cast<size_t>(MultiByteToWideChar(CP_UTF8,0,bytes.data(),static_cast<int>(bytes.size()),nullptr,0)),L'\0');
            MultiByteToWideChar(CP_UTF8,0,bytes.data(),static_cast<int>(bytes.size()),all.data(),static_cast<int>(all.size()));
            static constexpr const wchar_t* Kinds[]={L"none",L"timer",L"remind",L"todo-add",L"todo-done",L"todo-list",L"alarm-list",L"cancel-timers",L"today",L"focus-start",L"focus-stop"};
            const std::int64_t now=Now();
            std::wstringstream lines(all);
            if(FILE* out=nullptr;_wfopen_s(&out,argv[3],L"w, ccs=UTF-8")==0&&out)
            {
                for(std::wstring line;std::getline(lines,line);)
                {
                    if(!line.empty()&&line.back()==L'\r')line.pop_back();
                    if(line.empty())continue;
                    const bool loose=line.rfind(L"~",0)==0;
                    const PlanRequest r=ReadPlanRequest(loose?line.substr(1):line,now,loose);
                    SetLanguage(std::any_of(line.begin(),line.end(),[](wchar_t c){return c>=0x3040;})?L"ja":L"en");
                    fwprintf(out,L"%s\t%s\t%s\t%s\t%s%s\t[%s]%s\n",line.c_str(),Kinds[static_cast<int>(r.kind)],
                             r.due?TimeText(r.due,now,r.daily).c_str():L"",r.seconds?DurationText(r.seconds).c_str():L"",
                             r.daily?L"daily":L"",r.sure?L"":L"unsure",r.text.c_str(),r.deadline.empty()?L"":(L" due "+r.deadline).c_str());
                }
                fclose(out);
            }
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

    // The language first: the message below comes before the settings are read in full.
    {
        Settings early;
        early.Load(Settings::DefaultPath());
        SetLanguage(early.language);
    }

    // One mascot at a time (diagnostic runs excepted). The mutex goes away with the process,
    // or just before a restart starts the next one.
    HANDLE instanceMutex=nullptr;
    if(snapshotPath.empty())
    {
        instanceMutex=CreateMutexW(nullptr,FALSE,L"Local\\MikuMikuDesktopPerch");
        if(instanceMutex&&GetLastError()==ERROR_ALREADY_EXISTS)
        {
            MessageBoxW(nullptr,T(L"MikuMikuDesktopPerch はすでに起動しています。\n\nタスクトレイのアイコンを右クリックすると、メニューを開けます。",L"MikuMikuDesktopPerch is already running.\n\nRight-click its icon in the notification area for the menu."),
                        L"MikuMikuDesktopPerch",MB_ICONINFORMATION);
            CloseHandle(instanceMutex);
            CoUninitialize();
            return 0;
        }
    }

    app.settingsPath=Settings::DefaultPath();
    app.settings.Load(app.settingsPath);
    SetLanguage(app.settings.language);
    app.diagnostic=!snapshotPath.empty();
    if(!app.diagnostic)app.planner.Load();
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
            modelPath=OpenFileDialog(nullptr,T(L"PMX モデル (*.pmx)\0*.pmx\0",L"PMX model (*.pmx)\0*.pmx\0"),{});
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
        ShowMessage(Fmt(T(L"「{0}」を読み込めなかったため、外しました。\n\n",L"{0} could not be loaded, so it was taken off.\n\n"),{app.settings.characters[slot].modelPath})+error,MB_ICONWARNING);
        app.settings.characters.erase(app.settings.characters.begin()+static_cast<std::ptrdiff_t>(slot));
    }
    app.settings.Save(app.settingsPath);
    app.nextChatterAt=Interval(app.settings.chatterMinutes);
    app.nextBanterAt=Interval(std::max(app.settings.banterMinutes,1));
    if(!app.diagnostic)Updater::CleanUp();

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
    if(snapshotScale>0)ApplyScale(std::clamp(snapshotScale/10*10,Settings::MinScale,Settings::MaxScale));

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
            if(const HWND planner=PlannerWindowHandle();planner&&IsDialogMessageW(planner,&msg))continue;
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
        if(!forcedActivity){MaybeMorningSummary();MaybeSpeakFirst();UpdateBanter();MaybeCheckForUpdate();MaybeAskUpdate();UpdateFocus();UpdateRest();UpdateDeadlines();UpdateCalendar();UpdatePlanner();}
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
            MakeRoom(m);
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
            UpdateFacing(m);
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
