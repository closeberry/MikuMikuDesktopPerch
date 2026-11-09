#include "BlockTower.hpp"
#include "Text.hpp"
#include <btBulletDynamicsCommon.h>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace MikuMikuDesktopPerch::BlockTower
{
    namespace
    {
        constexpr double Step=1.0/480;            // the physics' fixed step (s): with longer ones a tall tower creeps
        constexpr float Gravity=981;              // cm/s^2
        constexpr float Weight=Gravity;           // a block's weight (its mass is 1)
        constexpr float Pitch=Width+0.03f;        // between the middles of blocks side by side
        // The magic's hold on the block it moves: a spring along its length (and how far the place
        // it draws toward may run ahead), the most it pulls (as a finger would: 25 blocks' weight, some
        // 400 g, far more than a loaded block's friction, so a tight one drags its neighbours), a softer
        // hold across it and up (it floats once it is out over the edge), its turn kept.
        constexpr float PullSpring=20000,PullDamp=280,PullMost=25*Weight,PullFelt=6*Weight;
        constexpr float SideSpring=300,SideDamp=30,SideMost=0.6f*Weight;
        constexpr float LiftSpring=600,LiftDamp=40;
        constexpr float TurnSpring=300,TurnDamp=55,TurnMost=2500;
        constexpr float OutAt=Length+0.3f;        // out of the tower this far along
        constexpr float BackShare=1.0f/3;         // may go back while less than this share is out
        constexpr float FallDrop=0.55f*Thick,FallTilt=0.45f;
        constexpr double RiseTime=0.6,CrossTime=0.9,LowerTime=0.5;
        constexpr double CalmNeeded=0.4,SettleMost=4,FallenShown=8;

        btVector3 V(const std::array<float,3>& a){return {a[0],a[1],a[2]};}
        std::array<float,3> A(const btVector3& v){return {v.x(),v.y(),v.z()};}
        btQuaternion Q(const std::array<float,4>& q){return {q[0],q[1],q[2],q[3]};}
        std::array<float,4> A(const btQuaternion& q){return {q.x(),q.y(),q.z(),q.w()};}
        float Ease(float u){u=std::clamp(u,0.0f,1.0f);return u*u*(3-2*u);}
        float Smooth(float a,float b,float x){return Ease((x-a)/(b-a));}
        // A level's blocks lie along z (even levels) or x (odd).
        btQuaternion LevelTurn(int level)
        {
            return level%2==0?btQuaternion(btVector3(0,1,0),-SIMD_HALF_PI):btQuaternion(0,0,0,1);
        }
    }

    // The physics: the table (and the rim round it, and the floor) and the blocks, each a box of
    // its own size.
    struct World
    {
        btDefaultCollisionConfiguration config;
        btCollisionDispatcher dispatcher{&config};
        btDbvtBroadphase broadphase;
        btSequentialImpulseConstraintSolver solver;
        btDiscreteDynamicsWorld world{&dispatcher,&broadphase,&solver,&config};
        std::vector<std::unique_ptr<btCollisionShape>> shapes;
        std::vector<std::unique_ptr<btRigidBody>> statics;
        std::array<std::unique_ptr<btBoxShape>,Blocks> boxes;
        std::array<std::unique_ptr<btRigidBody>,Blocks> bodies;
        std::array<bool,Blocks> held{};          // moved by hand (kinematic)
        std::array<float,Blocks> loads{};        // the weight resting on each (Weights), after Loads

        World()
        {
            world.setGravity({0,-Gravity,0});
            auto& info=world.getSolverInfo();
            // (With one friction direction the blocks crept sideways on each other.)
            info.m_numIterations=40;   // (fewer: the whole tower rocked a little at every pull)
            info.m_solverMode|=SOLVER_USE_2_FRICTION_DIRECTIONS;
            info.m_splitImpulse=1;
            // Less of each contact's last push carried into the next step (0.85 by default): with
            // more, a knocked tower swayed on and on, its sway growing; with less, it crept.
            info.m_warmstartingFactor=0.7f;
            // The table: its top at y 0 (cm, the tower's middle over x 0, z 0); a raised rim round
            // it; the floor below.
            // (Sized as the window draws the table: a block is BlockShare of her upper body D long,
            // the table 2.6 D across, from 0.85 D on her side of the tower to 1.0 D on yours.)
            const float D=Length/BlockShare,rim=0.11f*D,rimTop=0.04f*D;
            AddStatic({0,-2,-0.075f*D},{1.3f*D,2,0.925f*D},0.6f);
            AddStatic({0,rimTop/2,-1.0f*D-rim/2},{1.3f*D+rim,rimTop/2,rim/2},0.6f);
            AddStatic({0,rimTop/2,0.85f*D+rim/2},{1.3f*D+rim,rimTop/2,rim/2},0.6f);
            AddStatic({-1.3f*D-rim/2,rimTop/2,-0.075f*D},{rim/2,rimTop/2,0.925f*D},0.6f);
            AddStatic({1.3f*D+rim/2,rimTop/2,-0.075f*D},{rim/2,rimTop/2,0.925f*D},0.6f);
            Floor(-1.4f*D);
        }
        ~World()
        {
            for(auto& b:bodies)if(b)world.removeRigidBody(b.get());
            for(auto& b:statics)world.removeRigidBody(b.get());
        }
        void AddStatic(const std::array<float,3>& at,const std::array<float,3>& half,float friction)
        {
            shapes.push_back(std::make_unique<btBoxShape>(V(half)));
            btRigidBody::btRigidBodyConstructionInfo info(0,nullptr,shapes.back().get());
            info.m_startWorldTransform.setIdentity();
            info.m_startWorldTransform.setOrigin(V(at));
            info.m_friction=friction;
            info.m_restitution=0.05f;
            statics.push_back(std::make_unique<btRigidBody>(info));
            statics.back()->setUserIndex(-1);
            world.addRigidBody(statics.back().get());
        }
        void Floor(float y)
        {
            shapes.push_back(std::make_unique<btStaticPlaneShape>(btVector3(0,1,0),y));
            btRigidBody::btRigidBodyConstructionInfo info(0,nullptr,shapes.back().get());
            info.m_friction=0.6f;
            statics.push_back(std::make_unique<btRigidBody>(info));
            statics.back()->setUserIndex(-1);
            world.addRigidBody(statics.back().get());
        }
        void Load(const std::array<Block,Blocks>& blocks)
        {
            for(int b=0;b<Blocks;++b)
            {
                const Block& k=blocks[static_cast<size_t>(b)];
                boxes[static_cast<size_t>(b)]=std::make_unique<btBoxShape>(btVector3(k.size[0]/2,k.size[1]/2,k.size[2]/2));
                btVector3 inertia;
                boxes[static_cast<size_t>(b)]->calculateLocalInertia(1,inertia);
                btRigidBody::btRigidBodyConstructionInfo info(1,nullptr,boxes[static_cast<size_t>(b)].get(),inertia);
                info.m_startWorldTransform=btTransform(Q(k.turn),V(k.at));
                info.m_friction=0.45f;
                info.m_restitution=0.05f;
                info.m_linearDamping=0.02f;
                info.m_angularDamping=0.05f;
                auto& body=bodies[static_cast<size_t>(b)];
                body=std::make_unique<btRigidBody>(info);
                body->setUserIndex(b);
                body->setActivationState(DISABLE_DEACTIVATION);
                world.addRigidBody(body.get());
            }
        }
        btRigidBody& Body(int b){return *bodies[static_cast<size_t>(b)];}
        // A block taken up by the magic (moved where it is put, touching nothing on its way), or let go.
        void Hold(int b,bool on)
        {
            if(held[static_cast<size_t>(b)]==on)return;
            held[static_cast<size_t>(b)]=on;
            btRigidBody& body=Body(b);
            world.removeRigidBody(&body);
            if(on)
            {
                body.setMassProps(0,{0,0,0});
                body.setCollisionFlags(body.getCollisionFlags()|btCollisionObject::CF_KINEMATIC_OBJECT|btCollisionObject::CF_NO_CONTACT_RESPONSE);
            }
            else
            {
                btVector3 inertia;
                boxes[static_cast<size_t>(b)]->calculateLocalInertia(1,inertia);
                body.setCollisionFlags(body.getCollisionFlags()&~(btCollisionObject::CF_KINEMATIC_OBJECT|btCollisionObject::CF_NO_CONTACT_RESPONSE));
                body.setMassProps(1,inertia);
                body.updateInertiaTensor();
            }
            body.setLinearVelocity({0,0,0});
            body.setAngularVelocity({0,0,0});
            body.setActivationState(DISABLE_DEACTIVATION);
            world.addRigidBody(&body);
        }
        // One step; the knocks of blocks meeting (how hard, 0-1) added to knocks.
        void Step(double h,std::vector<float>* knocks)
        {
            world.stepSimulation(static_cast<btScalar>(h),0,static_cast<btScalar>(h));
            if(!knocks)return;
            const int count=dispatcher.getNumManifolds();
            for(int i=0;i<count;++i)
            {
                const btPersistentManifold* m=dispatcher.getManifoldByIndexInternal(i);
                float most=0;
                for(int p=0;p<m->getNumContacts();++p)
                {
                    const btManifoldPoint& c=m->getContactPoint(p);
                    if(c.getLifeTime()<=1)most=std::max(most,c.getAppliedImpulse());
                }
                if(most>6)knocks->push_back(std::min(1.0f,most/60.0f));
            }
        }
        // The weight resting on each block from the ones above it (in block weights), from the
        // contacts of the last step.
        void Loads(double h)
        {
            loads.fill(0);
            const int count=dispatcher.getNumManifolds();
            for(int i=0;i<count;++i)
            {
                const btPersistentManifold* m=dispatcher.getManifoldByIndexInternal(i);
                const int a=m->getBody0()->getUserIndex(),b=m->getBody1()->getUserIndex();
                if(a<0||b<0)continue;
                for(int p=0;p<m->getNumContacts();++p)
                {
                    const btManifoldPoint& c=m->getContactPoint(p);
                    const float push=c.getAppliedImpulse()/static_cast<float>(h)/Weight;
                    // The normal points from b to a: a above b when it points up.
                    if(c.m_normalWorldOnB.y()>0.7f)loads[static_cast<size_t>(b)]+=push;
                    else if(c.m_normalWorldOnB.y()<-0.7f)loads[static_cast<size_t>(a)]+=push;
                }
            }
        }
    };

    Game::Game()=default;
    Game::~Game(){Join();}

    void Game::Join()
    {
        if(worker.joinable())worker.join();
        thinkingNow=false;
    }

    void Game::NewGame(std::uint32_t seed,int first,double howCarefully)
    {
        Join();
        random.seed(seed);
        care=std::clamp(howCarefully,0.0,1.0);
        // The blocks, each a little off the size it should be (some thicker than the rest of their
        // level, which then carry the weight above), stacked level on level.
        std::normal_distribution<float> thick(0,0.022f);
        constexpr float variance=0.006f;   // the two sides' difference
        std::uniform_real_distribution<float> any(-1,1);
        float base=0;
        for(int l=0;l<Levels;++l)
        {
            // The two sides alike (a level thicker on one side would lean the tower), a little
            // apart; the middle thicker (then it carries the level above, the sides loose) or
            // thinner (the sides carry it, the middle loose).
            float tallest=0;
            const float sides=Thick+std::clamp(thick(random),-0.04f,0.04f)*0.3f,middle=Thick+std::clamp(thick(random),-0.05f,0.05f);
            for(int s=0;s<PerLevel;++s)
            {
                Block& k=blocks[static_cast<size_t>(l*PerLevel+s)];
                const float t=s==1?middle:sides+variance*any(random);
                k.size={Length+0.02f*any(random),t,Width-0.015f-0.015f*std::abs(any(random))};
                tallest=std::max(tallest,k.size[1]);
            }
            for(int s=0;s<PerLevel;++s)
            {
                Block& k=blocks[static_cast<size_t>(l*PerLevel+s)];
                k.level=l;k.slot=s;
                const float across=(s-1)*Pitch+0.01f*any(random),along=0.08f*any(random);
                k.at={l%2==0?across:along,base+k.size[1]/2+0.001f,l%2==0?along:across};
                k.turn=A(LevelTurn(l)*btQuaternion(btVector3(0,1,0),0.004f*any(random)));
            }
            base+=tallest+0.001f;
        }
        world=std::make_unique<World>();
        world->Load(blocks);
        phase=Phase::Settle;
        active=-1;
        for(int i=0;i<static_cast<int>(1.0/Step);++i)StepOnce(Step);
        toMove=first;
        loser=-1;
        turns=0;
        StartTurn();
    }

    // The blocks as they lie now, still: a turn begins (or goes on after a block went back).
    void Game::Rest()
    {
        for(int b=0;b<Blocks;++b)
        {
            btRigidBody& body=world->Body(b);
            if(!world->held[static_cast<size_t>(b)]){body.setLinearVelocity({0,0,0});body.setAngularVelocity({0,0,0});}
            rest[static_cast<size_t>(b)]=blocks[static_cast<size_t>(b)].at;
            restTurn[static_cast<size_t>(b)]=blocks[static_cast<size_t>(b)].turn;
        }
    }

    void Game::StartTurn()
    {
        Rest();
        phase=Phase::Choose;
        phaseTime=0;
        active=-1;
        side=0;
        wanted=target=0;
        resistance=wobble=0;
        turnWobble=turnResistance=0;
        slotChosen=-1;
        laid=false;
        returning=false;
        committed=false;
        pending=-1;
        hersRunning=false;
        thoughtDone=false;
        herChoice=-1;
        stuckFor=0;
    }

    int Game::Height() const
    {
        int top=-1;
        for(const Block& k:blocks)top=std::max(top,k.level);
        return top+1;
    }

    int Game::TopCount() const
    {
        const int top=Height()-1;
        return static_cast<int>(std::count_if(blocks.begin(),blocks.end(),[&](const Block& k){return k.level==top;}));
    }

    std::wstring Game::WhyNot(int b) const
    {
        if(b<0||b>=Blocks)return L"-";
        const Block& k=blocks[static_cast<size_t>(b)];
        if(phase==Phase::Fallen)return T(L"塔は崩れました",L"The tower has fallen");
        if(k.level<0)return T(L"このブロックは動かしているところです",L"That block is on its way");
        const int top=Height()-1;
        if(k.level==top)return T(L"一番上の段からは抜けません",L"Not from the top level");
        if(k.level==top-1&&TopCount()<PerLevel)return T(L"一番上の段がそろうまで、その下の段からは抜けません",L"Not from the level under an unfinished top");
        return L"";
    }

    bool Game::Legal(int b) const
    {
        if(b<0||b>=Blocks||!WhyNot(b).empty())return false;
        if(phase==Phase::Choose)return true;
        return phase==Phase::Pull&&CanPutBack()&&b!=active;
    }

    int Game::Pick(const std::array<float,3>& from,const std::array<float,3>& to) const
    {
        btCollisionWorld::ClosestRayResultCallback hit(V(from),V(to));
        world->world.rayTest(V(from),V(to),hit);
        return hit.hasHit()&&hit.m_collisionObject?hit.m_collisionObject->getUserIndex():-1;
    }

    std::array<float,3> Game::Axis(int b) const
    {
        const btQuaternion q=Q(b==active?homeTurn:restTurn[static_cast<size_t>(b)]);
        return A(quatRotate(q,btVector3(1,0,0)));
    }

    float Game::Offset() const
    {
        if(active<0)return 0;
        const btVector3 d=V(blocks[static_cast<size_t>(active)].at)-V(home);
        return d.dot(V(Axis(active)));
    }

    bool Game::CanPutBack() const
    {
        return phase==Phase::Pull&&active>=0&&!returning&&!committed&&std::abs(Offset())<Length*BackShare;
    }

    bool Game::Choose(int b)
    {
        if(!Legal(b))return false;
        if(phase==Phase::Pull)
        {
            // Another block while this one may still go back: back it goes first.
            PutBack();
            pending=b;
            return true;
        }
        active=b;
        home=blocks[static_cast<size_t>(b)].at;
        homeTurn=blocks[static_cast<size_t>(b)].turn;
        homeLevel=blocks[static_cast<size_t>(b)].level;
        homeSlot=blocks[static_cast<size_t>(b)].slot;
        side=0;
        wanted=target=0;
        resistance=0;
        returning=false;
        committed=false;
        phase=Phase::Pull;
        phaseTime=0;
        return true;
    }

    void Game::PullTo(float offset)
    {
        if(phase!=Phase::Pull||returning)return;
        wanted=std::clamp(offset,-Length-1.0f,Length+1.0f);
        if(side==0&&std::abs(wanted)>0.05f)side=wanted>0?1:-1;
    }

    void Game::PutBack()
    {
        if(!CanPutBack())return;
        returning=true;
        wanted=0;
    }

    std::vector<int> Game::FreeSlots() const
    {
        std::vector<int> out;
        if(phase!=Phase::Place)return out;
        const int count=TopCount(),top=Height()-1;
        for(int s=0;s<PerLevel;++s)
        {
            const bool taken=count<PerLevel&&std::any_of(blocks.begin(),blocks.end(),[&](const Block& k){return k.level==top&&k.slot==s;});
            if(!taken)out.push_back(s);
        }
        return out;
    }

    bool Game::Hovering() const {return phase==Phase::Place&&slotChosen<0&&floatTime>=RiseTime;}

    bool Game::Place(int slot)
    {
        const auto free=FreeSlots();
        if(slotChosen>=0||std::find(free.begin(),free.end(),slot)==free.end())return false;
        slotChosen=slot;
        floatTime=std::max(floatTime,RiseTime);
        crossFrom=floatTime;
        return true;
    }

    Block Game::SlotBlock(int slot) const
    {
        // The level it goes on: a new one when the top is full.
        const int height=Height();
        const bool fresh=TopCount()>=PerLevel;
        const int level=fresh?height:height-1;
        Block k;
        k.level=level;
        k.slot=slot;
        k.size=active>=0?blocks[static_cast<size_t>(active)].size:std::array<float,3>{Length,Thick,Width};
        // Where the tower stands there. Across: next to the blocks already on the level (a block's
        // width and a little from its neighbour, so as not to lie on it), or on a new level from
        // the blocks two levels down (the same way). Along: from the level below's blocks, which
        // lie across this level's way (their own across places give this level's middle along).
        auto acrossAt=[](const Block& b){return b.level%2==0?b.at[0]:b.at[2];};
        auto acrossOf=[&](const Block& b){return acrossAt(b)-(b.slot-1)*Pitch;};
        float across=0,along=0;
        int nAcross=0,nAlong=0;
        const Block* side[PerLevel]{};
        for(const Block& b:blocks)
        {
            if(b.level<0)continue;
            if(!fresh&&b.level==level)side[b.slot]=&b;
            if(b.level==(fresh?level-2:level)){across+=acrossOf(b);++nAcross;}
            if(b.level==level-1){along+=acrossOf(b);++nAlong;}
        }
        if(nAcross==0)
            for(const Block& b:blocks)if(b.level==level-2){across+=acrossOf(b);++nAcross;}
        across=nAcross?across/nAcross:0;
        along=nAlong?along/nAlong:0;
        float a=across+(slot-1)*Pitch;
        if(!fresh)
        {
            // Beside a neighbour: one pitch from it (between two: halfway, or as near as fits).
            const Block* lower=slot>0?side[slot-1]:nullptr;
            const Block* upper=slot<PerLevel-1?side[slot+1]:nullptr;
            if(lower&&upper)a=(acrossAt(*lower)+acrossAt(*upper))/2;
            else if(lower)a=acrossAt(*lower)+Pitch;
            else if(upper)a=acrossAt(*upper)-Pitch;
            else if(slot!=1&&side[1])a=acrossAt(*side[1])+(slot-1)*Pitch;
            else if(slot==0&&side[2])a=acrossAt(*side[2])-2*Pitch;
            else if(slot==2&&side[0])a=acrossAt(*side[0])+2*Pitch;
            // On a level begun, along: as the blocks already there lie.
            float sum=0;
            int n=0;
            for(const Block* b:side)if(b){sum+=b->level%2==0?b->at[2]:b->at[0];++n;}
            if(n)along=sum/n;
        }
        float support=0;
        for(const Block& b:blocks)if(b.level==level-1)support=std::max(support,b.at[1]+b.size[1]/2);
        k.at={level%2==0?a:along,support+k.size[1]/2+0.015f,level%2==0?along:a};
        k.turn=A(LevelTurn(level));
        return k;
    }

    // The magic's hold on the block it moves, for the step to come.
    void Game::Drive(double h)
    {
        if(active<0||world->held[static_cast<size_t>(active)])return;
        btRigidBody& body=world->Body(active);
        const btVector3 a=V(Axis(active)),up(0,1,0),across=up.cross(a).normalized();
        const btVector3 d=body.getCenterOfMassPosition()-V(home),v=body.getLinearVelocity();
        const float s=d.dot(a),vs=v.dot(a);
        const float speed=hersRunning?herSpeed:MostSpeed;
        target+=std::clamp(wanted-target,-speed*static_cast<float>(h),speed*static_cast<float>(h));
        const float most=PullMost,lead=most/PullSpring*1.05f;
        target=std::clamp(target,s-lead,s+lead);
        const float pull=std::clamp(PullSpring*(target-s)-PullDamp*vs,-most,most);
        // How hard it pulls, eased (shown against PullFelt: a block that pulls that hard feels tight).
        const float k=static_cast<float>(1-std::exp(-h/0.15));
        resistance+=(std::min(1.0f,std::abs(pull)/PullFelt)-resistance)*k;
        turnResistance=std::max(turnResistance,resistance);
        // Across and up: held to its line, and floating, once out over the edge. While it is still
        // in, its slot guides it: drawn along its own length as it lies now, nothing holding it
        // to where it lay (with the tower leaning, that held it against its neighbours, stuck).
        const float u=Smooth(0.30f*Length,0.62f*Length,std::abs(s));
        btVector3 own=quatRotate(body.getOrientation(),btVector3(1,0,0));
        if(own.dot(a)<0)own=-own;
        const btVector3 along=(own*(1-u)+a*u).normalized();
        const float e=d.dot(across),ve=v.dot(across);
        const float hold=u*std::clamp(-SideSpring*e-SideDamp*ve,-SideMost,SideMost);
        const float lift=u*(Weight+std::clamp(-LiftSpring*d.y()-LiftDamp*v.y(),-2*Weight,2*Weight));
        // (No shaking hand added: what the tower does comes of the physics, friction and all.)
        body.applyCentralForce(along*pull+across*hold+up*lift);
        // Its turn kept as it lay.
        const btQuaternion q=body.getOrientation()*Q(homeTurn).inverse();
        btVector3 axis=q.getAxis();
        float angle=q.getAngle();
        if(angle>SIMD_PI)angle-=SIMD_2_PI;
        if(!std::isfinite(axis.x()))axis={0,1,0};
        btVector3 torque=-TurnSpring*angle*axis-TurnDamp*body.getAngularVelocity();
        if(torque.length()>TurnMost)torque*=TurnMost/torque.length();
        body.applyTorque(torque*(0.1f+0.9f*u));
        body.activate(true);
    }

    void Game::Fell(int who)
    {
        fellDuring=phase;
        phase=Phase::Fallen;
        loser=who;
        phaseTime=0;
        hersRunning=false;
        if(active>=0&&world->held[static_cast<size_t>(active)])
        {
            const btVector3 at=world->Body(active).getCenterOfMassPosition();
            world->Hold(active,false);
            world->Body(active).setCenterOfMassTransform(btTransform(world->Body(active).getOrientation(),at));
        }
    }

    // The tower down: a block (not the one being moved) has dropped by half its thickness or more,
    // or tipped over.
    void Game::CheckFall()
    {
        float most=0;
        for(int b=0;b<Blocks;++b)
        {
            if(b==active&&phase!=Phase::Settle)continue;
            const Block& k=blocks[static_cast<size_t>(b)];
            const auto& r=rest[static_cast<size_t>(b)];
            most=std::max(most,std::hypot(k.at[0]-r[0],k.at[2]-r[2]));
            const float tilt=Q(k.turn).angleShortestPath(Q(restTurn[static_cast<size_t>(b)]));
            if(k.at[1]<r[1]-FallDrop||tilt>FallTilt)
            {
                fallSeen=Fmt(L"block {0} level {1} slot {2} dropped {3} tilted {4} ({5} s into phase {6}, laid {7}, active {8})",
                             {std::to_wstring(b),std::to_wstring(k.level),std::to_wstring(k.slot),std::to_wstring(r[1]-k.at[1]),std::to_wstring(tilt),
                              std::to_wstring(phaseTime),std::to_wstring(static_cast<int>(phase)),std::to_wstring(laid),std::to_wstring(active)});
                Fell(toMove);
                return;
            }
        }
        wobble=most;
        turnWobble=std::max(turnWobble,most);
    }

    void Game::StepOnce(double h)
    {
        phaseTime+=h;
        if(phase==Phase::Choose)return;
        if(phase==Phase::Fallen&&phaseTime>FallenShown)return;
        // Hers (or one played for): the block drawn out at her pace; one that sticks while it may
        // still go back is put back, and the next tried.
        if(hersRunning&&phase==Phase::Pull&&!returning)
        {
            wanted=static_cast<float>(herSide)*(Length+1.0f);
            if(side==0)side=herSide;
            const float s=Offset();
            stuckFor=resistance>0.9f?stuckFor+h:0;
            if(stuckFor>1.0&&std::abs(s)<Length*BackShare&&herIndex+1<static_cast<int>(herOrder.size()))
            {
                PutBack();
                pending=herOrder[static_cast<size_t>(++herIndex)];
                stuckFor=0;
            }
        }
        if(phase==Phase::Pull)Drive(h);
        if(phase==Phase::Place&&active>=0)
        {
            // Out: it rises clear of the tower, waits over the top, crosses to its slot and is laid.
            floatTime+=h;
            if(hersRunning&&slotChosen<0&&floatTime>RiseTime+herWait)Place(herSlot>=0?herSlot:ChooseSlot());
            const float rise=Ease(static_cast<float>(std::min(floatTime,RiseTime)/RiseTime));
            float topY=0;
            for(const Block& k:blocks)if(k.level>=0)topY=std::max(topY,k.at[1]+k.size[1]/2);
            const float hoverY=topY+2.4f*Thick;
            btVector3 at=V(outAt);
            at.setY(outAt[1]+(std::max(hoverY,outAt[1])-outAt[1])*rise);
            btQuaternion turn=Q(outTurn);
            if(slotChosen<0&&floatTime>RiseTime)at.setY(at.y()+0.12f*std::sin(static_cast<float>(floatTime-RiseTime)*3.0f));
            if(slotChosen>=0)
            {
                const Block goal=SlotBlock(slotChosen);
                const double t=floatTime-crossFrom;
                const float cross=Ease(static_cast<float>(t/CrossTime)),lower=Ease(static_cast<float>((t-CrossTime)/LowerTime));
                const btVector3 over(goal.at[0],hoverY,goal.at[2]);
                at=at.lerp(over,cross);
                at.setY(at.y()+(goal.at[1]-hoverY)*lower);
                turn=slerp(Q(outTurn),Q(goal.turn),cross);
                if(t>=CrossTime+LowerTime)
                {
                    // Laid: let go, to rest on the top.
                    world->Hold(active,false);
                    world->Body(active).setCenterOfMassTransform(btTransform(Q(goal.turn),V(goal.at)));
                    Block& k=blocks[static_cast<size_t>(active)];
                    k.level=goal.level;
                    k.slot=goal.slot;
                    k.at=goal.at;k.turn=goal.turn;
                    rest[static_cast<size_t>(active)]=goal.at;
                    restTurn[static_cast<size_t>(active)]=goal.turn;
                    laid=true;
                    phase=Phase::Settle;
                    phaseTime=0;
                    calmFor=0;
                }
            }
            if(phase==Phase::Place)
            {
                btRigidBody& body=world->Body(active);
                const btTransform place(turn,at);
                body.setWorldTransform(place);
                body.setInterpolationWorldTransform(place);
            }
        }
        world->Step(h,&knocks);
        for(int b=0;b<Blocks;++b)
        {
            const btRigidBody& body=world->Body(b);
            blocks[static_cast<size_t>(b)].at=A(body.getCenterOfMassPosition());
            blocks[static_cast<size_t>(b)].turn=A(body.getOrientation());
        }
        if(knocks.size()>64)knocks.erase(knocks.begin(),knocks.end()-64);
        if(phase==Phase::Fallen)return;
        if(phase==Phase::Pull&&active>=0)
        {
            const float s=Offset();
            if(returning&&std::abs(s)<0.08f&&std::abs(world->Body(active).getLinearVelocity().length())<0.6f)
            {
                // Back where it lay: the turn goes on from the choice, or with the next one asked for.
                const int next=pending;
                blocks[static_cast<size_t>(active)].level=homeLevel;
                blocks[static_cast<size_t>(active)].slot=homeSlot;
                active=-1;
                returning=false;
                pending=-1;
                phase=Phase::Choose;
                Rest();
                if(next>=0)Choose(next);
                return;
            }
            if(!returning&&std::abs(s)>=Length*BackShare)committed=true;   // (past a third: it can't go back, even pushed in again)
            if(!returning&&std::abs(s)>=OutAt)
            {
                // Out: the magic takes it up.
                blocks[static_cast<size_t>(active)].level=-1;
                outAt=blocks[static_cast<size_t>(active)].at;
                outTurn=blocks[static_cast<size_t>(active)].turn;
                world->Hold(active,true);
                phase=Phase::Place;
                phaseTime=0;
                floatTime=0;
                resistance=0;
            }
        }
        CheckFall();
        if(phase==Phase::Settle)
        {
            float fastest=0,spin=0;
            int which=-1;
            float worst=0;
            for(int b=0;b<Blocks;++b)
            {
                const float v=world->Body(b).getLinearVelocity().length(),w=world->Body(b).getAngularVelocity().length();
                if(v+4*w>worst){worst=v+4*w;which=b;}
                fastest=std::max(fastest,v);
                spin=std::max(spin,w);
            }
            calmFor=fastest<0.6f&&spin<0.12f?calmFor+h:0;
            // Still a little astir after a while: eased to rest (as friction would hold it) rather
            // than stopped dead when the time is up.
            if(phaseTime>2.0&&fastest<2.0f&&spin<0.2f)
            {
                const float keep=static_cast<float>(std::exp(-h/0.25));
                for(int b=0;b<Blocks;++b)
                {
                    btRigidBody& body=world->Body(b);
                    if(world->held[static_cast<size_t>(b)])continue;
                    body.setLinearVelocity(body.getLinearVelocity()*keep);
                    body.setAngularVelocity(body.getAngularVelocity()*keep);
                }
            }
            if(laid&&(calmFor>=CalmNeeded||phaseTime>=SettleMost))
            {
                if(calmFor<CalmNeeded){++settleTimeouts;settleNote=Fmt(L"block {0} level {1}: {2} cm/s, {3} rad/s",{std::to_wstring(which),std::to_wstring(which>=0?blocks[static_cast<size_t>(which)].level:-1),std::to_wstring(fastest),std::to_wstring(spin)});}
                settleSeconds+=phaseTime;
                ++turns;
                toMove=1-toMove;
                StartTurn();
            }
            else if(!laid&&(calmFor>=CalmNeeded||phaseTime>=SettleMost))StartTurn();
        }
    }

    void Game::Advance(double seconds)
    {
        accumulated=std::min(accumulated+seconds,0.25);
        while(accumulated>=Step)
        {
            accumulated-=Step;
            StepOnce(Step);
        }
    }

    std::vector<float> Game::TakeKnocks()
    {
        std::vector<float> out;
        out.swap(knocks);
        return out;
    }

    // Her slot: the middle when it is free (the steadiest), else either side.
    int Game::ChooseSlot() const
    {
        const auto free=FreeSlots();
        if(free.empty())return 1;
        if(std::find(free.begin(),free.end(),1)!=free.end())return 1;
        return free[static_cast<size_t>(random()%free.size())];
    }

    // Her choice: the blocks that carry least weight (as a hand trying them would feel), the most
    // promising of them pulled out in her head, the one that shook the tower least chosen.
    namespace
    {
        // What taking a block leaves of its level, as she sees it: one side block alone (the level
        // above then rocks on it and falls in a turn or two) is to be shunned; the middle alone
        // holds, a little less surely.
        double Leaves(const std::array<Block,Blocks>& blocks,int taken)
        {
            const int level=blocks[static_cast<size_t>(taken)].level;
            int left=0,slot=-1;
            for(int b=0;b<Blocks;++b)
                if(b!=taken&&blocks[static_cast<size_t>(b)].level==level){++left;slot=blocks[static_cast<size_t>(b)].slot;}
            if(left==1)return slot==1?3:500;
            return 0;
        }
    }

    std::vector<int> Game::Judge(const std::array<Block,Blocks>& blocks,const std::vector<int>& legal,int toMove,
                                  double care,std::uint32_t seed,float speed,std::vector<Trial>* report)
    {
        std::mt19937 random(seed);
        std::vector<Trial> trials;
        {
            World w;
            w.Load(blocks);
            for(int i=0;i<24;++i)w.Step(Step,nullptr);
            std::array<float,Blocks> load{};
            for(int i=0;i<6;++i){w.Step(Step,nullptr);w.Loads(Step);for(int b=0;b<Blocks;++b)load[static_cast<size_t>(b)]+=w.loads[static_cast<size_t>(b)]/6;}
            for(int b:legal)trials.push_back({b,1,false,false,0,0,0,load[static_cast<size_t>(b)]});
        }
        std::uniform_real_distribution<float> any(0,1);
        // Her feel for the weight on each is a little off, more so with less care.
        for(auto& t:trials)t.load*=1+(0.15f+0.5f*static_cast<float>(1-care))*(any(random)-0.5f);
        std::sort(trials.begin(),trials.end(),[&](const Trial& a,const Trial& b){return a.load+Leaves(blocks,a.block)<b.load+Leaves(blocks,b.block);});
        const size_t tried=std::min<size_t>(trials.size(),4);
        for(size_t i=0;i<tried;++i)
        {
            Trial& t=trials[i];
            const Block& k=blocks[static_cast<size_t>(t.block)];
            t.side=k.level%2==0?-1:(any(random)<0.5f?1:-1);   // along z: toward you, where you see it come
            Game g;
            g.world=std::make_unique<World>();
            g.blocks=blocks;
            g.world->Load(blocks);
            g.toMove=toMove;
            g.random.seed(seed+static_cast<std::uint32_t>(i)*7919u);
            g.phase=Phase::Settle;
            for(int s=0;s<12;++s)g.StepOnce(Step);
            g.StartTurn();
            g.herOrder={t.block};
            g.herIndex=0;
            g.herSide=t.side;
            g.herSpeed=speed;
            g.Choose(t.block);
            g.hersRunning=true;
            int steps=0;
            while(g.phase==Phase::Pull&&steps<static_cast<int>(6.0/Step)){g.StepOnce(Step);++steps;}
            t.out=g.phase==Phase::Place;
            // (and a moment after, as the tower settles without it)
            for(int s=0;s<static_cast<int>(1.0/Step)&&g.phase==Phase::Place;++s)g.StepOnce(Step);
            t.fell=g.phase==Phase::Fallen;
            t.wobble=g.turnWobble;
            t.resistance=g.turnResistance;
            t.seconds=static_cast<float>(steps*Step);
            t.score=(t.fell?1000:0)+(t.out?0:300)+t.wobble*30+t.resistance*2+t.seconds*0.1+Leaves(blocks,t.block);
        }
        std::sort(trials.begin(),trials.begin()+static_cast<std::ptrdiff_t>(tried),[](const Trial& a,const Trial& b){return a.score<b.score;});
        // Now and then, with less care, the next one down.
        if(tried>1&&any(random)<(1-care)*0.6&&trials[1].score<trials[0].score+200)std::swap(trials[0],trials[1]);
        std::vector<int> order;
        for(const Trial& t:trials)order.push_back(t.block*2+(t.side>0?1:0));
        if(report)*report=trials;
        return order;
    }

    void Game::Think()
    {
        if(thinkingNow||phase!=Phase::Choose)return;
        Join();
        thoughtDone=false;
        thinkingNow=true;
        std::vector<int> legal;
        for(int b=0;b<Blocks;++b)if(Legal(b))legal.push_back(b);
        std::uniform_real_distribution<float> any(0,1);
        herSpeed=2.6f+0.8f*any(random)+static_cast<float>(1-care)*2.5f;
        const std::uint32_t seed=static_cast<std::uint32_t>(random());
        const auto snapshot=blocks;
        const int who=toMove;
        const double c=care;
        const float speed=herSpeed;
        worker=std::thread([this,snapshot,legal,who,c,seed,speed]{
            const auto order=Judge(snapshot,legal,who,c,seed,speed,nullptr);
            herOrder.clear();
            herSides.clear();
            for(int o:order){herOrder.push_back(o/2);herSides.push_back(o%2?1:-1);}
            thoughtDone=true;
        });
    }

    void Game::BeginHers()
    {
        if(!thoughtDone||phase!=Phase::Choose||herOrder.empty())return;
        Join();
        herIndex=0;
        herChoice=herOrder[0];
        herSide=herSides.empty()?1:herSides[0];
        herSlot=-1;
        std::uniform_real_distribution<double> any(0,1);
        herWait=0.3+0.6*any(random);
        if(Choose(herChoice))hersRunning=true;
    }

    void Game::PlayTurn(bool wild)
    {
        if(phase!=Phase::Choose)return;
        if(wild)
        {
            std::vector<int> legal;
            for(int b=0;b<Blocks;++b)if(Legal(b))legal.push_back(b);
            if(legal.empty())return;
            herOrder={legal[static_cast<size_t>(random()%legal.size())]};
            herSides={random()%2?1:-1};
            herSpeed=MostSpeed;
            thoughtDone=true;
        }
        else
        {
            Think();
            Join();
        }
        BeginHers();
        int steps=0;
        const int first=turns;
        while(phase!=Phase::Fallen&&turns==first&&steps<static_cast<int>(40.0/Step)){StepOnce(Step);++steps;}
    }

    void Game::Shove()
    {
        if(phase==Phase::Fallen)return;
        const int height=Height();
        for(int b=0;b<Blocks;++b)
            if(blocks[static_cast<size_t>(b)].level>=height-5)world->Body(b).applyCentralImpulse({40,0,10});
        phase=Phase::Settle;
        laid=true;
        phaseTime=0;
        calmFor=0;
    }

    std::wstring Game::Report() const
    {
        return Fmt(L"phase {0}, to move {1}, turns {2}, height {3}, top {4}, active {5}, offset {6}, resistance {7}, wobble {8}, loser {9}",
                   {std::to_wstring(static_cast<int>(phase)),std::to_wstring(toMove),std::to_wstring(turns),std::to_wstring(Height()),std::to_wstring(TopCount()),
                    std::to_wstring(active),std::to_wstring(Offset()),std::to_wstring(resistance),std::to_wstring(wobble),std::to_wstring(loser)});
    }

    // ---- Checks ----

    std::wstring CheckRules()
    {
        std::wstring out;
        int failed=0;
        auto check=[&](bool ok,const std::wstring& what){if(!ok)++failed;out+=(ok?L"ok   ":L"FAIL ")+what+L"\n";};
        auto ms=[](auto from){return std::to_wstring(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-from).count());};
        {
            Game g;
            g.NewGame(1,You);
            const auto before=g.blocks;
            check(g.Height()==Levels&&g.TopCount()==3&&g.Now()==Phase::Choose,L"a new tower: 18 levels, the top full, a block to choose");
            // Left alone 10 seconds (as if every step were a pull), nothing moves.
            g.phase=Phase::Settle;
            g.laid=false;
            const auto from=std::chrono::steady_clock::now();
            float most=0;
            for(int i=0;i<static_cast<int>(10/Step);++i)
            {
                g.StepOnce(Step);
                if(g.phase!=Phase::Settle)g.phase=Phase::Settle,g.phaseTime=0;
            }
            for(int b=0;b<Blocks;++b)most=std::max(most,(V(g.blocks[static_cast<size_t>(b)].at)-V(before[static_cast<size_t>(b)].at)).length());
            check(most<0.15f&&g.Now()!=Phase::Fallen,Fmt(L"left alone 10 s, the most a block moved: {0} cm ({1} ms)",{std::to_wstring(most),ms(from)}));
            g.StartTurn();
            check(!g.Legal(17*3+1)&&g.Legal(16*3+1)&&g.Legal(0),L"not from the top level; from the one under it and the bottom");
            // Her judgment on the tower.
            std::vector<Game::Trial> trials;
            std::vector<int> legal;
            for(int b=0;b<Blocks;++b)if(g.Legal(b))legal.push_back(b);
            const auto judged=std::chrono::steady_clock::now();
            Game::Judge(g.blocks,legal,You,1,5,3,&trials);
            out+=L"  her judgment ("+ms(judged)+L" ms):\n";
            for(const Game::Trial& t:trials)
                if(t.score!=0||t.fell||t.out)
                    out+=Fmt(L"    block {0} (level {1}) load {2}: {3} wobble {4} resistance {5} in {6} s, score {7}\n",
                             {std::to_wstring(t.block),std::to_wstring(t.block/3),std::to_wstring(t.load),t.fell?L"fell":t.out?L"out":L"stuck",
                              std::to_wstring(t.wobble),std::to_wstring(t.resistance),std::to_wstring(t.seconds),std::to_wstring(t.score)});
            int loose=0,tight=0;
            for(const Game::Trial& t:trials){if(t.load<0.3f)++loose;if(t.load>3)++tight;}
            out+=Fmt(L"  {0} legal blocks: {1} carry under 0.3 weights, {2} over 3\n",{std::to_wstring(trials.size()),std::to_wstring(loose),std::to_wstring(tight)});
            check(!trials.empty()&&trials[0].out&&!trials[0].fell,L"her best block comes out without the tower falling");
        }
        {
            // A pull out and a block laid on the top.
            Game g;
            g.NewGame(2,You);
            std::vector<int> legal;
            for(int b=0;b<Blocks;++b)if(g.Legal(b))legal.push_back(b);
            std::vector<Game::Trial> trials;
            Game::Judge(g.blocks,legal,You,1,9,3,&trials);
            const int b=trials[0].block;
            check(g.Choose(b)&&g.Now()==Phase::Pull,L"a block chosen: being pulled");
            g.PullTo(1.0f);
            for(int i=0;i<static_cast<int>(1.0/Step);++i)g.StepOnce(Step);
            check(std::abs(g.Offset()-1.0f)<0.3f&&g.CanPutBack(),Fmt(L"pulled to 1 cm: {0}, may go back",{std::to_wstring(g.Offset())}));
            g.PutBack();
            for(int i=0;i<static_cast<int>(2.0/Step)&&g.Now()==Phase::Pull;++i)g.StepOnce(Step);
            check(g.Now()==Phase::Choose&&g.ToMove()==You&&g.Active()<0,L"put back: the same turn, a block to choose");
            g.Choose(b);
            g.PullTo(Length+1);
            int steps=0;
            const auto start=g.blocks;
            std::array<float,Blocks> moved{};
            while(g.Now()==Phase::Pull&&steps<static_cast<int>(6/Step))
            {
                g.StepOnce(Step);++steps;
                for(int k=0;k<Blocks;++k)if(k!=b)moved[static_cast<size_t>(k)]=std::max(moved[static_cast<size_t>(k)],(V(g.blocks[static_cast<size_t>(k)].at)-V(start[static_cast<size_t>(k)].at)).length());
            }
            const float rest=*std::max_element(moved.begin(),moved.end());
            out+=Fmt(L"  block {0} (level {1}, load {2}) pulled: the rest moved {3} cm at most\n",{std::to_wstring(b),std::to_wstring(b/3),std::to_wstring(trials[0].load),std::to_wstring(rest)});
            check(g.Now()==Phase::Place,Fmt(L"pulled out in {0} s (resistance at most {1}, wobble {2})",{std::to_wstring(steps*Step),std::to_wstring(g.TurnResistance()),std::to_wstring(g.TurnWobble())}));
            const auto free=g.FreeSlots();
            check(free.size()==3,L"over a full top: three slots of a new level");
            for(int i=0;i<static_cast<int>(1.0/Step);++i)g.StepOnce(Step);
            check(g.Hovering()&&g.Place(0)&&!g.Place(1),L"waiting over the top; slot 0 chosen, no other after");
            steps=0;
            while(g.Now()!=Phase::Choose&&g.Now()!=Phase::Fallen&&steps<static_cast<int>(8/Step)){g.StepOnce(Step);++steps;}
            check(g.Now()==Phase::Choose&&g.ToMove()==Her&&g.Turns()==1&&g.Height()==Levels+1&&g.TopCount()==1,
                  Fmt(L"laid on a new level, her turn ({0})",{g.Report()}));
            const int top=Levels;
            int onTop=-1;
            for(int k=0;k<Blocks;++k)if(g.BlockAt(k).level==top)onTop=k;
            check(onTop==b&&!g.Legal(b)&&!g.Legal(17*3)&&g.Legal(16*3+1),L"now not from the top two levels (the top unfinished)");
            check(std::abs(g.BlockAt(b).at[1]-(Levels*Thick+Thick/2))<0.25f,Fmt(L"it lies on the top: y {0}",{std::to_wstring(g.BlockAt(b).at[1])}));
        }
        {
            // The tower shifted aside after a block was chosen (as it sways): the block still comes out.
            Game g;
            g.NewGame(2,You);
            std::vector<int> legal;
            for(int b=0;b<Blocks;++b)if(g.Legal(b))legal.push_back(b);
            std::vector<Game::Trial> trials;
            Game::Judge(g.blocks,legal,You,1,9,3,&trials);
            int b=-1;
            for(const auto& t:trials)if(t.block/3>=4){b=t.block;break;}
            g.Choose(b);
            const int level=b/3;
            const btVector3 aside=V(g.Axis(b)).cross(btVector3(0,1,0)).normalized()*0.3f;
            for(int k=0;k<Blocks;++k)
                if(k/3>=level-1)
                {
                    btRigidBody& body=g.world->Body(k);
                    body.setCenterOfMassTransform(btTransform(body.getOrientation(),body.getCenterOfMassPosition()+aside));
                }
            for(int k=0;k<Blocks;++k)if(k/3>=level-1)g.rest[static_cast<size_t>(k)]=A(V(g.rest[static_cast<size_t>(k)])+aside);
            g.PullTo(Length+1);
            int steps=0;
            while(g.Now()==Phase::Pull&&steps<static_cast<int>(6/Step)){g.StepOnce(Step);++steps;}
            check(g.Now()==Phase::Place,Fmt(L"the tower moved 3 mm aside under a chosen block: it still comes out ({0} s, offset {1})",{std::to_wstring(steps*Step),std::to_wstring(g.Offset())}));
        }
        {
            // A tight block (the most weight on it, low in the tower) pulled anyway: it moves,
            // dragging what touches it, rather than staying put.
            Game g;
            g.NewGame(2,You);
            std::vector<int> legal;
            for(int b=0;b<Blocks;++b)if(g.Legal(b))legal.push_back(b);
            std::vector<Game::Trial> trials;
            Game::Judge(g.blocks,legal,You,1,9,3,&trials);
            int b=-1;
            float most=-1;
            for(const auto& t:trials)if(t.block/3>=2&&t.block/3<=8&&t.load>most){most=t.load;b=t.block;}
            const auto before=g.blocks;
            g.Choose(b);
            g.PullTo(Length+1);
            int steps=0;
            float touching=0,rest=0,moved=0;
            while(g.Now()==Phase::Pull&&steps<static_cast<int>(2/Step)){g.StepOnce(Step);++steps;}
            moved=std::abs(g.Offset());
            for(int k=0;k<Blocks;++k)
            {
                if(k==b)continue;
                const float d=(V(g.blocks[static_cast<size_t>(k)].at)-V(before[static_cast<size_t>(k)].at)).length();
                (std::abs(k/3-b/3)<=1?touching:rest)=std::max(std::abs(k/3-b/3)<=1?touching:rest,d);
            }
            check(moved>1,Fmt(L"the tightest block (level {0}, {1} weights on it) pulled 2 s: it moved {2} cm (resistance {3}), the levels touching it {4} cm, the rest {5} cm, {6}",
                              {std::to_wstring(b/3),std::to_wstring(most),std::to_wstring(moved),std::to_wstring(g.TurnResistance()),std::to_wstring(touching),std::to_wstring(rest),
                               g.Now()==Phase::Fallen?L"the tower fell":L"the tower stands"}));
        }
        {
            // A light knock at the top: the sway dies away and the tower comes to rest.
            Game g;
            g.NewGame(3,You);
            g.phase=Phase::Settle;
            g.laid=false;
            g.phaseTime=0;
            for(int k=0;k<Blocks;++k)if(k/3>=15)g.world->Body(k).applyCentralImpulse({1.5f,0,0});
            int steps=0;
            while(g.Now()==Phase::Settle&&steps<static_cast<int>(4/Step)){g.StepOnce(Step);++steps;}
            check(g.Now()==Phase::Choose&&steps*Step<2.5,Fmt(L"knocked at the top: at rest again in {0} s",{std::to_wstring(steps*Step)}));
        }
        {
            // A level taken away from under the tower: it falls, and the one to move loses.
            Game g;
            g.NewGame(3,Her);
            g.phase=Phase::Settle;
            for(int s=0;s<3;++s)
            {
                btRigidBody& body=g.world->Body(5*3+s);
                body.setCenterOfMassTransform(btTransform(body.getOrientation(),btVector3(-40+s*8.0f,1,-30)));
            }
            g.Rest();
            for(int s=0;s<3;++s)g.rest[static_cast<size_t>(5*3+s)][1]=0;
            g.laid=true;
            int steps=0;
            while(g.Now()!=Phase::Fallen&&steps<static_cast<int>(3/Step)){g.StepOnce(Step);++steps;}
            check(g.Now()==Phase::Fallen&&g.Loser()==Her,Fmt(L"a level taken away: the tower falls ({0} s), the one to move loses",{std::to_wstring(steps*Step)}));
        }
        {
            // Only one side block left in a level: it tips.
            Game g;
            g.NewGame(4,You);
            g.phase=Phase::Settle;
            for(int s:{1,2})
            {
                btRigidBody& body=g.world->Body(8*3+s);
                body.setCenterOfMassTransform(btTransform(body.getOrientation(),btVector3(-40+s*8.0f,1,-30)));
            }
            g.Rest();
            for(int s:{1,2})g.rest[static_cast<size_t>(8*3+s)][1]=0;
            int steps=0;
            while(g.Now()!=Phase::Fallen&&steps<static_cast<int>(4/Step)){g.StepOnce(Step);++steps;}
            check(g.Now()==Phase::Fallen&&g.Loser()==You,L"one side block left in a level: the tower falls");
        }
        {
            // Turns played with her judgment.
            Game g;
            g.NewGame(6,You);
            const auto from=std::chrono::steady_clock::now();
            for(int i=0;i<6&&g.Now()!=Phase::Fallen;++i)g.PlayTurn();
            check(g.Turns()==6&&g.Now()==Phase::Choose,Fmt(L"six turns of her judgment ({0} ms): {1}",{ms(from),g.Report()}));
        }
        return std::to_wstring(failed)+L" failed\n"+out;
    }

    std::wstring CheckPlaying(int count,std::uint32_t seed)
    {
        std::wstring out;
        int wrong=0;
        // Her against herself, and against one pulling any block quickly.
        for(int style=0;style<2;++style)
        {
            int lost[2]{},turnsAll=0,most=0,least=1000,during[5]{};
            for(int n=0;n<count;++n)
            {
                Game g;
                g.NewGame(seed+static_cast<std::uint32_t>(n)*101u,n%2);
                while(g.Now()!=Phase::Fallen&&g.Turns()<200)
                {
                    const int before=g.Turns();
                    g.PlayTurn(style==1&&g.ToMove()==You);
                    if(g.Now()!=Phase::Fallen&&g.Turns()==before){++wrong;break;}
                }
                out+=Fmt(L"  settled by the clock {0} of {1} turns (settling {2} s on average) {3}\n",{std::to_wstring(g.settleTimeouts),std::to_wstring(g.Turns()),std::to_wstring(g.settleSeconds/std::max(1,g.Turns())),g.settleNote});
                if(g.Now()==Phase::Fallen){++lost[g.Loser()];++during[static_cast<int>(g.FellDuring())];out+=L"  "+g.fallSeen+L" at height "+std::to_wstring(g.Height())+L" top "+std::to_wstring(g.TopCount())+L"\n";}
                turnsAll+=g.Turns();
                most=std::max(most,g.Turns());
                least=std::min(least,g.Turns());
            }
            out+=Fmt(L"{0}: her side lost {1}, the other {2}; turns {3} on average ({4}-{5}); fell while pulling {6}, laying {7}, settling {8}\n",
                     {style==0?L"her against herself":L"her against any block quickly",std::to_wstring(lost[Her]),std::to_wstring(lost[You]),
                      std::to_wstring(turnsAll/std::max(1,count)),std::to_wstring(least),std::to_wstring(most),
                      std::to_wstring(during[static_cast<int>(Phase::Pull)]),std::to_wstring(during[static_cast<int>(Phase::Place)]),std::to_wstring(during[static_cast<int>(Phase::Settle)])});
        }
        return std::to_wstring(wrong)+L" wrong\n"+out;
    }
}
