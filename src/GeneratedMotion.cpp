#define NOMINMAX
#include "GeneratedMotion.hpp"
#include <algorithm>
#include <cmath>
#include <string>

namespace MikuMikuDesktopPerch
{
    using namespace DirectX;

    namespace
    {
        constexpr float Deg=XM_PI/180.0f;

        // Shared gait for walking and running. Lengths are in leg lengths, angles in radians.
        struct Gait
        {
            double cycle;     // seconds per two steps
            float stance;     // share of the cycle a foot is planted (< 0.5 gives a flight phase)
            float stride;     // foot travel per stance
            float lift,bobBase,bob,lean,armSwing,elbow,hipTwist,chestTwist;
            float heel,toeOff;   // foot pitch at heel strike (toes up) and at push-off (heel up)
            float heelRise;      // share of the stance after which the heel comes up
            float kick,kickAt;   // swing: the foot tips on to this pitch (toes down) by kickAt, then turns for the landing
            float swingDelay;    // share of the swing the foot stays back (kicking up) before it comes forward
            float back;          // the stance sits this share of the stride behind the hip
            float liftSkew;      // < 1 brings the swing foot's highest point earlier (a kick back)
            float lowAt;         // where in the cycle the body is lowest (a little after contact)
            float easeSeconds;
        };
        constexpr Gait WalkGait{1.07,0.625f,0.5f,0.08f,0.003f,0.031f,5.0f*Deg,15.0f*Deg,8.0f*Deg,5.0f*Deg,5.0f*Deg,
                                20.0f*Deg,30.0f*Deg,0.8f,30.0f*Deg,0.5f,0.0f,0.04f,1.0f,0.125f,0.35f};
        constexpr Gait RunGait{0.667,0.4f,0.9f,0.45f,0.03f,0.055f,15.0f*Deg,35.0f*Deg,90.0f*Deg,5.0f*Deg,12.0f*Deg,
                               20.0f*Deg,50.0f*Deg,0.62f,85.0f*Deg,0.25f,0.2f,0.055f,0.55f,0.15f,0.22f};

        // Walking and running are in profile so planted feet never slide in depth; the chest
        // and neck turn back toward the viewer a little.
        constexpr float WalkYaw=90.0f*Deg;
        constexpr float WalkChestYaw=72.0f*Deg;
        constexpr float WalkNeckYaw=18.0f*Deg;
        // Generated poses start from arms hanging this far below horizontal (rest poses are
        // A- or T-stance), slightly away from the body so the hands clear the hips and skirt.
        constexpr float HangAngle=72.0f*Deg;
        // Sitting on an edge: thighs slope down instead of lying level, so skirts do not get
        // pierced by raised knees, and the shins hang almost straight down.
        constexpr float SitThighDown=40.0f*Deg;
        constexpr float SitShinForward=10.0f*Deg;

        int Find(const PMX::PMXModel& pmx,std::initializer_list<std::wstring> names)
        {
            for(const auto& name:names)
                for(size_t i=0;i<pmx.bones.size();++i)
                    if(pmx.bones[i].name==name)return static_cast<int>(i);
            return -1;
        }
        float Smooth(float t){t=std::clamp(t,0.0f,1.0f);return t*t*(3-2*t);}
        float Frac(double v){return static_cast<float>(v-std::floor(v));}
        float Approach(float value,float target,float step){return value<target?std::min(value+step,target):std::max(value-step,target);}
        XMVECTOR Rotation(const XMFLOAT3& axis,float angle){return XMQuaternionRotationNormal(XMLoadFloat3(&axis),angle);}
        // Row-vector convention used by PoseSolver: rotate by a, then by b.
        XMVECTOR Then(FXMVECTOR a,FXMVECTOR b){return XMQuaternionMultiply(a,b);}
    }

