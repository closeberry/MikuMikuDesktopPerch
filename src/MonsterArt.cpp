#define NOMINMAX
#include "MonsterArt.hpp"
#include <DirectXMath.h>
#include <cmath>

using namespace DirectX;

namespace MikuMikuDesktopPerch
{
    namespace
    {
        // The geometry shader's patterns by the colour's alpha (see GeometryPS.hlsl): lit with
        // grime on it (from the place on the monster itself, so it does not slide as the monster
        // moves), or glowing (unlit, for eyes and spirits).
        constexpr float Grimy=0,Glow=0.25f;

        struct Colour {float r,g,b,a=Grimy;};

        // Solids in the monster's own frame (+x its right, y up, +z the way it faces), placed in
        // the world by one matrix.
        struct Shapes
        {
            std::vector<MascotRenderer::GeometryVertex>& out;
            XMMATRIX world;

            void Vertex(FXMVECTOR p,FXMVECTOR n,const Colour& c)
            {
                MascotRenderer::GeometryVertex v{};
                XMFLOAT3 wp,wn;
                XMStoreFloat3(&wp,XMVector3TransformCoord(p,world));
                XMStoreFloat3(&wn,XMVector3Normalize(XMVector3TransformNormal(n,world)));
                v.position[0]=wp.x;v.position[1]=wp.y;v.position[2]=wp.z;
                v.normal[0]=wn.x;v.normal[1]=wn.y;v.normal[2]=wn.z;
                v.color[0]=c.r;v.color[1]=c.g;v.color[2]=c.b;v.color[3]=c.a;
                // Where on the monster it is (the grime's pattern follows it); the third stays 0,
                // which leaves it out of the walls' sinking.
                v.wall[0]=XMVectorGetX(p);v.wall[1]=XMVectorGetY(p);v.wall[2]=0;v.wall[3]=XMVectorGetZ(p);
                out.push_back(v);
            }

            // A flat triangle (wings, membranes): lit as its face points.
            void Triangle(XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 c,const Colour& colour)
            {
                const XMVECTOR pa=XMLoadFloat3(&a),pb=XMLoadFloat3(&b),pc=XMLoadFloat3(&c);
                XMVECTOR n=XMVector3Cross(XMVectorSubtract(pb,pa),XMVectorSubtract(pc,pa));
                if(XMVectorGetY(n)<0)n=XMVectorNegate(n);   // lit from above whichever way it was wound
                Vertex(pa,n,colour);Vertex(pb,n,colour);Vertex(pc,n,colour);
            }

            // An ellipsoid around centre with radii, turned by (pitch, yaw, roll).
            void Ellipsoid(XMFLOAT3 centre,XMFLOAT3 radii,const Colour& c,XMFLOAT3 turn={0,0,0},int segments=12,int rings=8)
            {
                const XMMATRIX r=XMMatrixRotationRollPitchYaw(turn.x,turn.y,turn.z);
                const XMVECTOR o=XMLoadFloat3(&centre);
                auto at=[&](int i,int j,XMVECTOR& p,XMVECTOR& n){
                    const float v=XM_PI*j/rings,u=XM_2PI*i/segments;
                    const XMVECTOR unit=XMVectorSet(std::sin(v)*std::cos(u),std::cos(v),std::sin(v)*std::sin(u),0);
                    p=XMVectorAdd(o,XMVector3TransformNormal(XMVectorMultiply(unit,XMLoadFloat3(&radii)),r));
                    n=XMVector3TransformNormal(XMVectorDivide(unit,XMLoadFloat3(&radii)),r);
                };
                for(int j=0;j<rings;++j)
                    for(int i=0;i<segments;++i)
                    {
                        XMVECTOR p[4],n[4];
                        at(i,j,p[0],n[0]);at(i+1,j,p[1],n[1]);at(i+1,j+1,p[2],n[2]);at(i,j+1,p[3],n[3]);
                        for(int k:{0,1,2,0,2,3})Vertex(p[k],n[k],c);
                    }
            }

