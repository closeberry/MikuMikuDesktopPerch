#define NOMINMAX
#include "WesternRoom.hpp"
#include "Log.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>

namespace MikuMikuDesktopPerch
{
    namespace
    {
        using V=std::array<float,3>;
        // The patterns GeometryPS.hlsl draws for this room (its colour's alpha 0.12, the kind as
        // minus uv's z); wall holds where on the thing the vertex is.
        enum Kind {Wallpaper=1,Parquet,Rug,Painting,Curtain};
        constexpr float WoodLook=0.10f,PatternLook=0.12f,GlowLook=0.25f;

        struct Colour {float r,g,b;};
        constexpr Colour Mahogany{0.23f,0.105f,0.06f},Walnut{0.17f,0.09f,0.05f},Moulding{0.29f,0.15f,0.08f},Brass{0.74f,0.56f,0.27f},
                         PaperRed{0.42f,0.085f,0.10f},Plaster{0.62f,0.56f,0.46f},Stone{0.56f,0.52f,0.46f},Drape{0.40f,0.07f,0.09f},Sash{0.16f,0.085f,0.05f};

        struct Builder
        {
            std::vector<MascotRenderer::GeometryVertex>& out;
            int grain=0;           // the way wood runs: 0 along x, 1 along z, 2 upright
            float grainScale=1;

            void Face(const V& a,const V& b,const V& c,const V& d,const V& n,const float (&col)[4],const float* local=nullptr,float kind=0)
            {
                const V* p[4]={&a,&b,&c,&d};
                for(int k:{0,1,2,0,2,3})
                {
                    const V& q=*p[k];
                    MascotRenderer::GeometryVertex v{{q[0],q[1],q[2]},{n[0],n[1],n[2]},{col[0],col[1],col[2],col[3]}};
                    if(col[3]==WoodLook)
                    {
                        const float along=q[grain==0?0:grain==1?2:1],up=q[grain==2?0:1],across=q[grain==1?0:2];
                        v.wall[0]=along/grainScale;v.wall[1]=up/grainScale;v.wall[3]=across/grainScale;
                    }
                    if(local)
                    {
                        v.wall[0]=local[k*2];v.wall[1]=local[k*2+1];
                        v.uv[2]=-kind;
                    }
                    out.push_back(v);
                }
            }
            void Plain(const V& a,const V& b,const V& c,const V& d,const V& n,Colour c0)
            {
                const float col[4]={c0.r,c0.g,c0.b,1};
                Face(a,b,c,d,n,col);
            }
            void Box(float x0,float y0,float z0,float x1,float y1,float z1,const float (&c)[4])
            {
                Face({x0,y1,z0},{x1,y1,z0},{x1,y1,z1},{x0,y1,z1},{0,1,0},c);
                Face({x0,y0,z0},{x1,y0,z0},{x1,y0,z1},{x0,y0,z1},{0,-1,0},c);
                Face({x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},{0,0,-1},c);
                Face({x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1},{0,0,1},c);
                Face({x0,y0,z0},{x0,y0,z1},{x0,y1,z1},{x0,y1,z0},{-1,0,0},c);
                Face({x1,y0,z0},{x1,y0,z1},{x1,y1,z1},{x1,y1,z0},{1,0,0},c);
            }
            void Wood(float x0,float y0,float z0,float x1,float y1,float z1,Colour c0,int way,float scale=0.3f)
            {
                grain=way;
                grainScale=scale;
                const float c[4]={c0.r,c0.g,c0.b,WoodLook};
                Box(x0,y0,z0,x1,y1,z1,c);
            }
            void Solid(float x0,float y0,float z0,float x1,float y1,float z1,Colour c0)
            {
                const float c[4]={c0.r,c0.g,c0.b,1};
                Box(x0,y0,z0,x1,y1,z1,c);
            }
            void Glowing(float x0,float y0,float z0,float x1,float y1,float z1,Colour c0)
            {
                const float c[4]={c0.r,c0.g,c0.b,GlowLook};
                Box(x0,y0,z0,x1,y1,z1,c);
            }
            // A quad patterned by kind, its corners at these places on the pattern.
            void Patterned(const V& a,const V& b,const V& c,const V& d,const V& n,Colour c0,Kind kind,float u0,float v0,float u1,float v1)
            {
                const float col[4]={c0.r,c0.g,c0.b,PatternLook};
                const float local[8]={u0,v0,u1,v0,u1,v1,u0,v1};
                Face(a,b,c,d,n,col,local,static_cast<float>(kind));
            }
        };

