#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch
{
    // What the generated motion layer is doing. Idle plays the VMD (plus procedural layers).
    // Sit, Lie and Hang rest on an edge at world y=0 (a window's top edge).
    enum class Activity { Idle, Walk, Run, Jump, Sit, Lie, Hang, Grabbed, Fall };

    // A Jump starts with this long a crouch before the body leaves the ground.
    constexpr double JumpCrouchSeconds=0.22;

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
        bool React(bool head);
        bool Reacting() const;

        // A motion played over the idle pose now and then: PlayMotion blends it in, plays it
        // once and blends back out. An empty path removes it. Load drops it (set it again).
        bool SetMotion(const std::wstring& path,std::wstring& error);
        bool HasMotion() const;
        void PlayMotion();
        void StopMotion();          // blend out early
        bool MotionPlaying() const; // true until fully blended out
        // A gesture of the generated standing pose leads the head (look-at should pause).
        bool HeadBusy() const;
        bool ForceGesture(const std::wstring& name);   // diagnostics
        void SetPhysics(bool enabled);
        bool PhysicsEnabled() const;

        // Procedural motion layered on top of the VMD (each can be toggled independently).
        void SetProcedural(bool blink,bool breath,bool lookAt);
        // Mouth shapes while a reply is "spoken": weights for the あ い う え お ん morphs
        // (those the model has), over whatever the face already shows. active=false lets go.
        void SetMouth(const std::array<float,6>& weights,bool active);
        // The model's name as written in the PMX file.
        std::wstring ModelName() const;
        // Desired look direction in radians: yaw > 0 turns toward screen right, pitch > 0 looks up.
        // active=false lets the head ease back to the motion's own pose.
        void SetLookTarget(float yaw,float pitch,bool active);
        // World position of the head bone from the latest solve; false if the model has none.
        bool HeadPosition(std::array<float,3>& position) const;
        // World-space bounds of all bone origins from the latest solve (hair bones included).
        bool BoneBounds(std::array<float,3>& lo,std::array<float,3>& hi) const;
        // The same for 下半身 and the leg bones only (what covers a window when sitting on it).
        bool LegBounds(std::array<float,3>& lo,std::array<float,3>& hi) const;
        // Ease the simulated bones (hair, skirt) back to the animated pose with physics off,
        // then put the bodies there with no momentum and resume physics (about half a second).
        // Also done by itself while standing still, when hair or a skirt stays raised against
        // gravity. PhysicsResets counts both, for diagnostics.
        void ResetPhysics();
        std::uint32_t PhysicsResets() const;
        const std::wstring& PhysicsResetReason() const;   // last automatic one, for diagnostics

        // Generated motion layer (see GeneratedMotion.hpp). direction: -1 left, +1 right for Walk.
        void SetActivity(Activity activity,int direction=0);
        Activity CurrentActivity() const;
        bool CanSit() const;
        std::wstring DescribeRig() const;
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