            // A tapered limb from a (radius ra) to b (radius rb), closed at both ends.
            void Limb(XMFLOAT3 a,XMFLOAT3 b,float ra,float rb,const Colour& c,int segments=8)
            {
                const XMVECTOR pa=XMLoadFloat3(&a),pb=XMLoadFloat3(&b);
                const XMVECTOR axis=XMVector3Normalize(XMVectorSubtract(pb,pa));
                const XMVECTOR other=std::abs(XMVectorGetY(axis))<0.9f?XMVectorSet(0,1,0,0):XMVectorSet(1,0,0,0);
                const XMVECTOR side=XMVector3Normalize(XMVector3Cross(axis,other)),up=XMVector3Cross(side,axis);
                const float slope=(ra-rb)/std::max(0.001f,XMVectorGetX(XMVector3Length(XMVectorSubtract(pb,pa))));
                for(int i=0;i<segments;++i)
                {
                    const float u0=XM_2PI*i/segments,u1=XM_2PI*(i+1)/segments;
                    const XMVECTOR d0=XMVectorAdd(XMVectorScale(side,std::cos(u0)),XMVectorScale(up,std::sin(u0)));
                    const XMVECTOR d1=XMVectorAdd(XMVectorScale(side,std::cos(u1)),XMVectorScale(up,std::sin(u1)));
                    const XMVECTOR n0=XMVectorAdd(d0,XMVectorScale(axis,slope)),n1=XMVectorAdd(d1,XMVectorScale(axis,slope));
                    const XMVECTOR q[4]={XMVectorAdd(pa,XMVectorScale(d0,ra)),XMVectorAdd(pa,XMVectorScale(d1,ra)),
                                         XMVectorAdd(pb,XMVectorScale(d1,rb)),XMVectorAdd(pb,XMVectorScale(d0,rb))};
                    const XMVECTOR qn[4]={n0,n1,n1,n0};
                    for(int k:{0,1,2,0,2,3})Vertex(q[k],qn[k],c);
                    if(ra>0.01f){Vertex(pa,XMVectorNegate(axis),c);Vertex(q[1],XMVectorNegate(axis),c);Vertex(q[0],XMVectorNegate(axis),c);}
                    if(rb>0.01f){Vertex(pb,axis,c);Vertex(q[2],axis,c);Vertex(q[3],axis,c);}
                }
            }

            // A box around centre with half sides, turned by (pitch, yaw, roll).
            void Box(XMFLOAT3 centre,XMFLOAT3 half,const Colour& c,XMFLOAT3 turn={0,0,0})
            {
                const XMMATRIX r=XMMatrixRotationRollPitchYaw(turn.x,turn.y,turn.z);
                const XMVECTOR o=XMLoadFloat3(&centre);
                auto corner=[&](float x,float y,float z){return XMVectorAdd(o,XMVector3TransformNormal(XMVectorSet(x*half.x,y*half.y,z*half.z,0),r));};
                // Each face: its normal and its corners (as signs of the half sides).
                struct Face {float n[3];float p[4][3];};
                static constexpr Face faces[6]={
                    {{1,0,0},{{1,-1,-1},{1,1,-1},{1,1,1},{1,-1,1}}},{{-1,0,0},{{-1,-1,-1},{-1,-1,1},{-1,1,1},{-1,1,-1}}},
                    {{0,1,0},{{-1,1,-1},{-1,1,1},{1,1,1},{1,1,-1}}},{{0,-1,0},{{-1,-1,-1},{1,-1,-1},{1,-1,1},{-1,-1,1}}},
                    {{0,0,1},{{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}}},{{0,0,-1},{{-1,-1,-1},{-1,1,-1},{1,1,-1},{1,-1,-1}}}};
                for(const auto& f:faces)
                {
                    const XMVECTOR n=XMVector3TransformNormal(XMVectorSet(f.n[0],f.n[1],f.n[2],0),r);
                    XMVECTOR p[4];
                    for(int k=0;k<4;++k)p[k]=corner(f.p[k][0],f.p[k][1],f.p[k][2]);
                    for(int k:{0,1,2,0,2,3})Vertex(p[k],n,c);
                }
            }

            // Both sides at once: f(side) with side +1 and -1 (mirrored across x).
            template<class F> void Both(F f){f(1.0f);f(-1.0f);}
        };

        Colour Tint(std::uint32_t argb,float k=1)
        {
            return {((argb>>16)&255)/255.0f*k,((argb>>8)&255)/255.0f*k,(argb&255)/255.0f*k};
        }

        const Colour Black{0.05f,0.04f,0.06f},White{0.95f,0.95f,0.92f};

