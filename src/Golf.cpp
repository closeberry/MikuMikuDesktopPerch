#define NOMINMAX
#include "Golf.hpp"
#include <windows.h>
#include "Text.hpp"
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cstring>
#include <tuple>

namespace MikuMikuDesktopPerch::Golf
{
    namespace
    {
        constexpr float Pi=3.14159265f,Deg=Pi/180,G=9.81f;
        // The air on the ball: half the air's density times its cross-section, over its mass.
        constexpr float AirK=0.5f*1.2f*Pi*BallRadius*BallRadius/0.04593f;

        float Smooth(float t){t=std::clamp(t,0.0f,1.0f);return t*t*(3-2*t);}

        // How the ground takes the ball: how much of its speed into the ground comes back, how
        // much of its speed along the ground a bounce takes, how hard it is to roll on.
        struct Turf {float bounce,grab,roll;};
        Turf TurfOf(Lie lie)
        {
            switch(lie)
            {
            case Lie::Green:return {0.25f,0.26f,0.065f};
            case Lie::Rough:return {0.18f,0.55f,0.30f};
            case Lie::Bunker:return {0.04f,0.90f,1.20f};
            default:return {0.30f,0.32f,0.13f};   // fairway, tee
            }
        }

        // The spot struck, up and down (-1 below .. 1 above): how fast the ball leaves and how
        // much it spins back. Below: a little faster, much more spin (it climbs and stops);
        // above: slower, little spin (low, and it runs on).
        float SpotSpeed(float y){return y<0?1+0.025f*-y:1-0.06f*y;}
        float SpotSpin(float y){return y<0?1+0.6f*-y:1-0.55f*y;}

        constexpr Club Clubs[ClubCount]={
            {L"1W",L"1W",10.5f,69.0f,2900},
            {L"3W",L"3W",15.0f,62.0f,3700},
            {L"5I",L"5I",22.0f,53.0f,5000},
            {L"7I",L"7I",30.0f,47.0f,6600},
            {L"9I",L"9I",39.0f,41.0f,8000},
            {L"PW",L"PW",45.0f,37.0f,8800},
            {L"SW",L"SW",56.0f,30.0f,9600},
            {L"PT",L"PT",2.0f,0,0}};

        // Lie: speed and spin the ball leaves with, from there, with a club.
        void LieEffect(Lie lie,int club,float& speed,float& spin)
        {
            speed=spin=1;
            if(lie==Lie::Rough){speed=0.88f;spin=0.5f;}
            else if(lie==Lie::Bunker){if(club==Wedge){speed=0.84f;spin=0.75f;}else{speed=0.6f;spin=0.5f;}}
            if(club==Driver&&lie!=Lie::Tee)speed*=0.93f;
        }
    }

