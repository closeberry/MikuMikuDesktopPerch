#define NOMINMAX
#include "MascotModel.hpp"
#include "GeneratedMotion.hpp"
#include <btBulletDynamicsCommon.h>
#include <windows.h>
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string_view>
#include <fstream>
#include <random>

import PMXLoader;

namespace MikuMikuDesktopPerch
{
    namespace
    {
        struct VertexOffset { std::uint32_t vertex; float x,y,z; };
        struct UvOffset { std::uint32_t vertex; float u,v; };
        struct MorphDeltas
        {
            std::vector<VertexOffset> positions;
            std::vector<UvOffset> uvs;
        };

        // Upper arms within this many degrees of horizontal count as T-stance and are
        // lowered to the typical A-stance angle of MMD standard models.
        constexpr float TStanceThreshold=20.0f;
        constexpr float StandardArmAngle=35.0f;

        // Last-resort guard for PMXLoader code paths that index file data before we can
        // validate it (e.g. a joint naming a missing rigid body inside the PMXModel constructor).
        // Only hardware faults are handled here; C++ exceptions keep propagating normally.
        bool RunGuarded(void(*body)(void*),void* context)
        {
            __try
            {
                body(context);
                return true;
            }
            __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION||GetExceptionCode()==EXCEPTION_ARRAY_BOUNDS_EXCEEDED?
                     EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH)
            {
                return false;
            }
        }

        // Before a file reaches PMXLoader: it must be a regular file of sane size that starts
        // like the format it claims, so a renamed or truncated file is turned away up front.
        bool HasHeader(const std::wstring& path,std::uintmax_t maxBytes,std::initializer_list<std::string_view> magics,
                       std::string& head)
        {
            std::error_code ec;
            if(!std::filesystem::is_regular_file(path,ec))return false;
            const auto size=std::filesystem::file_size(path,ec);
            if(ec||size>maxBytes)return false;
            std::ifstream file(std::filesystem::path(path),std::ios::binary);
            head.assign(32,'\0');
            file.read(head.data(),static_cast<std::streamsize>(head.size()));
            head.resize(static_cast<size_t>(std::max<std::streamsize>(file.gcount(),0)));
            for(const auto magic:magics)
                if(head.size()>=magic.size()&&head.compare(0,magic.size(),magic)==0)return true;
            return false;
        }

        bool IsPmxFile(const std::wstring& path)
        {
            std::string head;
            if(!HasHeader(path,std::uintmax_t{1}<<30,{"PMX "},head)||head.size()<8)return false;
            float version=0;
            std::memcpy(&version,head.data()+4,sizeof(version));
            return version==2.0f||version==2.1f;
        }

        bool IsVmdFile(const std::wstring& path)
        {
            std::string head;
            return HasHeader(path,std::uintmax_t{512}<<20,{"Vocaloid Motion Data 0002","Vocaloid Motion Data file"},head);
        }

        // A VMD and its sampler (a PoseSolver without physics), guarded like the model load:
        // false (with nothing kept) when the data faults the parser.
        bool LoadVmd(const std::wstring& path,PMX::PMXModel* pmx,std::unique_ptr<PMX::VMD>& vmd,std::unique_ptr<PMX::PoseSolver>& sampler)
        {
            struct Context {const std::wstring* path;PMX::PMXModel* pmx;PMX::VMD* vmd;PMX::PoseSolver* sampler;} context{&path,pmx,nullptr,nullptr};
            const bool ok=RunGuarded([](void* c){
                auto* ctx=static_cast<Context*>(c);
                ctx->vmd=new PMX::VMD(ctx->path->c_str(),932,ctx->pmx);
                ctx->sampler=new PMX::PoseSolver(nullptr,ctx->pmx,ctx->vmd);
            },&context);
            if(!ok)return false;   // a fault mid-parse: what was built is abandoned, not freed
            vmd.reset(context.vmd);
            sampler.reset(context.sampler);
            return true;
        }

        // PoseSolver indexes bones, morphs and bodies straight from the file, so a broken
        // reference would read out of bounds. Describe the first one found, or return "".
        std::wstring FindBrokenReference(const PMX::PMXModel& pmx)
        {
            const int vertices=static_cast<int>(pmx.vertices.size()),bones=static_cast<int>(pmx.bones.size());
            const int morphs=static_cast<int>(pmx.morphs.size()),materials=static_cast<int>(pmx.materials.size());
            const int textures=static_cast<int>(pmx.textures.size()),bodies=static_cast<int>(pmx.bodies.size());
            auto inRange=[](int v,int count,bool allowNone){return (allowNone&&v==-1)||(v>=0&&v<count);};
            auto at=[](const wchar_t* what,size_t index){return std::wstring(what)+L" "+std::to_wstring(index);};

            for(size_t i=0;i<pmx.indices.size();++i)
                if(!inRange(pmx.indices[i],vertices,false))return at(L"面の頂点番号",i);
            long long faces=0;
            for(const auto& m:pmx.materials)faces+=m.vertexCount;
            if(faces>static_cast<long long>(pmx.indices.size()))return L"材質の面数が面の総数を超えています";
            for(size_t i=0;i<pmx.vertices.size();++i)
                for(int j=0;j<4;++j)
                    if(pmx.vertices[i].weight[j]!=0.0f&&!inRange(pmx.vertices[i].bone[j],bones,true))return at(L"頂点のボーン番号",i);
            for(size_t i=0;i<pmx.materials.size();++i)
            {
                const auto& m=pmx.materials[i];
                if(!inRange(m.tex,textures,true)||!inRange(m.spTex,textures,true)||(m.toonFlag==0&&!inRange(m.toonTex,textures,true)))
                    return at(L"材質のテクスチャ番号",i);
            }
            for(size_t i=0;i<pmx.bones.size();++i)
            {
                const auto& b=pmx.bones[i];
                if(!inRange(b.parent,bones,true))return at(L"ボーンの親番号",i);
                // An append flag with no append parent (-1) is dereferenced by PoseSolver too.
                if(!inRange(b.appendParent,bones,!(b.IsAppendRotation()||b.IsAppendTranslation())))return at(L"ボーンの付与親番号",i)+L"（"+b.name+L"）";
                if(!inRange(b.toBone,bones,true))return at(L"ボーンの接続先番号",i);
                if(b.IsIK())
                {
                    if(!inRange(b.IKTarget,bones,false))return at(L"IKのターゲット番号",i);
                    for(const auto& l:b.IKLinks)if(!inRange(l.bone,bones,false))return at(L"IKのリンク番号",i);
                }
            }
            for(size_t i=0;i<pmx.morphs.size();++i)
            {
                const auto& m=pmx.morphs[i];
                const auto* data=m.offsets.data();
                for(int j=0;j<m.offsetCount;++j)
                {
                    bool ok=true;
                    switch(m.kind)
                    {
                    case 0:ok=inRange(reinterpret_cast<const PMX::PMXGroupMorphOffset*>(data)[j].morph,morphs,false);break;
                    case 1:ok=inRange(reinterpret_cast<const PMX::PMXVertexMorphOffset*>(data)[j].vertex,vertices,false);break;
                    case 2:ok=inRange(reinterpret_cast<const PMX::PMXBoneMorphOffset*>(data)[j].bone,bones,false);break;
                    case 3:case 4:case 5:case 6:case 7:ok=inRange(reinterpret_cast<const PMX::PMXUVMorphOffset*>(data)[j].vertex,vertices,false);break;
                    case 8:ok=inRange(reinterpret_cast<const PMX::PMXMaterialMorphOffset*>(data)[j].material,materials,true);break;
                    case 9:ok=inRange(reinterpret_cast<const PMX::PMXFlipMorphOffset*>(data)[j].morph,morphs,false);break;
                    case 10:ok=inRange(reinterpret_cast<const PMX::PMXImpulseMorphOffset*>(data)[j].body,bodies,false);break;
                    }
                    if(!ok)return at(L"モーフの参照番号",i)+L"（"+m.name+L"）";
                }
            }
            for(size_t i=0;i<pmx.bodies.size();++i)
                if(!inRange(pmx.bodies[i].bone,bones,true))return at(L"剛体のボーン番号",i);
            for(size_t i=0;i<pmx.joints.size();++i)
                if(!inRange(pmx.joints[i].bodyA,bodies,false)||!inRange(pmx.joints[i].bodyB,bodies,false))return at(L"ジョイントの剛体番号",i);
            return {};
        }

        std::array<float,4> ToArray(const DirectX::XMFLOAT4& v){return {v.x,v.y,v.z,v.w};}
        std::array<float,3> ToArray(const DirectX::XMFLOAT3& v){return {v.x,v.y,v.z};}