        // Wallpaper from wall to ceiling over raised panels, with the rail, skirting and cornice.
        // from, to: the wall's ends on the floor; inward: the way it faces (x or z, +1 or -1).
        void AddWall(Builder& b,const V& from,const V& to,const V& inward,const WesternRoomSize& s,float wain,float ceiling)
        {
            const float D=s.upper,floor=s.floor;
            const bool alongX=std::abs(from[0]-to[0])>std::abs(from[2]-to[2]);
            const float len=alongX?std::abs(to[0]-from[0]):std::abs(to[2]-from[2]);
            auto point=[&](float u,float y,float out=0){
                return V{from[0]+(to[0]-from[0])*u/len+inward[0]*out,y,from[2]+(to[2]-from[2])*u/len+inward[2]*out};
            };
            auto span=[&](float u0,float y0,float u1,float y1,float out0,float out1,Colour c,bool wood){
                // A box along the wall between u0 and u1, standing out0 to out1 from it.
                const V a=point(u0,y0,out0),d=point(u1,y1,out1);
                const float x0=std::min(a[0],d[0]),x1=std::max(a[0],d[0]),z0=std::min(a[2],d[2]),z1=std::max(a[2],d[2]);
                if(wood)b.Wood(x0,y0,z0,x1,y1,z1,c,alongX?0:1);
                else b.Solid(x0,y0,z0,x1,y1,z1,c);
            };
            b.Patterned(point(0,wain),point(len,wain),point(len,ceiling),point(0,ceiling),inward,PaperRed,Wallpaper,0,wain/D,len/D,ceiling/D);
            // The panelling: a flat back and raised panels with frames.
            span(0,floor,len,wain,0,0.015f*D,Mahogany,true);
            const float panel=1.05f*D;
            const int count=std::max(1,static_cast<int>(len/panel));
            const float step=len/count,inset=0.09f*D;
            for(int i=0;i<count;++i)
            {
                const float u0=i*step+inset,u1=(i+1)*step-inset,y0=floor+0.16f*D,y1=wain-0.09f*D;
                span(u0,y0,u1,y1,0.015f*D,0.035f*D,Moulding,true);
                const float e=0.035f*D;
                span(u0+e,y0+e,u1-e,y1-e,0.035f*D,0.05f*D,Mahogany,true);
            }
            span(0,wain-0.03f*D,len,wain+0.05f*D,0,0.065f*D,Moulding,true);       // the rail
            span(0,floor,len,floor+0.13f*D,0,0.05f*D,Walnut,true);                // the skirting
            span(0,ceiling-0.20f*D,len,ceiling,0,0.09f*D,Moulding,true);          // the cornice
            span(0,ceiling-0.26f*D,len,ceiling-0.22f*D,0,0.04f*D,Brass,false);
        }