    void GeneratedMotion::Setup(const PMX::PMXModel& pmx,const std::array<ArmRest,2>& arms)
    {
        rig={};
        overrides.clear();
        initialized=false;
        activity=Activity::Idle;

        // Rest geometry in the frame the keys are authored in: T-stance arms already lowered.
        auto corrected=[&](int bone,FXMVECTOR v,bool point)->XMVECTOR{
            for(const auto& a:arms)
            {
                if(a.arm<0)continue;
                if(bone!=a.arm&&std::find(a.descendants.begin(),a.descendants.end(),bone)==a.descendants.end())continue;
                const XMMATRIX r=XMMatrixRotationQuaternion(XMLoadFloat4(&a.rotation));
                if(!point)return XMVector3TransformNormal(v,r);
                const XMVECTOR pivot=XMLoadFloat3(&pmx.bones[a.arm].position);
                return XMVectorAdd(pivot,XMVector3TransformNormal(XMVectorSubtract(v,pivot),r));
            }
            return v;
        };
        auto restPos=[&](int bone){return corrected(bone,XMLoadFloat3(&pmx.bones[bone].position),true);};

        // Local X along the limb, local Y the forward-bending axis (model local axes when present).
        auto limbAxes=[&](int bone,int child,XMFLOAT3& bend,XMFLOAT3& twist){
            const auto& b=pmx.bones[bone];
            const XMVECTOR along=XMVector3Normalize(XMVectorSubtract(restPos(child),restPos(bone)));
            XMVECTOR x=along,y=XMVector3Normalize(XMVector3Cross(XMVectorSet(0,0,-1,0),along));
            if(b.IsLocalFrame())
            {
                // Use the model's local axes only when local X really runs along the bone;
                // some models store axes that do not (e.g. X pointing up the arm's side).
                const XMVECTOR lx=XMVector3Normalize(corrected(bone,XMLoadFloat3(&b.localAxisX),false));
                const XMVECTOR lz=XMVector3Normalize(corrected(bone,XMLoadFloat3(&b.localAxisZ),false));
                const XMVECTOR ly=XMVector3Normalize(XMVector3Cross(lz,lx));
                if(std::abs(XMVectorGetX(XMVector3Dot(lx,along)))>0.8f&&std::abs(XMVectorGetX(XMVector3Dot(ly,along)))<0.3f)
                {
                    x=lx;
                    y=ly;
                }
            }
            // A positive angle about y must carry the limb toward the front (-Z).
            const XMVECTOR moved=XMVector3TransformNormal(x,XMMatrixRotationQuaternion(XMQuaternionRotationNormal(y,0.3f)));
            if(XMVectorGetZ(moved)>XMVectorGetZ(x))y=XMVectorNegate(y);
            XMStoreFloat3(&bend,y);
            XMStoreFloat3(&twist,x);
        };

        rig.center=Find(pmx,{L"センター"});
        rig.lowerBody=Find(pmx,{L"下半身"});
        rig.upperBody=Find(pmx,{L"上半身"});
        rig.neck=Find(pmx,{L"首"});
        rig.groove=Find(pmx,{L"グルーブ"});
        float hipHeight=0,legLength=0;
        int legs=0;
        for(int side=0;side<2;++side)
        {
            const std::wstring s=side==0?L"左":L"右";
            rig.leg[side]=Find(pmx,{s+L"足"});
            rig.knee[side]=Find(pmx,{s+L"ひざ",s+L"膝"});
            rig.ankle[side]=Find(pmx,{s+L"足首"});
            rig.legIK[side]=Find(pmx,{s+L"足ＩＫ",s+L"足IK"});
            rig.arm[side]=Find(pmx,{s+L"腕"});
            rig.elbow[side]=Find(pmx,{s+L"ひじ",s+L"肘"});
            rig.armTwist[side]=Find(pmx,{s+L"腕捩",s+L"腕捩り"});
            const int wrist=Find(pmx,{s+L"手首"});

            if(rig.leg[side]>=0&&rig.knee[side]>=0&&rig.legIK[side]>=0)
            {
                const XMVECTOR hip=restPos(rig.leg[side]),knee=restPos(rig.knee[side]);
                const XMVECTOR ankle=restPos(rig.ankle[side]>=0?rig.ankle[side]:rig.legIK[side]);
                XMStoreFloat3(&rig.hipRest[side],hip);
                XMStoreFloat3(&rig.ankleRest[side],restPos(rig.legIK[side]));
                rig.thigh=XMVectorGetX(XMVector3Length(XMVectorSubtract(knee,hip)));
                rig.shin=XMVectorGetX(XMVector3Length(XMVectorSubtract(ankle,knee)));
                hipHeight+=XMVectorGetY(hip);
                legLength+=XMVectorGetY(hip)-XMVectorGetY(ankle);
                ++legs;
            }
            if(rig.arm[side]>=0&&rig.elbow[side]>=0)
            {
                XMFLOAT3 unused{};
                limbAxes(rig.arm[side],rig.elbow[side],rig.armSwingAxis[side],unused);
                const XMVECTOR armDir=XMVectorSubtract(restPos(rig.elbow[side]),restPos(rig.arm[side]));
                const float rest=std::atan2(-XMVectorGetY(armDir),std::abs(XMVectorGetX(armDir)));
                // +Z roll raises the +X (left) arm, so the left arm is lowered with a negative roll.
                rig.armLower[side]=(HangAngle-rest)*(side==0?-1.0f:1.0f);
                rig.armRaise[side]=(XM_PIDIV2+rest)*(side==0?1.0f:-1.0f);
                const float upper=XMVectorGetX(XMVector3Length(armDir));
                const float fore=wrist>=0?XMVectorGetX(XMVector3Length(XMVectorSubtract(restPos(wrist),restPos(rig.elbow[side])))):upper;
                rig.armReach=upper+fore+0.35f*fore;   // hand length roughly a third of the forearm
                rig.upperArm=upper;
                rig.forearm=fore;
                XMStoreFloat3(&rig.shoulderPos[side],restPos(rig.arm[side]));
                rig.foreArm=fore*1.35f;
                rig.gripReach=fore*1.2f;
                XMStoreFloat3(&rig.armRestDir[side],XMVector3Normalize(armDir));
                rig.shoulderY=XMVectorGetY(restPos(rig.arm[side]));
                if(wrist>=0)limbAxes(rig.elbow[side],wrist,rig.elbowBendAxis[side],rig.elbowTwistAxis[side]);
                else rig.elbow[side]=-1;
                if(rig.armTwist[side]>=0)
                {
                    const auto& t=pmx.bones[rig.armTwist[side]];
                    const XMVECTOR armDir=XMVector3Normalize(XMVectorSubtract(restPos(Find(pmx,{s+L"ひじ",s+L"肘"})),restPos(rig.arm[side])));
                    XMVECTOR axis=armDir;
                    if(t.IsFixAxis())
                    {
                        // The twist axis must run along the upper arm to move twist without bending it.
                        const XMVECTOR fixed=XMVector3Normalize(corrected(rig.armTwist[side],XMLoadFloat3(&t.fixAxis),false));
                        if(std::abs(XMVectorGetX(XMVector3Dot(fixed,armDir)))>0.8f)axis=fixed;
                    }
                    XMStoreFloat3(&rig.armTwistAxis[side],axis);
                }
            }
        }
        // Fingers: the curl axis runs across the palm (index to little finger knuckles), signed
        // so the fingertips move toward the side the thumb is on (the palm side).
        for(int side=0;side<2;++side)
        {
            const std::wstring s=side==0?L"左":L"右";
            const int wrist=Find(pmx,{s+L"手首"});
            const int index1=Find(pmx,{s+L"人指１",s+L"人差指１"}),little1=Find(pmx,{s+L"小指１"});
            const int middle1=Find(pmx,{s+L"中指１"}),middle3=Find(pmx,{s+L"中指３"}),thumb=Find(pmx,{s+L"親指２",s+L"親指１"});
            if(wrist<0||index1<0||little1<0||middle1<0||middle3<0||thumb<0)continue;
            const XMVECTOR along=XMVector3Normalize(XMVectorSubtract(restPos(middle3),restPos(middle1)));
            XMVECTOR across=XMVectorSubtract(restPos(little1),restPos(index1));
            across=XMVector3Normalize(XMVectorSubtract(across,XMVectorScale(along,XMVectorGetX(XMVector3Dot(across,along)))));
            XMVECTOR palm=XMVector3Cross(along,across);
            if(XMVectorGetX(XMVector3Dot(palm,XMVectorSubtract(restPos(thumb),restPos(index1))))<0)palm=XMVectorNegate(palm);
            XMVECTOR axis=across;
            const XMVECTOR moved=XMVector3Rotate(along,XMQuaternionRotationNormal(axis,0.3f));
            if(XMVectorGetX(XMVector3Dot(moved,palm))<0)axis=XMVectorNegate(axis);
            XMStoreFloat3(&rig.fingerCurlAxis[side],axis);
            for(const wchar_t* finger:{L"人指",L"中指",L"薬指",L"小指"})
                for(int joint=0;joint<3;++joint)
                {
                    const wchar_t* number[3]={L"１",L"２",L"３"};
                    int bone=Find(pmx,{s+finger+number[joint]});
                    if(bone<0&&std::wstring(finger)==L"人指")bone=Find(pmx,{s+L"人差指"+number[joint]});
                    if(bone>=0)rig.fingers[side].push_back({bone,joint,static_cast<int>(rig.fingers[side].size()/3)});
                }
            // A curled hand grips about at the middle of the fingers.
            const float palmLength=XMVectorGetX(XMVector3Length(XMVectorSubtract(restPos(middle1),restPos(wrist))));
            const float fingerLength=XMVectorGetX(XMVector3Length(XMVectorSubtract(restPos(middle3),restPos(middle1))));
            if(rig.elbow[side]>=0)
                rig.gripReach=XMVectorGetX(XMVector3Length(XMVectorSubtract(restPos(wrist),restPos(rig.elbow[side]))))+palmLength+0.4f*fingerLength;
        }
        if(legs>0)rig.legLength=std::max(legLength/legs,0.1f);
        for(int side=0;side<2;++side)
        {
            const std::wstring s=side==0?L"左":L"右";
            const int toe=Find(pmx,{s+L"つま先ＩＫ",s+L"つま先IK",s+L"つま先"});
            const XMFLOAT3& ankle=rig.ankleRest[side];
            if(toe>=0)
            {
                const XMVECTOR tip=restPos(toe);
                rig.toeAhead[side]=std::clamp(ankle.z-XMVectorGetZ(tip),0.02f*rig.legLength,0.3f*rig.legLength);   // the front is -Z
                rig.toeBelow[side]=std::clamp(ankle.y-XMVectorGetY(tip),0.0f,0.3f*rig.legLength);
            }
            else
            {
                rig.toeAhead[side]=0.13f*rig.legLength;
                rig.toeBelow[side]=std::max(0.0f,ankle.y);
            }
        }
        if(rig.lowerBody>=0)XMStoreFloat3(&rig.lowerPivot,restPos(rig.lowerBody));
        for(int i=0;i<static_cast<int>(pmx.morphs.size());++i)
        {
            const auto& name=pmx.morphs[i].name;
            auto pick=[&](int& slot,std::initializer_list<const wchar_t*> names){
                if(slot>=0)return;
                for(const auto* n:names)if(name==n){slot=i;return;}
            };
            pick(rig.smileEyes,{L"笑い",L"にっこり"});
            pick(rig.smileMouth,{L"にやり",L"にっこり口",L"ワ"});
            pick(rig.wink,{L"ウィンク",L"ウインク",L"ウィンク２",L"ウィンク右"});
            pick(rig.eyesClosed,{L"まばたき",L"瞬き",L"blink",L"Blink"});
            pick(rig.mouthOpen,{L"あ",L"あ２",L"a"});
        }
        if(const int head=Find(pmx,{L"頭"});head>=0)rig.headY=XMVectorGetY(restPos(head));
        else rig.headY=rig.shoulderY+0.25f*rig.legLength;
        (void)hipHeight;

        for(const auto* name:{L"センター",L"上半身2",L"頭",L"左肩",L"右肩",L"左手首",L"右手首",L"左手捩",L"右手捩",
                              L"左つま先ＩＫ",L"右つま先ＩＫ",L"左つま先IK",L"右つま先IK",L"腰"})
            if(const int bone=Find(pmx,{name});bone>=0)rig.neutral.push_back(bone);

        slot.assign(pmx.bones.size(),-1);
        auto add=[&](int bone){if(bone>=0&&slot[bone]<0){slot[bone]=static_cast<int>(overrides.size());overrides.push_back(bone);}};
        add(rig.lowerBody);add(rig.upperBody);add(rig.neck);add(rig.groove);
        for(int side=0;side<2;++side){add(rig.legIK[side]);add(rig.arm[side]);add(rig.elbow[side]);add(rig.armTwist[side]);}
        for(int side=0;side<2;++side)for(const auto& f:rig.fingers[side])add(f.bone);
        for(int bone:rig.neutral)add(bone);
    }

    std::wstring GeneratedMotion::Describe() const
    {
        auto v=[](const XMFLOAT3& a){wchar_t b[64]{};swprintf_s(b,L"(%.2f,%.2f,%.2f)",a.x,a.y,a.z);return std::wstring(b);};
        wchar_t head[128]{};
        swprintf_s(head,L"legLength=%.2f thigh=%.2f shin=%.2f groove=%d\n",rig.legLength,rig.thigh,rig.shin,rig.groove);
        return std::wstring(head)+
            L"armSwingAxis L="+v(rig.armSwingAxis[0])+L" R="+v(rig.armSwingAxis[1])+L"\n"+
            L"elbowBendAxis L="+v(rig.elbowBendAxis[0])+L" R="+v(rig.elbowBendAxis[1])+L"\n"+
            L"elbowTwistAxis L="+v(rig.elbowTwistAxis[0])+L" R="+v(rig.elbowTwistAxis[1])+L"\n"+
            L"armTwistAxis L="+v(rig.armTwistAxis[0])+L" R="+v(rig.armTwistAxis[1])+L"\n"+
            L"morphs smileEyes="+std::to_wstring(rig.smileEyes)+L" smileMouth="+std::to_wstring(rig.smileMouth)+L" wink="+std::to_wstring(rig.wink)+L"\n";
    }

    void GeneratedMotion::SetActivity(Activity next,int dir)
    {
        const bool edgePose=next==Activity::Sit||next==Activity::Lie||next==Activity::Hang;
        if(edgePose&&!CanSit())next=Activity::Idle;   // lowering the body needs グルーブ
        const bool gait=next==Activity::Walk||next==Activity::Run;
        if(gait&&!CanWalk())next=Activity::Idle;
        const bool directional=gait||next==Activity::Jump||next==Activity::Sit;
        if(next==activity&&(!directional||dir==direction))return;
        snapshot=lastOutput;
        fadeTime=0;
        activityTime=0;
        switch(next)
        {
        case Activity::Idle:fadeDuration=0.45;break;
        case Activity::Walk:fadeDuration=0.35;break;
        case Activity::Run:fadeDuration=0.25;break;
        case Activity::Jump:fadeDuration=0.1;break;
        case Activity::Sit:fadeDuration=0.9;break;
        case Activity::Lie:fadeDuration=1.1;break;
        case Activity::Hang:fadeDuration=0.6;break;
        case Activity::Grabbed:fadeDuration=0.2;break;
        case Activity::Fall:fadeDuration=0.25;break;
        }
        if(directional)direction=dir<0?-1:(dir>0?1:0);
        // A new gait starts from standing; switching walk <-> run keeps the stride going.
        if(gait&&activity!=Activity::Walk&&activity!=Activity::Run)walkAmount=0;
        activity=next;
    }

