#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace MikuMikuDesktopPerch
{
    // What the generated motion layer is doing. Idle plays the VMD (plus procedural layers).
    // Sit, Lie and Hang rest on an edge at world y=0 (a window's top edge).
    // Bound: a hop off one foot like a triple jump's first step (the dungeon's jump ahead);
    // SideHop: a hop to the side off the far foot (direction: toward +x or -x of the model).
    enum class Activity { Idle, Walk, Run, Jump, Sit, Lie, Hang, Grabbed, Fall, Bound, SideHop };
    // The feeling on the face while talking (made of the model's standard face morphs).
    enum class Expression { Neutral, Smile, Surprise, Troubled, Angry, Sad, Shy };

    // A Jump starts with this long a crouch before the body leaves the ground.
    constexpr double JumpCrouchSeconds=0.22;
    // A Bound: its take-off, flight and landing.
    constexpr double BoundTakeoffSeconds=0.16,BoundAirSeconds=0.5,BoundLandSeconds=0.3;
    // A SideHop's take-off, flight and landing.
    constexpr double SideTakeoffSeconds=0.13,SideAirSeconds=0.34,SideLandSeconds=0.24;

    // Immutable per-vertex skinning data (vertex buffer slot 1).
    struct StaticVertex
    {
        std::array<float,3> normal{};
        std::array<std::int32_t,4> bone{};
        std::array<float,4> weight{};   // SDEF: weight[1..3] = (r0-r1)/2, as in PMX::GPUSkinning
        std::array<float,3> sdefC{};
        std::uint32_t weightType=0;     // 0-2 BDEF1/2/4, 3 SDEF, 4 QDEF (drawn as BDEF4)
        float edge=1.0f;
    };
    static_assert(sizeof(StaticVertex)==64);

    // Morph-applied position/uv (vertex buffer slot 0), rewritten only when morphs change.
    struct DynamicVertex
    {
        std::array<float,3> position{};
        std::array<float,2> uv{};
    };
    static_assert(sizeof(DynamicVertex)==20);

    // Material after material morphs; rebuilt every update.
    struct MaterialState
    {
        std::array<float,4> diffuse{};
        std::array<float,3> specular{};
        float shininess=0;
        std::array<float,3> ambient{};
        std::array<float,4> edgeColor{};
        float edgeSize=0;
        std::array<float,4> textureMul{1,1,1,1},textureAdd{};
        std::array<float,4> sphereMul{1,1,1,1},sphereAdd{};
        std::array<float,4> toonMul{1,1,1,1},toonAdd{};
        int drawFlag=0;
        int texture=-1;     // index into TexturePaths(), -1 none
        int sphere=-1;
        int sphereMode=0;   // 0 none, 1 multiply, 2 add, 3 sub-texture (unsupported, treated as none)
        bool sharedToon=false;
        int toon=-1;        // sharedToon: 0..9 -> toon01..toon10, otherwise texture index
        std::uint32_t indexStart=0,indexCount=0;
    };

    class MascotModel
    {
    public:
        MascotModel();
        ~MascotModel();
        MascotModel(const MascotModel&)=delete;
        MascotModel& operator=(const MascotModel&)=delete;

        // idleMotionPaths (played while idle) may be empty: the generated standing pose. Throws
        // nothing; returns false with error text.
        bool Load(const std::wstring& modelPath,const std::vector<std::wstring>& idleMotionPaths,bool physics,int physicsFps,std::wstring& error);
        // Replace the idle motions without reloading the model. One motion loops; with several,
        // each plays through and the next is picked at random (never the same twice in a row),
        // crossfading between them. On error the current motions stay.
        bool SetIdleMotions(const std::vector<std::wstring>& paths,std::wstring& error);
        // Answer a click on the model (head: on the head). Standing, a click motion plays if
        // any are set (one at random, never the same twice in a row), else a built-in reaction;
        // sitting or hanging, a shy tilt of the head. False when it cannot react now (already
        // reacting, the opened motion playing, walking, held, ...).
        bool SetClickMotions(const std::vector<std::wstring>& paths,std::wstring& error);
        // Walking (which 0) or running (1) in place, used instead of the generated gait (an
        // empty path: back to the generated one). Fails for a motion whose feet never stay on
        // the ground long enough to measure its speed.
        bool SetGaitMotion(int which,const std::wstring& path,std::wstring& error);
        bool React(bool head);
        bool Reacting() const;
        // True between idle motions (or loops of one) and whenever there is none playing:
        // starting to walk or sit is held back until then.
        bool IdleMotionAtBreak() const;

        // A motion played over the idle pose now and then: PlayMotion blends it in, plays it
        // once and blends back out. An empty path removes it. Load drops it (set it again).
        bool SetMotion(const std::wstring& path,std::wstring& error);
        bool HasMotion() const;
        void PlayMotion();
        void StopMotion();          // blend out early
        bool MotionPlaying() const; // true until fully blended out
        // A gesture of the generated standing pose leads the head (look-at should pause).
        bool HeadBusy() const;
        // diagnostics; "clip<n>" (or "clip<n>@seconds", frozen there) plays built-in clip n
        bool ForceGesture(const std::wstring& name);
        // Built-in idle clips: VMDs in the exe (resources of type IDLECLIP), played now and then
        // among the gestures of the generated standing pose. Diagnostics can add VMD files
        // (--idle-clip) before the models load.
        static void AddIdleClipFile(const std::wstring& path);
        int IdleClipCount() const;
        // Every gesture and built-in clip that can be picked while standing: key ("clip:<name>"
        // for a clip) and label. SetDisabledGestures leaves out those keys (kept across Load).
        static std::vector<std::pair<std::wstring,std::wstring>> IdleGestureChoices();
        void SetDisabledGestures(const std::vector<std::wstring>& keys);
        int IdleClipPlays() const;   // how many have started (diagnostics)
        void SetPhysics(bool enabled);
        // The drawing rate, which physics keeps in step with (one frame period per Update).
        void SetFrameRate(int fps);
        // Solver passes per physics step: more settle long chains (hair) that jitter.
        void SetPhysicsIterations(int iterations);
        // Smooth the simulated bones' small, quick trembling (0 off .. 3 strong): stronger while
        // she holds still, weaker while she moves, and large swings pass through.
        void SetPhysicsFilter(int strength);
        bool PhysicsEnabled() const;

        // Procedural motion layered on top of the VMD (each can be toggled independently).
        void SetProcedural(bool blink,bool breath,bool lookAt);
        // Mouth shapes while a reply is "spoken": weights for the あ い う え お ん morphs
        // (those the model has), over whatever the face already shows. active=false lets go.
        void SetMouth(const std::array<float,6>& weights,bool active);
        // Ease the face into a feeling (Neutral: back to the face the motion gives). gesture:
        // now and then add a fitting gesture too, when standing.
        void SetExpression(Expression expression,bool gesture);
        // Dozing off: the eyes close and the head droops and nods, eased in over a few seconds;
        // waking is quicker.
        void SetDozing(bool dozing);
        // Play a standing gesture once by name (as ForceGesture names them); false if not now.
        bool Emote(const std::wstring& name);
        bool Dozing() const;
        // The model's name as written in the PMX file.
        std::wstring ModelName() const;
        // Desired look direction in radians: yaw > 0 turns toward screen right, pitch > 0 looks up.
        // active=false lets the head ease back to the motion's own pose.
        void SetLookTarget(float yaw,float pitch,bool active);
        // Turn standing in place toward screen left (yaw > 0) or right, in radians: the hips
        // and the upper body turn, and the feet are set down turned with them (leg IK), so she
        // faces someone beside her. Eased in and out; only while standing (otherwise it eases
        // back to 0 quickly).
        void SetTurn(float yaw);
        // Walk and run the way she faces, in place: whoever draws her moves her at GroundSpeed
        // (units per second; the dungeon). Off: she turns to the side and travels (the desktop).
        void SetFacingWalk(bool on,float pace=1);
        float GroundSpeed() const;
        // World position of the head bone from the latest solve; false if the model has none.
        bool HeadPosition(std::array<float,3>& position) const;
        // The middle of a palm (side 0: the left hand), between 手首 and the middle finger's
        // root, from the latest solve. False on a model without them.
        bool PalmPosition(int side,std::array<float,3>& position) const;
        // Hold a forearm out in front, palm up, as if showing something on it, for seconds
        // (standing only; False when it cannot now).
        bool Present(int side,double seconds);
        // A sword's cut or a spell cast (the right hand), the dungeon's attacks; see
        // GeneratedMotion::Strike. False when it cannot now.
        bool Strike(bool magic);
        // An arm out to a point fromHead away from the head bone (model axes), as if to take a
        // tile; see GeneratedMotion::Reach. False when it cannot now.
        bool Reach(int side,const std::array<float,3>& fromHead,double seconds);
        static constexpr double StrikeHit=0.32;   // seconds in: the cut lands, the spell leaves the hand
        // A fist's grip from the latest solve (model space): its middle, and the way a sword held
        // in it points (along the forearm, tipped toward the thumb). False on a model without
        // those bones.
        bool Grip(int side,std::array<float,3>& middle,std::array<float,3>& along) const;
        void EndGestures();   // the gesture and reaction playing fade out
        // Between the eyes (左目 and 右目, or the one there is) from the latest solve; the head
        // bone when there is neither.
        bool EyePosition(std::array<float,3>& position) const;
        // World-space bounds of all bone origins from the latest solve (hair bones included).
        bool BoneBounds(std::array<float,3>& lo,std::array<float,3>& hi) const;
        // The same for 下半身 and the leg bones only (what covers a window when sitting on it).
        bool LegBounds(std::array<float,3>& lo,std::array<float,3>& hi) const;
        // Ease the simulated bones (hair, skirt) back to the animated pose with physics off,
        // then put the bodies there with no momentum and resume physics (about half a second).
        // Also done by itself while standing still, when hair or a skirt stays raised against
        // gravity. PhysicsResets counts both, for diagnostics.
        void ResetPhysics();
        // Back to the plain pose of what she is doing (standing: the idle motion or the generated
        // standing pose): the opened motion, a built-in clip, a gesture and a click reaction fade
        // out, while the simulated bones ease to their modelled shape; then the bodies and joints
        // are put there with no momentum and physics resumes (about one and a half seconds).
        void ResetPose();
        std::uint32_t PhysicsResets() const;
        const std::wstring& PhysicsResetReason() const;   // last automatic one, for diagnostics

        // Generated motion layer (see GeneratedMotion.hpp). direction: -1 left, +1 right for Walk.
        void SetActivity(Activity activity,int direction=0);
        // Window motions (see docs/WindowMotions.md): VMDs played resting on a window's top edge,
        // instead of the generated sitting or hanging, or moving along it. The built-in ones come
        // with the exe; paths are the user's own (kept per character).
        bool SetWindowMotions(const std::vector<std::wstring>& paths,std::wstring& error);
        // How to rest on a window: Sit or Hang, with the window motion to play (-1: the generated
        // pose), at random among those not left out (gesture keys window:sit, window:hang and
        // window:<name> for the built-in ones).
        std::pair<Activity,int> ChooseRest(float random01) const;
        // A window motion that moves along the edge on the same side of the window (behind or in
        // front), at random; -1 when there is none.
        int ChooseMove(bool behind,float random01) const;
        // The window motion for the next SetActivity: Sit or Hang for one that rests, Walk for one
        // that moves (Walk's direction is the way it goes). Any other activity ends it.
        void SetWindowMotion(int clip);
        // Hanging from the edge, or playing a window motion made below it: drawn behind the window.
        bool DrawnBehind() const;
        // Diagnostics: how each window motion was taken (behind or in front, resting or moving).
        std::wstring DescribeWindowMotions() const;
        // Diagnostics (--window-clip): a VMD file added to the built-in window motions.
        static void AddWindowClipFile(const std::wstring& path);
        Activity CurrentActivity() const;
        bool CanSit() const;
        std::wstring DescribeRig() const;
        // Diagnostics: the closest the forearm and hand (left, right) come to the rest of the
        // body as posed now (its vertices, hair and arms aside), in model units.
        std::array<float,2> ArmClearance(std::array<std::wstring,2>* nearestBone=nullptr) const;   // and the bones carrying the nearest vertices
        // World units センター has walked since the last call (positive = screen right).
        float TakeRootDelta();
        float RootOffset() const;
        // Window motion while it is dragged or falling, in world units (x right, y up):
        // velocity drives the dangling sway, acceleration is fed to physics as inertia.
        void SetCarryMotion(float vx,float vy,float ax,float ay);
        void Land(float impactSpeed);

        // Advance playback by dt seconds and re-solve pose, physics and morphs.
        void Update(double dt);

        const std::vector<StaticVertex>& StaticVertices() const;
        const std::vector<std::uint32_t>& Indices() const;
        const std::vector<DynamicVertex>& DynamicVertices() const;
        std::uint64_t DynamicRevision() const;  // bumps whenever DynamicVertices() changes
        const std::vector<std::wstring>& TexturePaths() const;
        const std::vector<MaterialState>& Materials() const;
        // Row-major-transposed (HLSL-ready) 4x4 float matrices, one per bone.
        const void* BoneMatrices() const;
        size_t BoneCount() const;
        std::array<float,3> BoundsMin() const;
        std::array<float,3> BoundsMax() const;
        float SoleFrontZ() const;   // front of the soles, where the ground line is placed

        // Rest-pose angle of each upper arm below horizontal (degrees, [0]=left, [1]=right)
        // and the correction applied so T-stance models play A-stance motions.
        std::array<float,2> ArmRestAngles() const;
        std::array<float,2> ArmCorrections() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