        float Slime(Shapes& s,const Dungeon::Monster& m)
        {
            // A soft drop of jelly: wide at the foot, a little tip on top, a face, a shine.
            const Colour body=Tint(m.color),foot=Tint(m.color,0.8f);
            s.Ellipsoid({0,1.0f,0},{3.9f,1.2f,3.9f},foot);
            s.Ellipsoid({0,2.4f,0},{3.4f,2.6f,3.4f},body);
            s.Limb({0,4.6f,0},{0,6.0f,-0.4f},1.0f,0.05f,body);
            s.Both([&](float o){
                s.Ellipsoid({o*1.1f,3.0f,2.9f},{0.45f,0.62f,0.3f},Black);
                s.Ellipsoid({o*1.0f,3.25f,3.12f},{0.14f,0.14f,0.08f},{1,1,1,Glow});
            });
            s.Ellipsoid({0,1.9f,3.15f},{0.7f,0.28f,0.2f},Black);
            s.Ellipsoid({-1.4f,4.1f,1.4f},{0.8f,0.4f,0.5f},{0.92f,1.0f,0.92f},{0,0,0.5f});
            return 6.0f;
        }

        float Bat(Shapes& s,const Dungeon::Monster& m)
        {
            // Hovering at her chest's height, wings spread, ears up, red eyes; its shadow below.
            const Colour body=Tint(m.color,0.7f),wing=Tint(m.color,0.5f);
            s.Ellipsoid({0,0.05f,0},{2.4f,0.02f,2.0f},{0.08f,0.07f,0.09f});
            s.Ellipsoid({0,9.0f,0},{1.3f,1.7f,1.2f},body);
            s.Ellipsoid({0,11.0f,0.4f},{1.05f,1.0f,1.0f},body);
            s.Both([&](float o){
                s.Limb({o*0.5f,11.6f,0.3f},{o*1.0f,13.2f,0.1f},0.38f,0.02f,body,6);
                s.Ellipsoid({o*0.42f,11.15f,1.32f},{0.18f,0.16f,0.1f},{1.0f,0.25f,0.2f,Glow});
                const XMFLOAT3 shoulder{o*1.0f,9.8f,0};
                const XMFLOAT3 tips[5]={{o*3.4f,11.2f,-0.4f},{o*7.0f,12.0f,-0.8f},{o*6.0f,8.6f,-0.9f},{o*4.0f,7.4f,-0.6f},{o*2.0f,7.9f,-0.3f}};
                for(int i=0;i+1<5;++i)s.Triangle(shoulder,tips[i],tips[i+1],wing);
                s.Limb(shoulder,tips[0],0.22f,0.15f,body,5);s.Limb(tips[0],tips[1],0.15f,0.05f,body,5);
                s.Limb({o*0.6f,7.6f,0},{o*0.7f,6.6f,0.2f},0.18f,0.1f,body,5);
            });
            s.Triangle({-0.3f,10.6f,1.3f},{0.3f,10.6f,1.3f},{0,10.1f,1.45f},White);
            return 13.5f;
        }

        float Goblin(Shapes& s,const Dungeon::Monster& m)
        {
            // Small and wiry, big pointed ears, a long nose, yellow eyes, a ragged tunic and a knife.
            const Colour skin=Tint(m.color),tunic{0.42f,0.30f,0.18f},belt{0.22f,0.15f,0.09f},metal{0.70f,0.72f,0.76f};
            s.Both([&](float o){
                s.Limb({o*0.9f,5.2f,0},{o*1.05f,0.9f,0.25f},0.62f,0.45f,skin);
                s.Box({o*1.05f,0.45f,0.7f},{0.55f,0.45f,0.9f},belt);
                s.Limb({o*1.9f,8.1f,0},{o*2.5f,5.8f,1.0f},0.5f,0.42f,skin);
                s.Ellipsoid({o*2.55f,5.5f,1.25f},{0.55f,0.55f,0.55f},skin);
                s.Limb({o*1.55f,10.2f,0},{o*3.8f,11.0f,-0.7f},0.6f,0.02f,skin,6);
                s.Ellipsoid({o*0.62f,10.25f,1.62f},{0.24f,0.2f,0.12f},{1.0f,0.85f,0.2f,Glow});
            });
            s.Ellipsoid({0,6.6f,0},{2.0f,2.3f,1.5f},tunic);
            s.Limb({0,5.2f,0},{0,5.6f,0},2.05f,1.95f,belt);
            s.Ellipsoid({0,9.8f,0.3f},{1.8f,1.7f,1.6f},skin);
            s.Limb({0,9.7f,1.6f},{0,9.25f,2.7f},0.38f,0.05f,skin,6);
            s.Ellipsoid({0,9.0f,1.65f},{0.7f,0.18f,0.2f},Black);
            // The knife in its right hand.
            s.Box({2.55f,5.7f,2.6f},{0.12f,0.32f,1.2f},metal);
            s.Box({2.55f,5.6f,1.4f},{0.3f,0.45f,0.18f},belt);
            return 11.6f;
        }