    void GeneratedMotion::Land(float impactSpeed)
    {
        landTime=0;
        landDepth=std::clamp(std::abs(impactSpeed)*0.012f,0.04f,0.22f)*rig.legLength;
    }

    GeneratedMotion::Key& GeneratedMotion::KeyOf(std::vector<Key>& keys,int bone)
    {
        static Key dummy;
        if(bone<0||slot[bone]<0){dummy={};return dummy;}
        return keys[slot[bone]];
    }

    void GeneratedMotion::SetRotation(std::vector<Key>& keys,int bone,FXMVECTOR rotation)
    {
        XMStoreFloat4(&KeyOf(keys,bone).rotation,XMQuaternionNormalize(rotation));
    }

    void GeneratedMotion::AimArm(std::vector<Key>& keys,int side,FXMVECTOR upper,FXMVECTOR fore)
    {
        if(rig.arm[side]<0||rig.elbow[side]<0)return;
        const float bendAngle=std::acos(std::clamp(XMVectorGetX(XMVector3Dot(upper,fore)),-1.0f,1.0f));
        // The bend axis that turns the upper arm's direction into the forearm's.
        XMVECTOR bendAxis=XMVector3Cross(upper,fore);
        if(XMVectorGetX(XMVector3LengthSq(bendAxis))<1e-8f)bendAxis=XMVector3Cross(upper,XMVectorSet(0,0,-1,0));
        bendAxis=XMVector3Normalize(bendAxis);
        if(XMVectorGetX(XMVector3Dot(XMVector3Rotate(upper,XMQuaternionRotationNormal(bendAxis,bendAngle)),fore))<0.99f)
            bendAxis=XMVectorNegate(bendAxis);
        // Rotation taking the rest frame (arm direction, elbow bend axis) onto that one.
        const XMVECTOR a0=XMLoadFloat3(&rig.armRestDir[side]);
        XMVECTOR b0=XMLoadFloat3(&rig.elbowBendAxis[side]);
        b0=XMVector3Normalize(XMVectorSubtract(b0,XMVectorScale(a0,XMVectorGetX(XMVector3Dot(a0,b0)))));
        const XMMATRIX from{a0,b0,XMVector3Cross(a0,b0),XMVectorSet(0,0,0,1)};
        const XMMATRIX to{upper,bendAxis,XMVector3Cross(upper,bendAxis),XMVectorSet(0,0,0,1)};
        SetRotation(keys,rig.arm[side],XMQuaternionRotationMatrix(XMMatrixTranspose(from)*to));
        SetRotation(keys,rig.elbow[side],Rotation(rig.elbowBendAxis[side],bendAngle));
    }

    void GeneratedMotion::ReachArm(std::vector<Key>& keys,int side,FXMVECTOR wrist,FXMVECTOR pole)
    {
        const float a=rig.upperArm,b=rig.forearm;
        const XMVECTOR shoulder=XMLoadFloat3(&rig.shoulderPos[side]);
        XMVECTOR toWrist=XMVectorSubtract(wrist,shoulder);
        const float d=std::clamp(XMVectorGetX(XMVector3Length(toWrist)),std::abs(a-b)+0.01f,a+b-0.01f);
        const XMVECTOR dir=XMVector3Normalize(toWrist);
        XMVECTOR side3=XMVectorSubtract(pole,XMVectorScale(dir,XMVectorGetX(XMVector3Dot(pole,dir))));
        if(XMVectorGetX(XMVector3LengthSq(side3))<1e-8f)side3=XMVectorSet(0,-1,0,0);
        side3=XMVector3Normalize(side3);
        const float cosA=std::clamp((a*a+d*d-b*b)/(2*a*d),-1.0f,1.0f);
        const XMVECTOR elbow=XMVectorAdd(shoulder,XMVectorAdd(XMVectorScale(dir,a*cosA),XMVectorScale(side3,a*std::sqrt(1-cosA*cosA))));
        const XMVECTOR reached=XMVectorAdd(shoulder,XMVectorScale(dir,d));
        AimArm(keys,side,XMVector3Normalize(XMVectorSubtract(elbow,shoulder)),XMVector3Normalize(XMVectorSubtract(reached,elbow)));
    }

    void GeneratedMotion::CurlFingers(std::vector<Key>& keys,int side,std::array<float,4> amount)
    {
        static constexpr float full[3]={70.0f*Deg,90.0f*Deg,60.0f*Deg};
        for(const auto& f:rig.fingers[side])
            SetRotation(keys,f.bone,Rotation(rig.fingerCurlAxis[side],full[f.joint]*amount[std::clamp(f.finger,0,3)]));
    }

    bool GeneratedMotion::HeadBusy() const
    {
        if(reaction!=Gesture::None)return true;
        if(gesture==Gesture::None||gestureWeight<=0.01f)return false;
        switch(gesture)
        {
        case Gesture::Wave:case Gesture::Peace:case Gesture::LookAtPalm:case Gesture::LookAround:case Gesture::HeadTilt:
        case Gesture::Stretch:case Gesture::TuckHair:case Gesture::Yawn:case Gesture::Think:case Gesture::Hum:
        case Gesture::LookFar:case Gesture::Cheeks:
            return true;
        default:
            return false;
        }
    }

    namespace
    {
        using Gesture=GeneratedMotion::Gesture;
        constexpr std::pair<const wchar_t*,Gesture> GestureNames[]={
            {L"hip",Gesture::HandOnHip},{L"wave",Gesture::Wave},{L"peace",Gesture::Peace},{L"behind",Gesture::HandsBehind},
            {L"palm",Gesture::LookAtPalm},{L"folded",Gesture::HandsFolded},{L"stretch",Gesture::Stretch},
            {L"look",Gesture::LookAround},{L"tilt",Gesture::HeadTilt},{L"stand",Gesture::Stand},
            {L"hair",Gesture::TuckHair},{L"yawn",Gesture::Yawn},{L"think",Gesture::Think},{L"hum",Gesture::Hum},
            {L"clap",Gesture::Clap},{L"far",Gesture::LookFar},{L"cheeks",Gesture::Cheeks},
            {L"pat",Gesture::PatHead},{L"surprised",Gesture::Surprised},{L"curious",Gesture::Curious},{L"shynod",Gesture::ShyNod}};
    }

    bool GeneratedMotion::Emote(const std::wstring& name)
    {
        if(reaction!=Gesture::None||overrides.empty()||activity!=Activity::Idle)return false;
        for(const auto& [n,g]:GestureNames)
            if(name==n)
            {
                reaction=g;
                reactionSide=Random01()<0.5f?0:1;
                reactionTime=0;
                reactionLength=static_cast<int>(g)>GestureCount?2.5:std::min(3.5,static_cast<double>(GestureLengths(g)[0]));
                reactionWeight=0;
                return true;
            }
        return false;
    }

    bool GeneratedMotion::ForceGesture(const std::wstring& name)
    {
        // name or name@seconds: hold the gesture, or freeze it that far into its timeline.
        const auto at=name.find(L'@');
        const std::wstring base=name.substr(0,at);
        for(const auto& [n,g]:GestureNames)
            if(base==n)
            {
                gesture=g;gestureSide=0;gestureTime=0;gestureLength=1e9;frozenAt=-1;
                if(at!=std::wstring::npos){frozenAt=std::stod(name.substr(at+1));gestureLength=GestureLengths(g)[1];}
                return true;
            }
        return false;
    }

