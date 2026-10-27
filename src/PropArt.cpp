#define NOMINMAX
#include "PropArt.hpp"
#include "ShapeBuilder.hpp"

namespace MikuMikuDesktopPerch
{
    namespace
    {
        using namespace Art;

        Shapes At(Geometry& out,float x,float z,float k,float yaw=0)
        {
            Shapes s{out,XMMatrixRotationY(yaw)*XMMatrixTranslation(x,0,z)};
            s.shade=k;
            return s;
        }

        // A ring (in its own xz plane, around the origin), as a chain of short limbs.
        void Ring(Shapes& s,float radius,float tube,const Colour& c,int pieces=14)
        {
            for(int i=0;i<pieces;++i)
            {
                const float a=XM_2PI*i/pieces,b=XM_2PI*(i+1)/pieces;
                s.Limb({radius*std::cos(a),0,radius*std::sin(a)},{radius*std::cos(b),0,radius*std::sin(b)},tube,tube,c,6);
            }
        }

        const Colour Gold{0.98f,0.78f,0.30f,Metal},Silver{0.80f,0.82f,0.86f,Metal},Iron{0.30f,0.29f,0.32f,Metal};
    }

    void AddChest(Geometry& out,float x,float z,float k)
    {
        // A wooden chest with a rounded lid, iron bands and corners, a gold lock plate; its
        // front to the camera above (toward -z).
        Shapes s=At(out,x,z,k,XM_PI);
        const Colour wood{0.62f,0.38f,0.17f,Wood},lid{0.52f,0.31f,0.14f,Wood};
        s.Box({0,1.7f,0},{3.0f,1.7f,2.1f},wood);
        s.Limb({-3.0f,3.4f,0},{3.0f,3.4f,0},2.1f,2.1f,lid,12);
        for(const float bx:{-1.9f,1.9f})
        {
            s.Box({bx,1.7f,0},{0.28f,1.72f,2.14f},Iron);
            s.Limb({bx-0.28f,3.4f,0},{bx+0.28f,3.4f,0},2.18f,2.18f,Iron,12);
        }
        s.Box({0,3.4f,0},{3.05f,0.14f,2.16f},Iron);
        for(const float cx:{-2.9f,2.9f})for(const float cz:{-2.0f,2.0f})s.Box({cx,0.5f,cz},{0.22f,0.5f,0.22f},Iron);
        s.Box({0,3.1f,2.18f},{0.6f,0.7f,0.12f},Gold);
        s.Box({0,3.0f,2.31f},{0.12f,0.28f,0.04f},Black);
    }

    void AddGold(Geometry& out,float x,float z,int amount,float k)
    {
        // Coins in a little heap, a few tipped over, more for a bigger find.
        Shapes s=At(out,x,z,k,static_cast<float>(amount%7));
        const int coins=std::clamp(4+amount/6,5,12);
        static constexpr float spots[12][3]={{0,0,0},{1.5f,0,0.6f},{-1.2f,0,0.9f},{0.3f,0,-1.4f},{-1.6f,0,-0.8f},{1.1f,0,-1.3f},
                                             {0.2f,0.3f,0.2f},{1.2f,0.3f,0.4f},{-0.9f,0.3f,0.6f},{0.4f,0.6f,0.3f},{2.2f,0,-0.4f},{-0.4f,0,1.9f}};
        for(int i=0;i<coins;++i)
        {
            const float tilt=i>=coins-2?0.9f:0.12f*(i%3);
            s.With(XMMatrixRotationRollPitchYaw(tilt,0.7f*i,0)*XMMatrixTranslation(spots[i][0],spots[i][1]+0.15f,spots[i][2]),[&]{
                s.Limb({0,0,0},{0,0.28f,0},0.85f,0.85f,Gold,10);
            });
        }
    }