        float Skeleton(Shapes& s,const Dungeon::Monster& m)
        {
            // Bare bones: a skull with dark sockets and a red glint, ribs, thin limbs, a rusty sword.
            const Colour bone=Tint(m.color),dim=Tint(m.color,0.8f),rust{0.55f,0.38f,0.26f};
            s.Both([&](float o){
                s.Limb({o*0.9f,8.0f,0},{o*1.0f,4.3f,0.35f},0.36f,0.3f,bone,6);
                s.Limb({o*1.0f,4.3f,0.35f},{o*1.0f,0.7f,0},0.3f,0.26f,bone,6);
                s.Ellipsoid({o*1.0f,4.3f,0.35f},{0.45f,0.45f,0.45f},dim);
                s.Box({o*1.0f,0.35f,0.5f},{0.5f,0.3f,0.9f},bone);
                s.Limb({o*2.1f,13.0f,0},{o*2.5f,10.4f,0.7f},0.3f,0.26f,bone,6);
                s.Limb({o*2.5f,10.4f,0.7f},{o*2.3f,8.4f,1.7f},0.26f,0.22f,bone,6);
                s.Ellipsoid({o*0.5f,14.8f,1.38f},{0.38f,0.44f,0.2f},Black);
                s.Ellipsoid({o*0.5f,14.75f,1.5f},{0.1f,0.1f,0.06f},{1.0f,0.2f,0.15f,Glow});
            });
            s.Box({0,8.2f,0},{1.4f,0.6f,0.7f},bone);
            s.Limb({0,8.6f,-0.2f},{0,13.0f,-0.2f},0.32f,0.3f,dim,6);
            for(int i=0;i<4;++i)s.Ellipsoid({0,9.9f+0.75f*i,0.35f},{1.75f-0.08f*i,0.17f,1.05f},bone,{0,0,0},12,4);
            s.Box({0,13.05f,0},{2.1f,0.25f,0.35f},bone);
            s.Limb({0,13.2f,0},{0,13.9f,0.1f},0.25f,0.25f,bone,6);
            s.Ellipsoid({0,14.7f,0.2f},{1.3f,1.45f,1.35f},bone);
            s.Box({0,13.45f,0.75f},{0.8f,0.32f,0.6f},dim);
            s.Triangle({-0.2f,14.1f,1.55f},{0.2f,14.1f,1.55f},{0,14.5f,1.5f},Black);
            s.Box({2.3f,9.6f,2.8f},{0.14f,2.4f,0.32f},rust,{0.5f,0,0});
            s.Box({2.3f,8.4f,1.75f},{0.6f,0.15f,0.2f},{0.3f,0.2f,0.12f});
            return 16.4f;
        }

        float Orc(Shapes& s,const Dungeon::Monster& m)
        {
            // Big and heavy: a broad chest, a small brow over red eyes, tusks, and a club.
            const Colour skin=Tint(m.color,0.9f),dark=Tint(m.color,0.6f),cloth{0.30f,0.24f,0.20f},tusk{0.92f,0.88f,0.75f},wood{0.40f,0.26f,0.14f};
            s.Both([&](float o){
                s.Limb({o*1.6f,8.4f,0},{o*1.8f,1.0f,0.2f},1.25f,1.0f,cloth);
                s.Box({o*1.8f,0.5f,0.7f},{0.95f,0.5f,1.4f},dark);
                s.Ellipsoid({o*3.6f,13.4f,0},{1.5f,1.3f,1.4f},skin);
                s.Limb({o*3.9f,13.0f,0.2f},{o*4.5f,8.8f,1.3f},1.05f,0.85f,skin);
                s.Ellipsoid({o*4.6f,8.2f,1.5f},{1.0f,1.0f,1.0f},skin);
                s.Limb({o*0.75f,15.3f,2.15f},{o*0.95f,16.6f,2.45f},0.3f,0.02f,tusk,6);
                s.Ellipsoid({o*0.7f,16.7f,1.95f},{0.22f,0.2f,0.12f},{1.0f,0.25f,0.15f,Glow});
            });
            s.Ellipsoid({0,8.8f,0},{3.0f,1.6f,2.2f},cloth);
            s.Ellipsoid({0,11.6f,0},{3.7f,3.4f,2.5f},skin);
            s.Ellipsoid({0,16.3f,0.7f},{1.9f,1.8f,1.9f},skin);
            s.Box({0,17.1f,2.05f},{1.4f,0.25f,0.3f},dark);
            s.Box({0,15.4f,1.6f},{1.2f,0.6f,0.9f},skin);
            // The club in its right hand, raised a little.
            s.Limb({4.6f,7.8f,1.6f},{4.9f,12.8f,4.2f},0.35f,0.95f,wood);
            return 18.6f;
        }

