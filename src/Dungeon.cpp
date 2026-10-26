#define NOMINMAX
#include "Dungeon.hpp"
#include "Text.hpp"
#include <algorithm>
#include <cstdlib>

namespace MikuMikuDesktopPerch::Dungeon
{
    namespace
    {
        constexpr int CellsX=4,CellsY=3,CellW=Game::Width/CellsX,CellH=Game::Height/CellsY;
        constexpr int LogLines=6;
        constexpr int SightSteps=5;   // how far she sees along open floor (corridors), in steps

        struct Gear {const wchar_t* ja;const wchar_t* en;int atk,def,mag,hp,mp;};
        // By tier (deeper floors, better tiers).
        constexpr Gear Swords[]={{L"木の剣",L"Wooden sword",2,0,0,0,0},{L"鉄の剣",L"Iron sword",4,0,0,0,0},
                                 {L"鋼の剣",L"Steel sword",6,0,0,0,0},{L"炎の剣",L"Flame sword",9,0,0,0,0}};
        constexpr Gear Staves[]={{L"木の杖",L"Wooden staff",1,0,2,0,0},{L"魔法の杖",L"Magic staff",1,0,4,0,0},
                                 {L"賢者の杖",L"Sage's staff",2,0,6,0,0},{L"星の杖",L"Star staff",2,0,9,0,0}};
        constexpr Gear Armors[]={{L"布の服",L"Cloth tunic",0,1,0,0,0},{L"革の鎧",L"Leather armor",0,2,0,0,0},
                                 {L"鎖かたびら",L"Chain mail",0,4,0,0,0},{L"鋼の鎧",L"Steel armor",0,6,0,0,0}};
        constexpr Gear Accessories[]={{L"力の指輪",L"Ring of strength",2,0,0,0,0},{L"守りの腕輪",L"Bracelet of guard",0,2,0,0,0},
                                      {L"魔力のペンダント",L"Pendant of magic",0,0,2,0,0},{L"命のお守り",L"Charm of life",0,0,0,10,0},
                                      {L"星のブローチ",L"Star brooch",0,0,0,0,5}};

        struct Kind {const wchar_t* ja;const wchar_t* en;int hp,atk,def,xp,minFloor;std::uint32_t color;};
        constexpr Kind Kinds[]={{L"スライム",L"Slime",6,3,0,2,1,0xFF5BC26A},{L"コウモリ",L"Bat",5,4,0,3,1,0xFF9A6BD1},
                                {L"ゴブリン",L"Goblin",10,5,1,5,2,0xFFA0A63C},{L"ガイコツ",L"Skeleton",14,6,2,7,3,0xFFE6E0C8},
                                {L"オーク",L"Orc",20,8,3,10,6,0xFFB0754A},{L"ゴーレム",L"Golem",30,9,5,14,8,0xFF9097A0},
                                {L"ゴースト",L"Ghost",18,11,2,16,10,0xFF8FD3F0}};
    }

    int Player::MaxHp() const {int v=baseHp;for(const int i:{weapon,armor,accessory})if(i>=0)v+=bag[i].hp;return v;}
    int Player::MaxMp() const {int v=baseMp;for(const int i:{weapon,armor,accessory})if(i>=0)v+=bag[i].mp;return v;}
    int Player::Atk() const {int v=baseAtk;for(const int i:{weapon,armor,accessory})if(i>=0)v+=bag[i].atk;return v;}
    int Player::Def() const {int v=baseDef;for(const int i:{weapon,armor,accessory})if(i>=0)v+=bag[i].def;return v;}
    int Player::Mag() const {int v=baseMag;for(const int i:{weapon,armor,accessory})if(i>=0)v+=bag[i].mag;return v;}

    int Game::Random(int lo,int hi)
    {
        random^=random<<13;random^=random>>17;random^=random<<5;
        return hi<=lo?lo:lo+static_cast<int>(random%static_cast<std::uint32_t>(hi-lo+1));
    }

    Tile Game::At(int x,int y) const
    {
        return x<0||y<0||x>=Width||y>=Height?Tile::Wall:tiles[static_cast<size_t>(y)*Width+x];
    }