    // The pose of one gesture at time seconds into it (length long, hand 0 the left), laid
    // over g; its face morphs, scaled by weight, go to morphs.
    void GeneratedMotion::PoseGesture(Gesture which,double time,double length,int hand,float weight,
                                      std::vector<Key>& g,std::vector<std::pair<int,float>>& morphs)
    {
        const float L=rig.legLength;
        const int s=hand;
        const float a=static_cast<float>(time),len=static_cast<float>(length);
        const float out=s==0?1.0f:-1.0f;      // the left side is +X; the front is -Z
        const XMFLOAT3 S=rig.shoulderPos[s];
        const float hipY=(rig.hipRest[0].y+rig.hipRest[1].y)*0.5f,hipZ=(rig.hipRest[0].z+rig.hipRest[1].z)*0.5f;
        const float u=static_cast<float>(time/length);
        auto both=[&](auto&& f){f(0,1.0f);f(1,-1.0f);};
        auto neck=[&](float pitch,float yawAngle,float roll){SetRotation(g,rig.neck,XMQuaternionRotationRollPitchYaw(pitch,yawAngle,roll));};
        auto face=[&](int morph,float amount){if(morph>=0)morphs.push_back({morph,amount*weight});};
        // 0 -> 1 over [from, from + span] seconds into the gesture.
        auto ramp=[&](float from,float span){return Smooth(std::clamp((a-from)/span,0.0f,1.0f));};
        auto mix=[](FXMVECTOR p,FXMVECTOR q,float w){return XMVectorLerp(p,q,w);};
        const float faceZ=S.z-0.12f*L;   // roughly the front of the face
        switch(which)
        {
        case Gesture::HandOnHip:
            // One hand on the hip, elbow out to the side and a little back.
            ReachArm(g,s,XMVectorSet(out*(std::abs(rig.hipRest[s].x)+0.11f*L),hipY+0.13f*L,hipZ+0.03f*L,0),XMVectorSet(out,0.1f,0.8f,0));
            CurlFingers(g,s,{0.25f,0.25f,0.3f,0.3f});
            neck(0,0,-out*3.0f*Deg);
            break;
        case Gesture::Wave:
        {
            // A hand raised beside the head, waving side to side, with a smile.
            const float wave=std::sin(XM_2PI*1.8f*a);
            ReachArm(g,s,XMVectorSet(S.x+out*(0.24f+0.07f*wave)*L,S.y+0.27f*L,S.z-0.10f*L,0),XMVectorSet(out,-1,0.3f,0));
            CurlFingers(g,s,{0.05f,0.05f,0.05f,0.05f});
            neck(0,0,-out*5.0f*Deg);
            face(rig.smileEyes,1.0f);
            face(rig.smileMouth,0.7f);
            break;
        }
        case Gesture::Peace:
            // A peace sign beside the face, head tilted toward it, with a wink.
            ReachArm(g,s,XMVectorSet(out*0.30f*L,rig.headY+0.02f*L,S.z-0.16f*L,0),XMVectorSet(out,-1,0.3f,0));
            CurlFingers(g,s,{0.0f,0.0f,1.0f,1.0f});
            neck(0,0,-out*8.0f*Deg);
            face(rig.wink,1.0f);
            face(rig.smileMouth,0.5f);
            break;
        case Gesture::HandsBehind:
            // Both hands behind the back, chest a little out.
            both([&](int side,float o){ReachArm(g,side,XMVectorSet(o*0.05f*L,hipY+0.03f*L,hipZ+0.15f*L,0),XMVectorSet(o*0.8f,-0.2f,1,0));CurlFingers(g,side,{0.4f,0.4f,0.4f,0.4f});});
            SetRotation(g,rig.upperBody,XMQuaternionRotationRollPitchYaw(3.0f*Deg,0,0));
            neck(2.0f*Deg,0,0);
            break;
        case Gesture::LookAtPalm:
            // Looking down at the open palm held in front of the chest.
            ReachArm(g,s,XMVectorSet(out*0.06f*L,S.y-0.16f*L,S.z-0.40f*L,0),XMVectorSet(out,-1,-0.1f,0));
            CurlFingers(g,s,{0.1f,0.1f,0.15f,0.2f});
            neck(-28.0f*Deg,-out*10.0f*Deg,0);
            break;
        case Gesture::HandsFolded:
            // Hands lowered and held together in front.
            both([&](int side,float o){ReachArm(g,side,XMVectorSet(o*0.03f*L,hipY-0.12f*L,hipZ-0.13f*L,0),XMVectorSet(o,-0.2f,0.6f,0));CurlFingers(g,side,{0.35f,0.35f,0.4f,0.4f});});
            neck(-4.0f*Deg,0,0);
            break;
        case Gesture::Stretch:
        {
            // A slow stretch: the arms rise over the head for about two seconds, push up further
            // while the body leans back and over to one side with the eyes shut, then come down
            // out to the sides with a breath out, the arms kept straight (swung round the shoulder
            // from overhead down to the side, not pulled in toward it).
            const float rise=ramp(0,2.0f),release=ramp(len-2.4f,1.7f);
            const float peak=std::sin(XM_PI*std::clamp((a-1.6f)/std::max(len-3.4f,0.5f),0.0f,1.0f));
            both([&](int side,float o){
                const float sz=rig.shoulderPos[side].z;
                const XMVECTOR front=XMVectorSet(o*0.16f*L,rig.shoulderPos[side].y+0.10f*L,sz-0.22f*L,0);
                const XMVECTOR up=XMVectorSet(o*0.07f*L,rig.headY+(0.36f+0.06f*peak)*L,sz+0.02f*L,0);
                const XMVECTOR shoulder=XMLoadFloat3(&rig.shoulderPos[side]);
                const XMVECTOR hang=XMVectorAdd(shoulder,XMVectorSet(o*0.05f*L,-0.40f*L,-0.06f*L,0));
                XMVECTOR wrist=mix(hang,front,ramp(0,0.9f));
                wrist=mix(wrist,up,ramp(0.8f,1.2f));
                // Coming down: a straight arm from 15° off vertical round to hanging at the side.
                const float swing=(15.0f+157.0f*release)*Deg;
                const XMVECTOR straight=XMVectorAdd(shoulder,XMVectorScale(
                    XMVector3Normalize(XMVectorSet(o*std::sin(swing),std::cos(swing),0.03f,0)),rig.upperArm+rig.forearm));
                const float straighten=std::min(1.0f,release*4.0f);
                wrist=mix(wrist,straight,straighten);
                ReachArm(g,side,wrist,mix(XMVectorSet(o,0.2f,0.4f,0),XMVectorSet(0,0,1,0),straighten));
                CurlFingers(g,side,{0.2f-0.15f*peak,0.2f-0.15f*peak,0.25f-0.1f*peak,0.3f-0.1f*peak});
            });
            const float lean=rise*(1-release);
            SetRotation(g,rig.upperBody,XMQuaternionRotationRollPitchYaw((4.0f+6.0f*peak)*Deg*lean,0,out*5.0f*Deg*peak));
            neck((6.0f+8.0f*peak)*Deg*lean,0,out*3.0f*Deg*peak);
            face(rig.eyesClosed>=0?rig.eyesClosed:rig.smileEyes,0.9f*peak);
            face(rig.smileMouth,0.4f*release);
            break;
        }
        case Gesture::LookAround:
            // Looking to one side, then the other.
            neck(-2.0f*Deg,out*34.0f*Deg*std::sin(XM_2PI*u),0);
            break;
        case Gesture::HeadTilt:
            neck(-3.0f*Deg,0,-out*12.0f*Deg);
            break;
        case Gesture::Stand:
            break;
        case Gesture::TuckHair:
        {
            // Tucking the hair behind the ear: the hand comes up beside the head and strokes
            // back and down, the head tilting a little the other way, eyes lowered.
            const float stroke=ramp(0.8f,1.4f);
            ReachArm(g,s,XMVectorSet(out*(0.17f+0.02f*stroke)*L,rig.headY+(0.10f-0.06f*stroke)*L,S.z-(0.10f-0.12f*stroke)*L,0),XMVectorSet(out,-1,0.2f,0));
            CurlFingers(g,s,{0.15f,0.15f,0.25f,0.35f});
            neck(-6.0f*Deg,-out*6.0f*Deg,out*6.0f*Deg);
            break;
        }
        case Gesture::Yawn:
        {
            // A yawn: the mouth opens wide behind the back of a hand, eyes shut, head back a
            // little, then a small blink of relief.
            const float open=std::sin(XM_PI*std::clamp((a-0.5f)/std::max(len-1.6f,0.5f),0.0f,1.0f));
            ReachArm(g,s,XMVectorSet(out*0.05f*L,rig.headY-0.08f*L,faceZ-0.14f*L,0),XMVectorSet(out,-1,0.1f,0));
            CurlFingers(g,s,{0.15f,0.15f,0.2f,0.25f});
            SetRotation(g,rig.upperBody,XMQuaternionRotationRollPitchYaw(3.0f*Deg*open,0,0));
            neck(8.0f*Deg*open,0,-out*4.0f*Deg);
            face(rig.mouthOpen,0.9f*open);
            face(rig.eyesClosed>=0?rig.eyesClosed:rig.smileEyes,std::min(1.0f,1.4f*open));
            break;
        }
        case Gesture::Think:
            // Thinking: a hand at the chin, the other arm across the front holding that elbow,
            // the head tilted and turned a little up.
            ReachArm(g,s,XMVectorSet(out*0.03f*L,rig.headY-0.10f*L,faceZ-0.10f*L,0),XMVectorSet(out*0.3f,-1,0,0));
            CurlFingers(g,s,{0.5f,0.6f,0.8f,0.8f});
            ReachArm(g,1-s,XMVectorSet(out*0.10f*L,S.y-0.34f*L,S.z-0.22f*L,0),XMVectorSet(-out*0.6f,-1,0.4f,0));
            CurlFingers(g,1-s,{0.3f,0.3f,0.35f,0.35f});
            neck(5.0f*Deg,out*8.0f*Deg,-out*9.0f*Deg+out*3.0f*Deg*std::sin(XM_2PI*a/3.0f));
            break;
        case Gesture::Hum:
        {
            // Humming along: the body sways gently side to side with a soft smile.
            const float sway=std::sin(XM_2PI*a/1.6f);
            SetRotation(g,rig.upperBody,XMQuaternionRotationRollPitchYaw(0,0,4.0f*Deg*sway));
            SetRotation(g,rig.lowerBody,XMQuaternionRotationRollPitchYaw(0,0,-1.5f*Deg*sway));
            neck(-2.0f*Deg,0,5.0f*Deg*std::sin(XM_2PI*a/1.6f-0.6f));
            face(rig.smileEyes,0.6f);
            face(rig.smileMouth,0.4f);
            break;
        }
        case Gesture::Clap:
        {
            // A few small claps in front of the chest, smiling.
            const float beat=0.5f+0.5f*std::cos(XM_2PI*2.4f*std::max(a-0.6f,0.0f));
            both([&](int side,float o){
                ReachArm(g,side,XMVectorSet(o*(0.045f+0.07f*beat)*L,S.y-0.14f*L,S.z-0.30f*L,0),XMVectorSet(o,-1,0.5f,0));
                CurlFingers(g,side,{0.1f,0.1f,0.15f,0.2f});
            });
            neck(-3.0f*Deg,0,0);
            face(rig.smileEyes,0.9f);
            face(rig.smileMouth,0.7f);
            break;
        }
        case Gesture::LookFar:
            // Shading the eyes with a hand and gazing into the distance, slowly scanning. The
            // hand is held at the temple, as in a salute, its fingers over the brow.
            ReachArm(g,s,XMVectorSet(out*0.19f*L,rig.headY+0.04f*L,faceZ-0.01f*L,0),XMVectorSet(out,-0.15f,-0.3f,0));
            CurlFingers(g,s,{0.05f,0.05f,0.1f,0.15f});
            neck(6.0f*Deg,out*18.0f*Deg*std::sin(XM_PI*(u-0.5f)),0);
            break;
        case Gesture::Cheeks:
            // Both hands on the cheeks with a happy smile, the head rocking a little.
            both([&](int side,float o){
                ReachArm(g,side,XMVectorSet(o*0.14f*L,rig.headY-0.04f*L,faceZ-0.04f*L,0),XMVectorSet(o,-1,0.1f,0));
                CurlFingers(g,side,{0.1f,0.1f,0.15f,0.2f});
            });
            neck(-4.0f*Deg,0,7.0f*Deg*std::sin(XM_2PI*a/2.2f));
            face(rig.smileEyes,1.0f);
            face(rig.smileMouth,0.8f);
            break;
        case Gesture::PatHead:
            // Patted on the head: eyes shut with a smile, the head bowed and tilted into it with
            // small nods, the hands held together in front, a little shy.
            both([&](int side_,float o){ReachArm(g,side_,XMVectorSet(o*0.03f*L,hipY-0.08f*L,hipZ-0.14f*L,0),XMVectorSet(o,-0.2f,0.6f,0));CurlFingers(g,side_,{0.35f,0.35f,0.4f,0.4f});});
            SetRotation(g,rig.upperBody,XMQuaternionRotationRollPitchYaw(-3.0f*Deg,0,out*2.0f*Deg));
            neck(-12.0f*Deg-4.0f*Deg*std::sin(XM_2PI*a/0.9f),0,-out*10.0f*Deg);
            face(rig.eyesClosed>=0&&rig.smileEyes<0?rig.eyesClosed:rig.smileEyes,1.0f);
            face(rig.smileMouth,0.6f);
            break;
        case Gesture::Surprised:
        {
            // Startled: the hands jump up to the chest and the body leans back, mouth open;
            // then it turns into a smile.
            const float relief=ramp(0.7f,0.6f);
            both([&](int side_,float o){
                ReachArm(g,side_,XMVectorSet(o*0.10f*L,rig.shoulderPos[side_].y-0.10f*L,rig.shoulderPos[side_].z-0.24f*L,0),XMVectorSet(o,-1,0.4f,0));
                CurlFingers(g,side_,{0.3f,0.3f,0.35f,0.4f});
            });
            SetRotation(g,rig.upperBody,XMQuaternionRotationRollPitchYaw((6.0f-3.0f*relief)*Deg,0,0));
            neck((7.0f-6.0f*relief)*Deg,0,-out*5.0f*Deg*relief);
            face(rig.mouthOpen,0.6f*(1-relief));
            face(rig.smileEyes,0.9f*relief);
            face(rig.smileMouth,0.6f*relief);
            break;
        }
        case Gesture::Curious:
            // "Hm?": the head tilts far to one side and turns a little, a faint smile.
            neck(-3.0f*Deg,out*8.0f*Deg,-out*16.0f*Deg);
            SetRotation(g,rig.upperBody,XMQuaternionRotationRollPitchYaw(0,0,-out*3.0f*Deg));
            face(rig.smileMouth,0.3f);
            break;
        case Gesture::ShyNod:
            // Sitting or hanging: only the head answers, a small shy tilt with a smile.
            neck(-7.0f*Deg,0,-out*9.0f*Deg);
            face(rig.smileEyes,0.8f);
            face(rig.smileMouth,0.5f);
            break;
        case Gesture::None:
            break;
        }
    }