    void AddKey(Geometry& out,float x,float z,float k)
    {
        // A large old key lying on the floor: a ring for the bow, a long shaft, its teeth.
        Shapes s=At(out,x,z,k,0.5f);
        s.With(XMMatrixTranslation(-2.2f,0.45f,0),[&]{Ring(s,1.0f,0.28f,Gold);});
        s.Ellipsoid({-2.2f,0.45f,0},{0.35f,0.35f,0.35f},{1.0f,0.4f,0.35f,Glow});
        s.Limb({-1.2f,0.45f,0},{2.8f,0.45f,0},0.3f,0.3f,Gold);
        s.Box({2.3f,0.45f,0.65f},{0.22f,0.22f,0.45f},Gold);
        s.Box({1.6f,0.45f,0.5f},{0.18f,0.22f,0.3f},Gold);
        s.Ellipsoid({-0.9f,0.45f,0},{0.4f,0.4f,0.4f},Gold);
    }

    void AddGoods(Geometry& out,float x,float z,const Dungeon::Item& item,float k)
    {
        Shapes s=At(out,x,z,k);
        // The mat, red with a gold edge.
        s.Box({0,0.08f,0},{4.2f,0.08f,4.2f},{0.62f,0.20f,0.22f,Lit});
        for(const float e:{-4.0f,4.0f})
        {
            s.Box({e,0.18f,0},{0.18f,0.04f,4.2f},{0.90f,0.72f,0.30f,Lit});
            s.Box({0,0.18f,e},{4.2f,0.04f,0.18f},{0.90f,0.72f,0.30f,Lit});
        }
        // The thing itself, half as big again as it would be in a hand, to be seen from above.
        s.world=XMMatrixScaling(1.5f,1.5f,1.5f)*s.world;
        using I=Dungeon::ItemKind;
        switch(item.kind)
        {
        case I::Potion:
        {
            // A round flask of red, corked.
            const Colour glass{0.95f,0.22f,0.28f,Metal};
            s.Ellipsoid({0,1.5f,0},{1.25f,1.3f,1.25f},glass);
            s.Limb({0,2.5f,0},{0,3.5f,0},0.5f,0.42f,glass);
            s.Box({0,3.75f,0},{0.42f,0.3f,0.42f},{0.62f,0.45f,0.28f,Wood});
            s.Ellipsoid({-0.5f,1.9f,-0.85f},{0.25f,0.4f,0.12f},{1.0f,0.85f,0.85f,Glow});
            break;
        }
        case I::Ether:
        {
            // A tall blue bottle.
            const Colour glass{0.30f,0.52f,1.0f,Metal};
            s.Limb({0,0.2f,0},{0,2.4f,0},0.95f,0.95f,glass,10);
            s.Ellipsoid({0,2.4f,0},{0.95f,0.6f,0.95f},glass);
            s.Limb({0,2.8f,0},{0,3.9f,0},0.38f,0.34f,glass);
            s.Box({0,4.1f,0},{0.36f,0.28f,0.36f},{0.62f,0.45f,0.28f,Wood});
            s.Ellipsoid({-0.45f,1.6f,-0.8f},{0.2f,0.6f,0.1f},{0.85f,0.95f,1.0f,Glow});
            break;
        }
        case I::Food:
        {
            // Meat on the bone (a bigger one for the big one).
            const float big=item.food>50?1.3f:1.0f;
            s.With(XMMatrixScaling(big,big,big)*XMMatrixRotationY(0.5f),[&]{
                s.Limb({-3.0f,1.3f,0},{3.0f,1.3f,0},0.32f,0.32f,White);
                for(const float e:{-3.1f,3.1f})
                {
                    s.Ellipsoid({e,1.6f,0},{0.45f,0.45f,0.45f},White);
                    s.Ellipsoid({e,1.0f,0},{0.45f,0.45f,0.45f},White);
                }
                s.Ellipsoid({0,1.4f,0},{2.1f,1.35f,1.5f},{0.58f,0.27f,0.12f,Lit});
                s.Ellipsoid({-0.4f,2.1f,-0.6f},{1.0f,0.45f,0.6f},{0.78f,0.42f,0.22f,Lit});
            });
            break;
        }
        case I::Weapon:
            s.With(XMMatrixRotationY(0.6f),[&]{
                if(item.staff)
                {
                    // A wooden staff with a glowing stone held in prongs at its head.
                    s.Limb({-3.6f,0.4f,0},{2.8f,0.4f,0},0.3f,0.38f,{0.48f,0.30f,0.15f,Wood});
                    for(const float a:{0.0f,2.1f,4.2f})s.Limb({2.8f,0.4f,0},{3.6f,0.4f+0.5f*std::cos(a),0.5f*std::sin(a)},0.15f,0.08f,Gold,5);
                    s.Ellipsoid({3.5f,0.45f,0},{0.55f,0.55f,0.55f},{0.70f,0.45f,1.0f,Glow});
                }
                else
                {
                    // A sword: blade, crossguard, grip and pommel.
                    s.Box({0.9f,0.3f,0},{2.6f,0.12f,0.38f},Silver);
                    s.Triangle({3.5f,0.42f,-0.38f},{3.5f,0.42f,0.38f},{4.4f,0.42f,0},Silver);
                    s.Box({-1.85f,0.35f,0},{0.22f,0.25f,1.15f},Gold);
                    s.Limb({-2.05f,0.35f,0},{-3.4f,0.35f,0},0.25f,0.25f,{0.35f,0.20f,0.12f,Wood});
                    s.Ellipsoid({-3.55f,0.35f,0},{0.38f,0.38f,0.38f},Gold);
                }
            });
            break;
        case I::Armor:
            // A breastplate on a little stand: the plate curved over the chest, a ridge down its
            // middle, a collar and shoulder guards, a gold trim at the waist.
            s.Limb({0,0.1f,0},{0,1.2f,0},0.25f,0.25f,{0.40f,0.26f,0.15f,Wood});
            s.Box({0,0.1f,0},{1.2f,0.1f,0.8f},{0.40f,0.26f,0.15f,Wood});
            s.Ellipsoid({0,2.8f,0},{1.9f,2.0f,1.0f},Silver);
            s.Limb({0,1.1f,0.95f},{0,4.3f,0.8f},0.16f,0.16f,Silver,6);
            s.Limb({0,1.15f,0},{0,1.35f,0},1.75f,1.75f,Gold,12);
            s.Limb({0,4.4f,0},{0,4.9f,0},0.8f,0.65f,Silver,10);
            s.Both([&](float o){s.Ellipsoid({o*1.9f,4.2f,0},{1.0f,0.5f,0.95f},Silver,{0,0,-o*0.35f});});
            break;
        default:
        {
            // A ring standing up with a gem set in it (an accessory, or a treasure).
            const Colour gem=item.kind==I::Accessory?Colour{0.35f,0.95f,0.65f,Glow}:Colour{1.0f,0.55f,0.85f,Glow};
            s.With(XMMatrixRotationX(XM_PIDIV2)*XMMatrixTranslation(0,1.6f,0),[&]{Ring(s,1.2f,0.24f,Gold);});
            s.Ellipsoid({0,2.95f,0},{0.48f,0.48f,0.48f},gem);
            break;
        }
        }
    }