    bool Game::Seen(int x,int y) const {return x>=0&&y>=0&&x<Width&&y<Height&&seen[static_cast<size_t>(y)*Width+x];}
    bool Game::Visible(int x,int y) const {return x>=0&&y>=0&&x<Width&&y<Height&&visible[static_cast<size_t>(y)*Width+x];}

    int Game::RoomAt(int x,int y) const
    {
        for(size_t i=0;i<rooms.size();++i)
        {
            const Room& r=rooms[i];
            if(x>=r.x&&x<r.x+r.w&&y>=r.y&&y<r.y+r.h)return static_cast<int>(i);
        }
        return -1;
    }

    bool Game::Free(int x,int y) const
    {
        if(At(x,y)==Tile::Wall||(x==player.x&&y==player.y))return false;
        if(std::any_of(monsters.begin(),monsters.end(),[&](const Monster& m){return m.x==x&&m.y==y;}))return false;
        return std::none_of(chests.begin(),chests.end(),[&](const Chest& c){return c.x==x&&c.y==y;});
    }

    bool Game::BossAlive() const
    {
        return std::any_of(monsters.begin(),monsters.end(),[](const Monster& m){return m.boss;});
    }

    void Game::Note(const std::wstring& text)
    {
        log.push_back(text);
        while(log.size()>LogLines)log.pop_front();
    }

    void Game::Say(const std::wstring& line)
    {
        Note(English()?hero+L": \""+line+L"\"":hero+L"「"+line+L"」");
    }

    Item Game::RandomItem()
    {
        const int tier=std::clamp((floor-1)/3+Random(-1,1),0,3);
        const int roll=Random(1,100);
        auto make=[&](ItemKind kind,const Gear& g,bool staff){
            Item item;
            item.kind=kind;
            item.name=English()?g.en:g.ja;
            item.atk=g.atk;item.def=g.def;item.mag=g.mag;item.hp=g.hp;item.mp=g.mp;item.staff=staff;
            return item;
        };
        if(roll<=35)return {ItemKind::Potion,T(L"回復薬",L"Potion"),0,0,0,20,0};
        if(roll<=50)return {ItemKind::Ether,T(L"魔法の水",L"Magic water"),0,0,0,0,8};
        if(roll<=70)return Random(0,1)?make(ItemKind::Weapon,Swords[tier],false):make(ItemKind::Weapon,Staves[tier],true);
        if(roll<=85)return make(ItemKind::Armor,Armors[tier],false);
        return make(ItemKind::Accessory,Accessories[Random(0,static_cast<int>(std::size(Accessories))-1)],false);
    }

    Monster Game::RandomMonster(bool boss)
    {
        Monster m;
        if(boss)
        {
            m.name=T(L"ドラゴン",L"Dragon");
            m.maxHp=25+6*floor;m.atk=5+floor;m.def=1+floor/3;m.xp=25+3*floor;
            m.color=0xFFE0483E;
            m.boss=true;
        }
        else
        {
            // Mostly the newest kinds the floor allows, now and then an older one.
            std::vector<const Kind*> allowed;
            for(const auto& k:Kinds)if(k.minFloor<=floor)allowed.push_back(&k);
            const int n=static_cast<int>(allowed.size());
            const Kind& k=*allowed[std::max(0,n-1-std::min(Random(0,3),n-1))];
            const int over=floor-k.minFloor;
            m.name=English()?k.en:k.ja;
            m.maxHp=k.hp+k.hp*over/5;m.atk=k.atk+over/2;m.def=k.def+over/3;m.xp=k.xp+over;
            m.color=k.color;
        }
        m.hp=m.maxHp;
        return m;
    }

    void Game::Start(std::uint32_t seed,const std::wstring& name)
    {
        random=seed?seed:0x9E3779B9u;
        for(int i=0;i<8;++i)Random(0,1);
        hero=name;
        player=Player{};
        floor=1;turns=0;kills=0;
        log.clear();
        Generate();
        Say(T(L"ダンジョンに着いたよ。いちばん奥の階段を目指そう！",L"We're in the dungeon. Let's find the stairs at the far end!"));
    }

