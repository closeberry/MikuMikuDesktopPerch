#pragma once
// Procedurally generated whole-body motion (standing idle, gestures, click reactions, walk,
// run, jump, sit, hang, grabbed, fall, landing).
// Follows docs/MotionRules.md: 全ての親 is never touched, センター only moves in X/Z (walking
// travel), vertical body movement uses グルーブ only, body turns use 上半身/下半身, ひじ rotates
// about its local Y only, 手首 is never rotated, and twist is handed to 腕捩 by DistributeTwist.
// Include only from .cpp files: it needs the PMXLoader module.
#include "MascotModel.hpp"
#include <DirectXMath.h>
#include <array>
#include <vector>

import PMXLoader;

namespace MikuMikuDesktopPerch
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
        // Without an idle VMD, Idle is a generated standing pose instead of the model's rest
        // (A-stance) pose.
        void SetGeneratedIdle(bool generated){generatedIdle=generated;}
        // Gestures now and then on the generated standing pose (hand on hip, wave, peace, ...);
        // off while an opened motion plays. HeadBusy: the current gesture leads the head, so
        // looking at the mouse should pause. ForceGesture (diagnostics) starts one by name.
        void SetGesturesAllowed(bool allowed){gesturesAllowed=allowed;}
        bool HeadBusy() const;
        // The current gesture shows a face morph (smile, wink): blinking on top over-deforms it.
        bool FaceBusy() const
        {
            for(const auto& m:morphOut)if(m.second>0.01f)return true;
            for(const auto& m:reactionMorphs)if(m.second>0.01f)return true;
            return false;
        }
        // Answer a click (head: on the head; otherwise the body). Standing: a reaction such as
        // being patted, startled, waving back; sitting or hanging: a shy tilt of the head.
        // False when busy reacting already or in no state to (walking, held, falling).
        bool React(bool head);
        bool Reacting() const {return reaction!=Gesture::None;}
        bool ForceGesture(const std::wstring& name);
        // Shift another pose (an opened motion blended over this layer's output) by the walking
        // travel, as Apply does to its own output.
        void AddRootOffset(PMX::Pose& pose) const;
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
            // The toe tip relative to the ankle at rest: how far forward and how far down it
            // is (the foot rolls over it when the heel comes up).
            std::array<float,2> toeAhead{},toeBelow{};
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
            struct Finger {int bone=-1,joint=0,finger=0;};   // finger: 0 index .. 3 little
            std::array<std::vector<Finger>,2> fingers{};
            std::array<DirectX::XMFLOAT3,2> fingerCurlAxis{};
            float legLength=8,thigh=4,shin=4;
            std::array<DirectX::XMFLOAT3,2> shoulderPos{};   // 腕 rest position
            float forearm=3;                                 // elbow to wrist
            // Face morphs for gestures (-1 when the model lacks them).
            int smileEyes=-1,smileMouth=-1,wink=-1,eyesClosed=-1,mouthOpen=-1;
        };

        // Stand is the plain standing pose, taken as one gesture among the others.
        enum class Gesture {None,HandOnHip,Wave,Peace,HandsBehind,LookAtPalm,HandsFolded,Stretch,LookAround,HeadTilt,Stand,
                            TuckHair,Yawn,Think,Hum,Clap,LookFar,Cheeks,
                            // Click reactions only (never picked at random).
                            PatHead,Surprised,Curious,ShyNod};
        static constexpr int GestureCount=17;

        void Generate(std::vector<Key>& keys,double dt);
        void DistributeTwist(std::vector<Key>& keys) const;
        Key& KeyOf(std::vector<Key>& keys,int bone);
        void SetRotation(std::vector<Key>& keys,int bone,DirectX::FXMVECTOR rotation);
        // Point the upper arm along upper and the forearm along fore (unit vectors, model space):
        // the upper arm turns about its own length so the elbow bends in the plane of the two.
        void AimArm(std::vector<Key>& keys,int side,DirectX::FXMVECTOR upper,DirectX::FXMVECTOR fore);
        // Put the wrist at a point, with the elbow toward pole (two-bone IK from the rest shoulder).
        void ReachArm(std::vector<Key>& keys,int side,DirectX::FXMVECTOR wrist,DirectX::FXMVECTOR pole);
        // Curl each finger but the thumb (0 straight .. 1 fully curled), index to little.
        void CurlFingers(std::vector<Key>& keys,int side,std::array<float,4> amount);
        void Gestures(std::vector<Key>& keys,double dt);
        void PoseGesture(Gesture which,double time,double length,int hand,float weight,
                         std::vector<Key>& g,std::vector<std::pair<int,float>>& morphs);
        void ApplyReaction(std::vector<Key>& keys,double dt);
        float Random01(){random^=random<<13;random^=random>>17;random^=random<<5;return (random&0xFFFFFF)/16777216.0f;}

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
        bool generatedIdle=false;
        bool gesturesAllowed=true;
        Gesture gesture=Gesture::None;
        std::array<Gesture,2> recentGestures{Gesture::None,Gesture::None};
        int gestureSide=0;                   // 0 left hand, 1 right
        double gestureTime=0,gestureLength=0,nextGestureAt=2;
        double frozenAt=-1;                  // diagnostics: hold the gesture at this time
        // Shortest and longest run of each gesture, in seconds.
        static std::array<float,2> GestureLengths(Gesture g)
        {
            static constexpr float lengths[GestureCount][2]={
                {5,8},{2.6f,3.6f},{3,4},{5,8},{3,4.5f},{5,8},{5.5f,7},{4,5},{2.5f,3.5f},{3,6},
                {3,4},{3.5f,4.5f},{4,6},{4,6},{2.5f,3.2f},{4,5.5f},{3,4}};
            if(static_cast<int>(g)>GestureCount)return {2.5f,2.5f};   // click reactions
            const auto& r=lengths[static_cast<int>(g)-1];
            return {r[0],r[1]};
        }
        // Standing weight: -1 on the right foot, +1 on the left, 0 on both. It moves from
        // weightFrom to weightTo over weightDuration, then stays until weightHoldUntil.
        float weightFrom=0,weightTo=0;
        double weightTime=1,weightDuration=1,weightHoldUntil=3;
        float gestureWeight=0;
        std::vector<std::pair<int,float>> morphOut;   // face morphs this frame (index, weight)
        Gesture reaction=Gesture::None;             // the click reaction playing
        double reactionTime=0,reactionLength=0;
        int reactionSide=0;
        float reactionWeight=0;
        std::vector<std::pair<int,float>> reactionMorphs;
        std::uint32_t random=0x9E3779B9u;
    };
}
