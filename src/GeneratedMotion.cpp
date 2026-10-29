#define NOMINMAX
#include "GeneratedMotion.hpp"
#include "Text.hpp"
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
        // Flatter at both ends than Smooth: a slow start and a slow settle, as an eased curve.
        float Smoother(float t){t=std::clamp(t,0.0f,1.0f);return t*t*t*(t*(6*t-15)+10);}
        float Frac(double v){return static_cast<float>(v-std::floor(v));}
        float Approach(float value,float target,float step){return value<target?std::min(value+step,target):std::max(value-step,target);}
        XMVECTOR Rotation(const XMFLOAT3& axis,float angle){return XMQuaternionRotationNormal(XMLoadFloat3(&axis),angle);}
        // Row-vector convention used by PoseSolver: rotate by a, then by b.
        XMVECTOR Then(FXMVECTOR a,FXMVECTOR b){return XMQuaternionMultiply(a,b);}

        // A hand's frame: along its fingers, its palm's facing (made square to them), and the third.
        XMMATRIX HandFrame(FXMVECTOR along,FXMVECTOR facing)
        {
            const XMVECTOR x=XMVector3Normalize(along);
            const XMVECTOR y=XMVector3Normalize(XMVectorSubtract(facing,XMVectorScale(x,XMVectorGetX(XMVector3Dot(facing,x)))));
            return XMMATRIX{x,y,XMVector3Cross(x,y),XMVectorSet(0,0,0,1)};
        }
    }

    void GeneratedMotion::Setup(const PMX::PMXModel& pmx,const std::array<ArmRest,2>& arms)
    {
        rig={};
        overrides.clear();
        initialized=false;
        activity=Activity::Idle;
        // The first gesture and weight shift at a time of their own.
        nextGestureAt=2.0+4.0*Random01();
        weightHoldUntil=1.0+3.0*Random01();

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
                XMStoreFloat3(&rig.hangElbow[side],XMVectorAdd(restPos(rig.arm[side]),
                              XMVector3Rotate(armDir,XMQuaternionRotationRollPitchYaw(0,0,rig.armLower[side]))));
                if(const int shoulder=Find(pmx,{s+L"肩"});shoulder>=0)
                {
                    rig.shoulder[side]=shoulder;
                    XMStoreFloat3(&rig.claviclePos[side],restPos(shoulder));
                }
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
            rig.wrist[side]=wrist;
            rig.handTwist[side]=Find(pmx,{s+L"手捩"});
            XMStoreFloat3(&rig.handRestDir[side],XMVector3Normalize(XMVectorSubtract(restPos(middle1),restPos(wrist))));
            XMStoreFloat3(&rig.palmRestNormal[side],XMVector3Normalize(palm));
            {
                // The palm's middle: part way from the wrist to the knuckles, out on the palm's
                // surface. The surface is the farthest the hand's own vertices (those the wrist
                // mostly moves) reach on the palm side there; without them, a guess from the
                // width of the hand.
                const XMVECTOR at=restPos(wrist),n=XMVector3Normalize(palm);
                const XMVECTOR knuckles=XMVectorSubtract(XMVectorScale(XMVectorAdd(restPos(index1),restPos(little1)),0.5f),at);
                const XMVECTOR middle=XMVectorScale(knuckles,0.6f);
                const XMVECTOR d=XMVector3Normalize(knuckles);
                const float reach=XMVectorGetX(XMVector3Length(knuckles));
                float surface=-1e9f;
                for(const auto& v:pmx.vertices)
                {
                    bool mine=false;
                    for(int k=0;k<4;++k)if(v.bone[k]==wrist&&(v.weightType==0?k==0:v.weight[k]>=0.5f))mine=true;
                    if(!mine)continue;
                    const XMVECTOR rel=XMVectorSubtract(corrected(wrist,XMLoadFloat3(&v.position),true),at);
                    const float along=XMVectorGetX(XMVector3Dot(rel,d));
                    if(along<0.2f*reach||along>1.0f*reach)continue;
                    surface=std::max(surface,XMVectorGetX(XMVector3Dot(rel,n)));
                }
                const float width=XMVectorGetX(XMVector3Length(XMVectorSubtract(restPos(little1),restPos(index1))));
                if(surface<=-1e8f||surface>width)surface=0.25f*width;
                XMStoreFloat3(&rig.palmContact[side],XMVectorAdd(middle,XMVectorScale(n,surface-XMVectorGetX(XMVector3Dot(middle,n)))));
            }
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

        // How far down the arms hang standing: as far as the hand stays clear of what is beside
        // it at that height (hips, thighs, a skirt), measured on the model's own mesh, between
        // 72° (as before: loose clothes at the sides, such as an open coat, do not push the arms
        // out further) and 86° below horizontal. Narrow hips and no skirt let the arms hang
        // straighter. Hair and the arms' own vertices do not count.
        {
            const int head=Find(pmx,{L"頭"});
            auto under=[&](int bone,int ancestor){
                for(int b=bone,guard=0;b>=0&&b<static_cast<int>(pmx.bones.size())&&guard<512;b=pmx.bones[b].parent,++guard)if(b==ancestor)return true;
                return false;
            };
            std::vector<XMFLOAT3> body;
            for(const auto& v:pmx.vertices)
            {
                int main=v.bone[0];
                for(int k=1;k<4;++k)if(v.bone[k]>=0&&v.weight[k]>v.weight[0]&&v.weight[k]>0.5f)main=v.bone[k];
                if(main<0||main>=static_cast<int>(pmx.bones.size())||(head>=0&&under(main,head)))continue;
                if((rig.arm[0]>=0&&under(main,rig.arm[0]))||(rig.arm[1]>=0&&under(main,rig.arm[1])))continue;
                body.push_back(v.position);
            }
            const float L=rig.legLength;
            // Kept (a few thousand of them) to keep hands out of the body in gestures.
            const size_t step=std::max<size_t>(1,body.size()/6000);
            for(size_t i=0;i<body.size();i+=step)rig.body.push_back(body[i]);
            for(int side=0;side<2;++side)
            {
                rig.hangDegrees[side]=HangAngle/Deg;
                const int wrist=rig.wrist[side];
                const int tip=Find(pmx,{std::wstring(side==0?L"左":L"右")+L"中指３"});
                if(rig.arm[side]<0||rig.elbow[side]<0||wrist<0||tip<0||body.empty())continue;
                const XMVECTOR shoulder=restPos(rig.arm[side]);
                const XMVECTOR armDir=XMVectorSubtract(restPos(rig.elbow[side]),shoulder);
                const float rest=std::atan2(-XMVectorGetY(armDir),std::abs(XMVectorGetX(armDir)));
                const float o=side==0?1.0f:-1.0f;   // the left side is +X
                // The hand's inner side is the palm's surface (the palm faces the leg when hanging).
                const float surface=std::max(0.0f,XMVectorGetX(XMVector3Dot(XMLoadFloat3(&rig.palmContact[side]),XMLoadFloat3(&rig.palmRestNormal[side]))));
                const XMVECTOR wristRel=XMVectorSubtract(restPos(wrist),shoulder),tipRel=XMVectorSubtract(restPos(tip),shoulder);
                float best=72.0f;
                for(float degrees=86.0f;degrees>=72.0f;degrees-=1.0f)
                {
                    const XMVECTOR turn=XMQuaternionRotationRollPitchYaw(0,0,(degrees*Deg-rest)*(side==0?-1.0f:1.0f));
                    const XMVECTOR w=XMVectorAdd(shoulder,XMVector3Rotate(wristRel,turn)),t=XMVectorAdd(shoulder,XMVector3Rotate(tipRel,turn));
                    bool clear=true;
                    for(float f=0;f<=1.0f&&clear;f+=0.25f)
                    {
                        const XMVECTOR p=XMVectorLerp(w,t,f);
                        const float inner=o*XMVectorGetX(p)-surface,y=XMVectorGetY(p),z=XMVectorGetZ(p);
                        for(const XMFLOAT3& b:body)
                            if(std::abs(b.y-y)<0.03f*L&&std::abs(b.z-z)<0.08f*L&&o*b.x>inner-0.012f*L){clear=false;break;}
                    }
                    if(clear){best=degrees;break;}
                }
                rig.hangDegrees[side]=best;
                rig.armLower[side]=(best*Deg-rest)*(side==0?-1.0f:1.0f);
                XMStoreFloat3(&rig.hangElbow[side],XMVectorAdd(shoulder,XMVector3Rotate(armDir,XMQuaternionRotationRollPitchYaw(0,0,rig.armLower[side]))));
            }
        }
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
            pick(rig.serious,{L"真面目",L"まじめ",L"真剣"});
        }
        if(const int head=Find(pmx,{L"頭"});head>=0)rig.headY=XMVectorGetY(restPos(head));
        else rig.headY=rig.shoulderY+0.25f*rig.legLength;
        // The mouth: where the vertices the あ morph moves are (their middle, weighted by how far
        // each moves), at the front of the lips. A group morph is followed to the vertex morphs
        // it is made of.
        if(rig.mouthOpen>=0)
        {
            XMVECTOR sum=XMVectorZero();
            float weight=0,front=1e9f;
            auto gather=[&](auto&& self,int index,float ratio,int depth)->void{
                if(index<0||index>=static_cast<int>(pmx.morphs.size())||depth>4||ratio<=0)return;
                const auto& morph=pmx.morphs[index];
                if(morph.kind==0)
                {
                    const auto* group=reinterpret_cast<const PMX::PMXGroupMorphOffset*>(morph.offsets.data());
                    for(int j=0;j<morph.offsetCount;++j)self(self,group[j].morph,ratio*group[j].ratio,depth+1);
                    return;
                }
                if(morph.kind!=1)return;
                const auto* offsets=reinterpret_cast<const PMX::PMXVertexMorphOffset*>(morph.offsets.data());
                for(int j=0;j<morph.offsetCount;++j)
                {
                    const int v=offsets[j].vertex;
                    if(v<0||v>=static_cast<int>(pmx.vertices.size()))continue;
                    const float moved=ratio*XMVectorGetX(XMVector3Length(XMLoadFloat3(&offsets[j].offset)));
                    if(moved<1e-4f)continue;
                    const XMVECTOR p=XMLoadFloat3(&pmx.vertices[v].position);
                    sum=XMVectorAdd(sum,XMVectorScale(p,moved));
                    weight+=moved;
                    front=std::min(front,XMVectorGetZ(p));
                }
            };
            gather(gather,rig.mouthOpen,1.0f,0);
            if(weight>0)
            {
                XMVECTOR mouth=XMVectorScale(sum,1.0f/weight);
                mouth=XMVectorSetZ(mouth,front);
                XMStoreFloat3(&rig.mouth,mouth);
                rig.hasMouth=true;
            }
        }
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
            L"morphs smileEyes="+std::to_wstring(rig.smileEyes)+L" smileMouth="+std::to_wstring(rig.smileMouth)+L" wink="+std::to_wstring(rig.wink)+L" serious="+std::to_wstring(rig.serious)+L"\n"+
            L"hang L="+std::to_wstring(static_cast<int>(rig.hangDegrees[0]))+L" R="+std::to_wstring(static_cast<int>(rig.hangDegrees[1]))+L"\n";
    }

    void GeneratedMotion::SetActivity(Activity next,int dir)
    {
        const bool edgePose=next==Activity::Sit||next==Activity::Lie||next==Activity::Hang;
        if(edgePose&&!CanSit())next=Activity::Idle;   // lowering the body needs グルーブ
        const bool gait=next==Activity::Walk||next==Activity::Run;
        if(gait&&!CanWalk())next=Activity::Idle;
        const bool directional=gait||next==Activity::Jump||next==Activity::Sit||next==Activity::SideHop;
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
        case Activity::Bound:fadeDuration=0.08;break;
        case Activity::SideHop:fadeDuration=0.08;break;
        case Activity::Sit:fadeDuration=0.9;break;
        case Activity::Lie:fadeDuration=1.1;break;
        case Activity::Hang:fadeDuration=0.6;break;
        case Activity::Grabbed:fadeDuration=0.2;break;
        case Activity::Fall:fadeDuration=0.25;break;
        case Activity::Air:fadeDuration=0.08;airFall=0;break;
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
        // That is the arm's turn in model space; its key is under the shoulder's turn.
        XMVECTOR shoulder=XMQuaternionIdentity();
        if(const int bone=rig.shoulder[side];bone>=0&&slot[bone]>=0)shoulder=XMLoadFloat4(&keys[slot[bone]].rotation);
        SetRotation(keys,rig.arm[side],XMQuaternionMultiply(XMQuaternionRotationMatrix(XMMatrixTranspose(from)*to),XMQuaternionInverse(shoulder)));
        SetRotation(keys,rig.elbow[side],Rotation(rig.elbowBendAxis[side],bendAngle));
    }

    XMVECTOR GeneratedMotion::Stopped(int side,FXMVECTOR target)
    {
        if(!wristDriven[side])XMStoreFloat3(&askedWrist[side],target);
        XMVECTOR wrist=target;
        for(int pass=0;pass<2;++pass)   // twice: a stop's push can take it past another's
            for(const ArmStop& s:armStops[side])
            {
                if(!s.used)continue;
                const XMVECTOR n=XMLoadFloat3(&s.normal);
                const float gap=s.offset-XMVectorGetX(XMVector3Dot(wrist,n));
                if(gap>0){wrist=XMVectorAdd(wrist,XMVectorScale(n,gap));wristHeld[side]=true;}
            }
        XMStoreFloat3(&stopWrist[side],wrist);
        wristDriven[side]=true;
        return wrist;
    }

    void GeneratedMotion::StopArm(int side,const XMFLOAT3& push,float depth)
    {
        if(side<0||side>1)return;
        const bool driven=wristDriven[side],held=wristHeld[side];
        if(!driven)return;   // not ReachArm's arm this frame (the idle motion's, a clip's)
        auto& stops=armStops[side];
        const XMVECTOR n=XMVector3Normalize(XMLoadFloat3(&push)),wrist=XMLoadFloat3(&stopWrist[side]);
        if(depth>0)
        {
            // Held from going that way: the stop of the same way moved out by how far it went
            // in (moving the wrist moves the forearm by less, so it goes on until clear).
            ArmStop* stop=nullptr;
            for(ArmStop& s:stops)if(s.used&&XMVectorGetX(XMVector3Dot(XMLoadFloat3(&s.normal),n))>0.9f){stop=&s;break;}
            if(!stop)
            {
                for(ArmStop& s:stops)if(!s.used){stop=&s;break;}
                if(!stop)stop=&*std::min_element(stops.begin(),stops.end(),[](const ArmStop& a,const ArmStop& b){return a.offset<b.offset;});
                XMStoreFloat3(&stop->normal,n);
                stop->offset=-1e9f;
                stop->used=true;
            }
            stop->offset=std::max(stop->offset,XMVectorGetX(XMVector3Dot(wrist,XMLoadFloat3(&stop->normal))))+depth;
            // Never far from where the motion wants the wrist: when the forearm or the upper arm
            // stays in, moving the wrist on would only fold the arm away.
            stop->offset=std::min(stop->offset,XMVectorGetX(XMVector3Dot(XMLoadFloat3(&askedWrist[side]),XMLoadFloat3(&stop->normal)))+0.4f*rig.legLength);
            ++armStopCount[side];
        }
        else if(held&&depth<0)
        {
            // Clear while held back: let it come a little nearer again, so a stop made in one
            // pose does not keep the hand off in another (the margin keeps it clear meanwhile).
            const float ease=static_cast<float>(0.25*rig.legLength*stopDt);
            for(ArmStop& s:stops)if(s.used)s.offset-=ease;
        }
    }

    void GeneratedMotion::StopLean(bool into,bool room)
    {
        if(into&&leanDriven&&lastLean>0)
        {
            leanLimit=std::max(0.0f,std::min(leanLimit,lastLean)-2.0f*Deg);
            ++leanStopCount;
        }
        else if(!into&&room)leanLimit=std::min(1.6f,leanLimit+static_cast<float>(10.0*stopDt)*Deg);
    }

    void GeneratedMotion::ClearStops()
    {
        for(auto& side:armStops)for(ArmStop& s:side)s=ArmStop{};
        wristDriven={};
        wristHeld={};
        leanLimit=1.6f;
        leanDriven=false;
    }

    float GeneratedMotion::IntoBody(FXMVECTOR point,float margin) const
    {
        const float L=rig.legLength,x=XMVectorGetX(point),y=XMVectorGetY(point),z=XMVectorGetZ(point);
        // The body's front at that point: the most forward of its surface around it.
        float front=1e9f;
        for(const XMFLOAT3& b:rig.body)
            if(std::abs(b.x-x)<0.035f*L&&std::abs(b.y-y)<0.035f*L&&b.z<front)front=b.z;
        if(front>=1e8f)return 0.0f;
        // Only what is in front of the body's back half counts (an arm behind the back is not in it).
        return std::max(0.0f,z-(front-margin));
    }

    void GeneratedMotion::ReachArm(std::vector<Key>& keys,int side,FXMVECTOR wristTarget,FXMVECTOR pole,float shoulderAmount,bool clear)
    {
        // Kept clear of the body: the forearm (and the hand beyond the wrist) is checked against
        // the body's front, and the wrist moved forward by the most any of it is in; twice, as
        // moving the wrist moves the elbow too.
        // Short of a solid it went into before (StopArm).
        XMVECTOR wrist=Stopped(side,wristTarget);
        if(clear&&!rig.body.empty())
            for(int pass=0;pass<2;++pass)
            {
                std::vector<Key> trial=keys;
                XMVECTOR elbow=XMVectorZero(),reached=wrist;
                ReachArm(trial,side,wrist,pole,shoulderAmount,false);
                // Where the elbow went: from the arm's key and the shoulder's.
                {
                    const XMVECTOR sh=rig.shoulder[side]>=0&&slot[rig.shoulder[side]]>=0?XMLoadFloat4(&trial[slot[rig.shoulder[side]]].rotation):XMQuaternionIdentity();
                    const XMVECTOR arm=XMQuaternionMultiply(XMLoadFloat4(&trial[slot[rig.arm[side]]].rotation),sh);
                    XMVECTOR root=XMLoadFloat3(&rig.shoulderPos[side]);
                    if(rig.shoulder[side]>=0){const XMVECTOR pivot=XMLoadFloat3(&rig.claviclePos[side]);root=XMVectorAdd(pivot,XMVector3Rotate(XMVectorSubtract(root,pivot),sh));}
                    elbow=XMVectorAdd(root,XMVector3Rotate(XMVectorScale(XMLoadFloat3(&rig.armRestDir[side]),rig.upperArm),arm));
                }
                const XMVECTOR along=XMVectorSubtract(reached,elbow);
                float push=0;
                for(float f=0.3f;f<=1.45f;f+=0.15f)
                    push=std::max(push,IntoBody(XMVectorAdd(elbow,XMVectorScale(along,f)),0.045f*rig.legLength));
                if(push<=0.002f*rig.legLength)break;
                wrist=Stopped(side,XMVectorSubtract(wrist,XMVectorSet(0,0,push,0)));
            }
        const float a=rig.upperArm,b=rig.forearm;
        // Two-bone IK from the arm's root: the elbow, and where the wrist gets to.
        auto solve=[&](FXMVECTOR root,XMVECTOR& elbow,XMVECTOR& reached){
            const XMVECTOR toWrist=XMVectorSubtract(wrist,root);
            const float d=std::clamp(XMVectorGetX(XMVector3Length(toWrist)),std::abs(a-b)+0.01f,a+b-0.01f);
            const XMVECTOR dir=XMVector3Normalize(toWrist);
            XMVECTOR side3=XMVectorSubtract(pole,XMVectorScale(dir,XMVectorGetX(XMVector3Dot(pole,dir))));
            if(XMVectorGetX(XMVector3LengthSq(side3))<1e-8f)side3=XMVectorSet(0,-1,0,0);
            side3=XMVector3Normalize(side3);
            const float cosA=std::clamp((a*a+d*d-b*b)/(2*a*d),-1.0f,1.0f);
            elbow=XMVectorAdd(root,XMVectorAdd(XMVectorScale(dir,a*cosA),XMVectorScale(side3,a*std::sqrt(1-cosA*cosA))));
            reached=XMVectorAdd(root,XMVectorScale(dir,d));
        };
        const XMVECTOR rest=XMLoadFloat3(&rig.shoulderPos[side]);
        XMVECTOR root=rest,elbow,reached;
        solve(root,elbow,reached);
        // The shoulder follows where the elbow goes (or, under the user's motion, stays as the
        // motion has it); the arm is then solved again from where the shoulder has carried its
        // root, so the hand still arrives where it was sent.
        if(const int shoulder=rig.shoulder[side];shoulder>=0&&slot[shoulder]>=0)
        {
            const XMVECTOR turn=motionShoulders?XMLoadFloat4(&keys[slot[shoulder]].rotation):ShoulderTurn(side,elbow,shoulderAmount);
            if(!motionShoulders)SetRotation(keys,shoulder,turn);
            const XMVECTOR pivot=XMLoadFloat3(&rig.claviclePos[side]);
            root=XMVectorAdd(pivot,XMVector3Rotate(XMVectorSubtract(rest,pivot),turn));
            solve(root,elbow,reached);
        }
        AimArm(keys,side,XMVector3Normalize(XMVectorSubtract(elbow,root)),XMVector3Normalize(XMVectorSubtract(reached,elbow)));
    }

    XMVECTOR GeneratedMotion::ShoulderTurn(int side,FXMVECTOR elbow,float amount) const
    {
        // MotionTraceEditor's automatic shoulder (ArmCorrectionCore): per upper-arm length the
        // elbow rises, 42° up (14° down when it is lower); per length it comes forward (and
        // 0.65 of its move toward the body's middle, 0.35 away), 26° forward (18° back).
        // At most 50° up, 15° down and 20° either way.
        const float upper=std::max(rig.upperArm,1e-4f);
        const XMVECTOR d=XMVectorSubtract(elbow,XMLoadFloat3(&rig.hangElbow[side]));
        const float inward=side==0?-XMVectorGetX(d):XMVectorGetX(d);   // the left side is +X
        const float up=XMVectorGetY(d)/upper;
        const float forward=(-XMVectorGetZ(d)+(inward>=0?0.65f:0.35f)*inward)/upper;
        const float upDegrees=amount*std::clamp(up*(up>=0?42.0f:14.0f),-15.0f,50.0f);
        const float forwardDegrees=amount*std::clamp(forward*(forward>=0?26.0f:18.0f),-20.0f,20.0f);
        // Turned about the axes that take the shoulder's line (clavicle to arm) up and forward.
        const XMVECTOR line=XMVector3Normalize(XMVectorSubtract(XMLoadFloat3(&rig.shoulderPos[side]),XMLoadFloat3(&rig.claviclePos[side])));
        auto toward=[&](FXMVECTOR target,float degrees){
            const XMVECTOR axis=XMVector3Cross(line,target);
            if(XMVectorGetX(XMVector3LengthSq(axis))<1e-8f)return XMQuaternionIdentity();
            return XMQuaternionRotationNormal(XMVector3Normalize(axis),degrees*Deg);
        };
        return XMQuaternionMultiply(toward(XMVectorSet(0,1,0,0),upDegrees),toward(XMVectorSet(0,0,-1,0),forwardDegrees));
    }

    XMVECTOR GeneratedMotion::HandTurn(int side,FXMVECTOR fingers,FXMVECTOR palm) const
    {
        return XMQuaternionRotationMatrix(XMMatrixTranspose(HandFrame(XMLoadFloat3(&rig.handRestDir[side]),XMLoadFloat3(&rig.palmRestNormal[side])))*
                                          HandFrame(fingers,palm));
    }

    XMVECTOR GeneratedMotion::PalmOffset(int side,FXMVECTOR fingers,FXMVECTOR palm) const
    {
        return XMVector3Rotate(XMLoadFloat3(&rig.palmContact[side]),HandTurn(side,fingers,palm));
    }

    void GeneratedMotion::AimHand(std::vector<Key>& keys,int side,FXMVECTOR fingers,FXMVECTOR palm)
    {
        const int wrist=rig.wrist[side];
        if(wrist<0||slot[wrist]<0||rig.arm[side]<0||rig.elbow[side]<0)return;
        auto turn=[&](int bone){return bone>=0&&slot[bone]>=0?XMLoadFloat4(&keys[slot[bone]].rotation):XMQuaternionIdentity();};
        // The forearm's turn in model space (上半身2 is held at rest).
        const XMVECTOR forearm=XMQuaternionMultiply(XMQuaternionMultiply(XMQuaternionMultiply(XMQuaternionMultiply(
            turn(rig.elbow[side]),turn(rig.armTwist[side])),turn(rig.arm[side])),turn(rig.shoulder[side])),turn(rig.upperBody));
        // Where the hand points with the wrist at rest, and the turn from there to the aim.
        const XMMATRIX now=HandFrame(XMVector3Rotate(XMLoadFloat3(&rig.handRestDir[side]),forearm),XMVector3Rotate(XMLoadFloat3(&rig.palmRestNormal[side]),forearm));
        const XMVECTOR aim=XMQuaternionRotationMatrix(XMMatrixTranspose(now)*HandFrame(fingers,palm));
        // The same turn as the wrist's own (in the forearm's frame).
        XMVECTOR hand=XMQuaternionNormalize(XMQuaternionMultiply(XMQuaternionMultiply(forearm,aim),XMQuaternionInverse(forearm)));
        if(const int twist=rig.handTwist[side];twist>=0&&slot[twist]>=0)
        {
            // The part about the forearm's length is 手捩's (MMD's wrist does not twist).
            const XMVECTOR axis=XMVector3Normalize(XMLoadFloat3(&rig.elbowTwistAxis[side]));
            const XMVECTOR along=XMVectorScale(axis,XMVectorGetX(XMVector3Dot(hand,axis)));
            XMVECTOR t=XMVectorSelect(XMVectorSplatW(hand),along,XMVectorSelectControl(1,1,1,0));
            t=XMVectorGetX(XMVector4LengthSq(t))<1e-8f?XMQuaternionIdentity():XMQuaternionNormalize(t);
            SetRotation(keys,twist,t);
            hand=XMQuaternionNormalize(XMQuaternionMultiply(hand,XMQuaternionInverse(t)));
        }
        SetRotation(keys,wrist,hand);
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
        case Gesture::Stretch:case Gesture::TuckHair:case Gesture::Yawn:case Gesture::Think:case Gesture::Ponder:case Gesture::Hum:
        case Gesture::LookFar:case Gesture::Cheeks:case Gesture::SwingArm:case Gesture::WristRoll:case Gesture::Shrug:
        case Gesture::Sigh:case Gesture::LookBack:case Gesture::LookDown:case Gesture::LookUp:case Gesture::Glance:
        case Gesture::NeckStretch:case Gesture::Nod:case Gesture::Drowse:case Gesture::Peek:case Gesture::Bounce:
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
            {L"dangle",Gesture::Dangle},{L"swing",Gesture::SwingArm},{L"shake",Gesture::ShakeHands},{L"wrist",Gesture::WristRoll},
            {L"fingers",Gesture::Fingers},{L"shoulders",Gesture::ShoulderRoll},{L"shrug",Gesture::Shrug},{L"sigh",Gesture::Sigh},
            {L"back",Gesture::LookBack},{L"down",Gesture::LookDown},{L"up",Gesture::LookUp},{L"glance",Gesture::Glance},
            {L"neck",Gesture::NeckStretch},{L"nod",Gesture::Nod},{L"drowse",Gesture::Drowse},{L"sway",Gesture::Sway},
            {L"twist",Gesture::Twist},{L"peek",Gesture::Peek},{L"bounce",Gesture::Bounce},{L"tap",Gesture::TapFoot},
            {L"pat",Gesture::PatHead},{L"surprised",Gesture::Surprised},{L"curious",Gesture::Curious},{L"shynod",Gesture::ShyNod},
            {L"oops",Gesture::Oops},{L"ponder",Gesture::Ponder},
            {L"present",Gesture::Present},{L"slash",Gesture::Slash},{L"cast",Gesture::Cast}};
    }

    std::vector<std::pair<std::wstring,std::wstring>> GeneratedMotion::IdleGestureList()
    {
        // Labels by gesture number (1..GestureCount).
        static constexpr std::pair<const wchar_t*,const wchar_t*> labels[GestureCount]={
            {L"腰に手を当てる",L"Hand on hip"},{L"手を振る",L"Wave"},{L"ピース",L"Peace sign"},{L"手を後ろで組む",L"Hands behind"},
            {L"手のひらを見る",L"Look at a palm"},{L"手を前で重ねる",L"Hands together in front"},{L"伸び",L"Stretch"},
            {L"きょろきょろ見回す",L"Look around"},{L"首をかしげる",L"Tilt the head"},{L"ただ立っている",L"Just stand"},
            {L"髪を耳にかける",L"Tuck hair"},{L"あくび",L"Yawn"},{L"考える",L"Think"},{L"鼻歌",L"Hum"},{L"拍手",L"Clap"},
            {L"遠くを見る",L"Look far away"},{L"ほっぺを押さえる",L"Hold the cheeks"},
            {L"腕をぶらぶら",L"Dangle the arms"},{L"片腕を振る",L"Swing an arm"},{L"手を振ってほぐす",L"Shake the hands loose"},
            {L"手首を回す",L"Roll a wrist"},{L"指を動かす",L"Move the fingers"},{L"肩を回す",L"Roll the shoulders"},
            {L"肩をすくめる",L"Shrug"},{L"ため息",L"Sigh"},{L"振り返る",L"Look back"},{L"足もとを見る",L"Look down"},
            {L"上を見る",L"Look up"},{L"ちらっと見る",L"Glance"},{L"首を伸ばす",L"Stretch the neck"},{L"うなずく",L"Nod"},
            {L"一瞬うとうと",L"Nod off"},{L"体を揺らす",L"Sway"},{L"腰をひねる",L"Twist the waist"},{L"のぞき込む",L"Peer"},
            {L"ひざでリズム",L"Bounce at the knees"},{L"つま先でトントン",L"Tap the toes"}};
        std::vector<std::pair<std::wstring,std::wstring>> list(GestureCount);
        for(const auto& [n,g]:GestureNames)
            if(const int i=static_cast<int>(g);i>=1&&i<=GestureCount)list[i-1]={n,T(labels[i-1].first,labels[i-1].second)};
        return list;
    }

    bool GeneratedMotion::Emote(const std::wstring& name)
    {
        if(reaction!=Gesture::None||overrides.empty()||activity!=Activity::Idle)return false;
        for(const auto& [n,g]:GestureNames)
            if(name==n)
            {
                reaction=g;reactionCut=-1;
                reactionSide=Random01()<0.5f?0:1;
                reactionTime=0;
                reactionLength=static_cast<int>(g)>GestureCount?2.5:std::min(3.5,static_cast<double>(GestureLengths(g)[0]));
                reactionWeight=0;
                return true;
            }
        return false;
    }

    bool GeneratedMotion::EmoteSeated(const std::wstring& name,double seconds)
    {
        if(reaction!=Gesture::None||overrides.empty()||(activity!=Activity::Idle&&activity!=Activity::Sit))return false;
        static constexpr Gesture upper[]={Gesture::Think,Gesture::Clap,Gesture::Cheeks,Gesture::Surprised,Gesture::Sigh,
                                          Gesture::ShyNod,Gesture::Curious,Gesture::Oops,Gesture::Ponder};
        for(const auto& [n,g]:GestureNames)
            if(name==n&&std::find(std::begin(upper),std::end(upper),g)!=std::end(upper))
            {
                reaction=g;reactionCut=-1;
                reactionSide=Random01()<0.5f?0:1;
                reactionTime=0;
                reactionLength=std::max(seconds,SlowBlendIn+SlowBlendOut);
                reactionWeight=0;
                reactionSlow=true;
                return true;
            }
        return false;
    }

    bool GeneratedMotion::Present(int side,double seconds)
    {
        if(overrides.empty()||activity!=Activity::Idle)return false;
        reaction=Gesture::Present;reactionCut=-1;
        reactionSide=side==0?0:1;
        reactionTime=0;
        reactionLength=seconds;
        reactionWeight=0;
        return true;
    }

    bool GeneratedMotion::Reach(int side,const XMFLOAT3& fromHead,double seconds,bool tap)
    {
        if(overrides.empty()||(activity!=Activity::Idle&&activity!=Activity::Sit))return false;
        if(reaction!=Gesture::None&&reaction!=Gesture::Reach)return false;
        reaction=Gesture::Reach;reactionCut=-1;reactionSlow=false;
        reactionSide=side==0?0:1;
        reachOffset=fromHead;
        reachTap=tap;
        reactionTime=0;
        reactionLength=seconds;
        reactionWeight=0;
        return true;
    }

    bool GeneratedMotion::Strike(bool magic)
    {
        if(overrides.empty()||activity!=Activity::Idle)return false;
        reaction=magic?Gesture::Cast:Gesture::Slash;reactionCut=-1;
        reactionSide=1;
        reactionTime=0;
        reactionLength=magic?1.25:1.15;
        reactionWeight=0;
        return true;
    }

    void GeneratedMotion::EndGestures()
    {
        // Each fades out from the weight it has now.
        if(gesture!=Gesture::None&&gestureCut<0)
        {
            if(frozenAt>=0){gestureTime=frozenAt;frozenAt=-1;}
            gestureCut=CutBlend;
        }
        if(reaction!=Gesture::None&&reactionCut<0)reactionCut=CutBlend;
        nextGestureAt=std::max(nextGestureAt,clock+3.0);
    }

    bool GeneratedMotion::ForceGesture(const std::wstring& name)
    {
        // name or name@seconds: hold the gesture, or freeze it that far into its timeline.
        const auto at=name.find(L'@');
        const std::wstring base=name.substr(0,at);
        for(const auto& [n,g]:GestureNames)
            if(base==n)
            {
                gesture=g;gestureSide=0;gestureTime=0;gestureLength=1e9;frozenAt=-1;gestureCut=-1;
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
        // A point held at the face (a hand at the brow or chin) moved with the head when the
        // neck turns it, so the face never turns into the hand.
        auto withHead=[&](FXMVECTOR p,float pitch,float yawAngle){
            const XMVECTOR pivot=XMVectorSet(0,rig.headY-0.12f*L,S.z,0);
            return XMVectorAdd(pivot,XMVector3TransformNormal(XMVectorSubtract(p,pivot),XMMatrixRotationRollPitchYaw(pitch,yawAngle,0)));
        };
        // For the casual gestures. An arm hanging from the shoulder as in the standing pose,
        // swung forward (or back), rolled out from where it hangs (never in) and bent at the
        // elbow: the hand stays out beside the hips as far as standing keeps it. A swing also
        // rolls it out by half as much, so the hand passes outside what hangs at the hips
        // (a pouch, a ribbon, the side of a skirt).
        auto hang=[&](int side,float swing,float outward,float bend){
            if(rig.arm[side]<0)return;
            const float o=side==0?1.0f:-1.0f;
            const float roll=std::max(outward,0.0f)+0.5f*std::abs(swing);
            SetRotation(g,rig.arm[side],Then(Rotation(rig.armSwingAxis[side],swing),
                                             XMQuaternionRotationRollPitchYaw(0,0,rig.armLower[side]+o*roll)));
            if(rig.elbow[side]>=0)SetRotation(g,rig.elbow[side],Rotation(rig.elbowBendAxis[side],bend));
        };
        // The hand bent at the wrist: toward the palm (bend) and toward the thumb (sideways).
        auto flex=[&](int side,float bend,float sideways){
            if(rig.wrist[side]<0)return;
            const XMVECTOR d=XMLoadFloat3(&rig.handRestDir[side]);
            XMVECTOR n=XMLoadFloat3(&rig.palmRestNormal[side]);
            n=XMVectorSubtract(n,XMVectorScale(d,XMVectorGetX(XMVector3Dot(n,d))));
            if(XMVectorGetX(XMVector3LengthSq(n))<1e-8f)return;
            n=XMVector3Normalize(n);
            SetRotation(g,rig.wrist[side],Then(XMQuaternionRotationNormal(XMVector3Normalize(XMVector3Cross(d,n)),bend),
                                               XMQuaternionRotationNormal(n,sideways)));
        };
        // A turn added to what the standing pose gives a bone (its lean and weight shift kept).
        auto turn=[&](int bone,float pitch,float yawAngle,float roll){
            if(bone<0)return;
            auto& k=KeyOf(g,bone);
            XMStoreFloat4(&k.rotation,XMQuaternionNormalize(XMQuaternionMultiply(XMLoadFloat4(&k.rotation),XMQuaternionRotationRollPitchYaw(pitch,yawAngle,roll))));
        };
        // Both shoulders up (and back): the arms go up and a little out with them.
        auto shoulders=[&](float up,float back){
            both([&](int side,float o){if(rig.shoulder[side]>=0)SetRotation(g,rig.shoulder[side],XMQuaternionRotationRollPitchYaw(0,-o*back,o*up));});
        };
        // The base standing arms (as Generate hangs them).
        const float baseSway=1.5f*Deg*std::sin(XM_2PI*static_cast<float>(clock)/5.3f);
        // Fading in after start and out before the end (for movements that keep going).
        const float envelope=ramp(0.2f,0.6f)*(1.0f-ramp(len-1.0f,0.7f));
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
            // The elbow is about at shoulder height: a wave lifts the shoulder only a little.
            ReachArm(g,s,XMVectorSet(S.x+out*(0.24f+0.07f*wave)*L,S.y+0.27f*L,S.z-0.10f*L,0),XMVectorSet(out,-1,0.3f,0),0.5f);
            CurlFingers(g,s,{0.05f,0.05f,0.05f,0.05f});
            neck(0,0,-out*5.0f*Deg);
            face(rig.smileEyes,1.0f);
            face(rig.smileMouth,0.7f);
            break;
        }
        case Gesture::Peace:
            // A peace sign beside the face, head tilted toward it, with a wink.
            // A hand up at the face lifts the shoulder only a little (half), as in the wave.
            ReachArm(g,s,XMVectorSet(out*0.30f*L,rig.headY+0.02f*L,S.z-0.16f*L,0),XMVectorSet(out,-1,0.3f,0),0.5f);
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
        case Gesture::Reach:
        {
            // The arm out to the point, palm down, fingers bent as if picking a tile up, eyes
            // on it. Out of reach: leaning forward from the waist, the hand stopping short.
            // From a seat the hand first comes up in front of the chest, the elbow back (clear of
            // a table's edge), then goes out; and back the same way before it lets go.
            const XMVECTOR shoulder=XMLoadFloat3(&S);
            const XMVECTOR pivot=XMVectorSet(0,hipY,hipZ,0);
            const float extend=Smooth(std::clamp(std::min((a-0.3f)/0.3f,(len-static_cast<float>(ReactionBlendOut)-0.05f-a)/0.3f),0.0f,1.0f));
            const XMVECTOR close=XMVectorSet(S.x+out*0.02f*L,S.y-0.20f*L,S.z-0.14f*L,0);
            XMVECTOR goal=XMVectorAdd(XMVectorSet(0,rig.headY,S.z,0),XMLoadFloat3(&reachOffset));
            if(reachTap)
            {
                // A light pat from above: out a little way toward it and a little up, then down
                // and up again at the touch.
                const XMVECTOR way=XMVectorMultiply(XMVectorSubtract(goal,close),XMVectorSet(1,0,1,0));
                const float n=std::max(XMVectorGetX(XMVector3Length(way)),1e-4f);
                const float pat=std::sin(std::clamp((a-0.5f)/0.3f,0.0f,1.0f)*XM_PI);
                goal=XMVectorAdd(XMVectorAdd(close,XMVectorScale(way,std::min(n,0.12f*L)/n)),XMVectorSet(0,(0.06f-0.08f*pat)*L,0,0));
            }
            XMVECTOR target=XMVectorLerp(close,goal,extend);
            const float reach=rig.upperArm+0.75f*rig.foreArm;   // to the wrist (foreArm reaches the fingertips)
            const float d=XMVectorGetX(XMVector3Length(XMVectorSubtract(target,shoulder)));
            lastLean=reachTap?0.0f:std::clamp((d-0.85f*reach)/(0.8f*reach),0.0f,1.0f)*22.0f*Deg;
            leanDriven=true;
            const float lean=std::min(lastLean,leanLimit);   // no further into a solid (StopLean)
            turn(rig.upperBody,-lean,0,0);
            // Where the point is as the leaning body sees it.
            target=XMVectorAdd(pivot,XMVector3Transform(XMVectorSubtract(target,pivot),XMMatrixRotationX(lean)));
            const XMVECTOR to=XMVectorSubtract(target,shoulder);
            const float far=0.95f*reach,dist=XMVectorGetX(XMVector3Length(to));
            if(dist>far)target=XMVectorAdd(shoulder,XMVectorScale(to,far/dist));
            ReachArm(g,s,target,XMVectorLerp(XMVectorSet(out*0.6f,-1,0.9f,0),XMVectorSet(out,-1,0.3f,0),extend),0.6f,true);
            if(reachTap)
            {
                // The hand flat and relaxed, palm down, for the pat.
                AimHand(g,s,XMVectorSet(-out*0.15f,-0.3f,-1,0),XMVectorSet(0,-1,0,0));
                CurlFingers(g,s,{0.12f,0.15f,0.2f,0.25f});
            }
            else
            {
                AimHand(g,s,XMVectorSet(-out*0.1f,-0.22f,-1,0),XMVectorSet(0,-1,0,0));   // fingers only a little down, clear of the table
                CurlFingers(g,s,{0.35f,0.4f,0.45f,0.5f});
            }
            neck(-16.0f*Deg+lean*0.3f,0,0);
            break;
        }
        case Gesture::Present:
            // The forearm held out level in front at the waist, the palm up and open (a thing
            // shown on it), eyes on it and a little smile.
            ReachArm(g,s,XMVectorSet(out*0.10f*L,S.y-0.34f*L,S.z-0.36f*L,0),XMVectorSet(out,-1,0.3f,0));
            AimHand(g,s,XMVectorSet(-out*0.15f,0.05f,-1,0),XMVectorSet(0,1,0,0));
            CurlFingers(g,s,{0.06f,0.06f,0.08f,0.1f});
            neck(-16.0f*Deg,-out*14.0f*Deg,0);
            face(rig.smileMouth,0.4f);
            break;
        case Gesture::Slash:
        {
            // A full-strength cut at what stands in front of her. She draws the sword in her fist
            // up over her shoulder, leaning back and turning that shoulder away; then steps in
            // with that foot, sinks, and brings it down hard in front of her (quicker and quicker
            // to the blow), the body leaning into it and the other arm swinging back. She holds
            // the follow-through a moment, then eases up.
            const float hit=static_cast<float>(StrikeHit);
            const float wind=ramp(0.0f,hit-0.12f);
            const float c=std::clamp((a-(hit-0.10f))/0.10f,0.0f,1.0f);
            const float cut=c*c;
            const float settle=ramp(hit+0.30f,0.45f);
            const float lunge=Smooth(c)*(1.0f-0.6f*settle);
            const float back=wind*(1.0f-Smooth(c));
            const XMVECTOR up=XMVectorSet(S.x+out*0.08f*L,S.y+0.40f*L,S.z+0.10f*L,0);
            const XMVECTOR down=XMVectorSet(-out*0.03f*L,hipY+0.14f*L,S.z-0.66f*L,0);
            const XMVECTOR wrist=XMVectorAdd(mix(up,down,cut),XMVectorSet(0,0.08f*L*std::sin(XM_PI*cut),-0.18f*L*std::sin(XM_PI*cut),0));
            ReachArm(g,s,wrist,mix(XMVectorSet(out,0.5f,0.6f,0),XMVectorSet(out,-0.3f,0.5f,0),cut),1.0f,true);
            CurlFingers(g,s,{0.95f,0.95f,0.95f,0.95f});
            hang(1-s,-30.0f*Deg*lunge+10.0f*Deg*back,12.0f*Deg,(25.0f+20.0f*lunge)*Deg);
            CurlFingers(g,1-s,{0.6f,0.65f,0.7f,0.75f});
            turn(rig.upperBody,(10.0f*back-32.0f*lunge)*Deg,out*(-26.0f*back+14.0f*lunge)*Deg,0);
            turn(rig.lowerBody,0,out*(-8.0f*back+6.0f*lunge)*Deg,0);
            neck((-6.0f*back+10.0f*lunge)*Deg,-out*(-14.0f*back+8.0f*lunge)*Deg,0);
            // The step in: the sword side's foot forward (up a little on the way), the hips after
            // it and lower.
            if(rig.legIK[s]>=0)
            {
                auto& k=KeyOf(g,rig.legIK[s]).position;
                k.z-=0.38f*L*lunge;k.y+=0.08f*L*std::sin(XM_PI*c);
            }
            if(rig.center>=0)KeyOf(g,rig.center).position.z-=0.18f*L*lunge;
            if(rig.groove>=0)KeyOf(g,rig.groove).position.y-=0.03f*L*back+0.08f*L*lunge;
            face(rig.mouthOpen,0.5f*Smooth(c)*(1.0f-settle));
            break;
        }
        case Gesture::Cast:
        {
            // A full-strength spell with both hands: drawn back to her side, palms together as
            // the power gathers (she sinks and leans back), then thrust out in front, palms
            // forward, with a step in and the body leaning after them. Held, then eased off.
            const float hit=static_cast<float>(StrikeHit);
            const float charge=ramp(0.0f,hit-0.08f);
            const float c=std::clamp((a-(hit-0.08f))/0.08f,0.0f,1.0f);
            const float push=1.0f-(1.0f-c)*(1.0f-c);
            const float settle=ramp(hit+0.35f,0.45f);
            const float thrust=push*(1.0f-0.5f*settle);
            const float back=charge*(1.0f-push);
            both([&](int side,float o){
                const XMVECTOR drawn=XMVectorSet(out*0.22f*L+o*0.03f*L,S.y-0.24f*L,S.z+0.02f*L,0);
                const XMVECTOR front=XMVectorSet(o*0.06f*L,S.y-0.04f*L,S.z-0.70f*L,0);
                ReachArm(g,side,mix(drawn,front,push),XMVectorSet(o,-0.6f,0.4f,0),1.0f,true);
                AimHand(g,side,XMVector3Normalize(mix(XMVectorSet(0,0.2f,-1,0),XMVectorSet(-o*0.15f,1,-0.3f,0),push)),
                        XMVector3Normalize(mix(XMVectorSet(-o,0,0,0),XMVectorSet(0,0,-1,0),push)));
                CurlFingers(g,side,{0.05f,0.05f,0.08f,0.1f});
            });
            turn(rig.upperBody,(10.0f*back-26.0f*thrust)*Deg,-out*20.0f*Deg*back,0);
            neck((-5.0f*back+8.0f*thrust)*Deg,out*12.0f*Deg*back,0);
            if(rig.legIK[s]>=0)
            {
                auto& k=KeyOf(g,rig.legIK[s]).position;
                k.z-=0.26f*L*thrust;k.y+=0.06f*L*std::sin(XM_PI*c);
            }
            if(rig.center>=0)KeyOf(g,rig.center).position.z-=0.12f*L*thrust;
            if(rig.groove>=0)KeyOf(g,rig.groove).position.y-=0.06f*L*back+0.06f*L*thrust;
            face(rig.mouthOpen,0.4f*push*(1.0f-settle));
            break;
        }
        case Gesture::LookAtPalm:
            // Looking down at the open palm held in front of the chest.
            ReachArm(g,s,XMVectorSet(out*0.06f*L,S.y-0.16f*L,S.z-0.40f*L,0),XMVectorSet(out,-1,-0.1f,0));
            CurlFingers(g,s,{0.1f,0.1f,0.15f,0.2f});
            neck(-28.0f*Deg,-out*10.0f*Deg,0);
            break;
        case Gesture::HandsFolded:
            // Hands lowered and held together in front, clear of the body: elbows a little out,
            // palms toward the body, fingers toward the other side, the right hand over the left.
            both([&](int side,float o){
                const float over=side==1?0.03f:0.0f;
                ReachArm(g,side,XMVectorSet(o*0.05f*L,hipY-0.06f*L,hipZ-0.24f*L-over*L,0),XMVectorSet(o,-0.3f,0.35f,0));
                AimHand(g,side,XMVectorSet(-o,-0.45f,-0.15f,0),XMVectorSet(0,0.15f,1,0));
                CurlFingers(g,side,{0.25f,0.25f,0.3f,0.3f});
            });
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
                const XMFLOAT3& sp=rig.shoulderPos[side];
                const float sz=sp.z;
                // Up past the face well in front of it (never through it), then over the head
                // with the arms almost straight, the hands apart enough for the forearms to
                // clear the head (and ears or hair on top).
                const XMVECTOR front=XMVectorSet(o*0.16f*L,sp.y+0.10f*L,sz-0.24f*L,0);
                const XMVECTOR beforeFace=XMVectorSet(o*0.27f*L,rig.headY+0.02f*L,sz-0.34f*L,0);
                const XMVECTOR overHead=XMVectorSet(o*0.18f*L,rig.headY+0.26f*L,sz-0.20f*L,0);
                const float reach=(rig.upperArm+rig.forearm)*(0.95f+0.04f*peak);
                const float across=std::abs(sp.x)-0.13f*L;
                const XMVECTOR up=XMVectorSet(o*0.13f*L,sp.y+std::sqrt(std::max(0.0f,reach*reach-across*across)),sz-0.03f*L,0);
                const XMVECTOR shoulder=XMLoadFloat3(&sp);
                const XMVECTOR hang=XMVectorAdd(shoulder,XMVectorSet(o*0.05f*L,-0.40f*L,-0.06f*L,0));
                XMVECTOR wrist=mix(hang,front,ramp(0,0.8f));
                wrist=mix(wrist,beforeFace,ramp(0.6f,0.6f));
                wrist=mix(wrist,overHead,ramp(1.05f,0.55f));
                wrist=mix(wrist,up,ramp(1.5f,0.8f));
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
            neck((3.0f+4.0f*peak)*Deg*lean,0,out*3.0f*Deg*peak);
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
            ReachArm(g,s,XMVectorSet(out*(0.17f+0.02f*stroke)*L,rig.headY+(0.10f-0.06f*stroke)*L,S.z-(0.10f-0.12f*stroke)*L,0),XMVectorSet(out,-1,0.2f,0),0.5f);
            CurlFingers(g,s,{0.15f,0.15f,0.25f,0.35f});
            neck(-6.0f*Deg,-out*6.0f*Deg,out*6.0f*Deg);
            break;
        }
        case Gesture::Yawn:
        {
            // A yawn: the mouth opens wide behind a hand, eyes shut, head back a
            // little, then a small blink of relief.
            const float open=std::sin(XM_PI*std::clamp((a-0.5f)/std::max(len-1.6f,0.5f),0.0f,1.0f));
            // The palm over the mouth (just in front of the lips, moving with the head as it goes
            // back), fingers across it toward the other side.
            {
                const XMVECTOR lips=rig.hasMouth?XMLoadFloat3(&rig.mouth):XMVectorSet(0,rig.headY,faceZ,0);
                const XMVECTOR mouth=withHead(XMVectorSubtract(lips,XMVectorSet(0,0.03f*L,0.025f*L,0)),8.0f*Deg*open,0);
                const XMVECTOR fingers=XMVectorSet(-out*0.95f,0.08f,-0.15f,0),palm=XMVectorSet(out*0.2f,0.05f,1,0);
                ReachArm(g,s,XMVectorSubtract(mouth,PalmOffset(s,fingers,palm)),XMVectorSet(out,-1,0.1f,0),0.5f);
                AimHand(g,s,fingers,palm);
            }
            CurlFingers(g,s,{0.1f,0.1f,0.12f,0.15f});
            SetRotation(g,rig.upperBody,XMQuaternionRotationRollPitchYaw(3.0f*Deg*open,0,0));
            neck(8.0f*Deg*open,0,-out*4.0f*Deg);
            face(rig.mouthOpen,0.9f*open);
            face(rig.eyesClosed>=0?rig.eyesClosed:rig.smileEyes,std::min(1.0f,1.4f*open));
            break;
        }
        case Gesture::Think:
            // Thinking: a hand at the chin, the other hand resting at the middle of the waist with its elbow a little out,
            // the head tilted and turned a little up.
            ReachArm(g,s,withHead(XMVectorSet(out*0.03f*L,rig.headY-0.10f*L,faceZ-0.12f*L,0),5.0f*Deg,out*8.0f*Deg),XMVectorSet(out*0.3f,-1,0,0),0.5f);
            CurlFingers(g,s,{0.5f,0.6f,0.8f,0.8f});
            ReachArm(g,1-s,XMVectorSet(-out*0.03f*L,S.y-0.34f*L,S.z-0.22f*L,0),XMVectorSet(-out,-0.35f,0.25f,0),1.0f,true);
            CurlFingers(g,1-s,{0.3f,0.3f,0.35f,0.35f});
            neck(5.0f*Deg,out*8.0f*Deg,-out*9.0f*Deg+out*3.0f*Deg*std::sin(XM_2PI*a/3.0f));
            break;
        case Gesture::Ponder:
        {
            // Pondering what is in front (her tiles): a hand at the chin as in Think, the head
            // bowed toward them and tilted a little, a serious face.
            const float pitch=-22.0f*Deg,yawAngle=out*4.0f*Deg;
            ReachArm(g,s,withHead(XMVectorSet(out*0.03f*L,rig.headY-0.10f*L,faceZ-0.12f*L,0),pitch,yawAngle),XMVectorSet(out*0.3f,-1,0,0),0.5f);
            CurlFingers(g,s,{0.5f,0.6f,0.8f,0.8f});
            ReachArm(g,1-s,XMVectorSet(-out*0.03f*L,S.y-0.34f*L,S.z-0.22f*L,0),XMVectorSet(-out,-0.35f,0.25f,0),1.0f,true);
            CurlFingers(g,1-s,{0.3f,0.3f,0.35f,0.35f});
            neck(pitch,yawAngle,-out*5.0f*Deg+out*2.0f*Deg*std::sin(XM_2PI*a/3.0f));
            face(rig.serious,0.5f);
            break;
        }
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
            // Palms facing each other, fingers up and a little forward.
            // The palms (measured on the model's hands) meet at the middle, whatever the build.
            both([&](int side,float o){
                const XMVECTOR fingers=XMVectorSet(o*0.1f,0.8f,-0.6f,0),palm=XMVectorSet(-o,0,0,0);
                const XMVECTOR contact=XMVectorSet(o*(0.004f+0.07f*beat)*L,S.y-0.16f*L,S.z-0.30f*L,0);
                ReachArm(g,side,XMVectorSubtract(contact,PalmOffset(side,fingers,palm)),XMVectorSet(o,-1,0.5f,0));
                AimHand(g,side,fingers,palm);
                CurlFingers(g,side,{0.08f,0.08f,0.1f,0.12f});
            });
            neck(-3.0f*Deg,0,0);
            face(rig.smileEyes,0.9f);
            face(rig.smileMouth,0.7f);
            break;
        }
        case Gesture::LookFar:
            // Shading the eyes with a hand and gazing into the distance, slowly scanning. The
            // hand is held at the temple, as in a salute, its fingers over the brow.
        {
            const float scan=out*18.0f*Deg*std::sin(XM_PI*(u-0.5f));
            ReachArm(g,s,withHead(XMVectorSet(out*0.18f*L,rig.headY+0.10f*L,faceZ-0.05f*L,0),6.0f*Deg,scan),XMVectorSet(out,-0.15f,-0.3f,0),0.5f);
            CurlFingers(g,s,{0.05f,0.05f,0.1f,0.15f});
            neck(6.0f*Deg,scan,0);
        }
            break;
        case Gesture::Cheeks:
            // Both hands on the cheeks with a happy smile, the head rocking a little.
            both([&](int side,float o){
                ReachArm(g,side,XMVectorSet(o*0.14f*L,rig.headY-0.04f*L,faceZ-0.04f*L,0),XMVectorSet(o,-1,0.1f,0),0.5f);
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
        case Gesture::Oops:
            // "Oops" (あちゃー): a hand to the forehead, palm on it, the head dropping and
            // tilting into the hand, the eyes shut with a wry smile, the shoulders down a little.
        {
            const float pitch=-12.0f*Deg,yawAngle=out*6.0f*Deg;
            ReachArm(g,s,withHead(XMVectorSet(out*0.07f*L,rig.headY+0.05f*L,faceZ-0.07f*L,0),pitch,yawAngle),XMVectorSet(out,-0.5f,0.2f,0),0.5f);
            AimHand(g,s,XMVectorSet(-out*0.7f,0.45f,-0.2f,0),XMVectorSet(0,0.1f,1,0));
            CurlFingers(g,s,{0.12f,0.15f,0.2f,0.25f});
            shoulders(-3.0f*Deg,0);
            neck(pitch,yawAngle,-out*7.0f*Deg);
            face(rig.eyesClosed,0.8f);
            face(rig.smileMouth,0.25f);
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
        case Gesture::Dangle:
        {
            // The arms swinging loosely, one forward as the other goes back, the hands trailing.
            const float phase=XM_2PI*a/1.3f,swing=9.0f*Deg*envelope*std::sin(phase);
            both([&](int side,float o){
                const float w=side==0?1.0f:-1.0f;
                hang(side,w*swing,2.0f*Deg,(10.0f+4.0f*envelope*(0.5f+0.5f*std::sin(phase*w-0.6f)))*Deg);
                flex(side,-w*7.0f*Deg*envelope*std::sin(phase-0.9f),0);
            });
            break;
        }
        case Gesture::SwingArm:
        {
            // One arm swung back and forth from the shoulder, idly, the eyes following it.
            const float phase=XM_2PI*a/1.5f;
            hang(s,(3.0f+13.0f*envelope*std::sin(phase))*Deg,3.0f*Deg,(12.0f+6.0f*envelope*(0.5f+0.5f*std::sin(phase-0.5f)))*Deg);
            flex(s,-10.0f*Deg*envelope*std::sin(phase-1.0f),0);
            neck(-10.0f*Deg*envelope,out*10.0f*Deg*envelope,0);
            break;
        }
        case Gesture::ShakeHands:
        {
            // Shaking the hands out loosely beside the hips, forearms a little up, wrists flopping.
            const float phase=XM_2PI*a*2.2f;
            both([&](int side,float o){
                const float shake=std::sin(phase+side*1.7f);
                hang(side,6.0f*Deg*envelope,(6.0f+2.0f*shake*envelope)*Deg,(10.0f+25.0f*envelope+4.0f*shake*envelope)*Deg);
                flex(side,32.0f*Deg*envelope*std::sin(phase+side*1.7f-1.2f),0);
            });
            break;
        }
        case Gesture::WristRoll:
        {
            // A hand brought up a little in front of the hip, the wrist turning round and round
            // as if loosening it, glanced at.
            const float phase=XM_2PI*a/0.9f;
            hang(s,12.0f*Deg*envelope,6.0f*Deg,(10.0f+50.0f*envelope)*Deg);
            flex(s,22.0f*Deg*envelope*std::sin(phase),18.0f*Deg*envelope*std::cos(phase));
            neck(-14.0f*Deg*envelope,out*8.0f*Deg*envelope,0);
            break;
        }
        case Gesture::Fingers:
        {
            // The fingers of both hands curling and opening one after another, absent-mindedly.
            both([&](int side,float o){
                std::array<float,4> curl{};
                for(int f=0;f<4;++f)curl[f]=0.25f+0.2f*envelope*std::sin(XM_2PI*a/0.8f-f*0.7f-side*0.4f);
                CurlFingers(g,side,curl);
            });
            break;
        }
        case Gesture::ShoulderRoll:
        {
            // Rolling the shoulders: up, back and down, twice, the head a little forward.
            const float phase=XM_2PI*std::clamp((a-0.4f)/std::max(len-1.2f,0.5f),0.0f,1.0f)*2.0f;
            shoulders(8.0f*Deg*envelope*(0.5f-0.5f*std::cos(phase)),7.0f*Deg*envelope*std::sin(phase));
            both([&](int side,float o){hang(side,(side==0?baseSway:-baseSway),2.0f*Deg,10.0f*Deg);});
            neck(-4.0f*Deg*envelope,0,0);
            break;
        }
        case Gesture::Shrug:
        {
            // A small shrug: shoulders up, the forearms out a little, head on one side.
            const float up=std::sin(XM_PI*std::clamp((a-0.2f)/std::max(len-0.6f,0.5f),0.0f,1.0f));
            shoulders(12.0f*Deg*up,0);
            both([&](int side,float o){hang(side,4.0f*Deg*up,8.0f*Deg*up,(10.0f+30.0f*up)*Deg);flex(side,-12.0f*Deg*up,0);});
            neck(2.0f*Deg*up,0,-out*7.0f*Deg*up);
            face(rig.smileMouth,0.3f*up);
            break;
        }
        case Gesture::Sigh:
        {
            // A sigh: a breath in (shoulders and head up), then out (they drop, the head hangs
            // a little, the eyes half closed).
            const float in=ramp(0.2f,1.0f)*(1.0f-ramp(1.3f,0.9f)),outBreath=ramp(1.3f,0.9f)*(1.0f-ramp(len-1.0f,0.7f));
            const float chest=(2.0f*in-1.5f*outBreath)*Deg;
            shoulders(7.0f*Deg*in,0);
            turn(rig.upperBody,chest,0,0);
            both([&](int side,float o){hang(side,(side==0?baseSway:-baseSway)+chest,2.0f*Deg,10.0f*Deg);});
            neck((5.0f*in-10.0f*outBreath)*Deg,0,0);
            face(rig.eyesClosed,0.45f*outBreath);
            break;
        }
        case Gesture::LookBack:
        {
            // Looking back over the shoulder: the chest turns a little, the head most of the way.
            const float look=ramp(0.2f,0.8f)*(1.0f-ramp(len-1.0f,0.8f));
            turn(rig.upperBody,0,out*16.0f*Deg*look,0);
            turn(rig.lowerBody,0,out*4.0f*Deg*look,0);
            neck(-3.0f*Deg*look,out*48.0f*Deg*look,0);
            both([&](int side,float o){hang(side,(side==0?baseSway:-baseSway),4.0f*Deg*look,10.0f*Deg);});
            break;
        }
        case Gesture::LookDown:
        {
            // Looking down at the feet (or the ground), the toes of one foot turning a little.
            const float look=ramp(0.1f,0.7f)*(1.0f-ramp(len-0.9f,0.7f));
            turn(rig.upperBody,-4.0f*Deg*look,0,0);
            both([&](int side,float o){hang(side,(side==0?baseSway:-baseSway)-4.0f*Deg*look,2.0f*Deg,10.0f*Deg);});
            neck(-30.0f*Deg*look,out*6.0f*Deg*std::sin(XM_2PI*a/3.0f)*look,0);
            break;
        }
        case Gesture::LookUp:
        {
            // Looking up at the sky (or the ceiling), the head going from one side a little to
            // the other.
            const float look=ramp(0.1f,0.8f)*(1.0f-ramp(len-0.9f,0.7f));
            turn(rig.upperBody,3.0f*Deg*look,0,0);
            both([&](int side,float o){hang(side,(side==0?baseSway:-baseSway)+3.0f*Deg*look,2.0f*Deg,10.0f*Deg);});
            neck(24.0f*Deg*look,out*12.0f*Deg*look*std::cos(XM_PI*u),0);
            break;
        }
        case Gesture::Glance:
        {
            // Quick glances to one side and the other, as at a sound.
            const float first=ramp(0.2f,0.25f)*(1.0f-ramp(0.9f,0.25f));
            const float second=ramp(1.1f,0.25f)*(1.0f-ramp(1.8f,0.3f));
            neck(-2.0f*Deg*(first+second),(out*28.0f*first-out*22.0f*second)*Deg,0);
            break;
        }
        case Gesture::NeckStretch:
        {
            // Stretching the neck: the head leans slowly over to one side, then the other, then
            // forward, the eyes shut.
            const float k=std::clamp((a-0.3f)/std::max(len-1.0f,0.5f),0.0f,1.0f)*3.0f;
            const float sideA=std::sin(XM_PI*std::clamp(k,0.0f,1.0f)),sideB=std::sin(XM_PI*std::clamp(k-1.0f,0.0f,1.0f)),fwd=std::sin(XM_PI*std::clamp(k-2.0f,0.0f,1.0f));
            neck(-18.0f*Deg*fwd,0,(out*18.0f*sideA-out*18.0f*sideB)*Deg);
            face(rig.eyesClosed,0.9f*std::max({sideA,sideB,fwd}));
            break;
        }
        case Gesture::Nod:
        {
            // Small nods to herself, as if agreeing with a thought.
            const float nods=std::max(0.0f,std::sin(XM_2PI*std::clamp((a-0.3f)/0.55f,0.0f,3.0f)));
            neck(-7.0f*Deg*nods*envelope,0,-out*3.0f*Deg*envelope);
            face(rig.smileMouth,0.3f*envelope);
            break;
        }
        case Gesture::Drowse:
        {
            // Nodding off for a moment: the head slowly droops as the eyes close, then comes up
            // with a start.
            const float droop=Smooth(std::clamp((a-0.3f)/std::max(len-1.3f,0.5f),0.0f,1.0f))*(1.0f-ramp(len-0.9f,0.18f));
            neck(-22.0f*Deg*droop,0,out*4.0f*Deg*droop);
            turn(rig.upperBody,-3.0f*Deg*droop,0,0);
            both([&](int side,float o){hang(side,(side==0?baseSway:-baseSway)-3.0f*Deg*droop,2.0f*Deg,10.0f*Deg);});
            face(rig.eyesClosed,std::min(1.0f,1.3f*droop));
            break;
        }
        case Gesture::Sway:
        {
            // Swaying gently from side to side from the waist, the arms hanging out a little
            // more so they stay clear of the hips.
            const float lean=3.5f*Deg*envelope*std::sin(XM_2PI*a/2.4f);
            turn(rig.upperBody,0,0,lean);
            turn(rig.lowerBody,0,0,-0.4f*lean);
            neck(0,0,-0.6f*lean);
            both([&](int side,float o){hang(side,(side==0?baseSway:-baseSway),(3.0f*Deg+1.5f*std::abs(lean)),10.0f*Deg);});
            break;
        }
        case Gesture::Twist:
        {
            // Twisting the waist one way and the other, the arms swinging loosely after it.
            const float phase=XM_2PI*a/2.2f,twist=11.0f*Deg*envelope*std::sin(phase);
            turn(rig.upperBody,0,twist,0);
            turn(rig.lowerBody,0,-0.25f*twist,0);
            neck(0,-0.6f*twist,0);
            both([&](int side,float o){hang(side,o*0.6f*twist,4.0f*Deg*envelope,(10.0f+5.0f*envelope)*Deg);});
            break;
        }
        case Gesture::Peek:
        {
            // Leaning forward a little to peer at something, the arms going back a little.
            const float lean=ramp(0.2f,0.7f)*(1.0f-ramp(len-0.9f,0.7f));
            turn(rig.upperBody,-7.0f*Deg*lean,out*5.0f*Deg*lean,0);
            neck(4.0f*Deg*lean,out*6.0f*Deg*lean,-out*5.0f*Deg*lean);
            both([&](int side,float o){hang(side,-7.0f*Deg*lean,2.0f*Deg*lean,12.0f*Deg);});
            break;
        }
        case Gesture::Bounce:
        {
            // Bouncing lightly at the knees as if to a tune, the head nodding along.
            const float beat=0.5f-0.5f*std::cos(XM_2PI*a/0.6f);
            auto& k=KeyOf(g,rig.groove);
            k.position.y-=0.018f*L*beat*envelope;
            neck(-4.0f*Deg*beat*envelope,0,0);
            both([&](int side,float o){hang(side,(side==0?baseSway:-baseSway)+2.0f*Deg*beat*envelope,(2.0f+2.0f*beat*envelope)*Deg,(10.0f+6.0f*beat*envelope)*Deg);});
            face(rig.smileMouth,0.3f*envelope);
            break;
        }
        case Gesture::TapFoot:
        {
            // Tapping the toes of the foot that carries no weight (heel down), a few times.
            const float shift=weightFrom+(weightTo-weightFrom)*Smooth(static_cast<float>(weightTime/std::max(weightDuration,1e-3)));
            const int foot=shift>0.3f?1:shift<-0.3f?0:s;
            const float tap=std::max(0.0f,std::sin(XM_2PI*a/0.5f))*envelope;
            if(rig.legIK[foot]>=0)
            {
                auto& k=KeyOf(g,rig.legIK[foot]);
                k.position.y+=0.012f*L*tap;
                XMStoreFloat4(&k.rotation,XMQuaternionNormalize(XMQuaternionMultiply(XMQuaternionRotationRollPitchYaw(16.0f*Deg*tap,0,0),XMLoadFloat4(&k.rotation))));
            }
            neck(-3.0f*Deg*tap,0,0);
            break;
        }
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
            if(!autoGestures){nextGestureAt=clock+1.0;return;}
            // A built-in clip counts as one gesture more; it is played by the caller (over this
            // layer, as the opened motion is), so no gesture here meanwhile. Only the ones the
            // user left on (all of them unless chosen otherwise); none: she just stands.
            std::vector<int> open,fresh;
            for(int p=1;p<=GestureCount+clipCount;++p)
                if(!(p<static_cast<int>(disabledPicks.size())&&disabledPicks[p]))open.push_back(p);
            if(open.empty()){nextGestureAt=clock+4.0;return;}
            for(int p:open)if(p!=recentGestures[0]&&p!=recentGestures[1])fresh.push_back(p);
            const auto& from=fresh.empty()?open:fresh;
            const int pick=from[std::min(from.size()-1,static_cast<size_t>(Random01()*from.size()))];
            recentGestures={pick,recentGestures[0]};
            if(pick>GestureCount)
            {
                clipRequest=pick-GestureCount-1;
                nextGestureAt=clock+1.5+Random01();
                return;
            }
            const Gesture next=static_cast<Gesture>(pick);
            gesture=next;
            gestureCut=-1;
            gestureSide=Random01()<0.5f?0:1;
            gestureTime=0;
            const auto range=GestureLengths(next);
            gestureLength=range[0]+(range[1]-range[0])*Random01();
        }
        gestureTime=frozenAt>=0?frozenAt:gestureTime+dt;
        if(gestureCut>=0)gestureCut-=dt;
        if(gestureTime>=gestureLength||(gestureCut<=0&&gestureCut>-1))
        {
            gesture=Gesture::None;
            gestureWeight=0;
            gestureCut=-1;
            nextGestureAt=std::max(nextGestureAt,clock+1.5+Random01());
            return;
        }
        gestureWeight=frozenAt>=0?1.0f:Smooth(static_cast<float>(std::min({gestureTime/GestureBlend,(gestureLength-gestureTime)/GestureBlend,1.0})));
        if(gestureCut>0)gestureWeight*=Smooth(static_cast<float>(gestureCut/CutBlend));

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
        reactionCut=-1;
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
        if(reactionCut>=0)reactionCut-=dt;
        if(reactionTime>=reactionLength||(reactionCut<=0&&reactionCut>-1)){reaction=Gesture::None;reactionWeight=0;reactionCut=-1;reactionSlow=false;return;}
        // EmoteSeated's gestures ease in and out slowly; the others come quickly.
        const double blendIn=reactionSlow?SlowBlendIn:reaction==Gesture::Slash||reaction==Gesture::Cast?0.15:reaction==Gesture::Surprised?0.12:0.3;
        const double blendOut=reactionSlow?SlowBlendOut:ReactionBlendOut;
        const float ramp=static_cast<float>(std::min({reactionTime/blendIn,(reactionLength-reactionTime)/blendOut,1.0}));
        reactionWeight=reactionSlow?Smoother(ramp):Smooth(ramp);
        if(reactionCut>0)reactionWeight*=Smooth(static_cast<float>(reactionCut/CutBlend));
        std::vector<Key> g=keys;
        PoseGesture(reaction,reactionTime,reactionLength,reactionSide,reactionWeight,g,reactionMorphs);
        for(size_t i=0;i<keys.size();++i)
        {
            XMStoreFloat4(&keys[i].rotation,XMQuaternionSlerp(XMLoadFloat4(&keys[i].rotation),XMLoadFloat4(&g[i].rotation),reactionWeight));
            XMStoreFloat3(&keys[i].position,XMVectorLerp(XMLoadFloat3(&keys[i].position),XMLoadFloat3(&g[i].position),reactionWeight));
        }
    }

    void GeneratedMotion::Hold(int side,const XMFLOAT3& wrist,bool on)
    {
        if(side<0||side>1)return;
        holds[side].on=on;
        if(on)holds[side].wrist=wrist;
    }

    void GeneratedMotion::ApplyHold(std::vector<Key>& keys,double dt)
    {
        for(int s=0;s<2;++s)
        {
            HoldArm& h=holds[s];
            h.weight=std::clamp(h.weight+static_cast<float>(dt/0.4)*(h.on?1.0f:-1.0f),0.0f,1.0f);
            if(h.weight<=0||rig.arm[s]<0||overrides.empty())continue;
            const float weight=Smooth(h.weight),out=s==0?1.0f:-1.0f;
            std::vector<Key> g=keys;
            const XMVECTOR shoulder=XMLoadFloat3(&rig.shoulderPos[s]);
            const float hipY=(rig.hipRest[0].y+rig.hipRest[1].y)*0.5f,hipZ=(rig.hipRest[0].z+rig.hipRest[1].z)*0.5f;
            const XMVECTOR pivot=XMVectorSet(0,hipY,hipZ,0);
            XMVECTOR target=XMLoadFloat3(&h.wrist);
            // Bent forward from the waist over the table as one plays, further toward a point
            // far off (no further into a solid).
            const float reach=rig.upperArm+0.75f*rig.foreArm;
            const float d=XMVectorGetX(XMVector3Length(XMVectorSubtract(target,shoulder)));
            lastLean=(18.0f+std::clamp((d-0.7f*reach)/(0.8f*reach),0.0f,1.0f)*17.0f)*Deg;
            leanDriven=true;
            const float lean=std::min(lastLean,leanLimit);
            // Turned from the waist toward the hand's way (part of it, eased: the shoulders lead
            // the arm a little, and the legs stay as they are).
            const float dx=XMVectorGetX(target)-XMVectorGetX(pivot),dz=XMVectorGetZ(target)-XMVectorGetZ(pivot);
            const float twist=std::clamp(0.45f*std::atan2(-dx,std::max(-dz,0.1f*reach)),-22.0f*Deg,22.0f*Deg);
            h.twist+=(twist-h.twist)*static_cast<float>(1.0-std::exp(-dt/0.22));
            // Only that lean and turn: the idle motion's sway of the upper body would carry the hand about.
            const XMVECTOR bend=XMQuaternionRotationRollPitchYaw(-lean,h.twist,0);
            if(rig.upperBody>=0)XMStoreFloat4(&KeyOf(g,rig.upperBody).rotation,bend);
            // Where the point is as the bent body sees it, and no further than the arm goes.
            target=XMVectorAdd(pivot,XMVector3Rotate(XMVectorSubtract(target,pivot),XMQuaternionConjugate(bend)));
            const XMVECTOR to=XMVectorSubtract(target,shoulder);
            const float far=0.95f*reach,dist=XMVectorGetX(XMVector3Length(to));
            if(dist>far)
            {
                // Out of reach: short of it across and along, at its height (the hand stays over
                // the table instead of dropping toward it on the line from the shoulder).
                const float y=XMVectorGetY(to),flat=std::sqrt(std::max(0.0f,dist*dist-y*y));
                if(std::abs(y)<far&&flat>1e-4f)
                {
                    const float keep=std::sqrt(far*far-y*y)/flat;
                    target=XMVectorAdd(shoulder,XMVectorSet(XMVectorGetX(to)*keep,y,XMVectorGetZ(to)*keep,0));
                }
                else target=XMVectorAdd(shoulder,XMVectorScale(to,far/dist));
            }
            // The elbow out and a little down, the hand flat over the grip.
            ReachArm(g,s,target,XMVectorSet(out*0.8f,-1,0.2f,0),0.6f,true);
            // (As the bent body sees them, as the target is: so the fingers lie over the knob
            // however far she leans, not tipped down into it.)
            const XMVECTOR unbend=XMQuaternionConjugate(bend);
            AimHand(g,s,XMVector3Rotate(XMVectorSet(-out*0.1f,-0.1f,-1,0),unbend),XMVector3Rotate(XMVectorSet(0,-1,0,0),unbend));
            CurlFingers(g,s,{0.12f,0.15f,0.18f,0.2f});   // lightly over the low knob, the fingertips clear of the table
            // Eased toward the pose just made (0.07 s): the small shakes of the target (the
            // mallet's moves, the stops in front of the table) are not seen in the hand.
            if(h.smooth.size()!=g.size()||h.weight<=static_cast<float>(dt/0.4)+1e-4f)h.smooth=g;
            else
            {
                const float follow=static_cast<float>(1.0-std::exp(-dt/0.07));
                for(size_t i=0;i<g.size();++i)
                {
                    XMStoreFloat4(&h.smooth[i].rotation,XMQuaternionSlerp(XMLoadFloat4(&h.smooth[i].rotation),XMLoadFloat4(&g[i].rotation),follow));
                    XMStoreFloat3(&h.smooth[i].position,XMVectorLerp(XMLoadFloat3(&h.smooth[i].position),XMLoadFloat3(&g[i].position),follow));
                }
                g=h.smooth;
            }
            for(size_t i=0;i<keys.size();++i)
            {
                XMStoreFloat4(&keys[i].rotation,XMQuaternionSlerp(XMLoadFloat4(&keys[i].rotation),XMLoadFloat4(&g[i].rotation),weight));
                XMStoreFloat3(&keys[i].position,XMVectorLerp(XMLoadFloat3(&keys[i].position),XMLoadFloat3(&g[i].position),weight));
            }
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
        const bool facing=facingWalk&&(activity==Activity::Walk||activity==Activity::Run);
        const float targetYaw=profile&&!facing?-direction*WalkYaw*(activity==Activity::Sit?0.5f:1.0f):0.0f;
        yaw+=(targetYaw-yaw)*follow;
        groundSpeed=0;
        // Relaxed hands: the fingers curl a little, more toward the little finger. Gestures and
        // resting poses set their own curl over this.
        for(int side=0;side<2;++side)CurlFingers(keys,side,{0.2f,0.25f,0.3f,0.35f});

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
            // Facing walk (the dungeon): quicker steps when asked, longer strides not (they would
            // look like a lunge); the stride's length stays, so the ground speed follows.
            // A run timed to a ground speed asked for (the ruins): the stride stays, the steps
            // quicken, though not past three a second for each foot.
            const double cycle=facing&&activity==Activity::Run&&facingRunSpeed>0?std::max(0.36,static_cast<double>(g.stride*L/(g.stance*facingRunSpeed))):
                               g.cycle/(facing&&activity==Activity::Walk?facingPace:1.0f);
            walkPhase+=dt/cycle;
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
            const float speed=g.stride*L/(g.stance*static_cast<float>(cycle));
            rootOffset+=speed*a*XMVectorGetX(forward)*static_cast<float>(dt);
            groundSpeed=speed*a;
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
            if(activity==Activity::Run)
            {
                // Running: the hands lightly closed, and the elbow closing as the arm comes
                // forward and opening as it goes back, so each fist pumps up to the chest and
                // down to the hip in time with the steps.
                arms(swing0,swing1,rig.armLower[0],rig.armLower[1],g.elbow+0.6f*swing0,g.elbow+0.6f*swing1);
                for(int side=0;side<2;++side)CurlFingers(keys,side,{0.65f,0.7f,0.75f,0.8f});
            }
            else
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
        case Activity::Bound:
        {
            // A hop off one foot, like a triple jump's first step, in three parts timed by
            // BoundTakeoffSeconds, BoundAirSeconds and BoundLandSeconds:
            // - the take-off: a dip over the back foot, which then pushes off from the toes,
            //   while the front knee drives forward and up and the arms swing against the legs;
            // - the flight: the back leg trails long behind, toes pointed, the front knee high,
            //   the body leaning into it; coming down, the front foot reaches for the ground;
            // - the landing: down onto the front foot, the knee giving under the weight, the
            //   back leg swinging through to stand beside it, the arms coming down.
            const float T0=static_cast<float>(BoundTakeoffSeconds),A=static_cast<float>(BoundAirSeconds),D=static_cast<float>(BoundLandSeconds);
            const float tau=static_cast<float>(activityTime);
            const float up=Smooth(tau/T0);                                  // into the flight pose
            const float v=std::clamp((tau-T0)/A,0.0f,1.0f);                 // through the flight
            const float reach=Smooth((v-0.45f)/0.55f);                      // the front foot reaching down
            const float w=std::clamp((tau-T0-A)/D,0.0f,1.0f);               // through the landing
            const float down=Smooth(w);
            // The front foot: up and forward through the take-off, high in flight, down to the
            // ground a little ahead at touch-down, then under her as she rises.
            float frontU=(0.32f*up+0.0f*reach)*L,frontH=0.42f*up*(1-reach)*L,frontPitch=12.0f*Deg*up*(1-reach);
            if(w>0){frontU=0.14f*(1-down)*L;frontH=0;frontPitch=0;}
            else frontU=(0.32f-0.18f*reach)*L*up;
            // The back foot: planted, heel rising as it pushes off, then trailing behind, then
            // swinging through to stand.
            const float push=Smooth((tau/T0-0.4f)/0.6f);
            float backU=-0.5f*Smooth(v/0.35f)*L,backH=(0.3f-0.06f*reach)*Smooth(v/0.35f)*L,backPitch=-(40.0f*push*(1-Smooth(v/0.3f))+50.0f*Smooth(v/0.35f))*Deg;
            if(w>0)
            {
                const float swing=Smooth(w/0.8f);
                backU=-0.5f*(1-swing)*L;
                backH=(0.24f*(1-swing)+0.1f*std::sin(XM_PI*swing))*L;
                backPitch=-50.0f*Deg*(1-swing);
            }
            footKey(0,standingFoot(0,frontU,frontH),frontPitch,yaw);
            footKey(1,standingFoot(1,backU,backH),backPitch,yaw);
            // The hips: a dip at the take-off, a deeper one taking the landing.
            const float dip=0.09f*std::sin(XM_PI*std::min(1.0f,tau/T0))*(tau<T0?1.0f:0.0f)+0.16f*std::sin(XM_PI*w);
            KeyOf(keys,rig.groove).position={0,-dip*L,0};
            SetRotation(keys,rig.lowerBody,XMQuaternionRotationRollPitchYaw(0,yaw,0));
            const float lean=(14.0f*up+6.0f*std::sin(XM_PI*std::min(1.0f,tau/T0)))*(1-down)+10.0f*std::sin(XM_PI*w);
            SetRotation(keys,rig.upperBody,Then(XMQuaternionRotationRollPitchYaw(-lean*Deg,0,0),
                                                XMQuaternionRotationRollPitchYaw(0,yaw*(WalkChestYaw/WalkYaw),0)));
            SetRotation(keys,rig.neck,XMQuaternionRotationRollPitchYaw(0.5f*lean*Deg,-yaw*(WalkNeckYaw/WalkYaw),0));
            const float arm=up*(1-down);
            arms(-45.0f*Deg*arm,65.0f*Deg*arm,rig.armLower[0],rig.armLower[1],(10.0f+40.0f*arm)*Deg,(10.0f+75.0f*arm)*Deg);
            for(int side=0;side<2;++side)CurlFingers(keys,side,{0.65f,0.7f,0.75f,0.8f});
            break;
        }
        case Activity::SideHop:
        {
            // A hop to the side (toward +x of the model when direction is 1, -x when -1), timed
            // by SideTakeoffSeconds, SideAirSeconds and SideLandSeconds: a dip, and the foot on
            // the far side pushes off from its toes while the near foot already reaches out;
            // in the air the near leg stretches out the way she goes and the far one is left
            // behind toward where she came from, the body leaning that way, the arms lifting a
            // little for balance;
            // she lands on the near foot, the knee giving, and the far foot comes in beside it.
            const float T0=static_cast<float>(SideTakeoffSeconds),A=static_cast<float>(SideAirSeconds),D=static_cast<float>(SideLandSeconds);
            const float tau=static_cast<float>(activityTime),way=direction<0?-1.0f:1.0f;
            const float takeoff=std::min(1.0f,tau/T0),up=Smooth(takeoff);
            const float v=std::clamp((tau-T0)/A,0.0f,1.0f),reach=Smooth((v-0.4f)/0.6f);
            const float w=std::clamp((tau-T0-A)/D,0.0f,1.0f),down=Smooth(w);
            const XMVECTOR sideways=XMVector3TransformNormal(XMVectorSet(way,0,0,0),XMMatrixRotationY(yaw));
            for(int side=0;side<2;++side)
            {
                const bool near=rig.ankleRest[side].x*way>0;
                float out=0,h=0,pitch=0;
                if(near)
                {
                    // Out the way she goes, off the ground in the air, down for the landing,
                    // then back under her.
                    if(w>0){out=0.1f*(1-down);h=0;}
                    else{out=0.1f*up+0.18f*Smooth(v/0.4f)*(1-0.85f*reach);h=(0.1f*up+0.12f*Smooth(v/0.4f))*(1-reach);}
                }
                else
                {
                    // Planted while it pushes off (the heel rising), then left behind toward
                    // where she came from, the leg long and the toes pointed, and brought in
                    // beside the other as she lands.
                    const float push=Smooth((takeoff-0.35f)/0.65f)*(1-Smooth(v/0.3f));
                    const float trail=Smooth(v/0.35f)*(1-Smooth(w/0.85f));
                    out=-0.2f*trail;
                    h=0.1f*trail+0.05f*std::sin(XM_PI*Smooth(w/0.85f))*(w>0?1.0f:0.0f);
                    pitch=-(35.0f*push+40.0f*trail)*Deg;
                }
                footKey(side,XMVectorAdd(standingFoot(side,0,h*L),XMVectorScale(sideways,out*L)),pitch,yaw);
            }
            const float dip=0.09f*std::sin(XM_PI*takeoff)*(tau<T0?1.0f:0.0f)+0.13f*std::sin(XM_PI*w);
            KeyOf(keys,rig.groove).position={0,-dip*L,0};
            // Leaning the way she goes (a roll about Z: positive tips the top toward -x).
            const float lean=(11.0f*up*(1-down)+5.0f*std::sin(XM_PI*w))*Deg;
            SetRotation(keys,rig.lowerBody,XMQuaternionRotationRollPitchYaw(0,yaw,-way*0.4f*lean));
            SetRotation(keys,rig.upperBody,XMQuaternionRotationRollPitchYaw(-4.0f*Deg*up*(1-down),0,-way*lean));
            SetRotation(keys,rig.neck,XMQuaternionRotationRollPitchYaw(0,0,way*0.6f*lean));
            const float lift=0.45f*up*(1-down);
            arms(8.0f*Deg*lift,8.0f*Deg*lift,rig.armLower[0]*(1-lift),rig.armLower[1]*(1-lift),(10.0f+25.0f*lift)*Deg,(10.0f+25.0f*lift)*Deg);
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
        case Activity::Air:
        {
            // In the air at once (no crouch: the jump has already left the ground): going up,
            // the front knee drives up and the back leg trails a little, toes pointed, the arms
            // swinging forward and up; coming down, the legs reach down for the ground a little
            // apart and the arms open out for balance. Which, from the vertical speed (up
            // positive), eased so the turn at the top is not a snap.
            const float want=std::clamp(0.5f-carryVelocity.y/(6.0f*L),0.0f,1.0f);
            airFall+=(want-airFall)*static_cast<float>(1.0-std::exp(-dt/0.12));
            const float up=Smooth(1-airFall),down=Smooth(airFall);
            const float in=Smooth(static_cast<float>(activityTime/0.12));   // from the take-off's pose
            footKey(0,standingFoot(0,(0.16f*up+0.08f*down)*L*in,(0.30f*up+0.06f*down)*L*in),(10.0f*up-8.0f*down)*Deg*in,yaw);
            footKey(1,standingFoot(1,(-0.14f*up-0.04f*down)*L*in,(0.16f*up+0.04f*down)*L*in),(-35.0f*up-12.0f*down)*Deg*in,yaw);
            SetRotation(keys,rig.lowerBody,XMQuaternionRotationRollPitchYaw(0,yaw,0));
            SetRotation(keys,rig.upperBody,XMQuaternionRotationRollPitchYaw((-7.0f*up+3.0f*down)*Deg*in,0,0));
            SetRotation(keys,rig.neck,XMQuaternionRotationRollPitchYaw((4.0f*up-4.0f*down)*Deg*in,0,0));
            const float open=0.45f*down*in;
            arms((-30.0f*up+20.0f*down)*Deg*in,(50.0f*up+20.0f*down)*Deg*in,rig.armLower[0]*(1-open),rig.armLower[1]*(1-open),
                 (15.0f+45.0f*up*in)*Deg,(15.0f+60.0f*up*in)*Deg);
            for(int side=0;side<2;++side)CurlFingers(keys,side,{0.5f,0.55f,0.6f,0.65f});
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
        // What this frame's motion does, for the stops after it is solved.
        wristDriven={};
        wristHeld={};
        leanDriven=false;
        stopDt=dt;

        std::vector<Key> target(overrides.size());
        motionShoulders=activity==Activity::Idle&&!generatedIdle;
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
        else if(perch&&(activity==Activity::Sit||activity==Activity::Hang||activity==Activity::Walk))PerchKeys(pose,target,dt);
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
        ApplyHold(out,dt);

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
        const float targetYaw=facingWalk?0.0f:-direction*WalkYaw;
        yaw+=(targetYaw-yaw)*follow;
        const float turned=1.0f-std::min(1.0f,std::abs(targetYaw-yaw)/(40.0f*Deg));
        walkAmount=Approach(walkAmount,turned,static_cast<float>(dt/0.3));
        for(size_t i=0;i<overrides.size();++i)
        {
            const auto& k=pose.boneKeys[overrides[i]];
            keys[i]={k.rotation,k.position};
        }
        TurnKeys(keys);
        const float speed=clipSpeed[activity==Activity::Run?1:0];
        rootOffset+=speed*walkAmount*XMVectorGetX(XMVector3TransformNormal(XMVectorSet(0,0,-1,0),XMMatrixRotationY(yaw)))*static_cast<float>(dt);
        groundSpeed=speed*walkAmount;
    }

    // A window motion (see SetPerch), taken as it is. One that moves forward is turned to the
    // side like a walking motion; the body travels by the motion's own step, so a planted hand
    // or knee stays put. Resting (or moving sideways), she turns back to face the viewer.
    void GeneratedMotion::PerchKeys(const PMX::Pose& pose,std::vector<Key>& keys,double dt)
    {
        activityTime+=dt;
        const float follow=static_cast<float>(1.0-std::exp(-dt/0.18));
        yaw+=((perchTurn?-direction*WalkYaw:0.0f)-yaw)*follow;
        walkAmount=0;
        for(size_t i=0;i<overrides.size();++i)
        {
            const auto& k=pose.boneKeys[overrides[i]];
            keys[i]={k.rotation,k.position};
        }
        if(std::abs(yaw)>1e-4f)TurnKeys(keys);
        rootOffset+=perchTurn?perchStep*XMVectorGetX(XMVector3TransformNormal(XMVectorSet(0,0,-1,0),XMMatrixRotationY(yaw))):perchStep;
        perchStep=0;
    }

    // Turns a motion made facing the front by yaw: the foot IK targets swing round the vertical
    // axis, 下半身 turns with them and 上半身 a little less (the chest stays toward the viewer).
    // センター's height goes to グルーブ (センター moves on the ground only).
    void GeneratedMotion::TurnKeys(std::vector<Key>& keys)
    {
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
