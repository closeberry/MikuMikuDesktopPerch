#pragma once
// Simple solids for the dungeon's things (monsters, chests, goods...): ellipsoids, tapered
// limbs, boxes and flat triangles in plain colours, added to geometry for MascotRenderer.
// Include from .cpp files only (it brings in DirectXMath's names).
#include "MascotRenderer.hpp"
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace MikuMikuDesktopPerch::Art
{
    using namespace DirectX;

    // The geometry shader's patterns by the colour's alpha (see GeometryPS.hlsl), patterned from
    // the place on the thing itself (so they do not slide as it moves): a monster's skin with
    // grime on it, wood, metal (with a shine), stone; plain lit; or glowing (unlit, for eyes,
    // spirits, flames and crystals).
    constexpr float Grimy=0,Wood=0.02f,Metal=0.04f,Stone=0.06f,Glow=0.25f,Lit=1;

    struct Colour {float r,g,b,a=Grimy;};

    // Solids in a thing's own frame (+x its right, y up, +z the way it faces), placed in the
    // world by one matrix; local moves a part (a limb turned at its joint) within it. shade dims
    // it (a thing remembered, out of sight).
    struct Shapes
    {
        std::vector<MascotRenderer::GeometryVertex>& out;
        DirectX::XMMATRIX world;
        DirectX::XMMATRIX local=DirectX::XMMatrixIdentity();
        float shade=1;

        // Draws f with the part moved by m (on top of what moves it already).
        template<class F> void With(FXMMATRIX m,F f){const XMMATRIX saved=local;local=m*local;f();local=saved;}
        // Turned about a joint at pivot by (pitch, yaw, roll).
        static XMMATRIX About(XMFLOAT3 pivot,float pitch,float yaw,float roll=0)
        {
            return XMMatrixTranslation(-pivot.x,-pivot.y,-pivot.z)*XMMatrixRotationRollPitchYaw(pitch,yaw,roll)*XMMatrixTranslation(pivot.x,pivot.y,pivot.z);
        }

        void Vertex(FXMVECTOR p,FXMVECTOR n,const Colour& c)
        {
            MascotRenderer::GeometryVertex v{};
            XMFLOAT3 wp,wn;
            const XMMATRIX to=local*world;
            XMStoreFloat3(&wp,XMVector3TransformCoord(p,to));
            XMStoreFloat3(&wn,XMVector3Normalize(XMVector3TransformNormal(n,to)));
            v.position[0]=wp.x;v.position[1]=wp.y;v.position[2]=wp.z;
            v.normal[0]=wn.x;v.normal[1]=wn.y;v.normal[2]=wn.z;
            v.color[0]=c.r*shade;v.color[1]=c.g*shade;v.color[2]=c.b*shade;v.color[3]=c.a;
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

    inline Colour Tint(std::uint32_t argb,float k=1)
    {
        return {((argb>>16)&255)/255.0f*k,((argb>>8)&255)/255.0f*k,(argb&255)/255.0f*k};
    }

    const Colour Black{0.05f,0.04f,0.06f},White{0.95f,0.95f,0.92f};
}