    void AddCampfire(Geometry& out,float x,float z,bool burning,float k)
    {
        // A ring of stones round logs leant together; glowing embers when it still burns (the
        // flames are AddFlames, every frame), ash after.
        Shapes s=At(out,x,z,k);
        const Colour stone{0.46f,0.44f,0.42f,Stone},log{0.42f,0.26f,0.13f,Wood},charred{0.10f,0.08f,0.07f,Lit};
        for(int i=0;i<9;++i)
        {
            const float a=XM_2PI*i/9;
            s.Ellipsoid({3.2f*std::cos(a),0.45f,3.2f*std::sin(a)},{0.95f,0.6f,0.8f},stone,{0,a,0},8,5);
        }
        for(int i=0;i<4;++i)
        {
            const float a=XM_2PI*i/4+0.4f;
            s.Limb({2.3f*std::cos(a),0.2f,2.3f*std::sin(a)},{0.2f*std::cos(a),2.6f,0.2f*std::sin(a)},0.38f,0.3f,log);
            s.Ellipsoid({0.25f*std::cos(a),2.55f,0.25f*std::sin(a)},{0.35f,0.3f,0.35f},charred);
        }
        if(burning)
        {
            for(int i=0;i<5;++i)
            {
                const float a=XM_2PI*i/5;
                s.Ellipsoid({1.3f*std::cos(a),0.25f,1.3f*std::sin(a)},{0.35f,0.15f,0.35f},{1.0f,0.30f,0.08f,Glow},{0,0,0},6,4);
            }
        }
        else s.Ellipsoid({0,0.1f,0},{1.8f,0.15f,1.8f},{0.32f,0.30f,0.29f,Stone});
    }