        float Golem(Shapes& s,const Dungeon::Monster& m)
        {
            // Rough blocks of stone, slightly askew, a glowing rune on the chest and glowing eyes.
            const Colour stone=Tint(m.color),dark=Tint(m.color,0.75f),glow{0.45f,0.95f,1.0f,Glow};
            s.Both([&](float o){
                s.Box({o*1.9f,3.4f,0},{1.4f,3.4f,1.5f},dark,{0.05f*o,0,0.04f});
                s.Box({o*2.0f,0.6f,0.6f},{1.6f,0.6f,2.0f},dark);
                s.Box({o*5.0f,14.6f,0},{1.8f,1.6f,1.8f},stone,{0.1f,0.2f*o,0.15f*o});
                s.Box({o*5.6f,10.0f,0.6f},{1.3f,3.2f,1.3f},dark,{-0.15f,0,0.06f*o});
                s.Box({o*5.8f,5.8f,1.2f},{1.6f,1.4f,1.6f},stone,{0.2f,0.3f*o,0});
                s.Box({o*0.6f,18.0f,1.85f},{0.32f,0.18f,0.1f},glow);
            });
            s.Box({0,8.6f,0},{3.2f,1.8f,2.2f},dark,{0,0.05f,0});
            s.Box({0,13.2f,0},{4.0f,3.3f,2.6f},stone,{0.04f,-0.04f,0.02f});
            s.Box({0,18.0f,0.6f},{1.7f,1.3f,1.4f},stone,{0.05f,0.08f,0});
            s.Box({0,13.4f,2.62f},{0.9f,0.15f,0.05f},glow);
            s.Box({0,13.4f,2.62f},{0.15f,1.0f,0.05f},glow);
            return 19.6f;
        }

        float Ghost(Shapes& s,const Dungeon::Monster& m)
        {
            // A pale glow drifting above the floor: a round head-body trailing into a wisp,
            // arms reaching out, dark hollow eyes and mouth.
            const Colour spirit{((m.color>>16)&255)/255.0f*0.85f,((m.color>>8)&255)/255.0f*0.85f,(m.color&255)/255.0f*0.9f,Glow};
            s.Ellipsoid({0,10.0f,0},{2.6f,3.0f,2.4f},spirit);
            s.Limb({0,8.6f,0},{0.4f,5.0f,-1.2f},2.3f,1.2f,spirit);
            s.Limb({0.4f,5.0f,-1.2f},{-0.6f,2.4f,-2.4f},1.2f,0.1f,spirit);
            s.Both([&](float o){
                s.Limb({o*2.2f,10.0f,0.5f},{o*3.8f,8.4f,2.9f},0.75f,0.3f,spirit);
                s.Ellipsoid({o*0.9f,10.8f,2.1f},{0.5f,0.7f,0.3f},Black);
            });
            s.Ellipsoid({0,9.0f,2.25f},{0.55f,0.8f,0.3f},Black);
            return 13.2f;
        }

