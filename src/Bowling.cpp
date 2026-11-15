#include "Bowling.hpp"
#include <btBulletDynamicsCommon.h>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace MikuMikuDesktopPerch::Bowling
{
    namespace
    {
        constexpr float Gravity=9.81f;
        constexpr float DeckFrom=PinDeckY-0.62f;   // the ball handed to the rigid bodies here
        constexpr double DeckStep=1.0/600;             // the rigid bodies' step (s)
        constexpr int BallIndex=100,WallIndex=-2,DeckIndex=-1;

        btVector3 V(const Vec3& a){return {a.x,a.y,a.z};}
        Vec3 A(const btVector3& v){return {v.x(),v.y(),v.z()};}
        btQuaternion Q(const Quat& q){return {q.x,q.y,q.z,q.w};}
        Quat A(const btQuaternion& q){return {q.x(),q.y(),q.z(),q.w()};}

        // The lane's grip: little on the oil, more beyond it (the ball skids, then hooks, then rolls).
        float Grip(float y)
        {
            const float u=std::clamp((y-OilLength+0.6f)/1.2f,0.0f,1.0f);
            return 0.035f+(0.17f-0.035f)*u*u*(3-2*u);
        }

        Quat Mul(const Quat& a,const Quat& b)
        {
            return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
        }
        // q turned on by angular velocity w (world) for h seconds.
        Quat Spin(const Quat& q,const Vec3& w,float h)
        {
            const float speed=std::sqrt(w.x*w.x+w.y*w.y+w.z*w.z);
            if(speed*h<1e-7f)return q;
            const float half=speed*h/2,s=std::sin(half)/speed;
            Quat r=Mul({w.x*s,w.y*s,w.z*s,std::cos(half)},q);
            const float n=std::sqrt(r.x*r.x+r.y*r.y+r.z*r.z+r.w*r.w);
            return {r.x/n,r.y/n,r.z/n,r.w/n};
        }

        // The ball on the lane: one step of h seconds, sliding against the lane's grip until it
        // rolls (a solid ball: I = 2/5 m r^2). gutter: it rolls straight in the gutter.
        struct LaneBall
        {
            Vec3 p{},v{},w{};
            Quat q{};
            bool gutter=false,stopped=false;
            void Start(const Throw& t)
            {
                p={t.x,0,BallRadius};
                v={t.speed*std::sin(t.angle),t.speed*std::cos(t.angle),0};
                // Turning as it leaves the hand: forward roll and side roll (its axis turned
                // toward where it goes by the spin), more turns with more spin.
                const float spin=std::clamp(t.spin,-1.0f,1.0f);
                const float revs=11.0f+22.0f*std::abs(spin),axis=1.20f*spin;
                // Along and across the way it goes.
                const float ax=std::sin(t.angle),ay=std::cos(t.angle);
                // The roll's axis lies across the way (to its left, -x for straight down the lane);
                // turned toward the way it goes for a hook.
                const float rx=-ay,ry=ax;   // across (to the left of the way)
                w={revs*(std::cos(axis)*rx-std::sin(axis)*ax),revs*(std::cos(axis)*ry-std::sin(axis)*ay),0};
                q={};
                gutter=stopped=false;
            }
            void StepOn(float h)
            {
                if(stopped)return;
                const float r=BallRadius;
                if(gutter)
                {
                    // Rolling along the gutter's bottom.
                    v.x=0;
                    v.y=std::max(0.0f,v.y-0.10f*Gravity*h);
                    w={-v.y/r,0,0};
                }
                else
                {
                    const float sx=v.x-r*w.y,sy=v.y+r*w.x,slip=std::hypot(sx,sy);
                    const float most=Grip(p.y)*Gravity*h;
                    float jx=0,jy=0;
                    if(slip<=3.5f*most){jx=-sx/3.5f;jy=-sy/3.5f;}    // rolling from here
                    else{jx=-most*sx/slip;jy=-most*sy/slip;}
                    v.x+=jx;v.y+=jy;
                    w.x+=2.5f*jy/r;w.y-=2.5f*jx/r;
                    // A little lost rolling, too.
                    const float speed=std::hypot(v.x,v.y);
                    if(speed>1e-4f)
                    {
                        const float drag=std::min(speed,0.008f*Gravity*h)/speed;
                        v.x-=v.x*drag;v.y-=v.y*drag;w.x-=w.x*drag;w.y-=w.y*drag;
                    }
                }
                p.x+=v.x*h;p.y+=v.y*h;
                q=Spin(q,w,h);
                if(!gutter&&std::abs(p.x)>LaneHalf+0.015f)
                {
                    // Over the edge: down into the gutter.
                    gutter=true;
                    p.x=(p.x>0?1.0f:-1.0f)*(LaneHalf+GutterWidth/2);
                    p.z=BallRadius-GutterDepth;
                }
                if(std::hypot(v.x,v.y)<0.05f)stopped=true;
            }
        };

        // The pin's shape: its radius up its height (metres from its base).
        constexpr std::array<std::array<float,2>,17> Profile{{
            {0.000f,0.0258f},{0.012f,0.0360f},{0.040f,0.0490f},{0.080f,0.0582f},{0.114f,0.0605f},{0.140f,0.0598f},
            {0.180f,0.0505f},{0.220f,0.0360f},{0.250f,0.0270f},{0.268f,0.0236f},{0.290f,0.0244f},{0.318f,0.0290f},
            {0.342f,0.0305f},{0.358f,0.0284f},{0.370f,0.0228f},{0.378f,0.0130f},{0.381f,0.0f}}};
    }

    Vec3 PinSpot(int n)
    {
        // Row by row from the head pin: 1; 2 3; 4 5 6; 7 8 9 10 (the 7 at the left).
        static const int rows[10]={0,1,1,2,2,2,3,3,3,3};
        static const int places[10]={0,0,1,0,1,2,0,1,2,3};
        const int row=rows[n],place=places[n];
        const float dy=PinSpacing*0.8660254f;
        return {(place-row/2.0f)*PinSpacing,PinDeckY+row*dy,0};
    }

    bool Predict(const Throw& t,float y,float& x,float* entry)
    {
        LaneBall b;
        b.Start(t);
        constexpr float h=1/240.0f;
        for(int i=0;i<240*12&&b.p.y<y;++i)
        {
            b.StepOn(h);
            if(b.gutter){x=b.p.x;return false;}
            if(b.stopped){x=b.p.x;return false;}
        }
        x=b.p.x;
        if(entry)*entry=std::atan2(b.v.x,b.v.y);
        return true;
    }

    std::vector<Vec3> Path(const Throw& t,float step)
    {
        LaneBall b;
        b.Start(t);
        std::vector<Vec3> out{b.p};
        constexpr float h=1/240.0f;
        for(int i=0;i<240*12&&b.p.y<PinDeckY&&!b.stopped;++i)
        {
            b.StepOn(h);
            if(b.p.y>=out.back().y+step||b.gutter)out.push_back(b.p);
            if(b.gutter)break;
        }
        return out;
    }

    // ---- The rigid bodies on the deck ----------------------------------------------------

    struct Roll::World
    {
        btDefaultCollisionConfiguration config;
        btCollisionDispatcher dispatcher{&config};
        btDbvtBroadphase broadphase;
        btSequentialImpulseConstraintSolver solver;
        btDiscreteDynamicsWorld world{&dispatcher,&broadphase,&solver,&config};
        std::vector<std::unique_ptr<btCollisionShape>> shapes;
        std::vector<std::unique_ptr<btRigidBody>> statics;
        std::unique_ptr<btConvexHullShape> pinShape;
        std::unique_ptr<btSphereShape> ballShape;
        std::array<std::unique_ptr<btRigidBody>,10> pins;
        std::unique_ptr<btRigidBody> ball;

        World()
        {
            world.setGravity({0,0,-Gravity});
            auto& info=world.getSolverInfo();
            info.m_numIterations=20;
            info.m_solverMode|=SOLVER_USE_2_FRICTION_DIRECTIONS;
            info.m_splitImpulse=1;
            const float back=DeckEnd+0.95f,from=DeckFrom-1.0f;
            // The deck (and the lane before it), the gutters each side, the pit behind.
            Box({0,(from+DeckEnd)/2,-0.1f},{LaneHalf,(DeckEnd-from)/2,0.1f},0.20f,0.40f,DeckIndex);
            for(float s:{-1.0f,1.0f})
            {
                Box({s*(LaneHalf+GutterWidth/2),(from+DeckEnd)/2,-GutterDepth-0.1f},{GutterWidth/2,(DeckEnd-from)/2,0.1f},0.25f,0.3f,DeckIndex);
                // The kickbacks (side walls) by the deck and the pit.
                Box({s*(LaneHalf+GutterWidth+0.05f),(DeckFrom+back)/2,0.25f},{0.05f,(back-DeckFrom)/2,0.32f},0.30f,0.55f,WallIndex);
            }
            Box({0,(DeckEnd+back)/2,-0.45f},{LaneHalf+GutterWidth,(back-DeckEnd)/2,0.05f},0.6f,0.1f,WallIndex);   // the pit's floor
            Box({0,back+0.05f,0.10f},{LaneHalf+GutterWidth+0.1f,0.05f,0.60f},0.4f,0.15f,WallIndex);              // the cushion behind
            // The pin: the shape of its profile turned round, its middle of mass at its origin.
            pinShape=std::make_unique<btConvexHullShape>();
            constexpr int around=14;
            for(const auto& pr:Profile)
                for(int i=0;i<around;++i)
                {
                    const float a=6.2831853f*i/around;
                    pinShape->addPoint({pr[1]*std::cos(a),pr[1]*std::sin(a),pr[0]-PinCentre},false);
                }
            pinShape->recalcLocalAabb();
            pinShape->setMargin(0.002f);
            ballShape=std::make_unique<btSphereShape>(BallRadius);
        }
        ~World()
        {
            for(auto& b:pins)if(b)world.removeRigidBody(b.get());
            if(ball)world.removeRigidBody(ball.get());
            for(auto& b:statics)world.removeRigidBody(b.get());
        }
        void Box(const Vec3& at,const Vec3& half,float friction,float bounce,int index)
        {
            shapes.push_back(std::make_unique<btBoxShape>(V(half)));
            btRigidBody::btRigidBodyConstructionInfo info(0,nullptr,shapes.back().get());
            info.m_startWorldTransform.setIdentity();
            info.m_startWorldTransform.setOrigin(V(at));
            info.m_friction=friction;
            info.m_restitution=bounce;
            statics.push_back(std::make_unique<btRigidBody>(info));
            statics.back()->setUserIndex(index);
            world.addRigidBody(statics.back().get());
        }
        void AddPin(int n,const Pin& p)
        {
            btVector3 inertia;
            pinShape->calculateLocalInertia(PinMass,inertia);
            // (The hull's box guess makes it too hard to topple: closer to a real pin's.)
            inertia*=0.85f;
            btRigidBody::btRigidBodyConstructionInfo info(PinMass,nullptr,pinShape.get(),inertia);
            info.m_startWorldTransform=btTransform(Q(p.q),V(p.p));
            info.m_friction=0.32f;
            info.m_restitution=0.62f;
            info.m_linearDamping=0.02f;
            info.m_angularDamping=0.08f;
            info.m_rollingFriction=0.002f;
            auto& body=pins[static_cast<size_t>(n)];
            body=std::make_unique<btRigidBody>(info);
            body->setUserIndex(n);
            body->setActivationState(DISABLE_DEACTIVATION);
            body->setCcdMotionThreshold(0.03f);
            body->setCcdSweptSphereRadius(0.02f);
            world.addRigidBody(body.get());
        }
        void AddBall(const Vec3& p,const Quat& q,const Vec3& v,const Vec3& w)
        {
            btVector3 inertia;
            ballShape->calculateLocalInertia(BallMass,inertia);
            btRigidBody::btRigidBodyConstructionInfo info(BallMass,nullptr,ballShape.get(),inertia);
            info.m_startWorldTransform=btTransform(Q(q),V(p));
            info.m_friction=0.22f;
            info.m_restitution=0.70f;
            info.m_rollingFriction=0.0005f;
            ball=std::make_unique<btRigidBody>(info);
            ball->setUserIndex(BallIndex);
            ball->setLinearVelocity(V(v));
            ball->setAngularVelocity(V(w));
            ball->setActivationState(DISABLE_DEACTIVATION);
            ball->setCcdMotionThreshold(0.05f);
            ball->setCcdSweptSphereRadius(0.08f);
            world.addRigidBody(ball.get());
        }
    };

    Roll::Roll()=default;
    Roll::~Roll()=default;

    namespace
    {
        // The lane ball of the throw going on (Roll keeps only its numbers).
        LaneBall Lane(const Vec3& p,const Vec3& v,const Vec3& w,const Quat& q,bool gutter)
        {
            LaneBall b;
            b.p=p;b.v=v;b.w=w;b.q=q;b.gutter=gutter;
            return b;
        }
    }

    void Roll::Start(const Throw& t,const std::array<Pin,10>& standing)
    {
        world.reset();
        pins=standing;
        LaneBall b;
        b.Start(t);
        ball=b.p;velocity=b.v;spin=b.w;turn=b.q;
        gutter=deck=stopped=false;
        clock=deckAt=quietFor=0;
        events.clear();
    }

    void Roll::LaneStep(float h)
    {
        LaneBall b=Lane(ball,velocity,spin,turn,gutter);
        const bool was=gutter;
        b.StepOn(h);
        ball=b.p;velocity=b.v;spin=b.w;turn=b.q;gutter=b.gutter;
        if(b.stopped)stopped=true;
        if(gutter&&!was)events.push_back({Event::Gutter,std::min(1.0f,std::hypot(velocity.x,velocity.y)/8),ball});
    }

    void Roll::ToDeck()
    {
        deck=true;
        deckAt=clock;
        world=std::make_unique<World>();
        for(int n=0;n<10;++n)if(pins[static_cast<size_t>(n)].there)world->AddPin(n,pins[static_cast<size_t>(n)]);
        world->AddBall(ball,turn,velocity,spin);
    }

    void Roll::Step(double dt)
    {
        if(Done())return;
        if(!deck)
        {
            // On the lane: small steps of its own, until it comes to the deck.
            const int steps=std::max(1,static_cast<int>(std::ceil(dt*480)));
            const float h=static_cast<float>(dt)/steps;
            for(int i=0;i<steps&&!deck&&!stopped;++i)
            {
                LaneStep(h);
                clock+=h;
                if(ball.y>=DeckFrom)ToDeck();
            }
            return;
        }
        auto& w=*world;
        double left=dt;
        while(left>1e-9)
        {
            const double h=std::min(DeckStep,left);
            w.world.stepSimulation(static_cast<btScalar>(h),0,static_cast<btScalar>(h));
            left-=h;
            clock+=h;
            // New contacts: the knocks heard.
            const int count=w.dispatcher.getNumManifolds();
            for(int i=0;i<count;++i)
            {
                const btPersistentManifold* m=w.dispatcher.getManifoldByIndexInternal(i);
                const int a=static_cast<const btCollisionObject*>(m->getBody0())->getUserIndex();
                const int b=static_cast<const btCollisionObject*>(m->getBody1())->getUserIndex();
                float most=0;
                btVector3 at{0,0,0};
                for(int p=0;p<m->getNumContacts();++p)
                {
                    const btManifoldPoint& c=m->getContactPoint(p);
                    if(c.getLifeTime()<=1&&c.getAppliedImpulse()>most){most=c.getAppliedImpulse();at=c.getPositionWorldOnA();}
                }
                if(most<0.4f)continue;
                const bool ballIn=a==BallIndex||b==BallIndex,pinA=a>=0&&a<10,pinB=b>=0&&b<10;
                Event e;
                e.at=A(at);
                if(ballIn&&(pinA||pinB)){e.kind=Event::BallPin;e.strength=std::min(1.0f,most/14);}
                else if(pinA&&pinB){e.kind=Event::PinPin;e.strength=std::min(1.0f,most/5);}
                else if((pinA||pinB)&&(a==WallIndex||b==WallIndex)){e.kind=Event::PinWall;e.strength=std::min(1.0f,most/5);}
                else if(ballIn&&(a==WallIndex||b==WallIndex)){e.kind=Event::Pit;e.strength=std::min(1.0f,most/30);}
                else continue;
                events.push_back(e);
            }
        }
        // Where everything is now.
        {
            const btTransform& t=w.ball->getWorldTransform();
            ball=A(t.getOrigin());
            turn=A(t.getRotation());
            velocity=A(w.ball->getLinearVelocity());
            spin=A(w.ball->getAngularVelocity());
        }
        float fastest=0;
        for(int n=0;n<10;++n)
        {
            auto& body=w.pins[static_cast<size_t>(n)];
            if(!body)continue;
            const btTransform& t=body->getWorldTransform();
            pins[static_cast<size_t>(n)].p=A(t.getOrigin());
            pins[static_cast<size_t>(n)].q=A(t.getRotation());
            // (One that has gone into the pit is out of it.)
            if(pins[static_cast<size_t>(n)].p.z>-0.2f)
                fastest=std::max({fastest,body->getLinearVelocity().length(),body->getAngularVelocity().length()*0.1f});
        }
        const bool ballGone=ball.y>DeckEnd||ball.z<-0.15f||velocity.y<0.05f;
        quietFor=fastest<0.04f&&ballGone?quietFor+dt:0;
    }

    bool Roll::Done() const
    {
        if(stopped&&!deck)return true;
        return deck&&((quietFor>0.5&&clock-deckAt>1.2)||clock-deckAt>5.5);
    }

    bool Roll::Standing(int n) const
    {
        const Pin& p=pins[static_cast<size_t>(n)];
        if(!p.there)return false;
        // Its up: the quaternion's z axis.
        const float up=1-2*(p.q.x*p.q.x+p.q.y*p.q.y);
        return up>0.94f&&std::abs(p.p.z-PinCentre)<0.03f&&std::abs(p.p.x)<LaneHalf&&p.p.y<DeckEnd;
    }

    std::array<Pin,10> Roll::Left() const
    {
        std::array<Pin,10> out=pins;
        for(int n=0;n<10;++n)
        {
            auto& p=out[static_cast<size_t>(n)];
            p.there=Standing(n);
            if(p.there){p.p.z=PinCentre;}
        }
        return out;
    }

    // ---- The score -------------------------------------------------------------------------

    void Game::New()
    {
        rolls={};
        turn=You;frame=0;ball=0;
        strikes={};spares={};
    }

    bool Game::FrameDone(int side,int f) const
    {
        const auto& r=rolls[side][f];
        if(f<9)return (r.size()==1&&r[0]==10)||r.size()==2;
        if(r.size()==3)return true;
        return r.size()==2&&r[0]+r[1]<10;
    }

    bool Game::FreshRack() const
    {
        const auto& r=rolls[turn][frame];
        if(r.empty())return true;
        if(frame<9)return false;
        if(r.size()==1)return r[0]==10;
        if(r.size()==2)return r[0]==10?r[1]==10:r[0]+r[1]==10;
        return false;
    }

    int Game::Record(int pins)
    {
        if(Over())return 0;
        const bool fresh=FreshRack();
        auto& r=rolls[turn][frame];
        // The pins that were standing before this ball.
        int before=10;
        if(!fresh)before=r.size()==1?10-r[0]:10-r[1];
        pins=std::clamp(pins,0,before);
        r.push_back(pins);
        int what=0;
        if(fresh&&pins==10){what=1;++strikes[turn];}
        else if(!fresh&&pins==before){what=2;++spares[turn];}
        ++ball;
        if(FrameDone(turn,frame))
        {
            ball=0;
            if(turn==You)turn=Her;
            else{turn=You;++frame;}
        }
        return what;
    }

    int Game::Total(int side,int f) const
    {
        int total=0;
        for(int i=0;i<=f;++i)
        {
            const auto& r=rolls[side][i];
            if(!FrameDone(side,i))return -1;
            if(i==9){for(int p:r)total+=p;continue;}
            // The balls after this frame's, for its bonus.
            std::vector<int> next;
            for(int j=i+1;j<10&&next.size()<2;++j)for(int p:rolls[side][j])next.push_back(p);
            if(r[0]==10)
            {
                if(next.size()<2)return -1;
                total+=10+next[0]+next[1];
            }
            else if(r[0]+r[1]==10)
            {
                if(next.empty())return -1;
                total+=10+next[0];
            }
            else total+=r[0]+r[1];
        }
        return total;
    }

    int Game::Score(int side) const
    {
        // What is known: the frames counted so far, plus the pins of the frames still waiting.
        int known=0,last=-1;
        for(int f=0;f<10;++f){const int t=Total(side,f);if(t<0)break;known=t;last=f;}
        for(int f=last+1;f<10;++f)for(int p:rolls[side][f])known+=p;
        return known;
    }

    int Game::Winner() const
    {
        if(!Over())return -1;
        const int a=Score(You),b=Score(Her);
        return a>b?You:b>a?Her:-1;
    }

    // ---- How she bowls -----------------------------------------------------------------

    Style LevelStyle(int level)
    {
        switch(level)
        {
        case 1:return {0.0075f,0.55f,0.07f,0.40f};
        case 3:return {0.0018f,0.14f,0.012f,0.60f};
        default:return {0.0036f,0.30f,0.030f,0.55f};
        }
    }

    namespace
    {
        // The angle that brings a throw from x to come to aimX at the pins.
        float AngleFor(Throw t,float aimX)
        {
            float lo=-0.09f,hi=0.09f;
            for(int i=0;i<24;++i)
            {
                t.angle=(lo+hi)/2;
                float x=0;
                if(!Predict(t,PinDeckY-0.10f,x))x=t.x+std::sin(t.angle)*40;   // (into a gutter: by which side)
                if(x<aimX)lo=t.angle;else hi=t.angle;
            }
            return (lo+hi)/2;
        }
    }

    Throw Choose(const std::array<bool,10>& standing,const Style& style,std::mt19937& random)
    {
        std::normal_distribution<float> noise(0,1);
        Throw t;
        int count=0;
        for(bool s:standing)count+=s?1:0;
        float aim=0;
        if(count==10)
        {
            // The pocket: between the head pin and the 3, hooking in from the right.
            t.spin=style.spin;
            t.speed=7.6f;
            t.x=0.20f;
            aim=0.065f;
        }
        else
        {
            // A spare: at the front pin standing, a little toward the rest, from the other side
            // of the lane, nearly straight and a little faster.
            int front=-1;
            float sum=0;
            for(int n=0;n<10;++n)
                if(standing[static_cast<size_t>(n)])
                {
                    sum+=PinSpot(n).x;
                    const Vec3 a=PinSpot(n);
                    if(front<0||a.y<PinSpot(front).y-0.01f||(std::abs(a.y-PinSpot(front).y)<0.01f&&std::abs(a.x)<std::abs(PinSpot(front).x)))front=n;
                }
            if(front<0)front=0;
            const float fx=PinSpot(front).x,mean=count>0?sum/count:0;
            aim=fx+std::clamp(mean-fx,-0.07f,0.07f)*0.5f;
            t.spin=style.spin*0.25f;
            t.speed=8.2f;
            t.x=std::clamp(-aim*0.7f+0.08f,-0.40f,0.40f);
        }
        t.angle=AngleFor(t,aim);
        t.angle+=noise(random)*style.aim;
        t.speed=std::clamp(t.speed+noise(random)*style.speed,5.0f,10.0f);
        t.x=std::clamp(t.x+noise(random)*style.stand,-LaneHalf+0.12f,LaneHalf-0.12f);
        return t;
    }

    // ---- Tests -------------------------------------------------------------------------

    namespace
    {
        // One throw played out; the pins knocked down.
        int Play(const Throw& t,std::array<Pin,10>& pins,int* steps=nullptr)
        {
            Roll roll;
            roll.Start(t,pins);
            int before=0;
            for(const auto& p:pins)before+=p.there?1:0;
            int n=0;
            while(!roll.Done()&&n<20000){roll.Step(1/60.0);++n;}
            if(steps)*steps=n;
            pins=roll.Left();
            int after=0;
            for(const auto& p:pins)after+=p.there?1:0;
            return before-after;
        }
        std::array<Pin,10> Rack()
        {
            std::array<Pin,10> pins{};
            for(int n=0;n<10;++n){pins[static_cast<size_t>(n)].p=PinSpot(n);pins[static_cast<size_t>(n)].p.z=PinCentre;pins[static_cast<size_t>(n)].q={};pins[static_cast<size_t>(n)].there=true;}
            return pins;
        }
    }

    std::wstring Check(int games,std::uint32_t seed)
    {
        std::wstring out;
        wchar_t line[400];
        int wrong=0;
        auto expect=[&](const wchar_t* what,int got,int want){
            swprintf_s(line,L"%s: %d (%s %d)\n",what,got,got==want?L"right":L"WRONG, want",want);
            out+=line;
            if(got!=want)++wrong;
        };
        // The score.
        {
            auto score=[](std::vector<int> balls){
                Game g;g.New();
                // (Her balls: all gutters, between yours.)
                size_t i=0;
                while(!g.Over()&&i<balls.size()+40)
                {
                    if(g.Turn()==Her){g.Record(0);continue;}
                    if(i>=balls.size())break;
                    g.Record(balls[i++]);
                }
                return g.Score(You);
            };
            expect(L"perfect game",score(std::vector<int>(12,10)),300);
            expect(L"all 5 and spare",score(std::vector<int>(21,5)),150);
            std::vector<int> nines;for(int f=0;f<10;++f){nines.push_back(9);nines.push_back(0);}
            expect(L"all 9-0",score(nines),90);
            expect(L"all gutters",score(std::vector<int>(20,0)),0);
            expect(L"strike then 3-4",score({10,3,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}),24);
            expect(L"tenth: spare then strike",score({0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,9,1,10}),20);
        }
        // The lane: where a ball comes to the pins.
        for(float spin:{0.0f,0.3f,0.6f,1.0f})
            for(float speed:{6.0f,7.5f,9.0f})
            {
                Throw t;t.x=0.2f;t.angle=0;t.speed=speed;t.spin=spin;
                float x=0,entry=0;
                const bool on=Predict(t,PinDeckY,x,&entry);
                swprintf_s(line,L"lane: from 0.20 straight on, %.1f m/s, spin %.1f: at the pins %.3f m across (%s), entering at %.1f degrees\n",
                           speed,spin,x,on?L"on":L"gutter",entry*57.2958f);
                out+=line;
            }
        // Throws at the pocket and the head pin, each pin count.
        {
            const auto start=std::chrono::steady_clock::now();
            for(float aimX:{0.0f,0.03f,0.065f,0.10f,0.14f,-0.065f})
            {
                Throw t;t.x=0.20f;t.speed=7.6f;t.spin=0.55f;
                t.angle=AngleFor(t,aimX);
                auto pins=Rack();
                int steps=0;
                const int down=Play(t,pins,&steps);
                std::wstring left;
                for(int n=0;n<10;++n)if(pins[static_cast<size_t>(n)].there)left+=std::to_wstring(n+1)+L" ";
                swprintf_s(line,L"rack at %.3f: %d down, left %s(%d frames)\n",aimX,down,left.c_str(),steps);
                out+=line;
            }
            swprintf_s(line,L"  (%.2f s)\n",std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());
            out+=line;
        }
        // Her games at each strength.
        std::mt19937 random(seed);
        for(int level=1;level<=3;++level)
        {
            const Style style=LevelStyle(level);
            double total=0;
            int strikes=0,spares=0,frames=0,most=0,least=999;
            for(int game=0;game<games;++game)
            {
                Game g;g.New();
                std::array<Pin,10> pins=Rack();
                while(!g.Over())
                {
                    if(g.FreshRack())pins=Rack();
                    std::array<bool,10> standing{};
                    for(int n=0;n<10;++n)standing[static_cast<size_t>(n)]=pins[static_cast<size_t>(n)].there;
                    const Throw t=Choose(standing,style,random);
                    const int side=g.Turn();
                    const int down=Play(t,pins);
                    g.Record(down);
                    if(g.Turn()!=side||g.FreshRack())pins=Rack();
                }
                const int s=g.Score(Her);
                total+=s;most=std::max(most,s);least=std::min(least,s);
                strikes+=g.Strikes(Her);spares+=g.Spares(Her);frames+=10;
            }
            swprintf_s(line,L"level %d: %d games, her average %.1f (%d to %d), strikes %.0f%% of frames, spares %.0f%%\n",
                       level,games,total/std::max(1,games),least,most,100.0*strikes/std::max(1,frames),100.0*spares/std::max(1,frames));
            out+=line;
        }
        swprintf_s(line,L"%d wrong\n",wrong);
        out+=line;
        return out;
    }
}
