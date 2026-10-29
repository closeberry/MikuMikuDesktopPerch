#define NOMINMAX
#include "Metroid.hpp"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <sstream>

namespace MikuMikuDesktopPerch::Metroid
{
    namespace
    {
        std::wstring Wide(const std::string& s)
        {
            if(s.empty())return {};
            std::wstring w(static_cast<size_t>(MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0)),L'\0');
            MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),w.data(),static_cast<int>(w.size()));
            return w;
        }

        // "日本語|English": the two names.
        std::array<std::wstring,2> Names(const std::string& rest)
        {
            const size_t bar=rest.find('|');
            const std::wstring ja=Wide(rest.substr(0,bar));
            return {ja,bar==std::string::npos?ja:Wide(rest.substr(bar+1))};
        }

        constexpr float Eps=1e-4f;
    }

    bool Map::Parse(const std::string& utf8,std::wstring& error)
    {
        rooms.clear();
        areas.clear();
        bool started=false;
        std::istringstream in(utf8);
        std::string line;
        int number=0;
        Room* open=nullptr;
        auto fail=[&](const std::wstring& why){error=L"map line "+std::to_wstring(number)+L": "+why;return false;};
        while(std::getline(in,line))
        {
            ++number;
            if(!line.empty()&&line.back()=='\r')line.pop_back();
            if(number==1&&line.rfind("\xEF\xBB\xBF",0)==0)line.erase(0,3);
            if(open)
            {
                if(line=="end")
                {
                    if(open->rows.empty()||open->rows.size()%ScreenHeight)return fail(L"a room's height is not whole screens");
                    open->width=static_cast<int>(open->rows[0].size());
                    if(open->width==0||open->width%ScreenWidth)return fail(L"a room's width is not whole screens");
                    for(const auto& r:open->rows)if(static_cast<int>(r.size())!=open->width)return fail(L"a room's rows differ in length");
                    open->height=static_cast<int>(open->rows.size());
                    for(int ty=0;ty<open->height;++ty)
                        for(int tx=0;tx<open->width;++tx)
                        {
                            char& c=open->rows[ty][tx];
                            if(c=='S'){startX=open->x+tx+0.5f;startY=static_cast<float>(open->y+ty+1);started=true;c='.';}
                            else if(c!='#'&&c!='.'&&c!='=')return fail(L"an unknown tile");
                        }
                    open=nullptr;
                }
                else open->rows.push_back(line);
                continue;
            }
            if(line.empty()||line[0]=='#')continue;
            std::istringstream words(line);
            std::string kind;
            words>>kind;
            if(kind=="area")
            {
                Area a;
                words>>a.number>>a.look;
                std::string rest;
                std::getline(words>>std::ws,rest);
                if(!words&&rest.empty())return fail(L"an area needs a number, a look and a name");
                a.name=Names(rest);
                areas.push_back(a);
            }
            else if(kind=="room")
            {
                Room r;
                words>>r.area>>r.x>>r.y;
                std::string rest;
                std::getline(words>>std::ws,rest);
                if(rest.empty())return fail(L"a room needs an area, where it is and a name");
                if(!AreaOf(r.area))return fail(L"a room in an area not given");
                r.x*=ScreenWidth;
                r.y*=ScreenHeight;
                r.name=Names(rest);
                rooms.push_back(std::move(r));
                open=&rooms.back();
            }
            else return fail(L"neither an area nor a room");
        }
        if(open)return fail(L"a room with no end");
        if(rooms.empty())return fail(L"no rooms");
        if(!started)return fail(L"no start (S)");
        return true;
    }

    bool Map::LoadBuiltIn(std::wstring& error)
    {
        const HRSRC info=FindResourceW(nullptr,L"MAP",L"METROIDMAP");
        const HGLOBAL data=info?LoadResource(nullptr,info):nullptr;
        const char* text=data?static_cast<const char*>(LockResource(data)):nullptr;
        if(!text){error=L"no map in the exe";return false;}
        return Parse(std::string(text,SizeofResource(nullptr,info)),error);
    }

    const Area* Map::AreaOf(int number) const
    {
        for(const auto& a:areas)if(a.number==number)return &a;
        return nullptr;
    }

    int Map::RoomAt(int tx,int ty) const
    {
        for(size_t i=0;i<rooms.size();++i)if(rooms[i].Contains(tx,ty))return static_cast<int>(i);
        return -1;
    }

    char Map::At(int tx,int ty) const
    {
        const int i=RoomAt(tx,ty);
        if(i<0)return '#';
        const Room& r=rooms[static_cast<size_t>(i)];
        return r.rows[static_cast<size_t>(ty-r.y)][static_cast<size_t>(tx-r.x)];
    }

    std::wstring Map::Check() const
    {
        std::wstring out;
        for(size_t i=0;i<rooms.size();++i)
            for(size_t j=i+1;j<rooms.size();++j)
            {
                const Room &a=rooms[i],&b=rooms[j];
                if(a.x<b.x+b.width&&b.x<a.x+a.width&&a.y<b.y+b.height&&b.y<a.y+a.height)
                    out+=L"rooms overlap: "+a.name[0]+L" and "+b.name[0]+L"\n";
            }
        // Each open tile on a room's edge must have a room beyond it, open there too.
        for(const auto& r:rooms)
        {
            auto edge=[&](int tx,int ty,int dx,int dy){
                if(r.rows[static_cast<size_t>(ty-r.y)][static_cast<size_t>(tx-r.x)]=='#')return;
                const int other=RoomAt(tx+dx,ty+dy);
                if(other<0)out+=r.name[0]+L": open to nothing at "+std::to_wstring(tx)+L","+std::to_wstring(ty)+L"\n";
                else if(At(tx+dx,ty+dy)=='#')out+=r.name[0]+L": open into rock at "+std::to_wstring(tx)+L","+std::to_wstring(ty)+L"\n";
            };
            for(int tx=r.x;tx<r.x+r.width;++tx){edge(tx,r.y,0,-1);edge(tx,r.y+r.height-1,0,1);}
            for(int ty=r.y;ty<r.y+r.height;++ty){edge(r.x,ty,-1,0);edge(r.x+r.width-1,ty,1,0);}
        }
        if(At(static_cast<int>(std::floor(startX)),static_cast<int>(startY))=='.')out+=L"the start is over nothing to stand on\n";
        return out;
    }

    Game::Game(const Map& map):map(map)
    {
        visited.assign(map.Rooms().size(),false);
        Place(map.StartX(),map.StartY());
    }

    void Game::Place(float x,float y)
    {
        body={x,y,0,0,body.facing,true};
        coyote=buffer=0;
        rising=false;
        room=map.RoomAt(static_cast<int>(std::floor(x)),static_cast<int>(std::floor(y-1)));
        if(room>=0)visited[static_cast<size_t>(room)]=true;
    }

    bool Game::Solid(int tx,int ty) const {return map.At(tx,ty)=='#';}

    bool Game::StandOn(int tx,int ty,float fromY) const
    {
        const char c=map.At(tx,ty);
        return c=='#'||(c=='='&&fromY<=ty+Eps);
    }

    void Game::Step(double dt,const Input& in)
    {
        // A jump pressed now (not held from before) is remembered for a moment.
        if(in.jump&&!jumpHeld)buffer=moves.buffer;
        jumpHeld=in.jump;
        const int steps=std::max(1,static_cast<int>(std::ceil(dt*240)));
        for(int i=0;i<steps;++i)Tick(static_cast<float>(dt/steps),in);
        clock+=dt;
        const int now=map.RoomAt(static_cast<int>(std::floor(body.x)),static_cast<int>(std::floor(body.y-moves.height/2)));
        if(now>=0&&now!=room)
        {
            room=now;
            visited[static_cast<size_t>(room)]=true;
            events.roomChanged=true;
        }
    }

    void Game::Tick(float h,const Input& in)
    {
        auto& b=body;
        const Moves& m=moves;
        buffer=std::max(0.0f,buffer-h);
        coyote=b.ground?m.coyote:std::max(0.0f,coyote-h);
        // Running: quick to the top speed, quicker to stop, a little less of both in the air.
        const int way=(in.right?1:0)-(in.left?1:0);
        if(way)b.facing=way;
        const float want=way*m.run;
        const float rate=b.ground?(way&&(way*b.vx>=0)?m.accelerate:m.stop):m.airAccelerate;
        if(b.vx<want)b.vx=std::min(want,b.vx+rate*h);
        else if(b.vx>want)b.vx=std::max(want,b.vx-rate*h);
        // Jumping: a full jump lifts her jumpHeight; letting go early keeps only some of the rise.
        if(buffer>0&&coyote>0)
        {
            b.vy=-std::sqrt(2*m.gravity*m.jumpHeight);
            b.ground=false;
            buffer=coyote=0;
            rising=true;
            events.jumped=true;
        }
        if(rising&&!in.jump&&b.vy<0){b.vy*=m.cut;rising=false;}
        if(b.vy>=0)rising=false;
        b.vy=std::min(m.fall,b.vy+m.gravity*h);

        // Across, then up and down, each stopped by rock.
        const int top=static_cast<int>(std::floor(b.y-m.height+Eps)),foot=static_cast<int>(std::floor(b.y-Eps));
        b.x+=b.vx*h;
        if(b.vx>0)
        {
            const int tx=static_cast<int>(std::floor(b.x+m.halfWidth));
            for(int ty=top;ty<=foot;++ty)if(Solid(tx,ty)){b.x=tx-m.halfWidth-Eps;b.vx=0;break;}
        }
        else if(b.vx<0)
        {
            const int tx=static_cast<int>(std::floor(b.x-m.halfWidth));
            for(int ty=top;ty<=foot;++ty)if(Solid(tx,ty)){b.x=tx+1+m.halfWidth+Eps;b.vx=0;break;}
        }
        const int left=static_cast<int>(std::floor(b.x-m.halfWidth+Eps)),right=static_cast<int>(std::floor(b.x+m.halfWidth-Eps));
        const float fromY=b.y;
        b.y+=b.vy*h;
        const bool was=b.ground;
        b.ground=false;
        if(b.vy>=0)
        {
            // Coming down (or standing): onto the top of the row her feet are in.
            const int ty=static_cast<int>(std::floor(b.y));
            if(b.y>ty+Eps||std::abs(b.y-ty)<=Eps)
                for(int tx=left;tx<=right;++tx)
                    if(StandOn(tx,ty,fromY))
                    {
                        if(!was&&b.vy>1)events.landed=b.vy;
                        b.y=static_cast<float>(ty);
                        b.vy=0;
                        b.ground=true;
                        break;
                    }
        }
        else
        {
            const int ty=static_cast<int>(std::floor(b.y-m.height));
            for(int tx=left;tx<=right;++tx)if(Solid(tx,ty)){b.y=ty+1+m.height+Eps;b.vy=0;rising=false;break;}
        }
    }
}