    // Now and then while standing idle, a gesture blends in over the standing pose for a few
    // seconds and back out. The next one never repeats the last two, so the cycle varies.
    void GeneratedMotion::Gestures(std::vector<Key>& keys,double dt)
    {
        const float L=rig.legLength;
        morphOut.clear();
        if(!gesturesAllowed){gesture=Gesture::None;gestureWeight=0;nextGestureAt=std::max(nextGestureAt,clock+4.0);return;}
        if(gesture==Gesture::None)
        {
            gestureWeight=0;
            if(clock<nextGestureAt)return;
            if(reaction!=Gesture::None){nextGestureAt=clock+1.5;return;}
            Gesture next;
            do next=static_cast<Gesture>(1+static_cast<int>(Random01()*GestureCount)%GestureCount);
            while(next==recentGestures[0]||next==recentGestures[1]);
            recentGestures={next,recentGestures[0]};
            gesture=next;
            gestureSide=Random01()<0.5f?0:1;
            gestureTime=0;
            const auto range=GestureLengths(next);
            gestureLength=range[0]+(range[1]-range[0])*Random01();
        }
        gestureTime=frozenAt>=0?frozenAt:gestureTime+dt;
        if(gestureTime>=gestureLength)
        {
            gesture=Gesture::None;
            gestureWeight=0;
            nextGestureAt=clock+1.5+Random01();
            return;
        }
        constexpr double Blend=0.7;
        gestureWeight=frozenAt>=0?1.0f:Smooth(static_cast<float>(std::min({gestureTime/Blend,(gestureLength-gestureTime)/Blend,1.0})));

        std::vector<Key> g=keys;
        PoseGesture(gesture,gestureTime,gestureLength,gestureSide,gestureWeight,g,morphOut);
        for(size_t i=0;i<keys.size();++i)
        {
            XMStoreFloat4(&keys[i].rotation,XMQuaternionSlerp(XMLoadFloat4(&keys[i].rotation),XMLoadFloat4(&g[i].rotation),gestureWeight));
            XMStoreFloat3(&keys[i].position,XMVectorLerp(XMLoadFloat3(&keys[i].position),XMLoadFloat3(&g[i].position),gestureWeight));
        }
    }

    bool GeneratedMotion::React(bool head)
    {
        if(reaction!=Gesture::None||overrides.empty())return false;
        Gesture pick;
        if(activity==Activity::Idle)
        {
            static constexpr Gesture headReactions[]={Gesture::PatHead,Gesture::PatHead,Gesture::Cheeks};
            static constexpr Gesture bodyReactions[]={Gesture::Surprised,Gesture::Wave,Gesture::Peace,Gesture::Curious};
            pick=head?headReactions[static_cast<int>(Random01()*3)%3]:bodyReactions[static_cast<int>(Random01()*4)%4];
        }
        else if(activity==Activity::Sit||activity==Activity::Hang)pick=Gesture::ShyNod;
        else return false;
        reaction=pick;
        reactionSide=Random01()<0.5f?0:1;
        reactionTime=0;
        reactionLength=pick==Gesture::PatHead?2.8:pick==Gesture::Curious||pick==Gesture::ShyNod?2.2:2.5;
        reactionWeight=0;
        return true;
    }

    // A click reaction over everything else this layer made (standing, gestures, an idle
    // motion passed through): quick in, gentle out.
    void GeneratedMotion::ApplyReaction(std::vector<Key>& keys,double dt)
    {
        reactionMorphs.clear();
        if(reaction==Gesture::None)return;
        reactionTime+=dt;
        // Picked up or set walking meanwhile: let go of it.
        if(activity!=Activity::Idle&&activity!=Activity::Sit&&activity!=Activity::Hang)
            reactionLength=std::min(reactionLength,reactionTime+0.5);
        if(reactionTime>=reactionLength){reaction=Gesture::None;reactionWeight=0;return;}
        const double blendIn=reaction==Gesture::Surprised?0.12:0.3;
        reactionWeight=Smooth(static_cast<float>(std::min({reactionTime/blendIn,(reactionLength-reactionTime)/0.5,1.0})));
        std::vector<Key> g=keys;
        PoseGesture(reaction,reactionTime,reactionLength,reactionSide,reactionWeight,g,reactionMorphs);
        for(size_t i=0;i<keys.size();++i)
        {
            XMStoreFloat4(&keys[i].rotation,XMQuaternionSlerp(XMLoadFloat4(&keys[i].rotation),XMLoadFloat4(&g[i].rotation),reactionWeight));
            XMStoreFloat3(&keys[i].position,XMVectorLerp(XMLoadFloat3(&keys[i].position),XMLoadFloat3(&g[i].position),reactionWeight));
        }
    }