        float Dragon(Shapes& s,const Dungeon::Monster& m)
        {
            // Four legs, a long neck and tail, horns, spines down the back and great wings.
            const Colour scale=Tint(m.color,0.85f),dark=Tint(m.color,0.55f),belly{0.95f,0.75f,0.45f},horn{0.92f,0.86f,0.72f};
            const Colour wing{0.50f,0.12f,0.12f},eye{1.0f,0.85f,0.2f,Glow};
            s.Ellipsoid({0,9.0f,-1.0f},{4.2f,4.0f,6.5f},scale);
            s.Ellipsoid({0,7.6f,0},{3.4f,2.8f,5.2f},belly);
            s.Limb({0,11.0f,3.6f},{0,17.0f,8.0f},2.3f,1.4f,scale);
            s.Ellipsoid({0,18.2f,9.6f},{2.0f,1.9f,2.6f},scale);
            s.Box({0,17.5f,12.1f},{1.35f,1.0f,1.6f},scale,{0.1f,0,0});
            s.Box({0,16.6f,12.0f},{1.2f,0.35f,1.5f},belly,{0.1f,0,0});
            const XMFLOAT3 tail[5]={{0,8.0f,-7.0f},{0,6.2f,-11.5f},{-1.5f,4.4f,-15.5f},{-3.2f,3.2f,-18.5f},{-4.6f,2.6f,-20.5f}};
            const float tailR[5]={2.4f,1.7f,1.1f,0.6f,0.1f};
            for(int i=0;i+1<5;++i)s.Limb(tail[i],tail[i+1],tailR[i],tailR[i+1],scale);
            for(int i=0;i<7;++i)
            {
                const float z=6.0f-2.4f*i,y=13.0f-0.55f*i*(i>3?1.6f:1.0f);
                s.Limb({0,y-0.3f,z},{0,y+1.4f-0.12f*i,z-0.8f},0.6f-0.05f*i,0.02f,dark,5);
            }
            s.Both([&](float o){
                s.Limb({o*3.1f,7.5f,3.2f},{o*3.6f,1.0f,4.4f},1.5f,1.0f,scale);
                s.Limb({o*3.4f,7.5f,-4.6f},{o*3.9f,1.0f,-4.2f},1.7f,1.1f,scale);
                s.Ellipsoid({o*3.6f,0.6f,5.0f},{1.2f,0.6f,1.6f},dark);
                s.Ellipsoid({o*3.9f,0.6f,-3.6f},{1.3f,0.6f,1.7f},dark);
                s.Limb({o*0.9f,19.4f,9.0f},{o*1.7f,22.6f,6.4f},0.55f,0.02f,horn,6);
                s.Ellipsoid({o*1.25f,18.8f,11.0f},{0.32f,0.3f,0.2f},eye);
                // The wing: a bone out from the shoulder, the skin hung from it in a fan.
                const XMFLOAT3 shoulder{o*3.0f,13.0f,1.0f},elbow{o*8.5f,18.5f,-0.5f},tip{o*14.0f,20.0f,-3.0f};
                const XMFLOAT3 edge[4]={{o*14.5f,12.5f,-6.0f},{o*11.0f,9.5f,-7.5f},{o*7.0f,9.8f,-7.0f},{o*3.6f,11.0f,-4.5f}};
                s.Limb(shoulder,elbow,0.6f,0.45f,dark);
                s.Limb(elbow,tip,0.45f,0.1f,dark);
                s.Triangle(shoulder,elbow,edge[3],wing);
                s.Triangle(elbow,edge[2],edge[3],wing);
                s.Triangle(elbow,edge[1],edge[2],wing);
                s.Triangle(elbow,tip,edge[1],wing);
                s.Triangle(tip,edge[0],edge[1],wing);
            });
            return 23.0f;
        }
    }

    float AddMonster(std::vector<MascotRenderer::GeometryVertex>& out,const Dungeon::Monster& m,float x,float z,float yaw)
    {
        // The dragon is huge where its body takes 3x3 tiles (its wings and tail still reach
        // past them); one squeezed into a single tile is drawn smaller.
        const float scale=m.boss?(m.size>0?1.45f:0.8f):1.0f;
        Shapes s{out,XMMatrixScaling(scale,scale,scale)*XMMatrixRotationY(yaw)*XMMatrixTranslation(x,0,z)};
        float height=0;
        if(m.boss)height=Dragon(s,m);
        else if(m.id==L"Slime")height=Slime(s,m);
        else if(m.id==L"Bat")height=Bat(s,m);
        else if(m.id==L"Goblin")height=Goblin(s,m);
        else if(m.id==L"Skeleton")height=Skeleton(s,m);
        else if(m.id==L"Orc")height=Orc(s,m);
        else if(m.id==L"Golem")height=Golem(s,m);
        else height=Ghost(s,m);
        return height*scale;
    }
}