    float Dot(const V3& a,const V3& b){return a.x*b.x+a.y*b.y+a.z*b.z;}
    V3 Cross(const V3& a,const V3& b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
    float Length(const V3& a){return std::sqrt(Dot(a,a));}
    V3 Normalized(const V3& a){const float l=Length(a);return l>1e-9f?a*(1/l):V3{};}
    float Flat(const V3& a,const V3& b){return std::hypot(a.x-b.x,a.z-b.z);}

    const wchar_t* LieName(Lie lie)
    {
        switch(lie)
        {
        case Lie::Tee:return T(L"ティー",L"Tee");
        case Lie::Fairway:return T(L"フェアウェイ",L"Fairway");
        case Lie::Rough:return T(L"ラフ",L"Rough");
        case Lie::Bunker:return T(L"バンカー",L"Bunker");
        case Lie::Green:return T(L"グリーン",L"Green");
        case Lie::Water:return T(L"池",L"Water");
        default:return T(L"OB",L"Out of bounds");
        }
    }

    const Club& ClubOf(int club){return Clubs[std::clamp(club,0,ClubCount-1)];}
    std::wstring ClubName(int club)
    {
        switch(club)
        {
        case 0:return T(L"ドライバー",L"Driver");
        case 1:return T(L"3番ウッド",L"3 wood");
        case 2:return T(L"5番アイアン",L"5 iron");
        case 3:return T(L"7番アイアン",L"7 iron");
        case 4:return T(L"9番アイアン",L"9 iron");
        case 5:return T(L"ピッチングウェッジ",L"Pitching wedge");
        case 6:return T(L"サンドウェッジ",L"Sand wedge");
        default:return T(L"パター",L"Putter");
        }
    }

    bool LoadScene(const wchar_t* name,SceneModel& model)
    {
        const HRSRC info=FindResourceW(nullptr,name,L"GOLFMODEL");
        const HGLOBAL data=info?LoadResource(nullptr,info):nullptr;
        if(!data)return false;
        const auto* at=static_cast<const std::uint8_t*>(LockResource(data));
        const auto* end=at+SizeofResource(nullptr,info);
        auto take=[&](void* to,size_t n){if(static_cast<size_t>(end-at)<n)return false;std::memcpy(to,at,n);at+=n;return true;};
        auto text=[&](std::string& out){std::uint8_t n=0;if(!take(&n,1)||end-at<n)return false;out.assign(reinterpret_cast<const char*>(at),n);at+=n;return true;};
        char magic[4];
        std::uint32_t counts[5];
        if(!take(magic,4)||std::memcmp(magic,"GSC1",4)!=0||!take(counts,sizeof(counts)))return false;
        model={};
        model.textures.resize(counts[0]);
        for(auto& t:model.textures)if(!text(t))return false;
        model.materials.resize(counts[1]);
        for(auto& m:model.materials)
        {
            std::int32_t texture=0;
            std::uint8_t kind=0;
            if(!text(m.name)||!take(&texture,4)||!take(m.rgb,12)||!take(&kind,1)||!take(&m.first,4)||!take(&m.count,4))return false;
            m.texture=texture;
            m.kind=kind;
        }
        model.vertices.resize(static_cast<size_t>(counts[2])*8);
        model.indices.resize(counts[3]);
        if(!take(model.vertices.data(),model.vertices.size()*4)||!take(model.indices.data(),model.indices.size()*4))return false;
        for(std::uint32_t i=0;i<counts[4];++i)
        {
            std::string bone;
            float p[3];
            if(!text(bone)||!take(p,12))return false;
            model.bones.push_back({bone,{p[0],p[1],p[2]}});
        }
        for(const auto i:model.indices)if(i>=counts[2])return false;
        for(const auto& m:model.materials)if(static_cast<size_t>(m.first)+m.count>model.indices.size())return false;
        return true;
    }

    // The course model's ground: the triangles one could stand on, listed in a grid of cells.
    struct Ground
    {
        std::vector<V3> points;
        struct Tri {int a,b,c;Lie lie;};
        std::vector<Tri> tris;
        std::vector<int> greens;            // the green's triangles
        static constexpr float Cell=2;
        float x0=0,z0=0;
        int nx=0,nz=0;
        std::vector<std::uint32_t> start;   // per cell, where its triangles start in list (and the next's)
        std::vector<int> list;
        V3 tee,cup,clubhouse;
        // In bounds, a metre a cell from (x0, z0): what can be reached from the tee without
        // crossing the out-of-bounds band (or the ground's end).
        std::vector<std::uint8_t> inside;
        int ix=0,iz=0;
        float inLo[2]{},inHi[2]{};   // the in-bounds part's own bounds
        bool In(float x,float z) const
        {
            const int i=static_cast<int>(std::floor(x-x0)),j=static_cast<int>(std::floor(z-z0));
            return i>=0&&j>=0&&i<ix&&j<iz&&inside[static_cast<size_t>(j)*ix+i];
        }
        // The highest triangle over (x, z): its height and what it is. False: none there.
        bool At(float x,float z,float& h,Lie& lie) const
        {
            const int i=static_cast<int>(std::floor((x-x0)/Cell)),j=static_cast<int>(std::floor((z-z0)/Cell));
            if(i<0||j<0||i>=nx||j>=nz)return false;
            const size_t c=static_cast<size_t>(j)*nx+i;
            bool found=false;
            for(std::uint32_t k=start[c];k<start[c+1];++k)
            {
                const Tri& t=tris[static_cast<size_t>(list[k])];
                const V3 &a=points[t.a],&b=points[t.b],&d=points[t.c];
                const float det=(b.z-d.z)*(a.x-d.x)+(d.x-b.x)*(a.z-d.z);
                if(std::abs(det)<1e-9f)continue;
                const float u=((b.z-d.z)*(x-d.x)+(d.x-b.x)*(z-d.z))/det,v=((d.z-a.z)*(x-d.x)+(a.x-d.x)*(z-d.z))/det,w=1-u-v;
                if(u<-1e-5f||v<-1e-5f||w<-1e-5f)continue;
                const float y=u*a.y+v*b.y+w*d.y;
                if(!found||y>h){h=y;lie=t.lie;found=true;}
            }
            return found;
        }
    };

    std::shared_ptr<const Ground> LoadGround()
    {
        static std::shared_ptr<const Ground> made;
        if(made)return made;
        SceneModel m;
        if(!LoadScene(L"COURSE",m))return nullptr;
        auto g=std::make_shared<Ground>();
        constexpr float M=1/12.5f;
        const size_t n=m.vertices.size()/8;
        g->points.resize(n);
        for(size_t i=0;i<n;++i)g->points[i]={m.vertices[i*8]*M,m.vertices[i*8+1]*M,m.vertices[i*8+2]*M};
        auto centre=[&](const SceneModel::Material& mt){
            V3 sum;
            for(std::uint32_t i=mt.first;i<mt.first+mt.count;++i)sum=sum+g->points[m.indices[i]];
            return mt.count?sum*(1.0f/mt.count):V3{};
        };
        float lo[2]={1e9f,1e9f},hi[2]={-1e9f,-1e9f};
        for(const auto& mt:m.materials)
        {
            if(mt.name.rfind("Cup_Interior",0)==0)g->cup=centre(mt);
            if(mt.kind==5)g->tee=centre(mt);
            Lie lie;
            switch(mt.kind)
            {
            case 1:lie=Lie::Rough;break;
            case 2:case 4:case 7:lie=Lie::Fairway;break;   // fairway, fringe, the cart path
            case 3:lie=Lie::Green;break;
            case 5:lie=Lie::Tee;break;
            case 6:lie=Lie::Bunker;break;
            case 8:lie=Lie::Water;break;
            case 11:lie=Lie::Out;break;   // the band round the course
            default:continue;
            }
            for(std::uint32_t i=mt.first;i+2<mt.first+mt.count;i+=3)
            {
                if(lie==Lie::Green)g->greens.push_back(static_cast<int>(g->tris.size()));
                g->tris.push_back({static_cast<int>(m.indices[i]),static_cast<int>(m.indices[i+1]),static_cast<int>(m.indices[i+2]),lie});
                for(int k=0;k<3;++k)
                {
                    const V3& p=g->points[m.indices[i+k]];
                    lo[0]=std::min(lo[0],p.x);hi[0]=std::max(hi[0],p.x);
                    lo[1]=std::min(lo[1],p.z);hi[1]=std::max(hi[1],p.z);
                }
            }
        }
        if(g->tris.empty())return nullptr;
        // The bone that is not the root (全ての親): where the clubhouse goes.
        for(const auto& [name,p]:m.bones)if(name!="\xE5\x85\xA8\xE3\x81\xA6\xE3\x81\xAE\xE8\xA6\xAA")g->clubhouse=p*M;
        g->x0=lo[0];g->z0=lo[1];
        g->nx=static_cast<int>(std::ceil((hi[0]-lo[0])/Ground::Cell))+1;
        g->nz=static_cast<int>(std::ceil((hi[1]-lo[1])/Ground::Cell))+1;
        std::vector<std::vector<int>> cells(static_cast<size_t>(g->nx)*g->nz);
        for(size_t t=0;t<g->tris.size();++t)
        {
            const auto& tr=g->tris[t];
            const V3 &a=g->points[tr.a],&b=g->points[tr.b],&c=g->points[tr.c];
            const int i0=static_cast<int>((std::min({a.x,b.x,c.x})-g->x0)/Ground::Cell),i1=static_cast<int>((std::max({a.x,b.x,c.x})-g->x0)/Ground::Cell);
            const int j0=static_cast<int>((std::min({a.z,b.z,c.z})-g->z0)/Ground::Cell),j1=static_cast<int>((std::max({a.z,b.z,c.z})-g->z0)/Ground::Cell);
            for(int j=std::max(0,j0);j<=std::min(g->nz-1,j1);++j)
                for(int i=std::max(0,i0);i<=std::min(g->nx-1,i1);++i)cells[static_cast<size_t>(j)*g->nx+i].push_back(static_cast<int>(t));
        }
        g->start.push_back(0);
        for(const auto& c:cells)
        {
            g->list.insert(g->list.end(),c.begin(),c.end());
            g->start.push_back(static_cast<std::uint32_t>(g->list.size()));
        }
        // In bounds: filled from the tee, a metre at a time, up to the band.
        g->ix=static_cast<int>(std::ceil(hi[0]-lo[0]))+1;
        g->iz=static_cast<int>(std::ceil(hi[1]-lo[1]))+1;
        g->inside.assign(static_cast<size_t>(g->ix)*g->iz,0);
        std::vector<std::uint8_t> open(g->inside.size(),0);
        for(int j=0;j<g->iz;++j)
            for(int i=0;i<g->ix;++i)
            {
                float h=0;Lie lie=Lie::Out;
                bool any=true;
                for(const float dx:{0.25f,0.75f})for(const float dz:{0.25f,0.75f})
                    if(!g->At(g->x0+i+dx,g->z0+j+dz,h,lie)||lie==Lie::Out)any=false;
                open[static_cast<size_t>(j)*g->ix+i]=any?1:0;
            }
        std::vector<std::pair<int,int>> todo{{static_cast<int>(g->tee.x-g->x0),static_cast<int>(g->tee.z-g->z0)}};
        g->inLo[0]=g->inLo[1]=1e9f;g->inHi[0]=g->inHi[1]=-1e9f;
        while(!todo.empty())
        {
            const auto [i,j]=todo.back();
            todo.pop_back();
            if(i<0||j<0||i>=g->ix||j>=g->iz)continue;
            const size_t c=static_cast<size_t>(j)*g->ix+i;
            if(!open[c]||g->inside[c])continue;
            g->inside[c]=1;
            g->inLo[0]=std::min(g->inLo[0],g->x0+i);g->inHi[0]=std::max(g->inHi[0],g->x0+i+1);
            g->inLo[1]=std::min(g->inLo[1],g->z0+j);g->inHi[1]=std::max(g->inHi[1],g->z0+j+1);
            todo.push_back({i+1,j});todo.push_back({i-1,j});todo.push_back({i,j+1});todo.push_back({i,j-1});
        }
        made=g;
        return made;
    }

    Course::Course(std::shared_ptr<const Ground> g,std::uint32_t s):ground(std::move(g)),seed(s)
    {
        pin=ground->cup;
        if(seed!=0&&!ground->greens.empty())
        {
            // Somewhere on the green, a few metres in from its edge and away from the model's cup.
            std::mt19937 r(seed*2654435761u+17u);
            std::uniform_real_distribution<float> u(0,1);
            for(int tries=0;tries<400;++tries)
            {
                const auto& t=ground->tris[static_cast<size_t>(ground->greens[std::min(ground->greens.size()-1,static_cast<size_t>(u(r)*ground->greens.size()))])];
                float a=u(r),b=u(r);
                if(a+b>1){a=1-a;b=1-b;}
                const V3& p0=ground->points[t.a];
                const V3 q=p0+(ground->points[t.b]-p0)*a+(ground->points[t.c]-p0)*b;
                bool fits=Flat(q,ground->cup)>4;
                for(int k=0;k<8&&fits;++k)
                {
                    const float an=k*Pi/4;
                    if(LieAt(q.x+3*std::cos(an),q.z+3*std::sin(an))!=Lie::Green)fits=false;
                }
                if(fits){pin=q;break;}
            }
        }
        pin.y=Height(pin.x,pin.z);
        static std::weak_ptr<const Ground> plantedOn;
        static std::shared_ptr<const Wood> planted;
        if(!planted||plantedOn.lock()!=ground){planted=PlantTrees(*ground);plantedOn=ground;}
        wood=planted;
    }

    V3 Course::Tee() const {return {ground->tee.x,Height(ground->tee.x,ground->tee.z),ground->tee.z};}
    V3 Course::ModelCup() const {return ground->cup;}
    V3 Course::Clubhouse() const {return ground->clubhouse;}
    V3 Course::GreenMiddle() const
    {
        V3 sum;
        for(const int t:ground->greens)sum=sum+ground->points[ground->tris[static_cast<size_t>(t)].a];
        if(!ground->greens.empty())sum=sum*(1.0f/ground->greens.size());
        return {sum.x,Height(sum.x,sum.z),sum.z};
    }
    float Course::MinX() const {return ground->inLo[0]-4;}
    float Course::MaxX() const {return ground->inHi[0]+4;}
    float Course::MinZ() const {return ground->inLo[1]-4;}
    float Course::MaxZ() const {return ground->inHi[1]+4;}
    float Course::LineX(float z) const
    {
        const V3 t=ground->tee;
        const float span=pin.z-t.z;
        return std::abs(span)<1e-3f?t.x:t.x+(pin.x-t.x)*std::clamp((z-t.z)/span,0.0f,1.0f);
    }

    // The trees' shapes, measured once on the five models (TREE1-5): the leaves' middle height,
    // their reach across and up and down, the trunk's radius at the ground (metres).
    const std::array<TreeShape,TreeKinds>& TreeShapes()
    {
        static std::array<TreeShape,TreeKinds> shapes{};
        static bool measured=false;
        if(measured)return shapes;
        measured=true;
        for(int k=0;k<TreeKinds;++k)
        {
            TreeShape& t=shapes[static_cast<size_t>(k)];
            t={5,4,3,0.25f};
            SceneModel m;
            const std::wstring name=L"TREE"+std::to_wstring(k+1);
            if(!LoadScene(name.c_str(),m))continue;
            constexpr float M=1/12.5f;
            float lo=1e9f,hi=-1e9f,across=0,trunk=0;
            for(const auto& mt:m.materials)
            {
                const bool leaves=mt.name.find("Lea")!=std::string::npos;
                for(std::uint32_t i=mt.first;i<mt.first+mt.count;++i)
                {
                    const float* v=&m.vertices[static_cast<size_t>(m.indices[i])*8];
                    const float r=std::hypot(v[0],v[2])*M,y=v[1]*M;
                    if(leaves){lo=std::min(lo,y);hi=std::max(hi,y);across=std::max(across,r);}
                    else if(y<1.0f)trunk=std::max(trunk,r);
                }
            }
            if(hi>lo)t={(lo+hi)/2,across*0.8f,(hi-lo)/2*0.9f,std::max(0.12f,trunk)};
        }
        return shapes;
    }

    std::shared_ptr<const Course::Wood> Course::PlantTrees(const Ground& g)
    {
        auto w=std::make_shared<Wood>();
        const Ground* ground=&g;
        auto& trees=w->trees;
        // A wood all round outside the band: a tree in each cell of a 5 m grid (shifted a
        // little at random), where there is ground, clear of the band by a few metres, off the
        // cart path and the water, not on the clubhouse's ground.
        constexpr float Step=5,WoodDepth=64;   // out to the ground's end
        std::mt19937 r(0x7EEu);
        std::uniform_real_distribution<float> u(0,1);
        const V3 c=ground->clubhouse;
        for(float z=ground->inLo[1]-WoodDepth;z<ground->inHi[1]+WoodDepth;z+=Step)
            for(float x=ground->inLo[0]-WoodDepth;x<ground->inHi[0]+WoodDepth;x+=Step)
            {
                const float px=x+(u(r)-0.5f)*Step*0.8f,pz=z+(u(r)-0.5f)*Step*0.8f;
                const int kind=static_cast<int>(u(r)*TreeKinds)%TreeKinds;
                // Taller the further out, so the wood closes off the view beyond it.
                const float outX=std::max({0.0f,ground->inLo[0]-px,px-ground->inHi[0]}),outZ=std::max({0.0f,ground->inLo[1]-pz,pz-ground->inHi[1]});
                const float yaw=u(r)*2*Pi,scale=0.85f+0.4f*u(r)+0.7f*std::min(1.0f,std::hypot(outX,outZ)/45);
                float h=0;
                Lie lie;
                if(ground->In(px,pz)||!ground->At(px,pz,h,lie)||(lie!=Lie::Rough&&lie!=Lie::Out))continue;
                if(std::abs(px-c.x)<30&&std::abs(pz-c.z)<24)continue;
                bool clear=true;
                for(int k=0;k<8&&clear;++k)
                {
                    const float an=k*Pi/4;
                    for(const float d:{2.5f,4.5f})
                    {
                        float h2=0;Lie l2;
                        const float qx=px+d*std::cos(an),qz=pz+d*std::sin(an);
                        if(ground->In(qx,qz)||(ground->At(qx,qz,h2,l2)&&(l2==Lie::Water||l2==Lie::Fairway)))clear=false;
                    }
                }
                if(clear)trees.push_back({px,h,pz,yaw,scale,kind});
            }
        // Each tree listed in the cells its leaves reach over (for the ball).
        const auto& shapes=TreeShapes();
        w->x0=ground->inLo[0]-70;w->z0=ground->inLo[1]-70;
        w->nx=static_cast<int>((ground->inHi[0]+70-w->x0)/Wood::Cell)+1;
        w->nz=static_cast<int>((ground->inHi[1]+70-w->z0)/Wood::Cell)+1;
        w->cells.assign(static_cast<size_t>(w->nx)*w->nz,{});
        for(size_t t=0;t<trees.size();++t)
        {
            const Tree& tr=trees[t];
            const float reach=shapes[static_cast<size_t>(tr.kind)].across*tr.scale;
            const int i0=static_cast<int>((tr.x-reach-w->x0)/Wood::Cell),i1=static_cast<int>((tr.x+reach-w->x0)/Wood::Cell);
            const int j0=static_cast<int>((tr.z-reach-w->z0)/Wood::Cell),j1=static_cast<int>((tr.z+reach-w->z0)/Wood::Cell);
            for(int j=std::max(0,j0);j<=std::min(w->nz-1,j1);++j)
                for(int i=std::max(0,i0);i<=std::min(w->nx-1,i1);++i)w->cells[static_cast<size_t>(j)*w->nx+i].push_back(static_cast<int>(t));
        }
        return w;
    }

    // A ball at p in the air: in a tree's leaves it is slowed hard (and knocked about a little);
    // against its trunk it bounces off. False when nothing is there.
    bool Course::ThroughTrees(V3& p,V3& v,V3& spin,float step) const
    {
        if(!wood||wood->trees.empty())return false;
        const Wood& w=*wood;
        const int i=static_cast<int>((p.x-w.x0)/Wood::Cell),j=static_cast<int>((p.z-w.z0)/Wood::Cell);
        if(i<0||j<0||i>=w.nx||j>=w.nz)return false;
        const auto& shapes=TreeShapes();
        bool hit=false;
        for(const int t:w.cells[static_cast<size_t>(j)*w.nx+i])
        {
            const Tree& tr=w.trees[static_cast<size_t>(t)];
            const TreeShape& s=shapes[static_cast<size_t>(tr.kind)];
            const float dx=p.x-tr.x,dz=p.z-tr.z,dy=p.y-(tr.y+s.middle*tr.scale);
            const float rx=s.across*tr.scale,ry=s.half*tr.scale;
            if((dx*dx+dz*dz)/(rx*rx)+dy*dy/(ry*ry)<1)
            {
                // Through leaves and twigs: most of its speed gone within a few metres.
                const float k=std::exp(-4*step);
                v=v*k;
                spin=spin*k;
                const float jig=std::sin(p.x*31.7f+p.y*17.3f+p.z*23.1f);
                v.x+=jig*2*step;v.z+=std::cos(p.x*13.1f+p.z*29.9f)*2*step;
                hit=true;
            }
            const float flat=std::hypot(dx,dz),trunk=s.trunk*tr.scale;
            if(flat<trunk+BallRadius&&p.y<tr.y+(s.middle-s.half*0.5f)*tr.scale)
            {
                // The trunk: back off it, the speed across it turned back and mostly lost.
                const V3 n=Normalized({dx,0,dz});
                const float in=Dot(v,n);
                if(in<0)v=v-n*(1.6f*in);
                v=v*0.5f;
                p=p+n*(trunk+BallRadius-flat);
                hit=true;
            }
        }
        return hit;
    }

    float Course::Height(float x,float z) const
    {
        float h=0;
        Lie lie;
        if(ground->At(x,z,h,lie))return h;
        // A gap in the ground (the model's own cup): the ground round it.
        float sum=0;
        int n=0;
        for(int k=0;k<8;++k)
        {
            const float an=k*Pi/4;
            if(ground->At(x+0.2f*std::cos(an),z+0.2f*std::sin(an),h,lie)){sum+=h;++n;}
        }
        return n?sum/n:0.0f;
    }

    V3 Course::Normal(float x,float z) const
    {
        constexpr float d=0.25f;
        const float hx=Height(x+d,z)-Height(x-d,z),hz=Height(x,z+d)-Height(x,z-d);
        return Normalized({-hx,2*d,-hz});
    }

    Lie Course::LieAt(float x,float z) const
    {
        if(!ground->In(x,z))return Lie::Out;   // beyond the band round the course
        // The clubhouse's ground (44 by 26 m round its middle, the steps out in front of it).
        const V3 c=ground->clubhouse;
        if(std::abs(x-c.x)<23.5f&&z>c.z-17&&z<c.z+14)return Lie::Out;
        float h=0;
        Lie lie=Lie::Rough;
        if(ground->At(x,z,h,lie))return lie;
        for(int k=0;k<8;++k)
        {
            const float an=k*Pi/4;
            if(ground->At(x+0.2f*std::cos(an),z+0.2f*std::sin(an),h,lie))return lie;
        }
        return Lie::Out;
    }

    V3 WindVector(const Wind& w){return {w.speed*std::sin(w.toward),0,w.speed*std::cos(w.toward)};}

    float PutterRange(float distance,float rise)
    {
        const float needed=(distance+std::max(0.0f,rise)/TurfOf(Lie::Green).roll)*1.4f;
        for(float r:{3.0f,5.0f,8.0f,12.0f,16.0f,20.0f,30.0f,45.0f,60.0f})if(r>=needed)return r;
        return 60;
    }

    float FlatCarry(int club,float spotX,float spotY)
    {
        const V3 l=FlatLanding(club,spotX,spotY);
        return std::hypot(l.x,l.z);
    }

    V3 FlatLanding(int club,float spotX,float spotY)
    {
        if(club==Putter)return {};
        // As Fly launches it, flown over level ground until it comes back down to it.
        const Club& c=ClubOf(club);
        float launch=c.loft*(club<=1?0.95f:0.86f)*Deg-spotY*4*Deg;
        const float speed=c.speed*SpotSpeed(spotY);
        const float back=c.spin*SpotSpin(spotY);
        const V3 d{0,0,1},up{0,1,0};
        V3 p,v=d*(std::cos(launch)*speed)+up*(std::sin(launch)*speed);
        V3 spin=Normalized(Cross(d,up))*(back*2*Pi/60)+up*(-spotX*1800*2*Pi/60);
        constexpr float step=1/120.0f;
        for(int i=0;i<12000;++i)
        {
            const float va=Length(v);
            V3 a{0,-G,0};
            const float s=BallRadius*Length(spin)/va;
            const float cl=std::min(0.25f,0.9f*s+0.05f),cd=0.21f+0.25f*std::min(s,0.4f);
            a=a-v*(AirK*cd*va);
            if(Length(spin)>1)a=a+Normalized(Cross(spin,v))*(AirK*cl*va*va);
            spin=spin*std::exp(-step/14);
            v=v+a*step;
            const V3 before=p;
            p=p+v*step;
            if(p.y<0&&v.y<0)
            {
                const float t=before.y/(before.y-p.y);
                return {before.x+(p.x-before.x)*t,0,before.z+(p.z-before.z)*t};
            }
        }
        return {p.x,0,p.z};
    }

    Flight Fly(const Course& course,V3 from,Lie lie,const Shot& shot,const Wind& wind,bool keepPath,float step)
    {
        Flight f;
        const Club& club=ClubOf(shot.club);
        const V3 up{0,1,0},pin=course.Pin();
        V3 p=from;
        p.y=course.Height(p.x,p.z)+BallRadius;
        float speedK=1,spinK=1;
        LieEffect(lie,shot.club,speedK,spinK);
        const float power=std::max(0.0f,shot.power);
        float speed,launch,back,side,aim=shot.aim;
        if(shot.club==Putter)
        {
            // Rolled: as fast as rolls range * power on level green.
            const float range=shot.range>0?shot.range:10.0f;
            speed=std::sqrt(2*TurfOf(Lie::Green).roll*G*range*power)*(lie==Lie::Green?1.0f:1.08f);
            launch=0;back=side=0;
            aim+=shot.accuracy*2.5f*Deg;
        }
        else
        {
            speed=club.speed*power*speedK;
            // Over full power the strike suffers: higher, less control (the timing errs too).
            // The spot struck: below climbs and spins (and goes a little further), above flies
            // low and rolls; to one side the ball curves the other way.
            speed*=SpotSpeed(shot.spotY);
            launch=(club.loft*(shot.club<=1?0.95f:0.86f)+(power<0.6f?(0.6f-power)*6:0)-shot.spotY*4)*Deg;
            back=club.spin*(0.55f+0.45f*power)*spinK*SpotSpin(shot.spotY);
            side=shot.accuracy*2300*std::min(1.0f,speed/55)-shot.spotX*1800*std::min(1.0f,speed/40);
            aim+=shot.accuracy*3.5f*Deg;
        }
        const V3 d{std::sin(aim),0,std::cos(aim)};
        V3 v=d*(std::cos(launch)*speed)+up*(std::sin(launch)*speed);
        V3 spin=Normalized(Cross(d,up))*(back*2*Pi/60)+up*(side*2*Pi/60);
        const V3 w=WindVector(wind);
        bool rolling=launch==0,landed=false;
        V3 lastLand=p;
        if(keepPath)f.path.push_back(p);
        const int steps=static_cast<int>(40/step);
        for(int i=0;i<steps;++i)
        {
            const V3 before=p;
            float ground=course.Height(p.x,p.z);
            if(!rolling)
            {
                // In the air: gravity, drag against the air, the spin's lift (Magnus).
                const V3 air=v-w*std::clamp((p.y-ground)/3,0.0f,1.0f);
                const float va=Length(air);
                V3 a{0,-G,0};
                if(va>0.1f)
                {
                    const float s=BallRadius*Length(spin)/va;
                    // Lift grows with the spin, and so does the drag.
                    const float cl=std::min(0.25f,0.9f*s+0.05f),cd=0.21f+0.25f*std::min(s,0.4f);
                    a=a-air*(AirK*cd*va);
                    if(Length(spin)>1)a=a+Normalized(Cross(spin,air))*(AirK*cl*va*va);
                }
                spin=spin*std::exp(-step/14);
                v=v+a*step;
                p=p+v*step;
                course.ThroughTrees(p,v,spin,step);
                ground=course.Height(p.x,p.z);
                if(p.y-BallRadius<=ground)
                {
                    const Lie at=course.LieAt(p.x,p.z);
                    if(!landed){landed=true;f.carry=Flat(from,p);f.landing=static_cast<int>(f.path.size());}
                    if(at==Lie::Out){f.out=true;f.rest=p;f.lie=at;if(keepPath)f.path.push_back(p);return f;}
                    if(at==Lie::Water){f.water=true;f.rest=p;f.lie=at;f.drop=lastLand;if(keepPath)f.path.push_back(p);return f;}
                    const V3 n=course.Normal(p.x,p.z);
                    const float vn=Dot(v,n);
                    p.y=ground+BallRadius;
                    if(vn<0)
                    {
                        const Turf g=TurfOf(at);
                        const V3 along=v-n*vn;
                        if(-vn>1.3f)
                        {
                            // A bounce: back up by the ground's give, slowed along it (more on a
                            // green the more it spins back).
                            float grab=g.grab;
                            if(at!=Lie::Rough&&at!=Lie::Bunker)grab+=std::min(0.5f,Length(spin)*60/(2*Pi)/(at==Lie::Green?14000.0f:13000.0f));
                            v=along*(1-std::min(grab,0.95f))-n*(vn*g.bounce);
                            spin=spin*0.4f;
                        }
                        else{v=along;rolling=true;}
                    }
                }
                else if(course.LieAt(p.x,p.z)!=Lie::Water)lastLand=p;
            }
            else
            {
                // On the ground: down the slope, held back by the grass.
                const Lie at=course.LieAt(p.x,p.z);
                if(at==Lie::Out){f.out=true;f.rest=p;f.lie=at;if(keepPath)f.path.push_back(p);return f;}
                if(at==Lie::Water){f.water=true;f.rest=p;f.lie=at;f.drop=lastLand;if(keepPath)f.path.push_back(p);return f;}
                lastLand=p;
                const V3 n=course.Normal(p.x,p.z);
                v=v-n*Dot(v,n);
                const V3 slope=V3{0,-G,0}-n*(-G*n.y);
                const float mu=TurfOf(at).roll*(at==Lie::Fairway&&course.LieAt(p.x,p.z)==Lie::Fairway?1.0f:1.0f);
                const float s=Length(v);
                if(s<0.04f&&Length(slope)<mu*1.25f*G*n.y)
                {
                    f.rest=p;f.lie=at;
                    if(keepPath)f.path.push_back(p);
                    return f;
                }
                const V3 next=v+(slope-(s>1e-4f?v*(1/s):V3{})*(mu*G*n.y))*step;
                v=s>0.04f&&Dot(next,v)<0?V3{}:next;
                p=p+v*step;
                p.y=course.Height(p.x,p.z)+BallRadius;
                if(!landed){landed=true;f.carry=0;f.landing=static_cast<int>(f.path.size());}
            }
            // The cup: passed over slowly enough, or fallen into.
            {
                const float sx=p.x-before.x,sz=p.z-before.z,len2=sx*sx+sz*sz;
                float t=len2>1e-12f?((pin.x-before.x)*sx+(pin.z-before.z)*sz)/len2:0;
                t=std::clamp(t,0.0f,1.0f);
                const float cx=before.x+sx*t-pin.x,cz=before.z+sz*t-pin.z;
                const float above=p.y-course.Height(p.x,p.z);
                if(cx*cx+cz*cz<CupRadius*CupRadius*0.8f&&Length(v)<1.35f&&above<0.12f)
                {
                    f.holed=true;
                    f.rest=pin;
                    f.lie=Lie::Green;
                    if(keepPath){f.path.push_back({pin.x,pin.y-0.03f,pin.z});f.path.push_back({pin.x,pin.y-0.08f,pin.z});}
                    return f;
                }
            }
            if(keepPath)f.path.push_back(p);
        }
        f.rest=p;
        f.lie=course.LieAt(p.x,p.z);
        return f;
    }

    std::vector<int> ClubsFor(Lie lie,float toPin)
    {
        if(lie==Lie::Green)return {Putter};
        std::vector<int> clubs;
        for(int c=0;c<Putter;++c)clubs.push_back(c);
        if(lie==Lie::Fairway&&toPin<25)clubs.push_back(Putter);
        return clubs;
    }

    int ClubFor(Lie lie,float toPin,const Course& course,V3 ball,const Wind& wind)
    {
        if(lie==Lie::Green)return Putter;
        const float aim=std::atan2(course.Pin().x-ball.x,course.Pin().z-ball.z);
        int best=Wedge;
        for(int c=Wedge;c>=0;--c)
        {
            if(c==Driver&&lie!=Lie::Tee)continue;
            Shot s;s.club=c;s.aim=aim;s.power=1;
            const Flight f=Fly(course,ball,lie,s,wind,false,1/30.0f);
            best=c;
            if(f.carry>=toPin*0.92f||Flat(ball,f.rest)>=toPin)break;
        }
        return best;
    }

    float StrokesLeft(const Course& course,V3 p,Lie lie)
    {
        const float d=Flat(p,course.Pin());
        if(lie==Lie::Green)return d<0.3f?1.0f:1.0f+0.95f*(1-std::exp(-d/4))+d/80;
        float s=2.25f+d/165;
        if(d<18)s=std::min(s,2.2f+d/60);
        if(lie==Lie::Rough)s+=0.25f;
        else if(lie==Lie::Bunker)s+=d>40?0.7f:0.45f;
        return s;
    }

    namespace
    {
        // How many strokes the shot leaves (this one counted): penalties for water and out of bounds.
        float Outcome(const Course& course,V3 from,Lie lie,const Flight& f)
        {
            if(f.holed)return 1;
            if(f.out)return 2+StrokesLeft(course,from,lie);
            if(f.water)return 2+StrokesLeft(course,f.drop,Lie::Rough);
            return 1+StrokesLeft(course,f.rest,f.lie);
        }
    }

    Shot Choose(const Course& course,V3 from,Lie lie,const Wind& wind)
    {
        const V3 pin=course.Pin();
        const float d=Flat(from,pin),toward=std::atan2(pin.x-from.x,pin.z-from.z);
        Shot best;
        float bestScore=1e9f;
        auto score=[&](const Shot& s,std::initializer_list<std::pair<float,float>> variants,float step){
            float sum=0,weight=0;
            for(const auto& [acc,w]:variants)
            {
                Shot t=s;
                t.accuracy=acc;
                sum+=Outcome(course,from,lie,Fly(course,from,lie,t,wind,false,step))*w;
                weight+=w;
            }
            return sum/weight;
        };
        for(const int club:ClubsFor(lie,d))
        {
            if(club==Putter)
            {
                Shot s;
                s.club=Putter;
                s.range=PutterRange(d,pin.y-from.y);
                // Coarse, then finer round the best.
                Shot coarse;float coarseScore=1e9f;
                for(float a=-6;a<=6.01f;a+=1.0f)
                    for(float pw=0.1f;pw<=1.001f;pw+=0.04f)
                    {
                        s.aim=toward+a*Deg;s.power=pw;
                        const float sc=score(s,{{0.0f,1.0f}},1/60.0f);
                        if(sc<coarseScore){coarseScore=sc;coarse=s;}
                    }
                for(float a=-1;a<=1.01f;a+=0.2f)
                    for(float pw=-0.04f;pw<=0.041f;pw+=0.01f)
                    {
                        s.aim=coarse.aim+a*Deg;s.power=std::clamp(coarse.power+pw,0.05f,1.0f);
                        const float sc=score(s,{{0.0f,2.0f},{0.25f,1.0f},{-0.25f,1.0f}},1/60.0f);
                        if(sc<bestScore){bestScore=sc;best=s;}
                    }
                continue;
            }
            if(club==Driver&&lie!=Lie::Tee)continue;
            // Only clubs that can get about there (or the longest, when none can).
            Shot full;full.club=club;full.aim=toward;full.power=1;
            const Flight f=Fly(course,from,lie,full,wind,false,1/30.0f);
            const float reach=Flat(from,f.rest);
            if(reach<d*0.45f&&club>Driver+1)continue;
            if(f.carry>d*1.6f&&club<Wedge)continue;
            for(float a=-12;a<=12.01f;a+=2.0f)
                for(float pw=0.35f;pw<=1.001f;pw+=0.065f)
                {
                    Shot s;s.club=club;s.aim=toward+a*Deg;s.power=pw;
                    const float sc=score(s,{{0.0f,2.0f},{0.35f,1.0f},{-0.35f,1.0f}},1/30.0f);
                    if(sc<bestScore){bestScore=sc;best=s;}
                }
        }
        return best;
    }

    void Execute(Shot& shot,float care,std::mt19937& random)
    {
        std::normal_distribution<float> n(0,1);
        const float loose=1-std::clamp(care,0.0f,1.0f);
        if(shot.club==Putter)
        {
            shot.power=std::clamp(shot.power*(1+n(random)*(0.07f+0.08f*loose)),0.02f,1.0f);
            shot.accuracy=n(random)*(0.35f+0.5f*loose);
        }
        else
        {
            shot.power=std::clamp(shot.power*(1+n(random)*(0.03f+0.05f*loose)),0.1f,1.03f);
            shot.accuracy=n(random)*(0.22f+0.35f*loose);
        }
    }

    std::wstring Check(int holes,std::uint32_t seed)
    {
        std::wstring out;
        int wrong=0;
        wchar_t line[300];
        const auto ground=LoadGround();
        if(!ground)return L"1 wrong\nthe course model could not be read\n";
        // The clubs on the first hole's tee, straight, no wind.
        {
            const Course c(ground,0);
            swprintf_s(line,L"course: tee %.1f,%.1f,%.1f; the model's cup %.1f,%.1f,%.1f (%.1f m); clubhouse %.1f,%.1f,%.1f; bounds x %.0f..%.0f z %.0f..%.0f\n",
                       c.Tee().x,c.Tee().y,c.Tee().z,c.Pin().x,c.Pin().y,c.Pin().z,c.Length(),c.Clubhouse().x,c.Clubhouse().y,c.Clubhouse().z,c.MinX(),c.MaxX(),c.MinZ(),c.MaxZ());
            out+=line;
            swprintf_s(line,L"ground x %.0f..%.0f z %.0f..%.0f; %zu trees\n",ground->x0,ground->x0+ground->nx*Ground::Cell,ground->z0,ground->z0+ground->nz*Ground::Cell,c.Trees().size());
            out+=line;
            // A ball driven into the wood: how much speed the leaves take (a few trees tried).
            for(size_t k=0;k<c.Trees().size()&&k<400;k+=97)
            {
                const auto& t=c.Trees()[k];
                const TreeShape& sh=TreeShapes()[static_cast<size_t>(t.kind)];
                const V3 middle{t.x,t.y+sh.middle*t.scale,t.z};
                V3 p=middle+V3{-15,0,0},v{40,0,0},spin{};

                for(int i=0;i<60;++i){c.ThroughTrees(p,v,spin,1/60.0f);p=p+v*(1/60.0f);}
                swprintf_s(line,L"tree %zu (kind %d, x%.2f): leaves %.1f m up, %.1f m across; a ball at 40 m/s through it leaves at %.1f m/s\n",k,t.kind+1,t.scale,sh.middle*t.scale,sh.across*t.scale,Length(v));
                out+=line;
            }
            out+=L"clubs from the tee (full power, no wind): carry, total\n";
            for(int k=0;k<Putter;++k)
            {
                Shot s;s.club=k;s.power=1;s.aim=0;
                const Flight f=Fly(c,c.Tee(),Lie::Tee,s,{},true);
                float top=0;
                for(const V3& p:f.path)top=std::max(top,p.y);
                swprintf_s(line,L"  %-4ls %6.1f m %6.1f m  apex %5.1f m  %.1f s in the air\n",ClubOf(k).en,f.carry,Flat(c.Tee(),f.rest),top,f.landing/60.0f);
                out+=line;
            }
            // The spot struck (7 iron): below, centre, above; left and right (how far it curves).
            for(const auto& [sx,sy,name]:{std::tuple<float,float,const wchar_t*>{0.0f,-1.0f,L"below"},{0.0f,0.0f,L"centre"},{0.0f,1.0f,L"above"},{-1.0f,0.0f,L"left"},{1.0f,0.0f,L"right"}})
            {
                Shot s;s.club=3;s.power=1;s.aim=0;s.spotX=sx;s.spotY=sy;
                const Flight f=Fly(c,c.Tee(),Lie::Tee,s,{},true);
                float top=0;
                for(const V3& p:f.path)top=std::max(top,p.y);
                swprintf_s(line,L"  7I struck %-6ls flat carry %5.1f m; here carry %5.1f m, total %5.1f m, apex %4.1f m, across %+5.1f m\n",name,FlatCarry(3,sx,sy),f.carry,Flat(c.Tee(),f.rest),top,f.rest.x-c.Tee().x);
                out+=line;
            }
            // Putts on level-ish green: the range.
            for(float r:{3.0f,10.0f,30.0f})
            {
                Shot s;s.club=Putter;s.range=r;s.power=1;
                const V3 from=c.GreenMiddle();
                s.aim=0.7f;
                const Flight f=Fly(c,from,Lie::Green,s,{},false);
                swprintf_s(line,L"  putt range %.0f m: rolled %.1f m\n",r,Flat(from,f.rest));
                out+=line;
            }
        }
        int strokes=0,pars=0;
        double thinking=0;
        for(int h=0;h<holes;++h)
        {
            const Course c(ground,h==0?0:seed*7919u+static_cast<std::uint32_t>(h));
            const int par=c.Par();
            std::wstring problem;
            if(c.LieAt(c.Tee().x,c.Tee().z)!=Lie::Tee)problem+=L" tee not tee;";
            if(c.LieAt(c.Pin().x,c.Pin().z)!=Lie::Green)problem+=L" pin off the green;";
            std::mt19937 random(seed+h);
            V3 ball=c.Tee();
            Lie lie=Lie::Tee;
            int n=0;
            bool holed=false;
            std::wstring way;
            while(n<12&&!holed)
            {
                LARGE_INTEGER t0,t1,rate;
                QueryPerformanceCounter(&t0);
                Shot s=Choose(c,ball,lie,{3,0.6f});
                QueryPerformanceCounter(&t1);
                QueryPerformanceFrequency(&rate);
                thinking=std::max(thinking,static_cast<double>(t1.QuadPart-t0.QuadPart)/rate.QuadPart);
                Execute(s,0.85f,random);
                const Flight f=Fly(c,ball,lie,s,{3,0.6f},false);
                ++n;
                swprintf_s(line,L" %ls%.0f%%->",ClubOf(s.club).en,s.power*100);
                way+=line;
                if(f.holed){holed=true;way+=L"in";break;}
                if(f.out){++n;way+=L"OB";continue;}
                if(f.water){++n;ball=f.drop;lie=Lie::Rough;way+=L"water";continue;}
                ball=f.rest;lie=f.lie;
                swprintf_s(line,L"%ls %.1fm",LieName(lie),Flat(ball,c.Pin()));
                way+=line;
            }
            if(!holed)problem+=L" not holed in 12;";
            if(!problem.empty())++wrong;
            strokes+=n;pars+=par;
            swprintf_s(line,L"hole %d (seed %u) par %d, %.0f m, pin %.1f,%.1f: %d strokes:%ls%ls\n",h+1,c.Seed(),par,c.Length(),c.Pin().x,c.Pin().z,n,way.c_str(),problem.empty()?L"":(L" WRONG"+problem).c_str());
            out+=line;
        }
        swprintf_s(line,L"%d wrong\n%d holes: %d strokes against par %d; longest think %.2f s\n",wrong,holes,strokes,pars,thinking);
        return line+out;
    }
}
