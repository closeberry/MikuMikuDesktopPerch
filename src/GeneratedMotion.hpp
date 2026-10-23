#pragma once
// Procedurally generated whole-body motion (walk, sit, grabbed, fall, landing).
// Follows docs/MotionRules.md: 全ての親 is never touched, センター only moves in X/Z (walking
// travel), vertical body movement uses グルーブ only, body turns use 上半身/下半身, ひじ rotates
// about its local Y only, 手首 is never rotated, and twist is handed to 腕捩 by DistributeTwist.
// Include only from .cpp files: it needs the PMXLoader module.
#include "MascotModel.hpp"
#include <DirectXMath.h>
#include <array>
#include <vector>

import PMXLoader;

namespace DesktopMascot
{
    class GeneratedMotion
    {
    public:
        // T-stance correction of one side: keys are generated in the corrected (A-stance) frame.
        struct ArmRest
        {
            int arm=-1;
            std::vector<int> descendants;
            DirectX::XMFLOAT4 rotation{0,0,0,1};
        };

        void Setup(const PMX::PMXModel& pmx,const std::array<ArmRest,2>& arms);
        std::wstring Describe() const;   // rig summary for diagnostics
        bool CanSit() const {return rig.groove>=0;}
        bool CanWalk() const {return rig.legIK[0]>=0&&rig.legIK[1]>=0;}

        // direction: -1 screen left, +1 screen right (Walk only).
        void SetActivity(Activity activity,int direction=0);
        Activity CurrentActivity() const {return activity;}
        // Window motion in world units/s (x right, y up) for sway while grabbed.
        void SetCarryVelocity(float vx,float vy){carryVelocity={vx,vy};}
        // Start a landing crouch; impactSpeed in world units/s.
        void Land(float impactSpeed);
        // Walking moves センター (and the foot IK targets) through the world along X.
        // RootOffset is the total travel; TakeRootDelta returns travel since the last call.
        float RootOffset() const {return rootOffset;}
        float TakeRootDelta(){const float d=rootOffset-reportedOffset;reportedOffset=rootOffset;return d;}

        // Blend the generated layer into pose (run before T-stance correction and procedural layers).
        void Apply(PMX::Pose& pose,double dt);

    private:
        struct Key {DirectX::XMFLOAT4 rotation{0,0,0,1};DirectX::XMFLOAT3 position{};};
        struct Rig
        {
            int center=-1,lowerBody=-1,upperBody=-1,neck=-1,groove=-1;
            // Held at rest while a generated activity plays, so the VMD's own lean or weight
            // shift (センター rotation, 上半身2, shoulders, head, wrists, toes) does not leak in.
            std::vector<int> neutral;
            std::array<int,2> leg{-1,-1},knee{-1,-1},ankle{-1,-1},legIK{-1,-1};
            // 手首 and 手捩 are never written: the layer leaves the wrist to the VMD.
            std::array<int,2> arm{-1,-1},elbow{-1,-1},armTwist{-1,-1};
            std::array<DirectX::XMFLOAT3,2> ankleRest{},hipRest{};
            std::array<DirectX::XMFLOAT3,2> armSwingAxis{},elbowBendAxis{};   // +angle moves the limb forward
            std::array<DirectX::XMFLOAT3,2> elbowTwistAxis{},armTwistAxis{};  // local X of ひじ, axis of 腕捩
            std::array<float,2> armLower{};    // roll that brings the rest-pose arm down to hang naturally
            std::array<float,2> armRaise{};    // roll that points the rest-pose arm straight up
            DirectX::XMFLOAT3 lowerPivot{};    // 下半身 rest position (the body's roll pivot)
            float shoulderY=14,armReach=6;     // shoulder height and shoulder-to-fingertip length
            float headY=16;                    // 頭 rest height
            std::array<DirectX::XMFLOAT3,2> armRestDir{};   // rest direction of the upper arm
            float upperArm=3,foreArm=3;        // shoulder to elbow, elbow to fingertips
            float gripReach=3;                 // elbow to the middle of a curled grip
            // Fingers other than the thumb (bone, joint 0..2 from the knuckle) and the axis that
            // curls them toward the palm, per side; empty when the model has no finger bones.
            struct Finger {int bone=-1,joint=0;};
            std::array<std::vector<Finger>,2> fingers{};
            std::array<DirectX::XMFLOAT3,2> fingerCurlAxis{};
            float legLength=8,thigh=4,shin=4;
        };

        void Generate(std::vector<Key>& keys,double dt);
        void DistributeTwist(std::vector<Key>& keys) const;
        Key& KeyOf(std::vector<Key>& keys,int bone);
        void SetRotation(std::vector<Key>& keys,int bone,DirectX::FXMVECTOR rotation);

        Rig rig;
        std::vector<int> overrides;          // bones this layer writes
        std::vector<int> slot;               // bone index -> position in overrides (-1 if none)
        std::vector<Key> snapshot,lastOutput;
        Activity activity=Activity::Idle;
        int direction=1;
        double fadeTime=1,fadeDuration=0.3;
        double activityTime=0;               // seconds since the current activity started
        double clock=0,walkPhase=0;
        float walkAmount=0;                  // 0..1, eases stride in and out
        float yaw=0;                         // current body yaw (radians)
        DirectX::XMFLOAT2 carryVelocity{},sway{};
        float rootOffset=0,reportedOffset=0;
        double landTime=10;
        float landDepth=0;
        bool initialized=false;
    };
}