    void GeneratedMotion::Generate(std::vector<Key>& keys,double dt)
    {
        const float L=rig.legLength;
        const float t=static_cast<float>(clock);
        const float follow=static_cast<float>(1.0-std::exp(-dt/0.18));
        activityTime+=dt;
        const bool profile=activity==Activity::Walk||activity==Activity::Run||((activity==Activity::Jump||activity==Activity::Sit)&&direction!=0);
        // Sitting turns only half as far as walking: enough for the knees to point aside.
        const float targetYaw=profile?-direction*WalkYaw*(activity==Activity::Sit?0.5f:1.0f):0.0f;
        yaw+=(targetYaw-yaw)*follow;

        auto footKey=[&](int side,FXMVECTOR target,float pitch,float footYaw){
            auto& k=KeyOf(keys,rig.legIK[side]);
            XMStoreFloat3(&k.position,XMVectorSubtract(target,XMLoadFloat3(&rig.ankleRest[side])));
            XMStoreFloat4(&k.rotation,Then(XMQuaternionRotationRollPitchYaw(pitch,0,0),XMQuaternionRotationRollPitchYaw(0,footYaw,0)));
        };
        // Arms: swing in the rest plane first, then roll about Z to the base angle (hanging by
        // default). A roll about Z leaves the forward component of the swing unchanged.
        auto arms=[&](float swing0,float swing1,float roll0,float roll1,float elbow0,float elbow1){
            const float swing[2]={swing0,swing1},roll[2]={roll0,roll1},elbow[2]={elbow0,elbow1};
            for(int side=0;side<2;++side)
            {
                if(rig.arm[side]>=0)SetRotation(keys,rig.arm[side],Then(Rotation(rig.armSwingAxis[side],swing[side]),
                                                                        XMQuaternionRotationRollPitchYaw(0,0,roll[side])));
                if(rig.elbow[side]>=0)SetRotation(keys,rig.elbow[side],Rotation(rig.elbowBendAxis[side],elbow[side]));
            }
        };
        auto limbs=[&](float armSwing0,float armSwing1,float elbowBend){
            arms(armSwing0,armSwing1,rig.armLower[0],rig.armLower[1],elbowBend,elbowBend);
        };
        // Planted foot under the hip for the current facing (ankle rest rotated with the body).
        auto standingFoot=[&](int side,float forwardOffset,float lift){
            const auto& rest=rig.ankleRest[side];
            const XMMATRIX turn=XMMatrixRotationY(yaw);
            const XMVECTOR forward=XMVector3TransformNormal(XMVectorSet(0,0,-1,0),turn);
            const XMVECTOR base=XMVector3TransformNormal(XMVectorSet(rest.x,0,rest.z,0),turn);
            return XMVectorAdd(XMVectorAdd(base,XMVectorScale(forward,forwardOffset)),XMVectorSet(0,rest.y+lift,0,0));
        };
        auto gait=[&](const Gait& g){
            // Step only once the body has mostly turned; the stride eases in and out.
            const float turned=1.0f-std::min(1.0f,std::abs(targetYaw-yaw)/(40.0f*Deg));
            walkAmount=Approach(walkAmount,turned,static_cast<float>(dt/g.easeSeconds));
            walkPhase+=dt/g.cycle;
            const float a=walkAmount,p=Frac(walkPhase),c=std::cos(XM_2PI*p);
            const float stride=g.stride*L*a,lift=g.lift*L*a;
            const XMVECTOR forward=XMVector3TransformNormal(XMVectorSet(0,0,-1,0),XMMatrixRotationY(yaw));
            // Where a foot is at its phase ph: how far forward of its rest place, how high, and
            // how it is tilted.
            auto place=[&](int side,float ph,float& u,float& h,float& pitch){
                // The foot rolls heel to toe: it lands on the heel with the toes up and is flat
                // almost at once, stays flat until the other foot is about to land, then the heel
                // rises and it pushes off from the toes. It swings through toes down, and the toes
                // come up again for the next landing.
                u=0;h=0;pitch=0;
                if(ph<g.stance)
                {
                    const float s=ph/g.stance;
                    u=stride*(0.5f-g.back-s);
                    pitch=g.heel*(1-Smooth(s/0.1f))-g.toeOff*Smooth((s-g.heelRise)/(1-g.heelRise));
                }
                else
                {
                    const float s=(ph-g.stance)/(1.0f-g.stance);
                    const float t=std::clamp((s-g.swingDelay)/(1-g.swingDelay),0.0f,1.0f);
                    u=stride*(0.5f*(t+Smooth(t))-0.5f-g.back);
                    // Up and down without a jolt (no speed at either end), highest early on.
                    const float arc=std::sin(XM_PI*std::pow(s,g.liftSkew));
                    h=lift*arc*arc;
                    // Toes down (further for a run's kick back), then the toes come up for the landing.
                    const float turn=Smooth((s-g.kickAt)/(1-g.kickAt));
                    pitch=-(g.toeOff+(g.kick-g.toeOff)*Smooth(s/g.kickAt))*(1-turn)+g.heel*turn;
                }
                pitch*=a;
                // Heel up: the foot turns about the toe tip, so the ankle rises (and comes a
                // little forward) instead of the toes sinking into the ground. Toes up: about
                // the heel, which is close under the ankle.
                const float d=rig.toeAhead[side],hA=rig.toeBelow[side];
                if(pitch<0)
                {
                    const float th=-pitch;
                    u+=d-d*std::cos(th)+hA*std::sin(th);
                    h+=d*std::sin(th)+hA*std::cos(th)-hA;
                }
                else if(pitch>0)
                {
                    const float e=0.25f*d;
                    u+=e*std::cos(pitch)-hA*std::sin(pitch)-e;
                    h+=std::max(0.0f,e*std::sin(pitch)+hA*std::cos(pitch)-hA);
                }
            };
            for(int side=0;side<2;++side)
            {
                float u,h,pitch;
                place(side,Frac(p+side*0.5),u,h,pitch);
                footKey(side,standingFoot(side,u,h),pitch,yaw);
            }
            // センター travels with the planted foot: stance feet move back at exactly this speed.
            const float speed=g.stride*L/(g.stance*static_cast<float>(g.cycle));
            rootOffset+=speed*a*XMVectorGetX(forward)*static_cast<float>(dt);
            const float chest=yaw*(WalkChestYaw/WalkYaw);
            // The hips ride a smooth wave, two dips a cycle. Walking is lowest a little after
            // contact (the weight comes onto the front leg) and highest as the other foot comes
            // through; running is lowest at mid-stance, highest in flight.
            auto wave=[&](float q){return 0.5f+0.5f*std::cos(2*XM_2PI*(q-g.lowAt));};
            const float depth=g.bob*L*a;
            KeyOf(keys,rig.groove).position={0,-(g.bobBase*L*a+depth*wave(p)),0};
            SetRotation(keys,rig.lowerBody,XMQuaternionRotationRollPitchYaw(0,yaw+g.hipTwist*a*c,0));
            SetRotation(keys,rig.upperBody,Then(XMQuaternionRotationRollPitchYaw(-g.lean*a,0,0),XMQuaternionRotationRollPitchYaw(0,chest-g.chestTwist*a*c,0)));
            // The neck turns back against the chest's twist so the head stays steady.
            SetRotation(keys,rig.neck,XMQuaternionRotationRollPitchYaw(g.lean*0.5f*a,-yaw*(WalkNeckYaw/WalkYaw)+0.9f*g.chestTwist*a*c,0));
            // Arms swing against the legs, a touch behind them, the elbow bending a little more
            // as the arm comes forward.
            const float lag=std::cos(XM_2PI*(p-0.03f));
            const float swing0=-g.armSwing*a*lag,swing1=g.armSwing*a*lag;
            arms(swing0,swing1,rig.armLower[0],rig.armLower[1],
                 g.elbow+0.3f*std::max(0.0f,swing0),g.elbow+0.3f*std::max(0.0f,swing1));
        };

        switch(activity)
        {
        case Activity::Idle:
        {
            // Standing at ease, feet a little apart, arms hanging relaxed. The weight rests on
            // one foot (or both) for a while, then moves over in about a second and a half.
            // センター carries the hips over the standing foot (X only), that hip rises (下半身
            // roll) and the chest leans back over it (上半身); the other leg relaxes with the
            // knee a little forward and the heel up.
            if(weightTime<weightDuration)weightTime+=dt;
            else if(clock>=weightHoldUntil)
            {
                float next;
                do next=static_cast<float>(static_cast<int>(Random01()*3.0f)-1);
                while(next==weightTo);
                weightFrom=weightTo;
                weightTo=next;
                weightTime=0;
                weightDuration=1.2+0.6*Random01();
                weightHoldUntil=clock+weightDuration+(next==0?2.5+3.0*Random01():4.0+6.0*Random01());
            }
            const float shift=weightFrom+(weightTo-weightFrom)*Smooth(static_cast<float>(weightTime/weightDuration));
            for(int side=0;side<2;++side)
            {
                const float out=side==0?1.0f:-1.0f;
                const float relaxed=std::clamp(-shift*out,0.0f,1.0f);   // this foot carries no weight
                footKey(side,XMVectorAdd(standingFoot(side,0.04f*L*relaxed,0.012f*L*relaxed),XMVectorSet(out*0.035f*L,0,0,0)),-7.0f*Deg*relaxed,0);
            }
            KeyOf(keys,rig.center).position={0.05f*L*shift,0,0};
            KeyOf(keys,rig.groove).position={0,-0.012f*L*std::abs(shift),0};
            SetRotation(keys,rig.lowerBody,XMQuaternionRotationRollPitchYaw(0,0,3.5f*Deg*shift));
            SetRotation(keys,rig.upperBody,XMQuaternionRotationRollPitchYaw(1.0f*Deg,0,-2.5f*Deg*shift));
            SetRotation(keys,rig.neck,XMQuaternionRotationRollPitchYaw(0,0,1.0f*Deg*shift));
            const float sway=1.5f*Deg*std::sin(XM_2PI*t/5.3f);
            limbs(sway,-sway,10.0f*Deg);
            if(generatedIdle)Gestures(keys,dt);
            break;
        }
        case Activity::Walk:gait(WalkGait);break;
        case Activity::Run:gait(RunGait);break;
        case Activity::Jump:
        {
            // Crouch (feet planted, arms back), push off, then in the air: knees tucked while
            // rising, legs reaching down for the landing as the fall speeds up. The window's
            // vertical speed (carryVelocity.y, up positive) tells which part of the arc it is.
            const float air=Smooth(static_cast<float>((activityTime-JumpCrouchSeconds)/0.12));
            const float crouch=Smooth(static_cast<float>(activityTime/JumpCrouchSeconds))*(1.0f-air);
            const float falling=std::clamp(-carryVelocity.y/(4.0f*L),0.0f,1.0f);
            const float tuck=air*(1.0f-0.75f*falling);
            for(int side=0;side<2;++side)
            {
                const float front=side==0?1.0f:-1.0f;
                footKey(side,standingFoot(side,tuck*(0.06f+0.08f*front)*L,tuck*0.28f*L),-20.0f*Deg*air,yaw);
            }
            KeyOf(keys,rig.groove).position={0,-0.2f*L*crouch,0};
            SetRotation(keys,rig.lowerBody,XMQuaternionRotationRollPitchYaw(0,yaw,0));
            SetRotation(keys,rig.upperBody,Then(XMQuaternionRotationRollPitchYaw((-18.0f*crouch-4.0f*air)*Deg,0,0),
                                                XMQuaternionRotationRollPitchYaw(0,yaw*(WalkChestYaw/WalkYaw),0)));
            SetRotation(keys,rig.neck,XMQuaternionRotationRollPitchYaw(0,-yaw*(WalkNeckYaw/WalkYaw),0));
            // Arms swing back in the crouch, forward and up through the push, and spread a little
            // lower for balance on the way down.
            const float swing=(-35.0f*crouch+65.0f*air-25.0f*air*falling)*Deg;
            limbs(swing,swing,(20.0f+15.0f*air)*Deg);
            break;
        }
        case Activity::Lie:
        {
            // Lying on the side along the edge, facing the viewer: head to screen left, the lower
            // arm as a pillow, the upper arm resting along the body. The whole body turns through
            // 上半身/下半身 (never センター) and グルーブ lowers it onto the edge.
            const float roll=90.0f*Deg;
            const float pivotHeight=0.17f*L;   // half the body's thickness above the edge
            const float groove=pivotHeight-rig.lowerPivot.y;
            KeyOf(keys,rig.groove).position={0,groove,0};
            const XMVECTOR q=XMQuaternionRotationRollPitchYaw(0,0,roll);
            SetRotation(keys,rig.lowerBody,q);
            SetRotation(keys,rig.upperBody,Then(XMQuaternionRotationRollPitchYaw(0,0,-4.0f*Deg),q));
            SetRotation(keys,rig.neck,XMQuaternionRotationRollPitchYaw(0,0,-12.0f*Deg));
            const XMMATRIX r=XMMatrixRotationQuaternion(q);
            const XMVECTOR pivot=XMVectorSet(rig.lowerPivot.x,pivotHeight,rig.lowerPivot.z,0);
            for(int side=0;side<2;++side)
            {
                // Hip joint after the roll, then the leg laid out along +X with a slight knee bend
                // toward the viewer; the upper (left) leg rests a little forward.
                const XMVECTOR hip=XMVectorAdd(pivot,XMVector3TransformNormal(XMVectorSubtract(XMLoadFloat3(&rig.hipRest[side]),XMLoadFloat3(&rig.lowerPivot)),r));
                const float sway=0.03f*L*std::sin(XM_2PI*t/3.4f+side*XM_PI);
                const XMVECTOR ankle=XMVectorAdd(hip,XMVectorSet(0.9f*L,0,(side==0?-0.18f:-0.06f)*L+sway,0));
                footKey(side,ankle,0,0);
                if(rig.legIK[side]>=0)XMStoreFloat4(&KeyOf(keys,rig.legIK[side]).rotation,q);
            }
            // Right (lower) arm up past the head as a pillow; left (upper) arm along the body.
            // (armLower[0] is negative: subtracting lowers the upper arm further, onto the hip.)
            arms(0,0,rig.armLower[0]-16.0f*Deg,rig.armRaise[1]-15.0f*Deg,20.0f*Deg,45.0f*Deg);
            break;
        }
        case Activity::Hang:
        {
            // Hanging from the edge with the face over it. グルーブ lowers the body until 頭 (at the
            // base of the head) is a little above the edge, so the chin is about at it and the
            // hands grip the edge beside the face. The window, drawn in front, hides the rest.
            const float headBelow=-0.10f*L;
            const float groove=-(rig.headY+headBelow);
            KeyOf(keys,rig.groove).position={0,groove,0};
            const float swing=2.0f*Deg*std::sin(XM_2PI*t/2.8f);
            for(int side=0;side<2;++side)
            {
                const auto& rest=rig.ankleRest[side];
                const float kick=0.03f*L*std::sin(XM_2PI*t/2.4f+side*0.6f);
                footKey(side,XMVectorSet(rest.x,rest.y+groove+0.03f*L,rest.z-0.05f*L+kick,0),-20.0f*Deg,0);
            }
            SetRotation(keys,rig.lowerBody,XMQuaternionRotationRollPitchYaw(0,0,swing));
            SetRotation(keys,rig.upperBody,XMQuaternionRotationRollPitchYaw(0,0,-swing*0.5f));
            // Top of a pull-up: upper arms out to the sides a little below the shoulders, elbows
            // bent so the forearms point up and the hands grip the edge beside the face. The
            // upper arm is turned about its own length so the elbow's bend axis faces the viewer
            // and the elbow bends upward; the elbow itself still only bends.
            const float handRise=rig.headY+headBelow-rig.shoulderY+0.03f*L;   // edge above the shoulders
            const float forearmUp=80.0f*Deg;                                   // forearms tilted slightly inward
            const float drop=std::asin(std::clamp((rig.gripReach*std::sin(forearmUp)-handRise)/rig.upperArm,-0.5f,0.95f));
            for(int side=0;side<2;++side)
            {
                if(rig.arm[side]<0||rig.elbow[side]<0)continue;
                const float out=side==0?1.0f:-1.0f;   // the left arm points to +X
                const XMVECTOR upper=XMVectorSet(out*std::cos(drop),-std::sin(drop),0,0);
                const XMVECTOR fore=XMVectorSet(-out*std::cos(forearmUp),std::sin(forearmUp),0,0);
                const float bendAngle=std::acos(std::clamp(XMVectorGetX(XMVector3Dot(upper,fore)),-1.0f,1.0f));
                // The bend axis that turns the upper arm's direction into the forearm's.
                XMVECTOR bendAxis=XMVector3Normalize(XMVector3Cross(upper,fore));
                if(XMVectorGetX(XMVector3Dot(XMVector3Rotate(upper,XMQuaternionRotationNormal(bendAxis,bendAngle)),fore))<0.99f)
                    bendAxis=XMVectorNegate(bendAxis);
                // Rotation taking the rest frame (arm direction, elbow bend axis) onto that one.
                const XMVECTOR a0=XMLoadFloat3(&rig.armRestDir[side]);
                XMVECTOR b0=XMLoadFloat3(&rig.elbowBendAxis[side]);
                b0=XMVector3Normalize(XMVectorSubtract(b0,XMVectorScale(a0,XMVectorGetX(XMVector3Dot(a0,b0)))));
                const XMMATRIX from{a0,b0,XMVector3Cross(a0,b0),XMVectorSet(0,0,0,1)};
                const XMMATRIX to{upper,bendAxis,XMVector3Cross(upper,bendAxis),XMVectorSet(0,0,0,1)};
                SetRotation(keys,rig.arm[side],XMQuaternionRotationMatrix(XMMatrixTranspose(from)*to));
                SetRotation(keys,rig.elbow[side],Rotation(rig.elbowBendAxis[side],bendAngle));
                // Fingers curled over the edge (the thumb stays as it is).
                static constexpr float curl[3]={55.0f*Deg,75.0f*Deg,50.0f*Deg};
                for(const auto& f:rig.fingers[side])SetRotation(keys,f.bone,Rotation(rig.fingerCurlAxis[side],curl[f.joint]));
            }
            break;
        }
        case Activity::Sit:
        {
            // Sit on the edge line (a window's top edge) with the legs hanging over it. Seated in
            // profile (direction: knees to that side) with 下半身, the chest and head turned back
            // toward the viewer; facing the viewer, raised knees would point at the camera.
            const float seat=0.12f*L;
            const float hipY=(rig.hipRest[0].y+rig.hipRest[1].y)*0.5f;
            KeyOf(keys,rig.groove).position={0,seat-hipY,0};
            const XMMATRIX turn=XMMatrixRotationY(yaw);
            const XMVECTOR pivot=XMVectorSet(rig.lowerPivot.x,0,rig.lowerPivot.z,0);
            for(int side=0;side<2;++side)
            {
                const auto& rest=rig.ankleRest[side];
                const auto& hip=rig.hipRest[side];
                // Gentle alternating swing of the lower legs.
                const float shinAngle=SitShinForward+8.0f*Deg*std::sin(XM_2PI*t/2.6f+side*XM_PI);
                // Ankle relative to the hip in the body's own frame (forward is -Z), then turned.
                const XMVECTOR ankle=XMVectorSet(rest.x-hip.x,
                                                 -rig.thigh*std::sin(SitThighDown)-rig.shin*0.97f*std::cos(shinAngle),
                                                 -rig.thigh*std::cos(SitThighDown)-rig.shin*0.97f*std::sin(shinAngle),0);
                const XMVECTOR hipTurned=XMVectorAdd(pivot,XMVector3TransformNormal(XMVectorSet(hip.x-rig.lowerPivot.x,0,hip.z-rig.lowerPivot.z,0),turn));
                const XMVECTOR target=XMVectorAdd(XMVectorAdd(hipTurned,XMVectorSet(0,seat,0,0)),XMVector3TransformNormal(ankle,turn));
                footKey(side,target,-25.0f*Deg,yaw);
            }
            SetRotation(keys,rig.lowerBody,XMQuaternionRotationRollPitchYaw(0,yaw,0));
            SetRotation(keys,rig.upperBody,Then(XMQuaternionRotationRollPitchYaw(3.0f*Deg,0,0),XMQuaternionRotationRollPitchYaw(0,yaw*0.5f,0)));
            SetRotation(keys,rig.neck,XMQuaternionRotationRollPitchYaw(0,-yaw*0.4f,0));
            limbs(-10.0f*Deg,-10.0f*Deg,12.0f*Deg);
            break;
        }
        case Activity::Grabbed:
        {
            // Hanging limp: legs dangle and trail behind the window's motion.
            const float k=static_cast<float>(1.0-std::exp(-dt/0.3));
            const float target=std::clamp(-0.03f*carryVelocity.x,-0.25f*L,0.25f*L);
            sway.x+=(target-sway.x)*k;
            for(int side=0;side<2;++side)
            {
                const auto& rest=rig.ankleRest[side];
                const float kick=0.04f*L*std::sin(XM_2PI*t/0.9f+side*XM_PI);
                footKey(side,XMVectorSet(rest.x+sway.x,rest.y+0.08f*L,rest.z-0.06f*L+kick,0),-30.0f*Deg,0);
            }
            SetRotation(keys,rig.lowerBody,XMQuaternionRotationRollPitchYaw(0,0,std::clamp(0.6f*sway.x/L,-0.3f,0.3f)));
            SetRotation(keys,rig.upperBody,XMQuaternionRotationRollPitchYaw(-3.0f*Deg,0,0));
            limbs(20.0f*Deg,20.0f*Deg,35.0f*Deg);
            break;
        }
        case Activity::Fall:
        {
            for(int side=0;side<2;++side)
            {
                const auto& rest=rig.ankleRest[side];
                const float kick=0.05f*L*std::sin(XM_2PI*t/0.5f+side*XM_PI);
                footKey(side,XMVectorSet(rest.x,rest.y+0.12f*L,rest.z-0.12f*L+kick,0),-15.0f*Deg,0);
            }
            SetRotation(keys,rig.upperBody,XMQuaternionRotationRollPitchYaw(5.0f*Deg,0,0));
            limbs(55.0f*Deg,55.0f*Deg,25.0f*Deg);
            break;
        }
        }
    }