        // A tall window with its sash bars, night behind it, curtains drawn to each side and a pelmet.
        void AddWindow(Builder& b,std::vector<MascotRenderer::Particle>& glows,float cx,float z,float y0,float y1,float halfWidth,float D)
        {
            const float glass[4]={0.11f,0.16f,0.30f,GlowLook};
            b.Face({cx-halfWidth,y0,z},{cx+halfWidth,y0,z},{cx+halfWidth,y1,z},{cx-halfWidth,y1,z},{0,0,-1},glass);
            const float bar=0.018f*D,edge=0.055f*D,out=0.05f*D;
            for(int i=0;i<=4;++i)
            {
                const float x=cx-halfWidth+2*halfWidth*i/4,w=i==0||i==4?edge:bar;
                b.Solid(x-w,y0-edge,z-out,x+w,y1+edge,z,Sash);
            }
            for(int i=0;i<=5;++i)
            {
                const float y=y0+(y1-y0)*i/5,w=i==0||i==5?edge:bar;
                b.Solid(cx-halfWidth-edge,y-w,z-out,cx+halfWidth+edge,y+w,z,Sash);
            }
            b.Solid(cx-halfWidth-2*edge,y0-2.2f*edge,z-2.2f*out,cx+halfWidth+2*edge,y0-edge,z,Sash);   // the sill
            // The curtains, in folds, gathered a little narrower toward the floor side.
            const float drop0=y0-0.42f*D,top=y1+0.20f*D,width=0.44f*halfWidth+0.22f*D,zc=z-3.2f*out;   // (in front of the sill)
            for(float side:{-1.0f,1.0f})
            {
                const float inner=cx+side*(halfWidth-0.42f*width),outer=cx+side*(halfWidth+0.58f*width);
                b.Patterned({outer,drop0,zc},{inner,drop0,zc},{inner,top,zc},{outer,top,zc},{0,0,-1},Drape,Curtain,0,drop0/D,width/D,top/D);
                // The tie-back, of the same cloth: from the wall's side it hangs down toward the window's middle.
                const float tieY=y0+0.20f*(y1-y0),sag=0.17f*D,band=0.075f*D,tz=zc-0.035f*D;
                constexpr int Steps=10;
                auto at=[&](int k,float drop,float forward){
                    const float u=static_cast<float>(k)/Steps;
                    return V{outer+(inner-outer)*0.88f*u,tieY-sag*u*u-drop*(1-0.25f*std::sin(u*3.1416f)),tz-forward};
                };
                for(int k=0;k<Steps;++k)
                {
                    b.Plain(at(k,band,0.012f*D),at(k+1,band,0.012f*D),at(k+1,0,0.012f*D),at(k,0,0.012f*D),{0,0,-1},Drape);
                    b.Plain(at(k,0,0),at(k+1,0,0),at(k+1,0,0.012f*D),at(k,0,0.012f*D),{0,1,0},Drape);
                    b.Plain(at(k,band,0),at(k+1,band,0),at(k+1,band,0.012f*D),at(k,band,0.012f*D),{0,-1,0},Drape);
                }
            }
            const float over=halfWidth+0.70f*width;
            b.Solid(cx-over,top-0.03f*D,zc-0.08f*D,cx+over,top+0.17f*D,z,Moulding);        // the pelmet
            b.Solid(cx-over,top-0.06f*D,zc-0.095f*D,cx+over,top-0.03f*D,z,Brass);
            for(int j=0;j<2;++j)
            {
                MascotRenderer::Particle g;
                g.position={cx,y0+(y1-y0)*(0.3f+0.4f*j),z-0.1f*D};
                g.size=1.5f*halfWidth;
                g.color={0.36f,0.48f,0.85f,0.07f};
                g.glow=true;
                glows.push_back(g);
            }
        }