    void AddFlames(Geometry& out,float x,float z,float time,float k)
    {
        // Tongues of flame, each rising, swaying and shrinking on its own rhythm (sums of
        // waves that never line up), the inner ones yellow and the outer ones red.
        Shapes s=At(out,x,z,k);
        auto wave=[&](float seed){return 0.5f*std::sin(time*7.3f+seed)+0.3f*std::sin(time*13.1f+seed*2.7f)+0.2f*std::sin(time*21.7f+seed*4.1f);};
        struct Tongue {float a,r,height,width;Colour c;};
        const Tongue tongues[]={
            {0,0,4.0f,1.5f,{0.95f,0.30f,0.05f,Glow}},
            {0.6f,0.1f,5.4f,0.75f,{1.0f,0.70f,0.20f,Glow}},
            {1.0f,0.9f,2.6f,0.6f,{0.90f,0.22f,0.04f,Glow}},
            {3.1f,0.9f,2.8f,0.6f,{0.95f,0.35f,0.06f,Glow}},
            {5.2f,0.9f,2.4f,0.6f,{0.90f,0.26f,0.05f,Glow}},
            {2.2f,0.3f,2.4f,0.45f,{1.0f,0.92f,0.55f,Glow}}};
        int n=0;
        for(const auto& t:tongues)
        {
            const float seed=1.7f*n++;
            const float tall=t.height*(1.0f+0.22f*wave(seed)),sway=0.35f*t.height*wave(seed+5.0f)/4.8f;
            const float lean=0.25f*t.height*wave(seed+9.0f)/4.8f;
            const XMFLOAT3 foot{t.r*std::cos(t.a),0.4f,t.r*std::sin(t.a)};
            // Bent: the lower half straight up, the tip swaying.
            const XMFLOAT3 mid{foot.x+sway*0.3f,0.4f+tall*0.5f,foot.z+lean*0.3f},tip{foot.x+sway,0.4f+tall,foot.z+lean};
            s.Limb(foot,mid,t.width,t.width*0.6f,t.c,8);
            s.Limb(mid,tip,t.width*0.6f,0.03f,t.c,8);
        }
    }