    void Game::Generate()
    {
        tiles.assign(static_cast<size_t>(Width)*Height,Tile::Wall);
        seen.assign(tiles.size(),0);
        visible.assign(tiles.size(),0);
        rooms.clear();monsters.clear();chests.clear();bolt.clear();
        state=State::Playing;
        auto carve=[&](int x,int y){if(x>0&&y>0&&x<Width-1&&y<Height-1)tiles[static_cast<size_t>(y)*Width+x]=Tile::Floor;};

        // A room in each cell.
        for(int cy=0;cy<CellsY;++cy)
            for(int cx=0;cx<CellsX;++cx)
            {
                Room r;
                r.w=Random(7,CellW-4);r.h=Random(5,CellH-4);
                r.x=cx*CellW+Random(1,CellW-r.w-1);r.y=cy*CellH+Random(1,CellH-r.h-1);
                for(int y=r.y;y<r.y+r.h;++y)for(int x=r.x;x<r.x+r.w;++x)carve(x,y);
                rooms.push_back(r);
            }

        // Neighbouring cells joined: a tree through all of them (a random walk), and a couple more.
        const int count=CellsX*CellsY;
        std::vector<std::pair<int,int>> links;
        std::vector<std::uint8_t> reached(count,0);
        std::vector<int> stack{Random(0,count-1)};
        reached[stack.back()]=1;
        auto neighbours=[&](int c){
            std::vector<int> out;
            const int cx=c%CellsX,cy=c/CellsX;
            if(cx>0)out.push_back(c-1);
            if(cx<CellsX-1)out.push_back(c+1);
            if(cy>0)out.push_back(c-CellsX);
            if(cy<CellsY-1)out.push_back(c+CellsX);
            return out;
        };
        while(!stack.empty())
        {
            std::vector<int> open;
            for(const int n:neighbours(stack.back()))if(!reached[n])open.push_back(n);
            if(open.empty()){stack.pop_back();continue;}
            const int next=open[Random(0,static_cast<int>(open.size())-1)];
            links.push_back({stack.back(),next});
            reached[next]=1;
            stack.push_back(next);
        }
        for(int extra=Random(1,3);extra>0;--extra)
        {
            const int a=Random(0,count-1);
            const auto ns=neighbours(a);
            const int b=ns[Random(0,static_cast<int>(ns.size())-1)];
            if(std::none_of(links.begin(),links.end(),[&](const auto& l){return (l.first==a&&l.second==b)||(l.first==b&&l.second==a);}))
                links.push_back({a,b});
        }
        for(auto [a,b]:links)
        {
            if(a>b)std::swap(a,b);
            const Room &A=rooms[a],&B=rooms[b];
            if(b==a+1)
            {
                // Side by side: straight across where they overlap, else with one bend.
                const int x0=A.x+A.w,x1=B.x-1,lo=std::max(A.y,B.y),hi=std::min(A.y+A.h,B.y+B.h)-1;
                if(lo<=hi){const int y=Random(lo,hi);for(int x=x0;x<=x1;++x)carve(x,y);}
                else
                {
                    const int ya=Random(A.y,A.y+A.h-1),yb=Random(B.y,B.y+B.h-1),mx=Random(x0,x1);
                    for(int x=x0;x<=mx;++x)carve(x,ya);
                    for(int y=std::min(ya,yb);y<=std::max(ya,yb);++y)carve(mx,y);
                    for(int x=mx;x<=x1;++x)carve(x,yb);
                }
            }
            else
            {
                const int y0=A.y+A.h,y1=B.y-1,lo=std::max(A.x,B.x),hi=std::min(A.x+A.w,B.x+B.w)-1;
                if(lo<=hi){const int x=Random(lo,hi);for(int y=y0;y<=y1;++y)carve(x,y);}
                else
                {
                    const int xa=Random(A.x,A.x+A.w-1),xb=Random(B.x,B.x+B.w-1),my=Random(y0,y1);
                    for(int y=y0;y<=my;++y)carve(xa,y);
                    for(int x=std::min(xa,xb);x<=std::max(xa,xb);++x)carve(x,my);
                    for(int y=my;y<=y1;++y)carve(xb,y);
                }
            }
        }

        // Start in one room; the stairs in the one farthest from it (by links).
        const int start=Random(0,count-1);
        std::vector<int> distance(count,-1);
        std::vector<int> queue{start};
        distance[start]=0;
        for(size_t i=0;i<queue.size();++i)
            for(const auto& [a,b]:links)
            {
                const int other=a==queue[i]?b:b==queue[i]?a:-1;
                if(other>=0&&distance[other]<0){distance[other]=distance[queue[i]]+1;queue.push_back(other);}
            }
        const int goal=static_cast<int>(std::max_element(distance.begin(),distance.end())-distance.begin());
        auto spot=[&](const Room& r){return std::pair{Random(r.x,r.x+r.w-1),Random(r.y,r.y+r.h-1)};};
        const auto [sx,sy]=spot(rooms[goal]);
        tiles[static_cast<size_t>(sy)*Width+sx]=Tile::Stairs;
        std::tie(player.x,player.y)=spot(rooms[start]);

        // Chests and monsters, never in the starting room.
        auto place=[&](int room)->std::pair<int,int>{
            for(int tries=0;tries<50;++tries)
            {
                const auto [x,y]=spot(rooms[room]);
                if(Free(x,y)&&At(x,y)==Tile::Floor)return {x,y};
            }
            return {-1,-1};
        };
        auto otherRoom=[&]{int r;do r=Random(0,count-1);while(r==start);return r;};
        for(int n=Random(3,5);n>0;--n)
            if(const auto [x,y]=place(otherRoom());x>=0)chests.push_back({x,y,RandomItem()});
        for(int n=std::min(3+floor,14);n>0;--n)
            if(const auto [x,y]=place(otherRoom());x>=0){Monster m=RandomMonster(false);m.x=x;m.y=y;monsters.push_back(m);}
        if(floor%5==0)
            if(const auto [x,y]=place(goal);x>=0){Monster m=RandomMonster(true);m.x=x;m.y=y;monsters.push_back(m);}
        Reveal();
    }