        void AddFireplace(Builder& b,std::vector<MascotRenderer::Particle>& glows,float z,float floor,float ceiling,float D)
        {
            const float half=0.95f*D,tall=1.25f*D,deep=0.32f*D,open=0.56f*D;
            b.Solid(-half,floor,z-deep,-open,floor+tall,z,Stone);
            b.Solid(open,floor,z-deep,half,floor+tall,z,Stone);
            b.Solid(-open,floor+0.82f*D,z-deep,open,floor+tall,z,Stone);
            b.Wood(-half-0.12f*D,floor+tall,z-deep-0.09f*D,half+0.12f*D,floor+tall+0.09f*D,z,Mahogany,0);   // the mantel
            b.Solid(-half-0.2f*D,floor,z-deep-0.26f*D,half+0.2f*D,floor+0.035f*D,z,Stone);                  // the hearth
            const float back[4]={0.05f,0.035f,0.03f,1};
            b.Face({-open,floor,z-0.02f*D},{open,floor,z-0.02f*D},{open,floor+0.82f*D,z-0.02f*D},{-open,floor+0.82f*D,z-0.02f*D},{0,0,-1},back);
            // The fire: logs and a few tongues of light.
            b.Solid(-0.36f*D,floor+0.05f*D,z-0.22f*D,0.36f*D,floor+0.13f*D,z-0.12f*D,Walnut);
            std::mt19937 r(7);
            std::uniform_real_distribution<float> any(0,1);
            for(int i=0;i<7;++i)
            {
                const float x=(i-3)*0.10f*D,tallFlame=(0.16f+0.26f*any(r)*(1-std::abs(i-3)/5.0f))*D;
                b.Glowing(x-0.035f*D,floor+0.12f*D,z-0.20f*D,x+0.035f*D,floor+0.12f*D+tallFlame,z-0.15f*D,{1.0f,0.48f+0.2f*any(r),0.12f});
            }
            for(const auto& [size,alpha]:{std::pair{0.55f,0.55f},std::pair{1.3f,0.26f}})
            {
                MascotRenderer::Particle g;
                g.position={0,floor+0.34f*D,z-0.3f*D};
                g.size=size*D;
                g.color={1.0f,0.56f,0.20f,alpha};
                g.glow=true;
                glows.push_back(g);
            }
            // A mirror in a gilt frame over it.
            const float y0=floor+tall+0.26f*D,y1=std::min(ceiling-0.42f*D,y0+1.5f*D),mw=0.72f*D;
            b.Solid(-mw-0.07f*D,y0-0.07f*D,z-0.04f*D,mw+0.07f*D,y1+0.07f*D,z,Brass);
            const float glass[4]={0.42f,0.42f,0.44f,1};
            b.Face({-mw,y0,z-0.045f*D},{mw,y0,z-0.045f*D},{mw,y1,z-0.045f*D},{-mw,y1,z-0.045f*D},{0,0,-1},glass);
        }

        void AddSconce(Builder& b,std::vector<MascotRenderer::Particle>& glows,float x,float y,float z,float D)
        {
            b.Solid(x-0.05f*D,y-0.10f*D,z-0.015f*D,x+0.05f*D,y+0.10f*D,z,Brass);
            b.Solid(x-0.018f*D,y-0.03f*D,z-0.13f*D,x+0.018f*D,y,z,Brass);
            b.Solid(x-0.05f*D,y,z-0.17f*D,x+0.05f*D,y+0.02f*D,z-0.08f*D,Brass);
            b.Solid(x-0.015f*D,y+0.02f*D,z-0.14f*D,x+0.015f*D,y+0.12f*D,z-0.11f*D,{0.85f,0.82f,0.72f});
            b.Glowing(x-0.012f*D,y+0.12f*D,z-0.135f*D,x+0.012f*D,y+0.18f*D,z-0.115f*D,{1.0f,0.82f,0.48f});
            MascotRenderer::Particle g;
            g.position={x,y+0.16f*D,z-0.15f*D};
            g.size=0.55f*D;
            g.color={1.0f,0.74f,0.42f,0.42f};
            g.glow=true;
            glows.push_back(g);
        }