        constexpr double ResetBlendSeconds=0.5;   // ease of the simulated bones in a physics reset
    }

    struct MascotModel::Impl
    {
        // Destruction order matters: solver detaches from physics, both reference pmx.
        std::unique_ptr<PMX::PMXModel> pmx;
        std::unique_ptr<PMX::Physics> physics;
        std::unique_ptr<PMX::PoseSolver> solver;
        int physicsFps=60;

        std::vector<StaticVertex> staticVertices;
        std::vector<std::uint32_t> indices;
        std::vector<DynamicVertex> baseVertices,dynamicVertices;
        std::uint64_t dynamicRevision=1;
        std::vector<MorphDeltas> morphDeltas;
        std::vector<float> appliedMorphs;
        std::vector<std::wstring> texturePaths;
        std::vector<MaterialState> materials;
        std::array<float,3> boundsMin{},boundsMax{};
        float soleFrontZ=0;   // front-most z of the soles (the face points to -Z)
        std::vector<int> legBones;   // 下半身 and both legs down to the toes

        // Physics bones, each with the nearest non-physics ancestor that carries it, for
        // spotting a skirt or hair stuck up against gravity (for example flipped up after a
        // fall). Sagging below the modelled shape is normal (sleeves droop a lot), so only
        // upward drift counts.
        struct PhysicsBone {int bone=-1,anchor=-1;float length=1;};
        std::vector<PhysicsBone> physicsBones;
        bool resetPhysicsPending=false;
        // Smooth reset: physics off, the simulated bones ease from their last physics result to
        // the animated pose, then the bodies are reset onto it and physics resumes.
        struct ResetFrom {int bone;DirectX::XMFLOAT4 rotation;DirectX::XMFLOAT3 position;};
        std::vector<ResetFrom> resetFrom;
        double resetBlend=-1;   // seconds into the ease, -1 when not resetting
        double floorWait=0;     // seconds the floor has waited for the bodies to come up
        bool floorOnReset=false;   // the pending reset is for the floor: turn it on after

        // Bottom of the lowest simulated (dynamic) rigid body, in model units.
        float LowestBodyY() const
        {
            float lowest=1e30f;
            auto* world=physics?physics->World():nullptr;
            if(!world)return lowest;
            const auto& objects=world->getCollisionObjectArray();
            for(int i=0;i<objects.size();++i)
            {
                const auto* body=btRigidBody::upcast(objects[i]);
                if(!body||body->isStaticOrKinematicObject())continue;
                btVector3 lo,hi;
                body->getAabb(lo,hi);
                // The box includes the collision margin: a body resting on the floor is not under it.
                lowest=std::min(lowest,static_cast<float>(lo.y()+body->getCollisionShape()->getMargin()));
            }
            return lowest;
        }

        void BeginPhysicsReset()
        {
            resetFrom.clear();
            const auto& keys=solver->physPose.boneKeys;
            for(size_t i=0;i<keys.size()&&i<pmx->bones.size();++i)
                if(pmx->bones[i].IsPhysics&&keys[i].physics)resetFrom.push_back({static_cast<int>(i),keys[i].rotation,keys[i].position});
            resetBlend=0;
        }

        void BlendPhysicsBones(double dt)
        {
            using namespace DirectX;
            resetBlend+=dt;
            float w=static_cast<float>(std::min(resetBlend/ResetBlendSeconds,1.0));
            w=w*w*(3-2*w);
            for(const auto& f:resetFrom)
            {
                auto& key=solver->solvedPose.boneKeys[f.bone];
                XMStoreFloat4(&key.rotation,XMQuaternionSlerp(XMLoadFloat4(&f.rotation),XMLoadFloat4(&key.rotation),w));
                XMStoreFloat3(&key.position,XMVectorLerp(XMLoadFloat3(&f.position),XMLoadFloat3(&key.position),w));
            }
        }
        std::uint32_t physicsResets=0;
        std::wstring resetReason;   // the bone that triggered the last automatic reset
        double stillTime=0,stuckTime=0,driftCheck=0,resetCooldown=0;

        // How far a physics bone is above where its anchor alone would carry it, per unit of
        // chain length (0 = as modelled; a chain swung 60 degrees upward reads up to about 1).
        float UpwardDrift(const PhysicsBone& p) const
        {
            using namespace DirectX;
            const XMVECTOR rest=XMLoadFloat3(&pmx->bones[p.bone].position);
            const XMVECTOR actual=XMVector3Transform(rest,XMMatrixTranspose(solver->boneMatrices[p.bone]));
            const XMVECTOR carried=XMVector3Transform(rest,XMMatrixTranspose(solver->boneMatrices[p.anchor]));
            return XMVectorGetY(XMVectorSubtract(actual,carried))/p.length;
        }

        // While standing still, reset physics when some bone has stayed well above its modelled
        // place for a second (checked twice a second; at most once every 6 seconds).
        void WatchPhysics(double dt)
        {
            const bool still=motion.CurrentActivity()==Activity::Idle&&frameAcceleration.x==0&&frameAcceleration.y==0;
            stillTime=still?stillTime+dt:0;
            resetCooldown-=dt;
            if(stillTime<2.0||physicsBones.empty()||solver->boneMatrices.size()<pmx->bones.size()){stuckTime=0;return;}
            if((driftCheck+=dt)<0.5)return;
            driftCheck=0;
            bool stuck=false;
            std::wstring reason;
            for(const auto& b:physicsBones)
                if(const float d=UpwardDrift(b);d>0.45f)
                {
                    stuck=true;
                    reason=pmx->bones[b.bone].name+L" raised "+std::to_wstring(d);
                    break;
                }
            if(stuck)resetReason=reason;
            stuckTime=stuck?stuckTime+0.5:0;
            if(stuckTime>=1.0&&resetCooldown<=0)
            {
                resetPhysicsPending=true;
                stuckTime=0;
                resetCooldown=6;
            }
        }

        // T-stance correction: per side, the upper arm and every bone below it.
        struct ArmCorrection
        {
            int arm=-1;
            std::vector<int> descendants;
            float restAngle=0,correction=0;   // degrees
            DirectX::XMFLOAT4 rotation{0,0,0,1},inverse{0,0,0,1};
        };
        std::array<ArmCorrection,2> arms;

        GeneratedMotion motion;

        // One-shot motion, sampled by a solver of its own (no physics) and blended over the
        // idle pose by shotWeight: the opened motion (shotVmd), or a click motion. shotSource
        // is the one playing.
        std::unique_ptr<PMX::VMD> shotVmd;
        std::unique_ptr<PMX::PoseSolver> shotSampler;
        PMX::PoseSolver* shotSource=nullptr;
        int shotLast=0;
        PMX::Pose shotPose;
        double shotFrame=0;
        float shotWeight=0;
        bool shotActive=false,shotEnding=false,shotIsClick=false;

        // A motion with a sampler of its own (no physics).
        struct Clip
        {
            std::unique_ptr<PMX::VMD> vmd;
            std::unique_ptr<PMX::PoseSolver> sampler;
        };
        // kind names the files in error text (待機モーション, ...).
        bool LoadClips(const std::vector<std::wstring>& paths,std::vector<Clip>& clips,const wchar_t* kind,std::wstring& error)
        {
            clips.clear();
            for(const auto& path:paths)
            {
                if(!std::filesystem::exists(path)){error=std::wstring(kind)+L"が見つかりません: "+path;return false;}
                if(!IsVmdFile(path)){error=std::wstring(kind)+L"はVMDファイルではないか、大きすぎます: "+path;return false;}
                try
                {
                    Clip clip;
                    if(!LoadVmd(path,pmx.get(),clip.vmd,clip.sampler)){error=std::wstring(kind)+L"のデータが壊れているため、読み込めませんでした: "+path;return false;}
                    clips.push_back(std::move(clip));
                }
                catch(...)
                {
                    error=std::wstring(kind)+L"を読み込めませんでした: "+path;
                    return false;
                }
            }
            return true;
        }

        // Walking and running in place (instead of the generated gait), sampled like idle motions.
        std::array<std::vector<Clip>,2> gaitClips;
        PMX::Pose gaitPose;
        double gaitFrame=0;
        Activity gaitLastActivity=Activity::Idle;

        // The motion's ground speed: how fast a planted foot IK target slides back, over the
        // frames where a foot is at its lowest (units per second, 30 fps).
        float GaitSpeed(Clip& clip)
        {
            const int last=std::max(clip.vmd->lastFrame,0);
            const int ik[2]={FindBone({L"左足ＩＫ",L"左足IK"}),FindBone({L"右足ＩＫ",L"右足IK"})};
            if(last<8||ik[0]<0||ik[1]<0)return 0;
            std::vector<std::array<float,4>> feet;   // y, z of each foot per frame
            PMX::Pose sample;
            for(int f=0;f<=last;++f)
            {
                clip.sampler->HiPose(sample,static_cast<float>(f),false,false);
                const auto& l=sample.boneKeys[ik[0]].position;
                const auto& r=sample.boneKeys[ik[1]].position;
                feet.push_back({l.y,l.z,r.y,r.z});
            }
            double travel=0;
            int frames=0;
            for(int side=0;side<2;++side)
            {
                float low=1e9f;
                for(const auto& f:feet)low=std::min(low,f[side*2]);
                for(size_t f=1;f<feet.size();++f)
                    if(feet[f][side*2]<=low+0.05f&&feet[f-1][side*2]<=low+0.05f)
                    {
                        travel+=feet[f][side*2+1]-feet[f-1][side*2+1];
                        ++frames;
                    }
            }
            // Planted feet slide back (+Z) while walking forward (-Z).
            return frames>=6?static_cast<float>(std::abs(travel)/frames*30.0):0.0f;
        }

        void SampleGait(PMX::Pose& pose,double dt)
        {
            const Activity a=motion.CurrentActivity();
            const int which=a==Activity::Walk?0:a==Activity::Run?1:-1;
            if(which<0||gaitClips[which].empty()){gaitLastActivity=a;return;}
            if(a!=gaitLastActivity)gaitFrame=0;
            gaitLastActivity=a;
            auto& clip=gaitClips[which].front();
            const double last=static_cast<double>(std::max(clip.vmd->lastFrame,1));
            gaitFrame=std::fmod(gaitFrame+dt*30.0,last);
            clip.sampler->HiPose(gaitPose,static_cast<float>(gaitFrame),false,false);
            for(size_t i=0;i<pose.boneKeys.size()&&i<gaitPose.boneKeys.size();++i)
            {
                pose.boneKeys[i].rotation=gaitPose.boneKeys[i].rotation;
                pose.boneKeys[i].position=gaitPose.boneKeys[i].position;
            }
        }

        // Click motions: one at random per click while standing (instead of a built-in reaction).
        std::vector<Clip> clickClips;
        int lastClick=-1;

        // Idle motions, each sampled by a solver of its own (no physics) into the base pose,
        // where the main solver has no motion. idlePrevious fades out over the switch.
        std::vector<Clip> idleClips;
        int idleCurrent=-1,idlePrevious=-1;
        Activity idleLastActivity=Activity::Idle;
        double idleFrame=0,idlePreviousFrame=0;
        float idleFade=1;
        PMX::Pose idlePose,idlePreviousPose;
        std::mt19937 idleRandom{std::random_device{}()};

        bool LoadIdle(const std::vector<std::wstring>& paths,std::wstring& error)
        {
            std::vector<Clip> clips;
            if(!LoadClips(paths,clips,L"待機モーション",error))return false;
            idleClips=std::move(clips);
            idleCurrent=idleClips.empty()?-1:static_cast<int>(idleRandom()%idleClips.size());
            idlePrevious=-1;
            idleFrame=0;
            idleFade=1;
            return true;
        }

        static constexpr double IdleFade=0.6;          // crossfade between two idle motions, seconds
        static constexpr double IdleMinimumPlay=4.0;   // a short (or one-frame pose) motion is held this long
        double IdleLast(int clip) const {return static_cast<double>(std::max(idleClips[clip].vmd->lastFrame,0));}

        // Between two idle motions (or loops of one): near the end of the one playing, or
        // just after the next began. Walking off or sitting down waits for such a moment.
        bool IdleAtBreak() const
        {
            if(idleCurrent<0||idlePrevious>=0)return true;
            constexpr double Near=0.4*30.0;
            const double last=IdleLast(idleCurrent);
            if(idleFrame<=Near)return true;
            const double end=idleClips.size()==1?last:std::max(last,IdleMinimumPlay*30.0)-IdleFade*30.0;
            return end<=0||end-idleFrame<=Near;
        }

        void SampleIdle(PMX::Pose& pose,double dt)
        {
            using namespace DirectX;
            if(idleCurrent<0)return;
            constexpr double Fade=IdleFade,MinimumPlay=IdleMinimumPlay;
            auto lastOf=[&](int clip){return IdleLast(clip);};
            // Back from walking, sitting and the like: start a motion from its beginning
            // rather than somewhere in the middle.
            const Activity activity=motion.CurrentActivity();
            if(activity==Activity::Idle&&idleLastActivity!=Activity::Idle)
            {
                idleCurrent=static_cast<int>(idleRandom()%idleClips.size());
                idlePrevious=-1;
                idleFrame=0;
                idleFade=1;
            }
            idleLastActivity=activity;
            idleFrame+=dt*30.0;
            if(idleClips.size()==1)
            {
                const double last=lastOf(idleCurrent);
                idleFrame=last>0?std::fmod(idleFrame,last):0;
            }
            else if(idlePrevious<0&&idleFrame>=std::max(lastOf(idleCurrent),MinimumPlay*30.0)-Fade*30.0)
            {
                int next;
                do next=static_cast<int>(idleRandom()%idleClips.size());
                while(next==idleCurrent);
                idlePrevious=idleCurrent;
                idlePreviousFrame=idleFrame;
                idleCurrent=next;
                idleFrame=0;
                idleFade=0;
            }
            idleClips[idleCurrent].sampler->HiPose(idlePose,static_cast<float>(std::min(idleFrame,lastOf(idleCurrent))),false,false);
            float w=1;
            if(idlePrevious>=0)
            {
                idlePreviousFrame=std::min(idlePreviousFrame+dt*30.0,lastOf(idlePrevious));
                idleClips[idlePrevious].sampler->HiPose(idlePreviousPose,static_cast<float>(idlePreviousFrame),false,false);
                idleFade=static_cast<float>(std::min(1.0,idleFade+dt/Fade));
                w=idleFade*idleFade*(3-2*idleFade);
                if(idleFade>=1)idlePrevious=-1;
            }
            for(size_t i=0;i<pose.boneKeys.size()&&i<idlePose.boneKeys.size();++i)
            {
                auto& k=pose.boneKeys[i];
                const auto& c=idlePose.boneKeys[i];
                if(w<1&&i<idlePreviousPose.boneKeys.size())
                {
                    const auto& p=idlePreviousPose.boneKeys[i];
                    XMStoreFloat4(&k.rotation,XMQuaternionSlerp(XMLoadFloat4(&p.rotation),XMLoadFloat4(&c.rotation),w));
                    XMStoreFloat3(&k.position,XMVectorLerp(XMLoadFloat3(&p.position),XMLoadFloat3(&c.position),w));
                }
                else {k.rotation=c.rotation;k.position=c.position;}
            }
            for(size_t i=0;i<pose.morphKeys.size()&&i<idlePose.morphKeys.size();++i)
            {
                const float c=idlePose.morphKeys[i].value;
                pose.morphKeys[i].value=w<1&&i<idlePreviousPose.morphKeys.size()?idlePreviousPose.morphKeys[i].value+(c-idlePreviousPose.morphKeys[i].value)*w:c;
            }
        }

        void ApplyShot(double dt)
        {
            using namespace DirectX;
            constexpr double FadeIn=0.3,FadeOut=0.45;
            if(!shotEnding)
            {
                shotFrame+=dt*30.0;
                if(shotFrame>=shotLast){shotFrame=shotLast;shotEnding=true;}
                shotWeight=static_cast<float>(std::min(1.0,shotWeight+dt/FadeIn));
            }
            else if((shotWeight-=static_cast<float>(dt/FadeOut))<=0)
            {
                shotWeight=0;
                shotActive=false;
                return;
            }
            shotSource->HiPose(shotPose,static_cast<float>(shotFrame),false,false);
            motion.AddRootOffset(shotPose);
            auto& pose=solver->solvedPose;
            const float w=shotWeight*shotWeight*(3-2*shotWeight);
            for(size_t i=0;i<pose.boneKeys.size()&&i<shotPose.boneKeys.size();++i)
            {
                auto& k=pose.boneKeys[i];
                const auto& s=shotPose.boneKeys[i];
                XMStoreFloat4(&k.rotation,XMQuaternionSlerp(XMLoadFloat4(&k.rotation),XMLoadFloat4(&s.rotation),w));
                XMStoreFloat3(&k.position,XMVectorLerp(XMLoadFloat3(&k.position),XMLoadFloat3(&s.position),w));
            }
            for(size_t i=0;i<pose.morphKeys.size()&&i<shotPose.morphKeys.size();++i)
                pose.morphKeys[i].value+=(shotPose.morphKeys[i].value-pose.morphKeys[i].value)*w;
        }
        DirectX::XMFLOAT2 frameAcceleration{};   // window acceleration, applied to physics as inertia

        void ApplyFrameAcceleration()
        {
            // Bullet's default MMD gravity is (0,-98,0); a window accelerating by a feels like g - a.
            if(physics&&physics->World())
                physics->World()->setGravity(btVector3(-frameAcceleration.x,-98.0f-frameAcceleration.y,0));
        }

        double frame=0;          // playback position in 30fps MMD frames
        bool continuous=false;   // false for the first solve after (re)start, per PoseSolver::Solve

        // Procedural layer. Bone/morph indices are -1 when the model lacks them.
        struct Procedural
        {
            bool blink=true,breath=true,lookAt=true;
            int blinkMorph=-1;
            std::array<int,6> mouthMorphs{-1,-1,-1,-1,-1,-1};   // あ い う え お ん
            std::array<float,6> mouth{};
            bool mouthActive=false;
            float talkAmount=0;           // eases in and out so the mouth does not snap
            float nod=0;                  // the head dips a little with each syllable
            bool dozing=false;
            float doze=0;                 // 0 awake .. 1 asleep
            // Feelings: the morphs each one sets (and how far), the one shown, and every
            // morph any of them uses with its current, eased weight.
            std::array<std::vector<std::pair<int,float>>,7> expressionMorphs;
            Expression expression=Expression::Neutral;
            std::vector<std::pair<int,float>> face;
            // Every morph that closes the eyes (blink, smiling eyes, winks, ...) with how far it
            // closes them at full weight. What this layer adds to one is capped so that, with the
            // others the motion, a gesture or this layer already show, the eyes are at most shut.
            std::vector<std::pair<int,float>> eyeMorphs;
            int upperBody=-1,leftShoulder=-1,rightShoulder=-1;
            int neck=-1,head=-1,eyes=-1;
            double clock=0;
            double nextBlink=2.0,blinkStart=-1;
            float blinkGate=1;   // 0 while the face is posed (gesture smile/wink, an opened motion)
            bool doubleBlink=false;
            float targetYaw=0,targetPitch=0,yaw=0,pitch=0;
            bool lookActive=false;
            std::uint32_t random=0x9E3779B9u;
            float Random01(){random=random*1664525u+1013904223u;return (random>>8)*(1.0f/16777216.0f);}
        } proc;

        int FindBone(std::initializer_list<const wchar_t*> names) const
        {
            for(auto n:names)if(auto it=solver->boneDict.find(n);it!=solver->boneDict.end())return it->second;
            return -1;
        }
        int FindMorph(std::initializer_list<const wchar_t*> names) const
        {
            for(auto n:names)if(auto it=solver->morphDict.find(n);it!=solver->morphDict.end())return it->second;
            return -1;
        }
        void ResolveProceduralTargets()
        {
            proc.blinkMorph=FindMorph({L"まばたき",L"瞬き",L"blink",L"Blink"});
            proc.eyeMorphs.clear();
            for(const auto& [name,closes]:std::initializer_list<std::pair<const wchar_t*,float>>{
                    {L"まばたき",1},{L"瞬き",1},{L"blink",1},{L"Blink",1},{L"笑い",1},{L"にっこり",1},{L"なごみ",1},{L"はぅ",1},
                    {L"目閉じ",1},{L"ウィンク",1},{L"ウインク",1},{L"ウィンク２",1},{L"ウィンク右",1},{L"ウィンク２右",1},{L"ｳｨﾝｸ２右",1},
                    {L"じと目",0.5f},{L"ジト目",0.5f}})
                if(const int morph=FindMorph({name});morph>=0&&
                   std::none_of(proc.eyeMorphs.begin(),proc.eyeMorphs.end(),[&](const auto& e){return e.first==morph;}))
                    proc.eyeMorphs.push_back({morph,closes});
            proc.mouthMorphs={FindMorph({L"あ",L"ア"}),FindMorph({L"い",L"イ"}),FindMorph({L"う",L"ウ"}),
                              FindMorph({L"え",L"エ"}),FindMorph({L"お",L"オ"}),FindMorph({L"ん",L"ン"})};
            {
                using E=Expression;
                struct Part {E e;std::initializer_list<const wchar_t*> names;float weight;};
                const Part parts[]={
                    {E::Smile,{L"笑い"},0.7f},{E::Smile,{L"にこり",L"にっこり"},0.5f},
                    {E::Surprise,{L"びっくり",L"驚き"},0.7f},{E::Surprise,{L"上"},0.6f},
                    {E::Troubled,{L"困る",L"困り"},0.8f},
                    {E::Angry,{L"怒り"},0.7f},{E::Angry,{L"ジト目",L"じと目"},0.3f},
                    {E::Sad,{L"困る",L"困り"},0.6f},{E::Sad,{L"悲しい",L"悲しみ"},0.5f},
                    {E::Shy,{L"照れ"},0.8f},{E::Shy,{L"にこり",L"にっこり"},0.4f},{E::Shy,{L"笑い"},0.3f}};
                for(auto& list:proc.expressionMorphs)list.clear();
                proc.face.clear();
                for(const auto& p:parts)
                {
                    const int morph=FindMorph(p.names);
                    if(morph<0)continue;
                    proc.expressionMorphs[static_cast<size_t>(p.e)].push_back({morph,p.weight});
                    if(std::none_of(proc.face.begin(),proc.face.end(),[&](const auto& f){return f.first==morph;}))proc.face.push_back({morph,0.0f});
                }
            }
            proc.upperBody=FindBone({L"上半身2",L"上半身"});
            proc.leftShoulder=FindBone({L"左肩"});
            proc.rightShoulder=FindBone({L"右肩"});
            proc.neck=FindBone({L"首"});
            proc.head=FindBone({L"頭"});
            proc.eyes=FindBone({L"両目"});
        }

        // Multiply an extra rotation (row-vector convention, applied after the key) into a bone key.
        void AddRotation(PMX::Pose& pose,int bone,float pitch,float yaw,float roll)
        {
            if(bone<0||static_cast<size_t>(bone)>=pose.boneKeys.size())return;
            auto& r=pose.boneKeys[bone].rotation;
            const auto key=DirectX::XMLoadFloat4(&r);
            const auto extra=DirectX::XMQuaternionRotationRollPitchYaw(pitch,yaw,roll);
            DirectX::XMStoreFloat4(&r,DirectX::XMQuaternionNormalize(DirectX::XMQuaternionMultiply(key,extra)));
        }

        float BlinkWeight(double dt)
        {
            proc.clock+=dt;
            constexpr double Close=0.07,Hold=0.03,Open=0.12,Total=Close+Hold+Open;
            if(proc.blinkStart<0&&proc.clock>=proc.nextBlink)proc.blinkStart=proc.clock;
            if(proc.blinkStart<0)return 0.0f;
            const double t=proc.clock-proc.blinkStart;
            if(t>=Total)
            {
                proc.blinkStart=-1;
                if(proc.doubleBlink){proc.doubleBlink=false;proc.nextBlink=proc.clock+0.12;}
                else{proc.doubleBlink=proc.Random01()<0.15f;proc.nextBlink=proc.clock+2.5+proc.Random01()*3.5;}
                return 0.0f;
            }
            const double w=t<Close?t/Close:(t<Close+Hold?1.0:1.0-(t-Close-Hold)/Open);
            return static_cast<float>(w*w*(3-2*w));
        }

        // Lower T-stance arms to the A-stance rest that MMD motions assume. The upper arm gets
        // C applied before its key; descendants get their keys conjugated (C, key, C^-1) so each
        // key still rotates about the same world axis it was authored for.
        void ApplyArmCorrection(PMX::Pose& pose)
        {
            using namespace DirectX;
            for(const auto& side:arms)
            {
                if(side.correction==0.0f||side.arm<0)continue;
                const XMVECTOR c=XMLoadFloat4(&side.rotation),ci=XMLoadFloat4(&side.inverse);
                auto& armKey=pose.boneKeys[side.arm].rotation;
                XMStoreFloat4(&armKey,XMQuaternionMultiply(c,XMLoadFloat4(&armKey)));
                for(int bone:side.descendants)
                {
                    auto& r=pose.boneKeys[bone].rotation;
                    XMStoreFloat4(&r,XMQuaternionMultiply(XMQuaternionMultiply(c,XMLoadFloat4(&r)),ci));
                }
            }
        }

        // How far morph may still close the eyes, given every other eye-closing morph's weight now.
        float EyeRoom(const PMX::Pose& pose,int morph) const
        {
            float closed=0;
            for(const auto& [e,closes]:proc.eyeMorphs)
                if(e!=morph&&static_cast<size_t>(e)<pose.morphKeys.size())closed+=closes*pose.morphKeys[e].value;
            return std::clamp(1.0f-closed,0.0f,1.0f);
        }
        bool IsEyeMorph(int morph) const
        {
            return std::any_of(proc.eyeMorphs.begin(),proc.eyeMorphs.end(),[&](const auto& e){return e.first==morph;});
        }

        void ApplyProcedural(PMX::Pose& pose,double dt)
        {
            // No blinking over a posed face: the morphs would add up and over-deform it.
            const bool facePosed=motion.FaceBusy()||shotActive||proc.expression!=Expression::Neutral||proc.doze>0;
            proc.blinkGate=std::clamp(proc.blinkGate+static_cast<float>(dt/0.12)*(facePosed?-1.0f:1.0f),0.0f,1.0f);
            const float blink=BlinkWeight(dt)*proc.blinkGate;
            if(proc.blink&&proc.blinkMorph>=0&&static_cast<size_t>(proc.blinkMorph)<pose.morphKeys.size())
            {
                auto& v=pose.morphKeys[proc.blinkMorph].value;
                v=std::max(v,std::min(blink,EyeRoom(pose,proc.blinkMorph)));
            }
            // Speaking: the lip-sync's mouth shapes, over whatever the face already shows.
            proc.talkAmount=std::clamp(proc.talkAmount+static_cast<float>(dt/0.12)*(proc.mouthActive?1.0f:-1.0f),0.0f,1.0f);
            if(proc.talkAmount>0)
                for(size_t i=0;i<proc.mouthMorphs.size();++i)
                {
                    const int morph=proc.mouthMorphs[i];
                    if(morph<0||static_cast<size_t>(morph)>=pose.morphKeys.size())continue;
                    auto& v=pose.morphKeys[morph].value;
                    v=std::max(v,proc.mouth[i]*proc.talkAmount);
                }
            // The feeling, eased in and out (a quarter of a second), over the face.
            {
                const auto& target=proc.expressionMorphs[static_cast<size_t>(proc.expression)];
                const float step=static_cast<float>(dt/0.25);
                for(auto& [morph,weight]:proc.face)
                {
                    float want=0;
                    for(const auto& [m,w]:target)if(m==morph)want=std::max(want,w);
                    weight+=std::clamp(want-weight,-step,step);
                    if(weight>0&&static_cast<size_t>(morph)<pose.morphKeys.size())
                    {
                        auto& v=pose.morphKeys[morph].value;
                        v=std::max(v,IsEyeMorph(morph)?std::min(weight,EyeRoom(pose,morph)):weight);
                    }
                }
            }
            // Dozing: eyes shut, the head down and nodding now and then, the chest sunk a little.
            proc.doze=std::clamp(proc.doze+static_cast<float>(dt/(proc.dozing?3.0:0.5))*(proc.dozing?1.0f:-1.0f),0.0f,1.0f);
            if(proc.doze>0)
            {
                const float d=proc.doze*proc.doze*(3-2*proc.doze);
                if(proc.blinkMorph>=0&&static_cast<size_t>(proc.blinkMorph)<pose.morphKeys.size())
                {
                    auto& v=pose.morphKeys[proc.blinkMorph].value;
                    v=std::max(v,std::min(d,EyeRoom(pose,proc.blinkMorph)));
                }
                // A slow sway, and every so often the head drops further and comes back up.
                const double t=proc.clock;
                const float nodOff=static_cast<float>(std::pow(std::max(0.0,std::sin(t*DirectX::XM_2PI/7.0)),8.0));
                const float sway=static_cast<float>(std::sin(t*DirectX::XM_2PI/5.0));
                AddRotation(pose,proc.neck,DirectX::XMConvertToRadians(-12.0f-6.0f*nodOff)*d,0,DirectX::XMConvertToRadians(2.0f)*sway*d);
                AddRotation(pose,proc.head,DirectX::XMConvertToRadians(-8.0f-6.0f*nodOff)*d,0,DirectX::XMConvertToRadians(3.0f)*sway*d);
                AddRotation(pose,proc.upperBody,DirectX::XMConvertToRadians(-3.0f)*d,0,0);
            }
            // Speaking, the head dips a little with the open syllables.
            {
                float open=0;
                for(const float m:proc.mouth)open+=m;
                open=std::min(1.0f,open)*proc.talkAmount;
                proc.nod+=(open-proc.nod)*static_cast<float>(1.0-std::exp(-dt/0.09));
                if(proc.nod>1e-3f)AddRotation(pose,proc.head,DirectX::XMConvertToRadians(-2.5f)*proc.nod,0,0);
            }
            if(proc.breath)
            {
                // About 15 breaths per minute: a slight chest lift and shoulder rise.
                const float s=static_cast<float>(std::sin(proc.clock*DirectX::XM_2PI/4.0));
                AddRotation(pose,proc.upperBody,DirectX::XMConvertToRadians(-0.8f)*s,0,0);
                const float shoulder=DirectX::XMConvertToRadians(0.9f)*(s*0.5f+0.5f);
                AddRotation(pose,proc.leftShoulder,0,0,shoulder);
                AddRotation(pose,proc.rightShoulder,0,0,-shoulder);
            }
            // Ease toward the target; release to the motion's own pose when inactive.
            const float follow=static_cast<float>(1.0-std::exp(-dt/0.18));
            const bool active=proc.lookAt&&proc.lookActive;
            proc.yaw+=((active?proc.targetYaw:0.0f)-proc.yaw)*follow;
            proc.pitch+=((active?proc.targetPitch:0.0f)-proc.pitch)*follow;
            if(std::abs(proc.yaw)>1e-4f||std::abs(proc.pitch)>1e-4f)
            {
                // The face points to -Z: +Y rotation turns it toward -X (screen left), +X rotation tilts it up.
                const float yaw=-proc.yaw,pitch=proc.pitch;
                const bool hasNeck=proc.neck>=0;
                AddRotation(pose,proc.neck,pitch*0.4f,yaw*0.4f,0);
                AddRotation(pose,proc.head,pitch*(hasNeck?0.6f:1.0f),yaw*(hasNeck?0.6f:1.0f),0);
                const float eyeLimit=DirectX::XMConvertToRadians(10.0f);
                // The eyes turn sideways half as far as they tilt (a full sideways turn looks wall-eyed).
                AddRotation(pose,proc.eyes,std::clamp(pitch*0.4f,-eyeLimit,eyeLimit),std::clamp(yaw*0.2f,-0.5f*eyeLimit,0.5f*eyeLimit),0);
            }
        }

        ~Impl()
        {
            solver.reset();
            physics.reset();
        }

        void CreateSolver(bool withPhysics)
        {
            solver.reset();
            physics.reset();
            if(withPhysics)
            {
                physics=std::make_unique<PMX::Physics>();
                physics->fps=physicsFps;
                physics->maxSubsteps=4;
            }
            solver=std::make_unique<PMX::PoseSolver>(physics.get(),pmx.get(),nullptr);
            ApplyFrameAcceleration();
            ResolveProceduralTargets();
            // Settle physics against the corrected pose so sleeves do not start from a T-stance.
            solver->HiPose(solver->solvedPose,static_cast<float>(frame),false,false);
            SampleIdle(solver->solvedPose,0);
            ApplyArmCorrection(solver->solvedPose);
            solver->PoseMining(solver->solvedPose);
            if(physics)
            {
                // Same settle sequence as MikuMikuDayo's CalmPhysics (default prewarm 5 steps).
                physics->Reset();
                for(int i=0;i<5;++i)physics->Update(1/30.0);
            }
            continuous=false;
            appliedMorphs.assign(pmx->morphs.size(),0.0f);
            dynamicVertices=baseVertices;
            ++dynamicRevision;
            ApplyMorphs();
            UpdateMaterials();
        }

        void ApplyMorphs()
        {
            const auto& values=solver->morphValues;
            bool changed=false;
            for(size_t i=0;i<morphDeltas.size()&&i<values.size();++i)
                if(values[i]!=appliedMorphs[i]&&(!morphDeltas[i].positions.empty()||!morphDeltas[i].uvs.empty()))
                {changed=true;break;}
            if(!changed)return;
            dynamicVertices=baseVertices;
            for(size_t i=0;i<morphDeltas.size()&&i<values.size();++i)
            {
                const float w=values[i];
                appliedMorphs[i]=w;
                if(w==0.0f)continue;
                for(const auto& o:morphDeltas[i].positions)
                {
                    auto& p=dynamicVertices[o.vertex].position;
                    p[0]+=o.x*w;p[1]+=o.y*w;p[2]+=o.z*w;
                }
                for(const auto& o:morphDeltas[i].uvs)
                {
                    auto& t=dynamicVertices[o.vertex].uv;
                    t[0]+=o.u*w;t[1]+=o.v*w;
                }
            }
            ++dynamicRevision;
        }

        void UpdateMaterials()
        {
            for(size_t i=0;i<materials.size()&&i<solver->materials.size();++i)
            {
                const auto& s=solver->materials[i];
                auto& m=materials[i];
                m.diffuse=ToArray(s.diffuse);
                m.specular=ToArray(s.specular);
                m.shininess=s.shininess;
                m.ambient=ToArray(s.ambient);
                m.edgeColor=ToArray(s.edgeColor);
                m.edgeSize=s.edgeSize;
                m.textureMul=ToArray(s.textureMulValue);m.textureAdd=ToArray(s.textureAddValue);
                m.sphereMul=ToArray(s.sphereMulValue);m.sphereAdd=ToArray(s.sphereAddValue);
                m.toonMul=ToArray(s.toonTextureMulValue);m.toonAdd=ToArray(s.toonTextureAddValue);
            }
        }
    };

    MascotModel::MascotModel():impl(std::make_unique<Impl>()){}
    MascotModel::~MascotModel()=default;

    bool MascotModel::SetMotion(const std::wstring& path,std::wstring& error)
    {
        auto& m=*impl;
        if(!m.shotIsClick){m.shotActive=false;m.shotWeight=0;}
        m.shotSampler.reset();
        m.shotVmd.reset();
        if(path.empty()||!m.pmx)return true;
        if(!std::filesystem::exists(path)){error=L"モーションファイルが見つかりません: "+path;return false;}
        if(!IsVmdFile(path)){error=L"VMDファイルではないか、大きすぎます: "+path;return false;}
        try
        {
            if(!LoadVmd(path,m.pmx.get(),m.shotVmd,m.shotSampler))
            {
                error=L"モーションのデータが壊れているため、読み込めませんでした: "+path;
                return false;
            }
        }
        catch(...)
        {
            m.shotSampler.reset();
            m.shotVmd.reset();
            error=L"モーションを読み込めませんでした: "+path;
            return false;
        }
        return true;
    }

    bool MascotModel::HasMotion() const {return impl->shotSampler!=nullptr;}

    void MascotModel::PlayMotion()
    {
        auto& m=*impl;
        if(!m.shotSampler)return;
        m.shotSource=m.shotSampler.get();
        m.shotLast=m.shotVmd->lastFrame;
        m.shotIsClick=false;
        m.shotFrame=0;
        m.shotEnding=false;
        m.shotActive=true;
    }

    void MascotModel::StopMotion(){if(impl->shotActive)impl->shotEnding=true;}
    bool MascotModel::MotionPlaying() const {return impl->shotActive&&!impl->shotIsClick;}

    bool MascotModel::SetClickMotions(const std::vector<std::wstring>& paths,std::wstring& error)
    {
        auto& m=*impl;
        if(!m.pmx)return paths.empty();
        std::vector<Impl::Clip> clips;
        if(!m.LoadClips(paths,clips,L"クリック時のモーション",error))return false;
        if(m.shotActive&&m.shotIsClick){m.shotActive=false;m.shotWeight=0;m.shotIsClick=false;}
        m.clickClips=std::move(clips);
        m.lastClick=-1;
        return true;
    }

    bool MascotModel::SetGaitMotion(int which,const std::wstring& path,std::wstring& error)
    {
        auto& m=*impl;
        if(which<0||which>1||!m.solver)return false;
        std::vector<Impl::Clip> clips;
        if(!path.empty())
        {
            if(!m.LoadClips({path},clips,which==0?L"歩くモーション":L"走るモーション",error))return false;
            const float speed=m.GaitSpeed(clips.front());
            if(speed<=0)
            {
                error=L"このモーションでは、足が地面について進む速さを測れませんでした（その場で足踏みする、ループの歩き・走りのモーションを選んでください）: "+path;
                return false;
            }
            m.motion.SetClipSpeed(which,speed);
        }
        else m.motion.SetClipSpeed(which,0);
        m.gaitClips[which]=std::move(clips);
        return true;
    }

    bool MascotModel::React(bool head)
    {
        auto& m=*impl;
        if(!m.solver||m.shotActive||m.motion.Reacting())return false;
        if(!m.clickClips.empty()&&m.motion.CurrentActivity()==Activity::Idle)
        {
            int pick=static_cast<int>(m.idleRandom()%m.clickClips.size());
            if(m.clickClips.size()>1)
                while(pick==m.lastClick)pick=static_cast<int>(m.idleRandom()%m.clickClips.size());
            m.lastClick=pick;
            m.shotSource=m.clickClips[pick].sampler.get();
            m.shotLast=m.clickClips[pick].vmd->lastFrame;
            m.shotIsClick=true;
            m.shotFrame=0;
            m.shotEnding=false;
            m.shotActive=true;
            return true;
        }
        return m.motion.React(head);
    }

    bool MascotModel::IdleMotionAtBreak() const {return impl->IdleAtBreak();}

    bool MascotModel::Reacting() const {return impl->motion.Reacting()||(impl->shotActive&&impl->shotIsClick);}
    bool MascotModel::HeadBusy() const {return impl->motion.HeadBusy();}
    bool MascotModel::ForceGesture(const std::wstring& name){return impl->motion.ForceGesture(name);}

    bool MascotModel::SetIdleMotions(const std::vector<std::wstring>& paths,std::wstring& error)
    {
        if(!impl->pmx)return paths.empty();
        return impl->LoadIdle(paths,error);
    }

    bool MascotModel::Load(const std::wstring& modelPath,const std::vector<std::wstring>& idleMotionPaths,bool physics,int physicsFps,std::wstring& error)
    {
        auto next=std::make_unique<Impl>();
        next->physicsFps=physicsFps;
        if(!IsPmxFile(modelPath))
        {
            error=L"PMX 2.0/2.1 のモデルファイルではないか、大きすぎるため、読み込めません。\n\nモデル: "+modelPath;
            return false;
        }
        try
        {
            struct Context {const std::wstring* path;PMX::PMXModel* model;} context{&modelPath,nullptr};
            if(!RunGuarded([](void* c){auto* ctx=static_cast<Context*>(c);ctx->model=new PMX::PMXModel(ctx->path->c_str());},&context))
            {
                error=L"モデルのデータが壊れているため、読み込めませんでした。\n\nモデル: "+modelPath;
                return false;
            }
            next->pmx.reset(context.model);
            if(!next->LoadIdle(idleMotionPaths,error))return false;
        }
        catch(const std::exception& e)
        {
            error=L"読み込みに失敗しました: "+modelPath+L"\n"+std::wstring(e.what(),e.what()+std::strlen(e.what()));
            return false;
        }
        catch(...)
        {
            error=L"読み込みに失敗しました: "+modelPath;
            return false;
        }

        // Some otherwise fine models carry an append flag whose parent is missing (often on
        // ribbon or tail bones). Drop just that append, as MMD effectively does, instead of
        // rejecting the model; the in-memory copy is ours, PMXLoader itself is untouched.
        for(auto& bone:next->pmx->bones)
            if((bone.IsAppendRotation()||bone.IsAppendTranslation())&&
               (bone.appendParent<0||bone.appendParent>=static_cast<int>(next->pmx->bones.size())))
                bone.flag&=~(0x0100|0x0200);

        const auto& pmx=*next->pmx;
        if(pmx.vertices.empty()||pmx.indices.empty()){error=L"頂点のないモデルです: "+modelPath;return false;}
        if(const auto broken=FindBrokenReference(pmx);!broken.empty())
        {
            error=L"モデルのデータに範囲外の参照があるため、表示できません。\n\nモデル: "+modelPath+L"\n箇所: "+broken;
            return false;
        }

        // Only humanoid models with the standard MMD skeleton can play motions sensibly.
        auto findBone=[&](std::initializer_list<const wchar_t*> names)->int{
            for(auto name:names)
                for(size_t i=0;i<pmx.bones.size();++i)
                    if(pmx.bones[i].name==name)return static_cast<int>(i);
            return -1;
        };
        {
            const std::initializer_list<const wchar_t*> required[]={
                {L"センター"},{L"上半身"},{L"下半身"},{L"首"},{L"頭"},
                {L"左腕"},{L"右腕"},{L"左ひじ",L"左肘"},{L"右ひじ",L"右肘"},
                {L"左足"},{L"右足"},{L"左ひざ",L"左膝"},{L"右ひざ",L"右膝"},
                {L"左足ＩＫ",L"左足IK"},{L"右足ＩＫ",L"右足IK"}};
            std::wstring missing;
            for(const auto& names:required)
                if(findBone(names)<0)missing+=(missing.empty()?L"":L"、")+std::wstring(*names.begin());
            if(!missing.empty())
            {
                error=L"人型モデルに必要なボーンが見つからないため、このモデルは表示できません。\n\n"
                      L"モデル: "+modelPath+L"\n不足しているボーン: "+missing;
                return false;
            }
            // Bone names alone are not enough (some animals reuse them): check the skeleton is upright.
            auto pos=[&](std::initializer_list<const wchar_t*> names){return pmx.bones[findBone(names)].position;};
            const auto head=pos({L"頭"}),neck=pos({L"首"}),upper=pos({L"上半身"});
            const auto knee=pos({L"左ひざ",L"左膝"}),arm=pos({L"左腕"}),elbow=pos({L"左ひじ",L"左肘"});
            const float spineX=head.x-upper.x,spineY=head.y-upper.y,spineZ=head.z-upper.z;
            const float spineTilt=DirectX::XMConvertToDegrees(std::atan2(std::sqrt(spineX*spineX+spineZ*spineZ),spineY));
            const float armDown=DirectX::XMConvertToDegrees(std::atan2(arm.y-elbow.y,std::abs(elbow.x-arm.x)));
            std::wstring reason;
            if(!(head.y>neck.y&&neck.y>upper.y&&upper.y>knee.y))reason=L"頭・首・上半身・ひざが上から順に並んでいません";
            else if(spineTilt>35.0f)reason=L"上半身から頭への向きが垂直ではありません（"+std::to_wstring(static_cast<int>(spineTilt))+L"°傾いています）";
            else if(armDown>70.0f)reason=L"腕が体の横に伸びていません（下向きに"+std::to_wstring(static_cast<int>(armDown))+L"°）";
            if(!reason.empty())
            {
                error=L"人型の骨格ではないため、このモデルは表示できません。\n\nモデル: "+modelPath+L"\n理由: "+reason;
                return false;
            }
        }

        // T-stance detection from the rest skeleton: upper arm direction (腕 -> ひじ).
        for(int side=0;side<2;++side)
        {
            auto& a=next->arms[side];
            a.arm=findBone({side==0?L"左腕":L"右腕"});
            const int elbow=findBone(side==0?std::initializer_list<const wchar_t*>{L"左ひじ",L"左肘"}:std::initializer_list<const wchar_t*>{L"右ひじ",L"右肘"});
            const auto& p0=pmx.bones[a.arm].position;
            const auto& p1=pmx.bones[elbow].position;
            a.restAngle=DirectX::XMConvertToDegrees(std::atan2(p0.y-p1.y,std::abs(p1.x-p0.x)));
            if(a.restAngle<TStanceThreshold)
            {
                a.correction=StandardArmAngle-a.restAngle;
                // +Z raises +X, so the left arm (+X side) is lowered with a negative roll.
                const float roll=DirectX::XMConvertToRadians(side==0?-a.correction:a.correction);
                const auto q=DirectX::XMQuaternionRotationRollPitchYaw(0,0,roll);
                DirectX::XMStoreFloat4(&a.rotation,q);
                DirectX::XMStoreFloat4(&a.inverse,DirectX::XMQuaternionInverse(q));
                for(size_t i=0;i<pmx.bones.size();++i)
                    for(int p=pmx.bones[i].parent,guard=0;p>=0&&guard<512;p=pmx.bones[p].parent,++guard)
                        if(p==a.arm){a.descendants.push_back(static_cast<int>(i));break;}
            }
        }

        next->staticVertices.resize(pmx.vertices.size());
        next->baseVertices.resize(pmx.vertices.size());
        next->boundsMin={1e30f,1e30f,1e30f};
        next->boundsMax={-1e30f,-1e30f,-1e30f};
        for(size_t i=0;i<pmx.vertices.size();++i)
        {
            const auto& v=pmx.vertices[i];
            const auto& skin=pmx.skinTable[i];
            auto& s=next->staticVertices[i];
            s.normal=ToArray(v.normal);
            for(int j=0;j<4;++j){s.bone[j]=skin.iBone[j];s.weight[j]=skin.weight[j];}
            s.sdefC=ToArray(skin.sdef_c);
            s.weightType=static_cast<std::uint32_t>(skin.weightType);
            s.edge=v.edge;
            auto& d=next->baseVertices[i];
            d.position=ToArray(v.position);
            d.uv={v.uv.x,v.uv.y};
            for(int a=0;a<3;++a)
            {
                next->boundsMin[a]=std::min(next->boundsMin[a],d.position[a]);
                next->boundsMax[a]=std::max(next->boundsMax[a],d.position[a]);
            }
        }
        // Front of the soles: the ground line is drawn there, so perspective does not push the
        // toes (nearer the camera) below the surface the mascot stands on.
        {
            const float soleTop=next->boundsMin[1]+0.03f*(next->boundsMax[1]-next->boundsMin[1]);
            float front=1e30f;
            for(const auto& d:next->baseVertices)if(d.position[1]<=soleTop)front=std::min(front,d.position[2]);
            next->soleFrontZ=front<1e29f?front:(next->boundsMin[2]+next->boundsMax[2])*0.5f;
        }
        // Leg bones for the cursor check, physics bones for the stuck-physics check.
        for(const wchar_t* side:{L"左",L"右"})
            for(const wchar_t* part:{L"足",L"ひざ",L"足首",L"つま先"})
                for(size_t i=0;i<pmx.bones.size();++i)
                    if(pmx.bones[i].name==std::wstring(side)+part){next->legBones.push_back(static_cast<int>(i));break;}
        for(size_t i=0;i<pmx.bones.size();++i)
            if(pmx.bones[i].name==L"下半身"){next->legBones.push_back(static_cast<int>(i));break;}
        {
            std::vector<bool> simulated(pmx.bones.size(),false);
            for(const auto& body:pmx.bodies)
                if(body.mode!=0&&body.bone>=0&&static_cast<size_t>(body.bone)<pmx.bones.size())simulated[body.bone]=true;
            for(size_t i=0;i<pmx.bones.size();++i)
            {
                if(!simulated[i])continue;
                int anchor=pmx.bones[i].parent;
                for(size_t guard=0;anchor>=0&&static_cast<size_t>(anchor)<pmx.bones.size()&&simulated[anchor]&&guard<pmx.bones.size();++guard)
                    anchor=pmx.bones[anchor].parent;
                if(anchor<0||static_cast<size_t>(anchor)>=pmx.bones.size()||simulated[anchor])continue;
                const auto& a=pmx.bones[anchor].position;
                const auto& b=pmx.bones[i].position;
                const float length=std::sqrt((b.x-a.x)*(b.x-a.x)+(b.y-a.y)*(b.y-a.y)+(b.z-a.z)*(b.z-a.z));
                if(length>0.3f)next->physicsBones.push_back({static_cast<int>(i),anchor,length});
            }
        }
        next->indices.assign(pmx.indices.begin(),pmx.indices.end());

        next->morphDeltas.resize(pmx.morphs.size());
        for(size_t i=0;i<pmx.morphs.size();++i)
        {
            const auto& morph=pmx.morphs[i];
            auto& deltas=next->morphDeltas[i];
            if(morph.kind==1)
            {
                const auto* o=reinterpret_cast<const PMX::PMXVertexMorphOffset*>(morph.offsets.data());
                for(int j=0;j<morph.offsetCount;++j)
                    if(o[j].vertex>=0&&static_cast<size_t>(o[j].vertex)<pmx.vertices.size())
                        deltas.positions.push_back({static_cast<std::uint32_t>(o[j].vertex),o[j].offset.x,o[j].offset.y,o[j].offset.z});
            }
            else if(morph.kind==3)
            {
                const auto* o=reinterpret_cast<const PMX::PMXUVMorphOffset*>(morph.offsets.data());
                for(int j=0;j<morph.offsetCount;++j)
                    if(o[j].vertex>=0&&static_cast<size_t>(o[j].vertex)<pmx.vertices.size())
                        deltas.uvs.push_back({static_cast<std::uint32_t>(o[j].vertex),o[j].offset.x,o[j].offset.y});
            }
        }

        // Textures are read only from local files: a path to another machine (\\server\share,
        // also the \\?\ and \\.\ device forms) would make Windows connect out and offer the
        // user's credentials, and a colon past the drive letter names a stream or a URL.
        next->texturePaths=pmx.absTextures;
        for(auto& path:next->texturePaths)
        {
            const bool remote=path.size()>=2&&(path[0]==L'\\'||path[0]==L'/')&&(path[1]==L'\\'||path[1]==L'/');
            const auto colon=path.find(L':',2);
            if(remote||colon!=std::wstring::npos)path.clear();
        }
        next->materials.resize(pmx.materials.size());
        std::uint32_t start=0;
        for(size_t i=0;i<pmx.materials.size();++i)
        {
            const auto& p=pmx.materials[i];
            auto& m=next->materials[i];
            m.drawFlag=p.drawFlag;
            m.texture=p.tex;
            m.sphere=p.spTex;
            m.sphereMode=p.spmode;
            m.sharedToon=p.toonFlag!=0;
            m.toon=p.toonTex;
            m.indexStart=start;
            m.indexCount=static_cast<std::uint32_t>(std::max(p.vertexCount,0));
            start+=m.indexCount;
        }

        {
            std::array<GeneratedMotion::ArmRest,2> rest;
            for(int side=0;side<2;++side)
            {
                if(next->arms[side].correction==0.0f)continue;
                rest[side].arm=next->arms[side].arm;
                rest[side].descendants=next->arms[side].descendants;
                rest[side].rotation=next->arms[side].rotation;
            }
            next->motion.Setup(pmx,rest);
        }
        try
        {
            next->CreateSolver(physics);
        }
        catch(...)
        {
            // PoseSolver rejects some malformed skeletons (e.g. broken IK or append chains) by throwing.
            next->solver.reset();
            next->physics.reset();
            error=L"このモデルのボーン構造を解釈できないため、表示できません。\n\nモデル: "+modelPath;
            return false;
        }
        if(next->arms[0].correction!=0.0f||next->arms[1].correction!=0.0f)
        {
            // Frame the window around the corrected arms, not the T-stance rest pose.
            using namespace DirectX;
            std::array<float,3> lo{1e30f,1e30f,1e30f},hi{-1e30f,-1e30f,-1e30f};
            const auto& bones=next->solver->boneMatrices;
            for(size_t i=0;i<next->staticVertices.size();++i)
            {
                const auto& s=next->staticVertices[i];
                const auto& p=next->baseVertices[i].position;
                XMVECTOR sum=XMVectorZero();
                const XMVECTOR v=XMVectorSet(p[0],p[1],p[2],1);
                for(int j=0;j<4;++j)
                    if(s.bone[j]>=0&&static_cast<size_t>(s.bone[j])<bones.size()&&(s.weightType==3?j<2:true))
                    {
                        const float w=s.weightType==3?(j==0?s.weight[0]:1.0f-s.weight[0]):s.weight[j];
                        sum=XMVectorAdd(sum,XMVectorScale(XMVector3Transform(v,XMMatrixTranspose(bones[s.bone[j]])),w));
                    }
                for(int a=0;a<3;++a)
                {
                    lo[a]=std::min(lo[a],XMVectorGetByIndex(sum,a));
                    hi[a]=std::max(hi[a],XMVectorGetByIndex(sum,a));
                }
            }
            next->boundsMin=lo;
            next->boundsMax=hi;
        }
        impl=std::move(next);
        return true;
    }

    void MascotModel::SetPhysics(bool enabled)
    {
        if(impl->pmx&&enabled!=PhysicsEnabled())impl->CreateSolver(enabled);
    }

    bool MascotModel::PhysicsEnabled() const {return impl->physics!=nullptr;}

    void MascotModel::SetActivity(Activity activity,int direction){impl->motion.SetActivity(activity,direction);}
    Activity MascotModel::CurrentActivity() const {return impl->motion.CurrentActivity();}
    bool MascotModel::CanSit() const {return impl->motion.CanSit();}
    std::wstring MascotModel::DescribeRig() const {return impl->motion.Describe();}
    float MascotModel::TakeRootDelta(){return impl->motion.TakeRootDelta();}
    float MascotModel::RootOffset() const {return impl->motion.RootOffset();}
    void MascotModel::Land(float impactSpeed){impl->motion.Land(impactSpeed);}

    void MascotModel::SetCarryMotion(float vx,float vy,float ax,float ay)
    {
        impl->motion.SetCarryVelocity(vx,vy);
        // Clamp so a flick of the mouse does not throw the hair around wildly.
        const float limit=3.0f*98.0f;
        impl->frameAcceleration={std::clamp(ax,-limit,limit),std::clamp(ay,-limit,limit)};
        impl->ApplyFrameAcceleration();
    }

    void MascotModel::SetExpression(Expression expression,bool gesture)
    {
        auto& m=*impl;
        if(expression==m.proc.expression)return;
        m.proc.expression=expression;
        if(!gesture||m.proc.Random01()>0.6f)return;
        const wchar_t* name=expression==Expression::Smile?L"tilt":expression==Expression::Surprise?L"surprised":
                            expression==Expression::Shy?L"cheeks":expression==Expression::Troubled?L"think":
                            expression==Expression::Angry?L"hip":nullptr;
        if(name&&!m.shotActive)m.motion.Emote(name);
    }

    void MascotModel::SetDozing(bool dozing){impl->proc.dozing=dozing;}
    bool MascotModel::Emote(const std::wstring& name){return !impl->shotActive&&impl->motion.Emote(name);}
    bool MascotModel::Dozing() const {return impl->proc.doze>0;}

    void MascotModel::SetMouth(const std::array<float,6>& weights,bool active)
    {
        if(active)impl->proc.mouth=weights;
        impl->proc.mouthActive=active;
    }

    std::wstring MascotModel::ModelName() const {return impl->pmx?impl->pmx->name:std::wstring();}

    void MascotModel::SetProcedural(bool blink,bool breath,bool lookAt)
    {
        impl->proc.blink=blink;
        impl->proc.breath=breath;
        impl->proc.lookAt=lookAt;
    }

    void MascotModel::SetLookTarget(float yaw,float pitch,bool active)
    {
        impl->proc.targetYaw=yaw;
        impl->proc.targetPitch=pitch;
        impl->proc.lookActive=active;
    }

    namespace
    {
        // Bounds of posed bone origins; bones=nullptr means every bone.
        bool PosedBounds(const PMX::PMXModel& pmx,const PMX::PoseSolver* solver,const std::vector<int>* bones,
                         std::array<float,3>& lo,std::array<float,3>& hi)
        {
            if(!solver||solver->boneMatrices.empty())return false;
            using namespace DirectX;
            XMVECTOR minV=XMVectorReplicate(1e30f),maxV=XMVectorReplicate(-1e30f);
            const size_t count=std::min(solver->boneMatrices.size(),pmx.bones.size());
            bool any=false;
            auto add=[&](size_t i){
                if(i>=count)return;
                const XMVECTOR p=XMVector3Transform(XMLoadFloat3(&pmx.bones[i].position),XMMatrixTranspose(solver->boneMatrices[i]));
                minV=XMVectorMin(minV,p);
                maxV=XMVectorMax(maxV,p);
                any=true;
            };
            if(bones)for(int i:*bones)add(static_cast<size_t>(i));
            else for(size_t i=0;i<count;++i)add(i);
            if(!any)return false;
            lo={XMVectorGetX(minV),XMVectorGetY(minV),XMVectorGetZ(minV)};
            hi={XMVectorGetX(maxV),XMVectorGetY(maxV),XMVectorGetZ(maxV)};
            return true;
        }
    }

    bool MascotModel::BoneBounds(std::array<float,3>& lo,std::array<float,3>& hi) const
    {
        return impl->pmx&&PosedBounds(*impl->pmx,impl->solver.get(),nullptr,lo,hi);
    }

    bool MascotModel::LegBounds(std::array<float,3>& lo,std::array<float,3>& hi) const
    {
        return impl->pmx&&PosedBounds(*impl->pmx,impl->solver.get(),&impl->legBones,lo,hi);
    }

    void MascotModel::ResetPhysics(){impl->resetPhysicsPending=true;}
    std::uint32_t MascotModel::PhysicsResets() const {return impl->physicsResets;}
    const std::wstring& MascotModel::PhysicsResetReason() const {return impl->resetReason;}

    bool MascotModel::HeadPosition(std::array<float,3>& position) const
    {
        const auto& m=*impl;
        const int head=m.proc.head;
        if(!m.solver||head<0||static_cast<size_t>(head)>=m.solver->boneMatrices.size())return false;
        const auto& origin=m.pmx->bones[head].position;
        const auto world=DirectX::XMVector3Transform(DirectX::XMLoadFloat3(&origin),DirectX::XMMatrixTranspose(m.solver->boneMatrices[head]));
        position={DirectX::XMVectorGetX(world),DirectX::XMVectorGetY(world),DirectX::XMVectorGetZ(world)};
        return true;
    }

    void MascotModel::Update(double dt)
    {
        auto& m=*impl;
        if(!m.solver)return;
        dt=std::clamp(dt,0.0,0.1);
        // Same as PoseSolver::Solve, with the procedural layer inserted before mining.
        // Physics reads solvedPose, so the layered pose also drives hair and skirt.
        // A physics reset starts from where physics last left each simulated bone.
        if(m.physics&&m.resetPhysicsPending&&m.resetBlend<0)m.BeginPhysicsReset();
        m.resetPhysicsPending=false;
        m.solver->HiPose(m.solver->solvedPose,static_cast<float>(m.frame),false,m.continuous);
        m.SampleIdle(m.solver->solvedPose,dt);
        m.SampleGait(m.solver->solvedPose,dt);
        m.motion.SetGeneratedIdle(m.idleClips.empty());
        m.motion.SetGesturesAllowed(!m.shotActive);
        m.motion.Apply(m.solver->solvedPose,dt);
        // The opened motion goes over everything the motion layer made, standing pose included.
        if(m.shotActive)m.ApplyShot(dt);   // authored in the A-stance frame, so before correction
        m.ApplyArmCorrection(m.solver->solvedPose);
        m.ApplyProcedural(m.solver->solvedPose,dt);
        if(m.resetBlend>=0)m.BlendPhysicsBones(dt);
        m.solver->PoseMining(m.solver->solvedPose);
        m.continuous=true;
        if(m.physics)
        {
            // The physics floor sits at y=0, the edge the mascot rests on. While hanging below
            // the edge, hair and skirt must be free to fall past it; while sitting on it, the
            // skirt and anything else below the hips would be squeezed against it and thrash.
            // Back on her feet, the floor returns once no simulated body is below it (turning
            // it on under one would throw it up); if one stays down, a smooth reset lifts it.
            const Activity a=m.motion.CurrentActivity();
            const bool floor=a!=Activity::Hang&&a!=Activity::Sit;
            if(!floor)
            {
                if(m.physics->GroundEnable())m.physics->GroundEnable(false);
                m.floorWait=0;
            }
            else if(!m.physics->GroundEnable())
            {
                if(m.LowestBodyY()>=-0.02f){m.physics->GroundEnable(true);m.floorWait=0;}
                else if((m.floorWait+=dt)>=1.5&&m.resetBlend<0&&!m.floorOnReset)
                {
                    m.resetPhysicsPending=true;
                    m.floorOnReset=true;
                    m.floorWait=0;
                }
            }
            if(m.resetBlend>=ResetBlendSeconds)
            {
                // The bones are back in their animated place: put the bodies there with no
                // momentum, and simulate again from this frame.
                m.physics->Reset();
                m.resetBlend=-1;
                // The bodies now sit on the animated pose, out of the way of the floor.
                if(m.floorOnReset&&floor)m.physics->GroundEnable(true);
                m.floorOnReset=false;
                m.resetFrom.clear();
                ++m.physicsResets;
            }
            // Physics stays off while the simulated bones ease back.
            if(m.resetBlend<0)
            {
                m.physics->Update(dt);
                m.WatchPhysics(dt);
            }
        }
        m.ApplyMorphs();
        m.UpdateMaterials();
    }

    const std::vector<StaticVertex>& MascotModel::StaticVertices() const {return impl->staticVertices;}
    const std::vector<std::uint32_t>& MascotModel::Indices() const {return impl->indices;}
    const std::vector<DynamicVertex>& MascotModel::DynamicVertices() const {return impl->dynamicVertices;}
    std::uint64_t MascotModel::DynamicRevision() const {return impl->dynamicRevision;}
    const std::vector<std::wstring>& MascotModel::TexturePaths() const {return impl->texturePaths;}
    const std::vector<MaterialState>& MascotModel::Materials() const {return impl->materials;}
    const void* MascotModel::BoneMatrices() const {return impl->solver?impl->solver->boneMatrices.data():nullptr;}
    size_t MascotModel::BoneCount() const {return impl->solver?impl->solver->boneMatrices.size():0;}
    std::array<float,2> MascotModel::ArmRestAngles() const {return {impl->arms[0].restAngle,impl->arms[1].restAngle};}
    std::array<float,2> MascotModel::ArmCorrections() const {return {impl->arms[0].correction,impl->arms[1].correction};}
    std::array<float,3> MascotModel::BoundsMin() const {return impl->boundsMin;}
    std::array<float,3> MascotModel::BoundsMax() const {return impl->boundsMax;}
    float MascotModel::SoleFrontZ() const {return impl->soleFrontZ;}
}