    void AddStairs(Geometry& out,float x,float z,float k)
    {
        // A stone landing at the middle of the tile (she stands on it), and before it, toward
        // -z (toward the camera above, which then sees the treads going down), a well with
        // steps down into the dark, edged with a stone kerb.
        Shapes s=At(out,x,z,k);
        const Colour kerb{0.62f,0.60f,0.56f,Stone},step{0.66f,0.63f,0.58f,Stone},side{0.40f,0.38f,0.36f,Stone};
        const float x0=-3.6f,x1=3.6f,z0=-4.4f,z1=-0.6f;
        s.Box({0,0.1f,2.0f},{4.6f,0.1f,2.6f},kerb);
        // The well's sides, facing in.
        const float depth=5.2f;   // just below the last step
        auto wall=[&](XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 n){
            const XMVECTOR na=XMLoadFloat3(&n);
            const XMVECTOR q[4]={XMVectorSet(a.x,0,a.z,0),XMVectorSet(b.x,0,b.z,0),XMVectorSet(b.x,-depth,b.z,0),XMVectorSet(a.x,-depth,a.z,0)};
            for(int i:{0,1,2,0,2,3})s.Vertex(q[i],na,side);
        };
        wall({x0,0,z0},{x0,0,z1},{1,0,0});
        wall({x1,0,z0},{x1,0,z1},{-1,0,0});
        wall({x0,0,z1},{x1,0,z1},{0,0,-1});
        wall({x0,0,z0},{x1,0,z0},{0,0,1});
        // Each step a block from its tread down to the bottom, the next one lower and nearer
        // the camera; a dark line along each step's edge.
        constexpr int Steps=5;
        const float run=(z1-z0)/Steps;
        for(int i=0;i<Steps;++i)
        {
            const float top=-0.6f-0.85f*i,back=z1-run*i;
            s.Box({0,(top-depth)/2,back-run/2},{(x1-x0)/2,(top+depth)/2,run/2},step);
            s.Box({0,top+0.02f,back-run+0.12f},{(x1-x0)/2,0.04f,0.12f},{0.18f,0.17f,0.16f,Stone});
        }
        s.Box({0,-depth+0.05f,(z0+z1)/2},{(x1-x0)/2,0.05f,(z1-z0)/2},{0.02f,0.02f,0.03f,Lit});
        for(const float e:{x0-0.5f,x1+0.5f})s.Box({e,0.25f,(z0+z1)/2},{0.5f,0.25f,(z1-z0)/2+0.5f},kerb);
        s.Box({0,0.25f,z0-0.5f},{(x1-x0)/2+1.0f,0.25f,0.5f},kerb);
    }

    void AddDoor(Geometry& out,float x,float z,float height,bool alongZ,float k)
    {
        // Planks of heavy wood across the passage, iron straps and studs, a ring for a handle,
        // a keyhole; both faces alike.
        Shapes s=At(out,x,z,k,alongZ?0.0f:XM_PIDIV2);
        const Colour wood{0.50f,0.31f,0.16f,Wood};
        s.Box({0,height/2,0},{4.8f,height/2,4.6f},wood);
        for(const float h:{0.22f,0.75f})s.Box({0,height*h,0},{4.9f,0.35f,4.7f},Iron);
        for(const float f:{-4.7f,4.7f})
        {
            for(const float px:{-3.6f,-1.2f,1.2f,3.6f})for(const float h:{0.22f,0.75f})s.Ellipsoid({px,height*h,f+(f>0?0.08f:-0.08f)},{0.22f,0.22f,0.12f},Iron,{0,0,0},6,4);
            s.With(XMMatrixRotationX(XM_PIDIV2)*XMMatrixTranslation(2.6f,height*0.45f,f+(f>0?0.3f:-0.3f)),[&]{Ring(s,0.7f,0.13f,Iron,10);});
            s.Box({-2.6f,height*0.45f,f+(f>0?0.06f:-0.06f)},{0.15f,0.4f,0.05f},Black);
        }
    }