        // A model built in (tools: build_model.py; resource type ROOMMODEL), scaled: its middle at
        // (x, z), its foot at y. quarter: turned so that what faces -Z in its file faces -X
        // (against a wall at +X, with z then its back). lights, sparks: where its flames and pieces of
        // glass are, added here.
        bool AddModel(std::vector<MascotRenderer::GeometryVertex>& out,std::vector<V>* lights,const wchar_t* name,float x,float y,float z,float height,bool quarter,float* scaleOut=nullptr,std::vector<V>* sparks=nullptr)
        {
            const HRSRC info=FindResourceW(nullptr,name,L"ROOMMODEL");
            const HGLOBAL data=info?LoadResource(nullptr,info):nullptr;
            if(!data)return false;
            const auto* bytes=static_cast<const std::uint8_t*>(LockResource(data));
            const size_t size=SizeofResource(nullptr,info);
            std::uint32_t head[5];
            float lo[3],hi[3];
            if(size<48||std::memcmp(bytes,"MDL2",4)!=0)return false;
            std::memcpy(head,bytes+4,20);
            std::memcpy(lo,bytes+24,12);
            std::memcpy(hi,bytes+36,12);
            const size_t coloursAt=48,lightsAt=coloursAt+head[0]*16ull,sparksAt=lightsAt+head[3]*12ull,verticesAt=sparksAt+head[4]*12ull,indicesAt=verticesAt+head[1]*12ull;
            if(indicesAt+head[2]*2ull>size)return false;
            const float scale=height/(hi[1]-lo[1]),midX=(lo[0]+hi[0])/2,midZ=(lo[2]+hi[2])/2;
            if(scaleOut)*scaleOut=scale;
            auto place=[&](float px,float py,float pz)->V{
                if(quarter)return {x+(pz-hi[2])*scale,y+(py-lo[1])*scale,z-(px-midX)*scale};
                return {x+(px-midX)*scale,y+(py-lo[1])*scale,z+(pz-midZ)*scale};
            };
            auto places=[&](std::vector<V>* list,size_t at,std::uint32_t count){
                if(!list)return;
                for(std::uint32_t i=0;i<count;++i)
                {
                    float q[3];
                    std::memcpy(q,bytes+at+i*12ull,12);
                    list->push_back(place(q[0],q[1],q[2]));
                }
            };
            places(lights,lightsAt,head[3]);
            places(sparks,sparksAt,head[4]);
            out.reserve(out.size()+head[2]);
            for(std::uint32_t i=0;i<head[2];++i)
            {
                std::uint16_t index;
                std::memcpy(&index,bytes+indicesAt+i*2ull,2);
                struct Raw {std::uint16_t x,y,z;std::int16_t u,v;std::uint8_t colour,pad;} raw;
                static_assert(sizeof(Raw)==12);
                std::memcpy(&raw,bytes+verticesAt+index*12ull,12);
                float nx=raw.u/32767.0f,ny=raw.v/32767.0f,nz=1-std::abs(nx)-std::abs(ny);
                if(nz<0)
                {
                    const float ox=nx;
                    nx=(1-std::abs(ny))*(ox>=0?1.0f:-1.0f);
                    ny=(1-std::abs(ox))*(ny>=0?1.0f:-1.0f);
                }
                const float l=std::sqrt(nx*nx+ny*ny+nz*nz);
                nx/=l;ny/=l;nz/=l;
                float colour[4];
                std::memcpy(colour,bytes+coloursAt+raw.colour*16ull,16);
                MascotRenderer::GeometryVertex v{};
                const V at=place(lo[0]+(hi[0]-lo[0])*raw.x/65535.0f,lo[1]+(hi[1]-lo[1])*raw.y/65535.0f,lo[2]+(hi[2]-lo[2])*raw.z/65535.0f);
                v.position[0]=at[0];v.position[1]=at[1];v.position[2]=at[2];
                v.normal[0]=quarter?nz:nx;
                v.normal[1]=ny;
                v.normal[2]=quarter?-nx:nz;
                if(colour[3]>1.5f)
                {
                    // Metal keeps some of its own shade, on the way to gold; glass only a tint.
                    const bool metal=colour[3]<2.5f;
                    const float gold[3]={0.86f,0.62f,0.28f};
                    for(int k=0;k<3;++k)v.color[k]=metal?colour[k]+(gold[k]-colour[k])*0.45f:colour[k];
                    v.color[3]=PatternLook;
                    v.uv[2]=metal?-6.0f:-7.0f;
                }
                else if(colour[3]>0.5f)
                {
                    // Giving light: drawn unlit, brighter and paler than its own colour.
                    v.color[0]=std::min(1.0f,colour[0]*1.1f+0.15f);v.color[1]=std::min(1.0f,colour[1]*1.05f+0.30f);v.color[2]=std::min(1.0f,colour[2]*1.05f+0.22f);
                    v.color[3]=GlowLook;
                }
                else
                {
                    v.color[0]=colour[0]*0.8f;v.color[1]=colour[1]*0.8f;v.color[2]=colour[2]*0.8f;v.color[3]=1;
                }
                out.push_back(v);
            }
            return true;
        }