    void GeneratedMotion::DistributeTwist(std::vector<Key>& keys) const
    {
        // Split ひじ into twist about its local X (moved to 腕捩) and a swing kept about local Y only.
        for(int side=0;side<2;++side)
        {
            const int elbow=rig.elbow[side];
            if(elbow<0||slot[elbow]<0)continue;
            auto& e=keys[slot[elbow]];
            const XMVECTOR q=XMLoadFloat4(&e.rotation);
            const XMVECTOR x=XMLoadFloat3(&rig.elbowTwistAxis[side]),y=XMLoadFloat3(&rig.elbowBendAxis[side]);
            const float w=XMVectorGetW(q);
            const float twistAngle=2.0f*std::atan2(XMVectorGetX(XMVector3Dot(q,x)),w);
            if(std::abs(twistAngle)>1e-4f)
            {
                const XMVECTOR twist=XMQuaternionRotationNormal(x,twistAngle);
                const XMVECTOR swing=XMQuaternionMultiply(q,XMQuaternionInverse(twist));   // q = twist (after) * swing
                const float bend=2.0f*std::atan2(XMVectorGetX(XMVector3Dot(swing,y)),XMVectorGetW(swing));
                XMStoreFloat4(&e.rotation,XMQuaternionRotationNormal(y,bend));
                const int armTwist=rig.armTwist[side];
                if(armTwist>=0&&slot[armTwist]>=0)
                {
                    auto& a=keys[slot[armTwist]];
                    const XMVECTOR axis=XMLoadFloat3(&rig.armTwistAxis[side]);
                    const float sign=XMVectorGetX(XMVector3Dot(axis,x))<0?-1.0f:1.0f;
                    const XMVECTOR moved=XMQuaternionRotationNormal(axis,twistAngle*sign);
                    XMStoreFloat4(&a.rotation,XMQuaternionNormalize(XMQuaternionMultiply(moved,XMLoadFloat4(&a.rotation))));
                }
            }
            else
            {
                const float bend=2.0f*std::atan2(XMVectorGetX(XMVector3Dot(q,y)),w);
                XMStoreFloat4(&e.rotation,XMQuaternionRotationNormal(y,bend));
            }
        }
    }