    void AddTrap(Geometry& out,float x,float z,Dungeon::TrapKind kind,float k)
    {
        Shapes s=At(out,x,z,k);
        using K=Dungeon::TrapKind;
        switch(kind)
        {
        case K::Spikes:
            // An iron plate bristling with spikes.
            s.Box({0,0.15f,0},{3.5f,0.15f,3.5f},Iron);
            for(int i=-1;i<=1;++i)for(int j=-1;j<=1;++j)s.Limb({2.1f*i,0.3f,2.1f*j},{2.1f*i,2.6f,2.1f*j},0.42f,0.02f,Silver,6);
            break;
        case K::Warp:
            // A glowing circle on the floor with a ring inside it and a bright middle.
            s.With(XMMatrixTranslation(0,0.15f,0),[&]{Ring(s,3.3f,0.16f,{0.72f,0.42f,1.0f,Glow},20);Ring(s,1.9f,0.12f,{0.85f,0.65f,1.0f,Glow},16);});
            s.Ellipsoid({0,0.1f,0},{1.1f,0.06f,1.1f},{0.95f,0.88f,1.0f,Glow});
            for(int i=0;i<6;++i)
            {
                const float a=XM_2PI*i/6;
                s.Ellipsoid({2.6f*std::cos(a),0.12f,2.6f*std::sin(a)},{0.3f,0.05f,0.3f},{0.85f,0.65f,1.0f,Glow},{0,0,0},6,3);
            }
            break;
        case K::Pit:
            // A black hole ringed with broken stones.
            s.Box({0,0.03f,0},{3.4f,0.03f,3.4f},{0.02f,0.015f,0.02f,Lit});
            for(int i=0;i<10;++i)
            {
                const float a=XM_2PI*i/10;
                s.Ellipsoid({3.6f*std::cos(a),0.25f,3.6f*std::sin(a)},{0.7f,0.35f,0.6f},{0.40f,0.38f,0.36f,Stone},{0,a,0},6,4);
            }
            break;
        case K::Alarm:
            // A stone plate with a red button, ringed in yellow.
            s.Box({0,0.15f,0},{2.6f,0.15f,2.6f},{0.50f,0.48f,0.45f,Stone});
            s.With(XMMatrixTranslation(0,0.32f,0),[&]{Ring(s,1.6f,0.14f,{0.95f,0.80f,0.20f,Metal},16);});
            s.Ellipsoid({0,0.35f,0},{1.2f,0.6f,1.2f},{0.88f,0.20f,0.18f,Metal});
            break;
        case K::Sleep:
            // An iron grate with pink sleeping gas welling up from it.
            s.Box({0,0.12f,0},{2.8f,0.12f,2.8f},Iron);
            for(int i=-2;i<=2;++i)s.Box({1.0f*i,0.25f,0},{0.18f,0.02f,2.3f},Black);
            s.Ellipsoid({0.3f,1.0f,0.2f},{1.3f,0.8f,1.2f},{1.0f,0.62f,0.82f,Glow});
            s.Ellipsoid({-0.5f,2.1f,-0.3f},{0.8f,0.55f,0.8f},{1.0f,0.72f,0.88f,Glow});
            s.Ellipsoid({0.4f,2.9f,0.3f},{0.45f,0.35f,0.45f},{1.0f,0.82f,0.92f,Glow});
            break;
        }
    }

    void AddCrystals(Geometry& out,float x,float z,float height,int seed,float k)
    {
        // Three six-sided crystals with pointed tips, leaning out from one root.
        Shapes s=At(out,x,z,k,0.7f*seed);
        const Colour colours[3]={{0.45f,0.85f,1.0f,Glow},{0.80f,0.60f,1.0f,Glow},{0.55f,0.95f,0.95f,Glow}};
        const float lean[3][2]={{0.1f,0},{0.45f,2.2f},{-0.4f,4.0f}};
        for(int i=0;i<3;++i)
        {
            const float h=height*(i==0?1.0f:0.55f+0.12f*((seed+i)%3)),r=i==0?0.9f:0.6f;
            s.With(XMMatrixRotationX(lean[i][0])*XMMatrixRotationY(lean[i][1]),[&]{
                s.Limb({0,0,0},{0,h*0.8f,0},r,r,colours[(seed+i)%3],6);
                s.Limb({0,h*0.8f,0},{0,h,0},r,0.02f,colours[(seed+i)%3],6);
            });
        }
    }
}