    void Game::Reveal()
    {
        std::fill(visible.begin(),visible.end(),0);
        auto show=[&](int x,int y){if(x>=0&&y>=0&&x<Width&&y<Height)visible[static_cast<size_t>(y)*Width+x]=1;};
        if(const int r=RoomAt(player.x,player.y);r>=0)
        {
            const Room& room=rooms[r];
            for(int y=room.y-1;y<=room.y+room.h;++y)for(int x=room.x-1;x<=room.x+room.w;++x)show(x,y);
        }
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx)show(player.x+dx,player.y+dy);
        // Along the open floor, as far as a room lets her see: in a corridor, its next few
        // steps round its bends; from a room, a little way into the corridors leaving it.
        std::vector<std::pair<int,int>> frontier{{player.x,player.y}};
        std::vector<std::uint8_t> reached(tiles.size(),0);
        reached[static_cast<size_t>(player.y)*Width+player.x]=1;
        for(int step=0;step<SightSteps&&!frontier.empty();++step)
        {
            std::vector<std::pair<int,int>> next;
            for(const auto& [x,y]:frontier)
                for(const auto& [dx,dy]:{std::pair{1,0},std::pair{-1,0},std::pair{0,1},std::pair{0,-1}})
                {
                    const int nx=x+dx,ny=y+dy;
                    if(At(nx,ny)==Tile::Wall||reached[static_cast<size_t>(ny)*Width+nx])continue;
                    reached[static_cast<size_t>(ny)*Width+nx]=1;
                    show(nx,ny);
                    next.push_back({nx,ny});
                }
            frontier=std::move(next);
        }
        for(size_t i=0;i<seen.size();++i)seen[i]|=visible[i];
        for(auto& m:monsters)
            if(!m.awake&&Visible(m.x,m.y))
            {
                m.awake=true;
                if(m.boss)Say(T(L"大きいのがいる…！気をつけて！",L"Something big is here... careful!"));
            }
    }

    void Game::GainXp(int xp)
    {
        player.xp+=xp;
        while(player.xp>=10*player.level)
        {
            player.xp-=10*player.level;
            ++player.level;
            player.baseHp+=6;player.baseMp+=2;++player.baseAtk;++player.baseDef;++player.baseMag;
            player.hp=std::min(player.MaxHp(),player.hp+10);
            player.mp=std::min(player.MaxMp(),player.mp+3);
            Say(Fmt(T(L"レベル{0}になったよ！",L"I'm level {0} now!"),{std::to_wstring(player.level)}));
        }
    }

    void Game::Hit(Monster& m,int damage,const wchar_t* how)
    {
        m.hp-=damage;
        m.awake=true;
        Note(Fmt(T(L"{0}、{1}に{2}のダメージ。",L"{0} {1} for {2}."),{how,m.name,std::to_wstring(damage)}));
        if(m.hp>0)return;
        ++kills;
        Note(Fmt(T(L"{0}を倒した！",L"Defeated the {0}!"),{m.name}));
        if(m.boss)Say(T(L"やった、倒した！これで階段に行けるね。",L"We did it! Now the stairs are open."));
        GainXp(m.xp);
    }

    bool Game::Move(int dx,int dy)
    {
        if(state!=State::Playing)return false;
        bolt.clear();
        player.dirX=dx;player.dirY=dy;
        const int x=player.x+dx,y=player.y+dy;
        const auto target=std::find_if(monsters.begin(),monsters.end(),[&](const Monster& m){return m.x==x&&m.y==y;});
        if(target!=monsters.end())
        {
            const int damage=std::max(1,player.Atk()+Random(0,player.Atk()/2)-target->def/2);
            Hit(*target,damage,T(L"剣で攻撃",L"Sword hits"));
            if(target->hp<=0)monsters.erase(target);
            EndTurn();
            return true;
        }
        if(At(x,y)==Tile::Wall)return false;
        player.x=x;player.y=y;
        if(const auto chest=std::find_if(chests.begin(),chests.end(),[&](const Chest& c){return c.x==x&&c.y==y;});chest!=chests.end())
        {
            if(static_cast<int>(player.bag.size())>=BagSize)Note(T(L"宝箱がある。でも持ち物がいっぱい。",L"A chest, but the bag is full."));
            else
            {
                const Item item=chest->item;
                chests.erase(chest);
                player.bag.push_back(item);
                const int index=static_cast<int>(player.bag.size())-1;
                int* slot=item.kind==ItemKind::Weapon?&player.weapon:item.kind==ItemKind::Armor?&player.armor:item.kind==ItemKind::Accessory?&player.accessory:nullptr;
                const bool equip=slot&&*slot<0;
                if(equip)*slot=index;
                Say(Fmt(equip?T(L"宝箱だ！{0}が入ってた。さっそく装備するね。",L"A chest! There's a {0}. I'll put it on."):
                              T(L"宝箱だ！{0}が入ってた。",L"A chest! There's a {0}."),{item.name}));
            }
        }
        if(At(x,y)==Tile::Stairs)
        {
            if(BossAlive())Note(T(L"階段だ。でも、ボスがいて進めない。",L"The stairs, but the boss is in the way."));
            else state=State::AtStairs;
        }
        EndTurn();
        return true;
    }

    bool Game::Cast()
    {
        if(state!=State::Playing)return false;
        bolt.clear();
        if(player.mp<MagicCost){Note(T(L"MPが足りない。",L"Not enough MP."));return false;}
        player.mp-=MagicCost;
        int x=player.x,y=player.y;
        for(int step=0;step<MagicRange;++step)
        {
            x+=player.dirX;y+=player.dirY;
            if(At(x,y)==Tile::Wall)break;
            bolt.push_back({x,y});
            const auto target=std::find_if(monsters.begin(),monsters.end(),[&](const Monster& m){return m.x==x&&m.y==y;});
            if(target!=monsters.end())
            {
                const int mag=player.Mag();
                Hit(*target,std::max(1,mag*2+Random(0,mag)-target->def/2),T(L"魔法が当たって",L"Magic hits"));
                if(target->hp<=0)monsters.erase(target);
                EndTurn();
                return true;
            }
        }
        Note(T(L"魔法は何にも当たらなかった。",L"The magic hit nothing."));
        EndTurn();
        return true;
    }

    bool Game::Rest()
    {
        if(state!=State::Playing)return false;
        bolt.clear();
        EndTurn();
        return true;
    }

    bool Game::Use(int index)
    {
        if(state!=State::Playing||index<0||index>=static_cast<int>(player.bag.size()))return false;
        bolt.clear();
        Item& item=player.bag[index];
        if(item.kind==ItemKind::Potion||item.kind==ItemKind::Ether)
        {
            if(item.kind==ItemKind::Potion)player.hp=std::min(player.MaxHp(),player.hp+item.hp);
            else player.mp=std::min(player.MaxMp(),player.mp+item.mp);
            Note(Fmt(T(L"{0}を使った。",L"Used the {0}."),{item.name}));
            player.bag.erase(player.bag.begin()+index);
            for(int* slot:{&player.weapon,&player.armor,&player.accessory})if(*slot>index)--*slot;
        }
        else
        {
            int& slot=item.kind==ItemKind::Weapon?player.weapon:item.kind==ItemKind::Armor?player.armor:player.accessory;
            if(slot==index){slot=-1;Note(Fmt(T(L"{0}を外した。",L"Took off the {0}."),{item.name}));}
            else{slot=index;Note(Fmt(T(L"{0}を装備した。",L"Equipped the {0}."),{item.name}));}
            player.hp=std::min(player.hp,player.MaxHp());
            player.mp=std::min(player.mp,player.MaxMp());
        }
        EndTurn();
        return true;
    }

    void Game::Descend()
    {
        if(state!=State::AtStairs)return;
        ++floor;
        Generate();
        Say(Fmt(T(L"{0}階に着いたよ。",L"Here's floor {0}."),{std::to_wstring(floor)}));
    }

    void Game::StepOffStairs()
    {
        if(state==State::AtStairs)state=State::Playing;
    }

    void Game::EndTurn()
    {
        ++turns;
        // A little back now and then, so a careful player is not stuck.
        if(turns%10==0)player.hp=std::min(player.MaxHp(),player.hp+1);
        if(turns%8==0)player.mp=std::min(player.MaxMp(),player.mp+1);
        MonstersAct();
        if(state!=State::Dead)Reveal();
    }

    void Game::MonstersAct()
    {
        const int before=player.hp;
        for(auto& m:monsters)
        {
            if(!m.awake)continue;
            const int dx=player.x-m.x,dy=player.y-m.y;
            if(std::abs(dx)+std::abs(dy)==1)
            {
                const int damage=std::max(1,m.atk+Random(0,m.atk/3)-player.Def()/2);
                player.hp-=damage;
                Note(Fmt(T(L"{0}の攻撃。{1}のダメージ。",L"The {0} attacks for {1}."),{m.name,std::to_wstring(damage)}));
                if(player.hp<=0)
                {
                    player.hp=0;
                    state=State::Dead;
                    Say(T(L"うう…もうだめ…。",L"Ugh... I can't go on..."));
                    return;
                }
                continue;
            }
            // A step toward her: along the longer way first, else the other.
            const int sx=dx>0?1:dx<0?-1:0,sy=dy>0?1:dy<0?-1:0;
            const std::pair<int,int> first=std::abs(dx)>=std::abs(dy)?std::pair{sx,0}:std::pair{0,sy};
            const std::pair<int,int> second=std::abs(dx)>=std::abs(dy)?std::pair{0,sy}:std::pair{sx,0};
            for(const auto& [mx,my]:{first,second})
                if((mx||my)&&Free(m.x+mx,m.y+my)){m.x+=mx;m.y+=my;break;}
        }
        if(before>player.MaxHp()*3/10&&player.hp<=player.MaxHp()*3/10)
            Say(T(L"ちょっと危ないかも…回復しよう。",L"This is getting dangerous... let's heal."));
    }
}