    void GeneratedMotion::Apply(PMX::Pose& pose,double dt)
    {
        if(overrides.empty())return;
        clock+=dt;
        landTime+=dt;
        fadeTime+=dt;

        std::vector<Key> target(overrides.size());
        if(activity==Activity::Idle&&!generatedIdle)
        {
            for(size_t i=0;i<overrides.size();++i)
            {
                const auto& k=pose.boneKeys[overrides[i]];
                target[i]={k.rotation,k.position};
            }
            walkAmount=0;
            yaw=0;
        }
        else if(ClipGait())ClipGaitKeys(pose,target,dt);
        else
        {
            if(activity==Activity::Idle){walkAmount=0;yaw=0;}
            Generate(target,dt);
            DistributeTwist(target);
        }
        if(!initialized)
        {
            snapshot=lastOutput=target;
            fadeTime=fadeDuration;
            initialized=true;
        }

        const float w=fadeDuration>0?Smooth(static_cast<float>(fadeTime/fadeDuration)):1.0f;
        std::vector<Key> out(overrides.size());
        for(size_t i=0;i<overrides.size();++i)
        {
            XMStoreFloat4(&out[i].rotation,XMQuaternionSlerp(XMLoadFloat4(&snapshot[i].rotation),XMLoadFloat4(&target[i].rotation),w));
            XMStoreFloat3(&out[i].position,XMVectorLerp(XMLoadFloat3(&snapshot[i].position),XMLoadFloat3(&target[i].position),w));
        }
        lastOutput=out;
        ApplyReaction(out,dt);

        // Landing crouch on top of whatever comes next (the knees bend through the foot IK).
        if(rig.groove>=0&&landTime<0.8)
        {
            const float lt=static_cast<float>(landTime);
            const float envelope=lt<0.07f?lt/0.07f:std::exp(-(lt-0.07f)/0.15f);
            out[slot[rig.groove]].position.y-=landDepth*envelope;
        }

        for(size_t i=0;i<overrides.size();++i)
        {
            auto& k=pose.boneKeys[overrides[i]];
            k.rotation=out[i].rotation;
            k.position=out[i].position;
        }
        // Face morphs of the current gesture (on top of what the pose already has).
        if(activity!=Activity::Idle||!generatedIdle){morphOut.clear();gesture=Gesture::None;gestureWeight=0;}
        // A reaction's face replaces the gesture's (a wink under a smile over-deforms).
        for(const auto& [morph,weight]:morphOut)
            if(static_cast<size_t>(morph)<pose.morphKeys.size())pose.morphKeys[morph].value=std::max(pose.morphKeys[morph].value,weight*(1-reactionWeight));
        for(const auto& [morph,weight]:reactionMorphs)
            if(static_cast<size_t>(morph)<pose.morphKeys.size())pose.morphKeys[morph].value=std::max(pose.morphKeys[morph].value,weight);

        AddRootOffset(pose);
    }

    // The user's walking (or running) motion, made in place facing the front, turned to the side
    // the way the generated gait is: the foot IK targets swing round the vertical axis, 下半身
    // turns with them and 上半身 a little less (the chest stays toward the viewer). センター's
    // height goes to グルーブ (センター moves on the ground only), and the body travels at the
    // motion's own ground speed so the planted foot stays put.
    void GeneratedMotion::ClipGaitKeys(const PMX::Pose& pose,std::vector<Key>& keys,double dt)
    {
        activityTime+=dt;
        const float follow=static_cast<float>(1.0-std::exp(-dt/0.18));
        const float targetYaw=-direction*WalkYaw;
        yaw+=(targetYaw-yaw)*follow;
        const float turned=1.0f-std::min(1.0f,std::abs(targetYaw-yaw)/(40.0f*Deg));
        walkAmount=Approach(walkAmount,turned,static_cast<float>(dt/0.3));
        for(size_t i=0;i<overrides.size();++i)
        {
            const auto& k=pose.boneKeys[overrides[i]];
            keys[i]={k.rotation,k.position};
        }
        const XMMATRIX turn=XMMatrixRotationY(yaw);
        const XMVECTOR yawTurn=XMQuaternionRotationRollPitchYaw(0,yaw,0);
        auto key=[&](int bone)->Key*{return bone>=0&&static_cast<size_t>(bone)<slot.size()&&slot[bone]>=0?&keys[slot[bone]]:nullptr;};
        for(int side=0;side<2;++side)
            if(Key* k=key(rig.legIK[side]))
            {
                const XMVECTOR rest=XMLoadFloat3(&rig.ankleRest[side]);
                const XMVECTOR at=XMVector3Transform(XMVectorAdd(rest,XMLoadFloat3(&k->position)),turn);
                XMStoreFloat3(&k->position,XMVectorSubtract(at,rest));
                XMStoreFloat4(&k->rotation,Then(XMLoadFloat4(&k->rotation),yawTurn));
            }
        if(Key* k=key(rig.lowerBody))XMStoreFloat4(&k->rotation,Then(XMLoadFloat4(&k->rotation),yawTurn));
        if(Key* k=key(rig.upperBody))XMStoreFloat4(&k->rotation,Then(XMLoadFloat4(&k->rotation),XMQuaternionRotationRollPitchYaw(0,yaw*(WalkChestYaw/WalkYaw),0)));
        if(Key* k=key(rig.neck))XMStoreFloat4(&k->rotation,Then(XMLoadFloat4(&k->rotation),XMQuaternionRotationRollPitchYaw(0,-yaw*(WalkNeckYaw/WalkYaw),0)));
        if(Key* k=key(rig.center))
        {
            const float height=k->position.y;
            k->position.y=0;
            XMStoreFloat3(&k->position,XMVector3Transform(XMLoadFloat3(&k->position),turn));
            if(Key* g=key(rig.groove))g->position.y+=height;
        }
        const float speed=clipSpeed[activity==Activity::Run?1:0];
        rootOffset+=speed*walkAmount*XMVectorGetX(XMVector3TransformNormal(XMVectorSet(0,0,-1,0),turn))*static_cast<float>(dt);
    }

    // Accumulated walking travel: センター carries the body, the foot IK targets (parented to
    // 全ての親, not センター) are shifted alongside so every activity stays under the body.
    void GeneratedMotion::AddRootOffset(PMX::Pose& pose) const
    {
        if(rootOffset==0.0f)return;
        if(rig.center>=0&&static_cast<size_t>(rig.center)<pose.boneKeys.size())pose.boneKeys[rig.center].position.x+=rootOffset;
        for(int side=0;side<2;++side)
            if(rig.legIK[side]>=0&&static_cast<size_t>(rig.legIK[side])<pose.boneKeys.size())pose.boneKeys[rig.legIK[side]].position.x+=rootOffset;
    }
}