        // The chandelier over the table on a rod from the ceiling, its foot at the top of what the
        // camera sees while she stands (so she comes in beneath it) and above what it sees once she
        // sits; each flame with a bright heart and a soft halo.
        void AddChandelier(std::vector<MascotRenderer::GeometryVertex>& out,std::vector<MascotRenderer::Particle>& glows,const WesternRoomSize& s,float ceiling)
        {
            const float D=s.upper,foot=s.tableTop+2.2f*D,length=std::min(1.3f*D,ceiling-foot);
            const size_t rodAt=out.size();
            {
                Builder b{out};
                const float h=0.016f*D;
                b.Solid(-h,foot+0.97f*length,s.lampZ-h,h,ceiling,s.lampZ+h,Brass);
            }
            for(size_t i=rodAt;i<out.size();++i)
            {
                out[i].color[3]=PatternLook;
                out[i].uv[2]=-6.0f;
            }
            std::vector<V> flames,sparks;
            float scale=1;
            if(!AddModel(out,&flames,L"CHANDELIER",0,foot,s.lampZ,length,false,&scale,&sparks))
            {
                Log(L"room: no chandelier");
                return;
            }
            for(const V& f:flames)
            {
                MascotRenderer::Particle g;
                g.position=f;
                g.glow=true;
                g.size=1.5f*scale;
                g.color={1.0f,0.86f,0.56f,0.95f};
                glows.push_back(g);
                g.size=4.2f*scale;
                g.color={1.0f,0.70f,0.36f,0.42f};
                glows.push_back(g);
            }
            for(const V& at:sparks)
            {
                MascotRenderer::Particle g;
                g.position=at;
                g.nearer=true;
                g.size=1.7f*scale;
                g.color={1.0f,0.97f,0.90f,1.0f};
                glows.push_back(g);
            }
            MascotRenderer::Particle g;
            g.position={0,foot+0.45f*length,s.lampZ};
            g.glow=true;
            g.size=1.5f*length;
            g.color={1.0f,0.78f,0.48f,0.20f};
            glows.push_back(g);
        }
    }

    void BuildWesternRoom(std::vector<MascotRenderer::GeometryVertex>& vertices,std::vector<MascotRenderer::Particle>& glows,const WesternRoomSize& s)
    {
        Builder b{vertices};
        glows.clear();
        const float D=s.upper,floor=s.floor,rx=6.0f*D,zBack=5.0f*D,zFront=-7.5f*D,ceiling=floor+1.9f*(s.hips+D),wain=floor+0.55f*s.hips;
        // The floor, and a rug under the table and her chair.
        b.Patterned({-rx,floor,zBack},{rx,floor,zBack},{rx,floor,zFront},{-rx,floor,zFront},{0,1,0},{0.36f,0.22f,0.115f},Parquet,-rx/D,zBack/D,rx/D,zFront/D);
        const float rugX=4.3f*D,rug0=-6.4f*D,rug1=3.5f*D,rugY=floor+0.006f*D;   // (the table hides much of what is near it)
        b.Patterned({-rugX,rugY,rug1},{rugX,rugY,rug1},{rugX,rugY,rug0},{-rugX,rugY,rug0},{0,1,0},{1,1,1},Rug,-1,1,1,-1);
        // The ceiling, with its beams crossing.
        b.Plain({-rx,ceiling,zBack},{rx,ceiling,zBack},{rx,ceiling,zFront},{-rx,ceiling,zFront},{0,-1,0},Plaster);
        for(float x=-rx*2/3;x<rx;x+=rx*2/3)b.Wood(x-0.1f*D,ceiling-0.12f*D,zFront,x+0.1f*D,ceiling,zBack,Moulding,1);
        for(float z=zFront+(zBack-zFront)/4;z<zBack-0.1f*D;z+=(zBack-zFront)/4)b.Wood(-rx,ceiling-0.12f*D,z-0.1f*D,rx,ceiling,z+0.1f*D,Moulding,0);
        AddWall(b,{-rx,floor,zBack},{rx,floor,zBack},{0,0,-1},s,wain,ceiling);
        AddWall(b,{rx,floor,zFront},{-rx,floor,zFront},{0,0,1},s,wain,ceiling);
        AddWall(b,{-rx,floor,zFront},{-rx,floor,zBack},{1,0,0},s,wain,ceiling);
        AddWall(b,{rx,floor,zBack},{rx,floor,zFront},{-1,0,0},s,wain,ceiling);
        // Behind her: the fireplace, two windows, candles on the wall between.
        const float sz=zBack-0.09f*D;
        AddFireplace(b,glows,zBack-0.06f*D,floor,ceiling,D);
        for(float side:{-1.0f,1.0f})
        {
            AddWindow(b,glows,side*2*rx/3,sz,wain+0.14f*D,ceiling-0.72f*D,0.62f*(rx/3),D);
            AddSconce(b,glows,side*0.30f*rx,floor+0.72f*(ceiling-floor),zBack-0.02f*D,D);
        }
        // On her left, seen from you, a painting in a gilt frame; on her right, a bookcase.
        {
            const float wx=-rx+0.03f*D,z0=-2.4f*D,z1=1.0f*D,y0=wain+0.42f*D,y1=std::min(ceiling-0.55f*D,y0+2.2f*D),f=0.11f*D;
            b.Solid(wx,y0-f,z0-f,wx+0.05f*D,y1+f,z1+f,Brass);
            b.Patterned({wx+0.052f*D,y0,z0},{wx+0.052f*D,y0,z1},{wx+0.052f*D,y1,z1},{wx+0.052f*D,y1,z0},{1,0,0},{1,1,1},Painting,0,0,1,1);
        }
        if(!AddModel(vertices,nullptr,L"BOOKSHELF",rx-0.01f*D,floor,-1.1f*D,ceiling-floor-0.62f*D,true))Log(L"room: no bookshelf");
        AddChandelier(vertices,glows,s,ceiling);
    }

    void ShowRoomGlows(std::vector<MascotRenderer::Particle>& shown,const std::vector<MascotRenderer::Particle>& glows,double clock)
    {
        const float t=static_cast<float>(clock);
        for(MascotRenderer::Particle g:glows)
        {
            // A star on the glass: each flashes now and then, on its own time.
            if(g.nearer)
            {
                auto own=[&](float k){const float h=std::sin(g.position[0]*12.9898f+g.position[1]*78.233f+g.position[2]*37.719f+k)*43758.5453f;return h-std::floor(h);};
                const float phase=t/(1.5f+2.3f*own(1))+own(2),from=std::abs(phase-std::floor(phase)-0.5f);
                const float flash=std::max(0.0f,1-from/0.12f);
                if(flash<=0)continue;
                g.color[3]=flash*flash;
                g.size*=(0.55f+0.45f*flash)*(0.75f+0.5f*own(3));
                g.angle=own(4)*6.2832f+t*0.35f;
                shown.push_back(g);
                continue;
            }
            // Warm ones are flames: each breathes on its own, by where it is.
            if(g.color[2]<0.7f*g.color[0])
            {
                const float seed=g.position[0]*3.1f+g.position[2]*2.3f;
                const float breath=0.5f*std::sin(t*7.3f+seed)+0.3f*std::sin(t*13.1f+seed*1.7f)+0.2f*std::sin(t*2.9f+seed*0.6f);
                g.color[3]*=0.88f+0.12f*breath;
                g.size*=0.97f+0.03f*breath;
            }
            shown.push_back(g);
        }
    }
}
