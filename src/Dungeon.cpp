#define NOMINMAX
#include "Dungeon.hpp"
#include "HttpJson.hpp"
#include "Text.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace MikuMikuDesktopPerch::Dungeon
{
    namespace
    {
        constexpr int CellsX=4,CellsY=3,CellW=Game::Width/CellsX,CellH=Game::Height/CellsY;
        constexpr int LogLines=6;
        constexpr int SightSteps=5;   // how far she sees along open floor (corridors, dark rooms), in steps

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
        struct Plain {const wchar_t* ja;const wchar_t* en;ItemKind kind;int hp,mp,food;};
        constexpr Plain Supplies[]={{L"回復薬",L"Potion",ItemKind::Potion,20,0,0},{L"魔法の水",L"Magic water",ItemKind::Ether,0,8,0},
                                    {L"骨付き肉",L"Meat on the bone",ItemKind::Food,0,0,50},{L"大きな骨付き肉",L"Big meat on the bone",ItemKind::Food,0,0,100}};
        // Found only from minFloor down: in vaults (and now and then a chest deep down), or
        // from a boss. The English name is the id (the records keep it). about: a few words
        // for her to talk about it.
        struct Rare {const wchar_t* ja;const wchar_t* en;int minFloor;bool boss;const wchar_t* aboutJa;const wchar_t* aboutEn;};
        constexpr Rare Treasures[]={
            {L"古代のコイン",L"Ancient coin",4,false,L"遠い昔の王国で使われていた、すり減った金貨",L"a worn gold coin from a kingdom long gone"},
            {L"星のかけら",L"Star fragment",5,false,L"夜空から落ちてきたという、ほのかに光る石",L"a softly glowing stone said to have fallen from the night sky"},
            {L"妖精の小瓶",L"Fairy's vial",6,false,L"妖精が集めた朝露が入っているという小さな瓶",L"a little bottle said to hold dew gathered by fairies"},
            {L"古の指輪",L"Old ring",7,false,L"赤い宝石のついた、誰かの思い出の指輪",L"a ring with a red stone that once meant a lot to someone"},
            {L"銀の手鏡",L"Silver hand mirror",8,false,L"のぞくと少しだけ昔の景色が映るという手鏡",L"a hand mirror said to show a glimpse of long ago"},
            {L"月のしずく",L"Moon drop",10,false,L"月の光が固まってできたという青いしずく",L"a blue drop said to be moonlight turned solid"},
            {L"琥珀の化石",L"Amber fossil",10,false,L"大昔の小さな生き物が閉じこめられた琥珀",L"amber holding a tiny creature from ages past"},
            {L"魔法の砂時計",L"Magic hourglass",12,false,L"砂が落ちる間だけ、時間がゆっくり流れるという砂時計",L"an hourglass said to slow time while its sand falls"},
            {L"虹色の貝殻",L"Rainbow shell",12,false,L"角度で七色に光る、海の底の貝殻",L"a shell from the sea floor that shines in seven colours"},
            {L"精霊のオルゴール",L"Spirit music box",14,false,L"ふたを開けると、知らない国の歌が流れるオルゴール",L"a music box that plays a song from an unknown land"},
            {L"女神の羽根",L"Goddess feather",16,false,L"女神の翼から落ちたという、白く輝く羽根",L"a shining white feather said to have fallen from a goddess's wing"},
            {L"古代竜の牙",L"Ancient dragon fang",18,false,L"何千年も前の竜の、大きな牙",L"the great fang of a dragon from thousands of years ago"},
            {L"伝説の王冠",L"Legendary crown",20,false,L"伝説の王がかぶっていたという、宝石だらけの王冠",L"a jewel-covered crown said to have belonged to a legendary king"},
            {L"竜のうろこ",L"Dragon scale",1,true,L"ボスのドラゴンを倒して手に入れた、赤く硬いうろこ",L"a hard red scale won by defeating the boss dragon"},
            {L"黄金の聖杯",L"Golden chalice",15,true,L"深い階のドラゴンが守っていた、黄金の杯",L"a golden chalice guarded by a dragon in the depths"}};

        struct Kind {const wchar_t* ja;const wchar_t* en;int hp,atk,def,xp,minFloor;std::uint32_t color;};
        constexpr Kind Kinds[]={{L"スライム",L"Slime",6,3,0,2,1,0xFF5BC26A},{L"コウモリ",L"Bat",5,4,0,3,1,0xFF9A6BD1},
                                {L"ゴブリン",L"Goblin",10,5,1,5,2,0xFFA0A63C},{L"ガイコツ",L"Skeleton",14,6,2,7,3,0xFFE6E0C8},
                                {L"オーク",L"Orc",20,8,3,10,6,0xFFB0754A},{L"ゴーレム",L"Golem",30,9,5,14,8,0xFF9097A0},
                                {L"ゴースト",L"Ghost",18,11,2,16,10,0xFF8FD3F0}};
        constexpr Kind Dragon{L"ドラゴン",L"Dragon",0,0,0,0,5,0xFFE0483E};

        const std::vector<Title> TitleList={
            {"first",L"初めての冒険",L"First adventure",L"冒険から帰ってくる（やられても）",L"Come back from an adventure (even beaten)"},
            {"floor5",L"5階の冒険者",L"Floor 5 explorer",L"5階に着く",L"Reach floor 5"},
            {"floor10",L"10階の冒険者",L"Floor 10 explorer",L"10階に着く",L"Reach floor 10"},
            {"floor20",L"深淵を見た者",L"Seen the depths",L"20階に着く",L"Reach floor 20"},
            {"dragon",L"ドラゴン退治",L"Dragon slayer",L"ボスを倒す",L"Defeat a boss"},
            {"sword",L"剣ひとすじ",L"Sword alone",L"魔法を使わずに5階に着く",L"Reach floor 5 without magic"},
            {"mage",L"大魔法使い",L"Archmage",L"1回の冒険で、魔法で20匹倒す",L"Defeat 20 with magic in one adventure"},
            {"untouched",L"無傷の踏破",L"Untouched",L"1つの階を、ダメージを受けずに階段で降りる",L"Take the stairs from a floor without being hurt there"},
            {"house",L"モンスターハウス制覇",L"House cleaner",L"モンスターハウスのモンスターを全部倒す",L"Defeat every monster of a monster house"},
            {"vault",L"宝物庫の主",L"Vault opener",L"鍵で宝物庫を開ける",L"Open a vault with its key"},
            {"patron",L"お得意様",L"Good customer",L"合わせて5回、買い物をする",L"Buy 5 things in all"},
            {"traps",L"罠に好かれる者",L"Trap magnet",L"合わせて10回、罠を踏む",L"Step on 10 traps in all"},
            {"camp",L"焚き火の語らい",L"Fireside talks",L"合わせて5回、焚き火で休む",L"Rest at 5 campfires in all"},
            {"collector",L"コレクター",L"Collector",L"20種類の持ち物を見つける",L"Find 20 kinds of things"},
            {"hunter",L"モンスター博士",L"Monster expert",L"すべての種類のモンスターを倒す",L"Defeat every kind of monster"},
            {"treasure",L"宝物ハンター",L"Treasure hunter",L"宝物をすべて見つける",L"Find every treasure"},
            {"daily",L"今日も冒険",L"Daily adventurer",L"今日のダンジョンから帰ってくる",L"Come back from today's dungeon"}};

        // Her words without the LLM: one of these, at random.
        using Lines=std::initializer_list<std::pair<const wchar_t*,const wchar_t*>>;

        // What a shop asks for it (and gives half of, buying it back).
        int Price(const Item& item)
        {
            switch(item.kind)
            {
            case ItemKind::Potion:return 30;
            case ItemKind::Ether:return 40;
            case ItemKind::Food:return item.food>50?40:20;
            default:return 25+item.atk*12+item.def*12+item.mag*12+item.hp*3+item.mp*5;
            }
        }
    }

    const std::vector<Title>& Titles(){return TitleList;}

    std::vector<std::pair<std::wstring,std::wstring>> MonsterBook()
    {
        std::vector<std::pair<std::wstring,std::wstring>> out;
        for(const auto& k:Kinds)out.push_back({k.en,English()?k.en:k.ja});
        out.push_back({Dragon.en,English()?Dragon.en:Dragon.ja});
        return out;
    }

    std::vector<std::pair<std::wstring,std::wstring>> ItemBook()
    {
        std::vector<std::pair<std::wstring,std::wstring>> out;
        for(const auto& s:Supplies)out.push_back({s.en,English()?s.en:s.ja});
        for(const auto* list:{&Swords,&Staves,&Armors})for(const auto& g:*list)out.push_back({g.en,English()?g.en:g.ja});
        for(const auto& g:Accessories)out.push_back({g.en,English()?g.en:g.ja});
        return out;
    }

    std::vector<std::pair<std::wstring,std::wstring>> TreasureBook()
    {
        std::vector<std::pair<std::wstring,std::wstring>> out;
        for(const auto& t:Treasures)out.push_back({t.en,English()?t.en:t.ja});
        return out;
    }

    bool TreasureInfo(const std::wstring& id,std::wstring& name,std::wstring& about)
    {
        for(const auto& t:Treasures)
            if(id==t.en){name=English()?t.en:t.ja;about=English()?t.aboutEn:t.aboutJa;return true;}
        return false;
    }

    std::wstring TreasurePlace(const std::wstring& where)
    {
        if(where==L"vault")return T(L"宝物庫",L"a vault");
        if(where==L"boss")return T(L"ボスのドラゴン",L"the boss dragon");
        return T(L"宝箱",L"a chest");
    }

    void Record::Load(const std::wstring& path)
    {
        *this=Record{};
        std::ifstream in(std::filesystem::path(path),std::ios::binary);
        std::string raw;
        while(std::getline(in,raw))
        {
            if(!raw.empty()&&raw.back()=='\r')raw.pop_back();
            const size_t eq=raw.find('=');
            if(eq==std::string::npos)continue;
            const std::string key=raw.substr(0,eq);
            const std::wstring value=Net::Wide(raw.substr(eq+1));
            const int n=std::max(0,std::atoi(raw.c_str()+eq+1));
            const std::wstring name=Net::Wide(key.substr(key.find('.')+1));
            if(key=="best")best=n;
            else if(key=="runs")runs=n;
            else if(key=="traps")traps=n;
            else if(key=="bought")bought=n;
            else if(key=="fires")fires=n;
            else if(key=="dailyDate")dailyDate=value;
            else if(key=="dailyBest")dailyBest=n;
            else if(key.rfind("kill.",0)==0)kills[name]=n;
            else if(key.rfind("item.",0)==0)items.insert(name);
            else if(key.rfind("treasure.",0)==0)
            {
                // date|floor|where|count ("1" from before these were kept: one, date unknown).
                Found f;
                std::wstring fields[4];
                size_t at=0;
                for(int i=0;i<4;++i){const size_t bar=value.find(L'|',at);fields[i]=value.substr(at,bar==std::wstring::npos?std::wstring::npos:bar-at);if(bar==std::wstring::npos)break;at=bar+1;}
                if(value.find(L'|')==std::wstring::npos)f.count=std::max(1,n);
                else
                {
                    f.date=fields[0];
                    f.floor=std::max(0,_wtoi(fields[1].c_str()));
                    f.where=fields[2];
                    f.count=std::max(1,_wtoi(fields[3].c_str()));
                }
                treasures[name]=f;
            }
            else if(key.rfind("title.",0)==0)titles.insert(name);
        }
    }

    bool Record::Save(const std::wstring& path) const
    {
        std::string text="best="+std::to_string(best)+"\nruns="+std::to_string(runs)+"\ntraps="+std::to_string(traps)+
                         "\nbought="+std::to_string(bought)+"\nfires="+std::to_string(fires)+
                         "\ndailyDate="+Net::Utf8(dailyDate)+"\ndailyBest="+std::to_string(dailyBest)+"\n";
        for(const auto& [id,n]:kills)text+="kill."+Net::Utf8(id)+"="+std::to_string(n)+"\n";
        for(const auto& id:items)text+="item."+Net::Utf8(id)+"=1\n";
        for(const auto& [id,f]:treasures)
            text+="treasure."+Net::Utf8(id)+"="+Net::Utf8(f.date)+"|"+std::to_string(f.floor)+"|"+Net::Utf8(f.where)+"|"+std::to_string(f.count)+"\n";
        for(const auto& id:titles)text+="title."+Net::Utf8(id)+"=1\n";
        std::ofstream out(std::filesystem::path(path),std::ios::binary|std::ios::trunc);
        out<<text;
        return static_cast<bool>(out);
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
        const Tile t=At(x,y);
        if(t==Tile::Wall||t==Tile::Door||(x==player.x&&y==player.y))return false;
        if(std::any_of(monsters.begin(),monsters.end(),[&](const Monster& m){return Occupies(m,x,y);}))return false;
        if(std::any_of(fires.begin(),fires.end(),[&](const Fire& f){return f.x==x&&f.y==y;}))return false;
        return std::none_of(chests.begin(),chests.end(),[&](const Chest& c){return c.x==x&&c.y==y;});
    }

    bool Game::Empty(int x,int y) const
    {
        auto at=[&](const auto& list){return std::any_of(list.begin(),list.end(),[&](const auto& o){return o.x==x&&o.y==y;});};
        return Free(x,y)&&At(x,y)==Tile::Floor&&!at(traps)&&!at(wares)&&!at(golds)&&
               std::none_of(keysOnFloor.begin(),keysOnFloor.end(),[&](const auto& k){return k.first==x&&k.second==y;});
    }

    bool Game::BossAlive() const
    {
        return std::any_of(monsters.begin(),monsters.end(),[](const Monster& m){return m.boss;});
    }

    const Ware* Game::WareHere() const
    {
        const auto w=std::find_if(wares.begin(),wares.end(),[&](const Ware& w){return w.x==player.x&&w.y==player.y;});
        return w==wares.end()?nullptr:&*w;
    }

    void Game::Note(const std::wstring& text)
    {
        log.push_back(text);
        while(log.size()>LogLines)log.pop_front();
    }

    void Game::Say(Event event,const std::wstring& line,const std::wstring& facts,int weight)
    {
        talks.push_back({event,line,facts,weight});
    }

    // One of the lines, at random, in the language in use.
    #define LINE(...) [&]{const Lines l={__VA_ARGS__};const auto& p=*(l.begin()+Random(0,static_cast<int>(l.size())-1));return std::wstring(English()?p.second:p.first);}()

    void Game::Earn(const char* id)
    {
        if(!record)return;
        const std::wstring key=Net::Wide(id);
        if(!record->titles.insert(key).second)return;
        newTitles.push_back(id);
        const auto t=std::find_if(TitleList.begin(),TitleList.end(),[&](const Title& t){return std::string(t.id)==id;});
        if(t==TitleList.end())return;
        const std::wstring name=English()?t->en:t->ja;
        Note(Fmt(T(L"称号「{0}」を手に入れた！",L"New title: \"{0}\"!"),{name}));
        Say(Event::Title,Fmt(LINE({L"称号「{0}」だって！なんだか誇らしいね。",L"The title \"{0}\"! I feel kind of proud."},
                                  {L"「{0}」…ふふ、いい響き。",L"\"{0}\"... I like the sound of that."}),{name}),
            Fmt(T(L"冒険の中で、称号「{0}」を手に入れました（{1}）",L"You earned the title \"{0}\" in the adventure ({1})"),{name,English()?t->hintEn:t->hintJa}),3);
    }

    void Game::Found(const Item& item,const char* where)
    {
        if(!record)return;
        if(item.kind==ItemKind::Treasure)
        {
            // The first one's day, floor and place are kept (what she remembers it by).
            auto& f=record->treasures[item.id];
            if(f.count==0){f.date=record->today;f.floor=floor;f.where=Net::Wide(where);}
            ++f.count;
            if(record->treasures.size()>=std::size(Treasures))Earn("treasure");
            return;
        }
        record->items.insert(item.id);
        if(record->items.size()>=20)Earn("collector");
    }

    Item Game::Meat(bool big)
    {
        const Plain& p=Supplies[big?3:2];
        Item item;
        item.kind=p.kind;
        item.name=English()?p.en:p.ja;
        item.id=p.en;
        item.food=p.food;
        return item;
    }

    Item Game::RandomItem(int tierBonus)
    {
        // Hungry with nothing to eat: food is likelier.
        if(player.food<=40&&std::none_of(player.bag.begin(),player.bag.end(),[](const Item& i){return i.kind==ItemKind::Food;})&&Random(1,100)<=50)
            return Meat(Random(1,4)==1);
        const int tier=std::clamp((floor-1)/3+Random(-1,1)+tierBonus,0,3);
        const int roll=Random(1,100);
        auto gear=[&](ItemKind kind,const Gear& g,bool staff){
            Item item;
            item.kind=kind;
            item.name=English()?g.en:g.ja;
            item.id=g.en;
            item.atk=g.atk;item.def=g.def;item.mag=g.mag;item.hp=g.hp;item.mp=g.mp;item.staff=staff;
            return item;
        };
        auto supply=[&](const Plain& p){
            Item item;
            item.kind=p.kind;
            item.name=English()?p.en:p.ja;
            item.id=p.en;
            item.hp=p.hp;item.mp=p.mp;item.food=p.food;
            return item;
        };
        if(roll<=28)return supply(Supplies[0]);
        if(roll<=40)return supply(Supplies[1]);
        if(roll<=50)return supply(Supplies[Random(1,4)==1?3:2]);
        if(roll<=70)return Random(0,1)?gear(ItemKind::Weapon,Swords[tier],false):gear(ItemKind::Weapon,Staves[tier],true);
        if(roll<=85)return gear(ItemKind::Armor,Armors[tier],false);
        return gear(ItemKind::Accessory,Accessories[Random(0,static_cast<int>(std::size(Accessories))-1)],false);
    }

    // A treasure the floor allows (a boss's own, or the others), or a plain item when none
    // does. One not brought home yet is a little likelier.
    Item Game::Treasure(bool boss)
    {
        std::vector<const Rare*> allowed;
        for(const auto& t:Treasures)if(t.boss==boss&&t.minFloor<=floor)allowed.push_back(&t);
        if(allowed.empty())return RandomItem(1);
        const Rare* pick=Pick(allowed);
        if(record&&record->treasures.count(pick->en))pick=Pick(allowed);
        const Rare& t=*pick;
        Item item;
        item.kind=ItemKind::Treasure;
        item.name=English()?t.en:t.ja;
        item.id=t.en;
        return item;
    }

    Monster Game::RandomMonster(bool boss)
    {
        Monster m;
        if(boss)
        {
            m.name=English()?Dragon.en:Dragon.ja;
            m.id=Dragon.en;
            m.maxHp=25+6*floor;m.atk=5+floor;m.def=1+floor/3;m.xp=25+3*floor;
            m.color=Dragon.color;
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
            m.id=k.en;
            m.maxHp=k.hp+k.hp*over/5;m.atk=k.atk+over/2;m.def=k.def+over/3;m.xp=k.xp+over;
            m.color=k.color;
        }
        m.hp=m.maxHp;
        return m;
    }

    void Game::Start(std::uint32_t startSeed,const std::wstring& name,int closeness,bool today,Record& kept)
    {
        seed=startSeed?startSeed:0x9E3779B9u;
        random=seed;
        hero=name;
        bond=std::clamp(closeness,0,4);
        daily=today;
        record=&kept;
        player=Player{};
        floor=1;turns=0;kills=0;rested=0;magicKills=0;casts=0;
        saved=false;
        log.clear();talks.clear();newTitles.clear();
        Generate();
        Arrived();
    }

    void Game::Generate()
    {
        // Each floor from the adventure's seed and its number: today's dungeon is the same all
        // day, whatever happened on the floors before.
        random=seed^(static_cast<std::uint32_t>(floor)*0x9E3779B1u);
        if(!random)random=1;
        for(int i=0;i<8;++i)Random(0,1);
        tiles.assign(static_cast<size_t>(Width)*Height,Tile::Wall);
        seen.assign(tiles.size(),0);
        visible.assign(tiles.size(),0);
        rooms.clear();monsters.clear();chests.clear();bolt.clear();flame.clear();born.clear();
        traps.clear();wares.clear();golds.clear();fires.clear();keysOnFloor.clear();
        state=State::Playing;
        hurtOnFloor=false;houseWoke=false;
        player.keys=0;
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
        roomsEntered.assign(rooms.size(),0);

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
        auto distances=[&](int from,int without){
            std::vector<int> distance(count,-1);
            std::vector<int> queue{from};
            distance[from]=0;
            for(size_t i=0;i<queue.size();++i)
                for(const auto& [a,b]:links)
                {
                    const int other=a==queue[i]?b:b==queue[i]?a:-1;
                    if(other>=0&&other!=without&&distance[other]<0){distance[other]=distance[queue[i]]+1;queue.push_back(other);}
                }
            return distance;
        };
        const auto distance=distances(start,-1);
        const int goal=static_cast<int>(std::max_element(distance.begin(),distance.end())-distance.begin());
        auto spot=[&](const Room& r){return std::pair{Random(r.x,r.x+r.w-1),Random(r.y,r.y+r.h-1)};};
        const auto [sx,sy]=spot(rooms[goal]);
        tiles[static_cast<size_t>(sy)*Width+sx]=Tile::Stairs;
        std::tie(player.x,player.y)=spot(rooms[start]);

        // Rooms of their own kind, never the start or the goal (a dark one may be the goal).
        std::vector<int> spare;
        for(int i=0;i<count;++i)if(i!=start&&i!=goal)spare.push_back(i);
        auto take=[&](RoomKind kind)->int{
            if(spare.empty())return -1;
            const int at=Random(0,static_cast<int>(spare.size())-1);
            const int room=spare[at];
            if(kind==RoomKind::Vault)
            {
                // Locked away, it must leave every other room reachable.
                const auto d=distances(start,room);
                for(int i=0;i<count;++i)if(i!=room&&d[i]<0)return -1;
            }
            spare.erase(spare.begin()+at);
            rooms[room].kind=kind;
            return room;
        };
        const bool bossFloor=floor%5==0;
        if(floor>=2&&(floor%5==4||Random(1,100)<=40))take(RoomKind::Camp);   // always just before a boss
        if(floor>=2&&Random(1,100)<=35)take(RoomKind::Shop);
        const int vault=floor>=3&&Random(1,100)<=30?take(RoomKind::Vault):-1;
        if(floor>=4&&!bossFloor&&Random(1,100)<=22)take(RoomKind::House);
        if(floor>=2&&Random(1,100)<=20)take(RoomKind::Crystal);
        if(floor>=3&&Random(1,100)<=25)
        {
            int r=goal;
            if(Random(0,1)&&!spare.empty())r=spare[Random(0,static_cast<int>(spare.size())-1)];
            if(rooms[r].kind==RoomKind::Normal)rooms[r].kind=RoomKind::Dark;
        }

        // The vault's ways in are doors (all opened by its one key, which lies elsewhere).
        if(vault>=0)
        {
            const Room& r=rooms[vault];
            auto door=[&](int x,int y){if(At(x,y)==Tile::Floor)tiles[static_cast<size_t>(y)*Width+x]=Tile::Door;};
            for(int x=r.x;x<r.x+r.w;++x){door(x,r.y-1);door(x,r.y+r.h);}
            for(int y=r.y;y<r.y+r.h;++y){door(r.x-1,y);door(r.x+r.w,y);}
        }

        auto place=[&](int room)->std::pair<int,int>{
            for(int tries=0;tries<60;++tries)
            {
                const auto [x,y]=spot(rooms[room]);
                if(Empty(x,y))return {x,y};
            }
            return {-1,-1};
        };
        auto otherRoom=[&](std::initializer_list<RoomKind> not_){
            for(int tries=0;tries<50;++tries)
            {
                const int r=Random(0,count-1);
                if(r!=start&&std::find(not_.begin(),not_.end(),rooms[r].kind)==not_.end())return r;
            }
            return -1;
        };
        for(int i=0;i<count;++i)
        {
            const Room& r=rooms[i];
            if(r.kind==RoomKind::Camp)
                if(const auto [x,y]=std::pair{r.x+r.w/2,r.y+r.h/2};Empty(x,y))fires.push_back({x,y});
            if(r.kind==RoomKind::Shop)
                for(int n=Random(3,5);n>0;--n)
                    if(const auto [x,y]=place(i);x>=0)
                    {
                        Item item=RandomItem(Random(0,1));
                        wares.push_back({x,y,item,Price(item)});
                    }
            if(r.kind==RoomKind::Vault)
                for(int n=Random(2,3);n>0;--n)
                    if(const auto [x,y]=place(i);x>=0)chests.push_back({x,y,floor>=4&&Random(1,100)<=40?Treasure():RandomItem(1)});
            if(r.kind==RoomKind::House)
                for(int n=Random(6,9);n>0;--n)
                    if(const auto [x,y]=place(i);x>=0){Monster m=RandomMonster(false);m.x=x;m.y=y;m.house=true;monsters.push_back(m);}
        }
        if(vault>=0)
            if(const int r=otherRoom({RoomKind::Vault});r>=0)
                if(const auto [x,y]=place(r);x>=0)keysOnFloor.push_back({x,y});
        for(int n=Random(3,5);n>0;--n)
            if(const int r=otherRoom({RoomKind::Shop,RoomKind::Vault});r>=0)
                if(const auto [x,y]=place(r);x>=0)chests.push_back({x,y,floor>=8&&Random(1,100)<=4?Treasure():RandomItem()});   // deep down, now and then a treasure
        // From floor 2, most floors have a chest with something to eat.
        if(floor>=2&&Random(1,100)<=70)
            if(const int r=otherRoom({RoomKind::Shop,RoomKind::Vault});r>=0)
                if(const auto [x,y]=place(r);x>=0)chests.push_back({x,y,Meat(Random(1,4)==1)});
        for(int n=Random(1,3);n>0;--n)
            if(const int r=otherRoom({RoomKind::Shop,RoomKind::Vault});r>=0)
                if(const auto [x,y]=place(r);x>=0)golds.push_back({x,y,Random(5,10)+floor*3});
        for(int n=std::min(2+floor/2,8);n>0;--n)
            if(const int r=otherRoom({RoomKind::Shop,RoomKind::Vault,RoomKind::Camp});r>=0)
                if(const auto [x,y]=place(r);x>=0)
                {
                    const int roll=Random(1,100);
                    traps.push_back({x,y,roll<=35?TrapKind::Spikes:roll<=55?TrapKind::Warp:roll<=70?TrapKind::Pit:roll<=85?TrapKind::Alarm:TrapKind::Sleep});
                }
        for(int n=std::min(3+floor,14);n>0;--n)
            if(const int r=otherRoom({RoomKind::Vault,RoomKind::Shop});r>=0)
                if(const auto [x,y]=place(r);x>=0){Monster m=RandomMonster(false);m.x=x;m.y=y;monsters.push_back(m);}
        if(bossFloor)
        {
            // The dragon is huge: it takes 3x3 tiles of the room (all of them clear floor), or
            // one where the room is too small for that.
            Monster m=RandomMonster(true);
            for(int tries=0;tries<200&&m.size==0;++tries)
            {
                const auto [x,y]=spot(rooms[goal]);
                bool clear=true;
                for(int oy=-1;oy<=1&&clear;++oy)for(int ox=-1;ox<=1&&clear;++ox)clear=Empty(x+ox,y+oy);
                if(clear){m.x=x;m.y=y;m.size=1;}
            }
            if(m.size==0)if(const auto [x,y]=place(goal);x>=0){m.x=x;m.y=y;}
            if(m.size>0||m.x>0)monsters.push_back(m);
        }
        // From here on, the floor's play has its own run of chance.
        random^=static_cast<std::uint32_t>(turns)*0x85EBCA6Bu+1;
        Reveal();
    }

    // On a new floor (the first, the stairs, a pit): she says so, and the records move on.
    void Game::Arrived()
    {
        const std::wstring n=std::to_wstring(floor);
        bool deeper=false;
        if(record)
        {
            if(floor>record->best&&floor>1){record->best=floor;deeper=true;}
            else record->best=std::max(record->best,floor);
            if(daily)record->dailyBest=std::max(record->dailyBest,floor);
            if(floor>=5)Earn("floor5");
            if(floor>=10)Earn("floor10");
            if(floor>=20)Earn("floor20");
            if(floor>=5&&casts==0)Earn("sword");
        }
        if(floor==1)
            Say(Event::Arrive,daily?LINE({L"今日のダンジョンだね。どこまで行けるかな？",L"Today's dungeon. How far can we get?"})
                                   :LINE({L"ダンジョンに着いたよ。いちばん奥の階段を目指そう！",L"We're in the dungeon. Let's find the stairs at the far end!"},
                                         {L"さあ、冒険の始まり！一緒にがんばろうね。",L"Here we go, our adventure begins! Let's do our best."}),
                Fmt(T(L"話し相手と一緒に、{0}ダンジョンの1階に入りました。話し相手があなたを操作して進みます",
                      L"You have just entered {0}dungeon with the person you talk with, on floor 1. They guide you through it"),
                    {daily?T(L"今日だけの",L"today's "):T(L"",L"a ")}),4);
        else if(deeper)
            Say(Event::Arrive,Fmt(LINE({L"{0}階！ここまで来たのは初めてだよ。",L"Floor {0}! We've never been this deep."},
                                       {L"{0}階…新記録だね！",L"Floor {0}... a new record!"}),{n}),
                Fmt(T(L"ダンジョンの{0}階に着きました。これまででいちばん深い階です",L"You reached floor {0} of the dungeon, the deepest you have ever been"),{n}),5);
        else
            Say(Event::Arrive,Fmt(LINE({L"{0}階に着いたよ。",L"Here's floor {0}."},{L"{0}階だね。気をつけて進もう。",L"Floor {0}. Let's go carefully."},
                                       {L"よし、{0}階！",L"All right, floor {0}!"}),{n}),
                Fmt(T(L"ダンジョンの{0}階に着きました",L"You reached floor {0} of the dungeon"),{n}),3);
        if(floor%5==0)Note(T(L"この階のどこかに、ボスがいる。",L"A boss is somewhere on this floor."));
    }

    void Game::Reveal()
    {
        std::fill(visible.begin(),visible.end(),0);
        auto show=[&](int x,int y){if(x>=0&&y>=0&&x<Width&&y<Height)visible[static_cast<size_t>(y)*Width+x]=1;};
        const int here=RoomAt(player.x,player.y);
        if(here>=0&&rooms[here].kind!=RoomKind::Dark)
        {
            const Room& room=rooms[here];
            for(int y=room.y-1;y<=room.y+room.h;++y)for(int x=room.x-1;x<=room.x+room.w;++x)show(x,y);
        }
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx)show(player.x+dx,player.y+dy);
        // Along the open floor, as far as a room lets her see: in a corridor, its next few
        // steps round its bends; from a room, a little way into the corridors leaving it; in a
        // dark room, a few steps around her. A door is seen but not seen past.
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
                    if(At(nx,ny)!=Tile::Door)next.push_back({nx,ny});
                }
            frontier=std::move(next);
        }
        for(size_t i=0;i<seen.size();++i)seen[i]|=visible[i];

        // A room entered for the first time: a word about it.
        if(here>=0&&!roomsEntered[here])
        {
            roomsEntered[here]=1;
            const Room& room=rooms[here];
            if(room.kind==RoomKind::House)
            {
                houseWoke=true;
                for(auto& m:monsters)if(m.house)m.awake=true;
                Note(T(L"モンスターハウスだ！",L"A monster house!"));
                Say(Event::House,LINE({L"うわっ、モンスターがいっぱい！",L"Whoa, monsters everywhere!"},{L"ここ、モンスターの巣だ…！",L"This is a monster nest...!"}),
                    T(L"部屋に入ったら、モンスターがたくさんいるモンスターハウスでした。みんな目を覚ましてこちらに来ます",
                      L"The room you walked into is a monster house, full of monsters that are now awake and coming"),6);
            }
            else if(room.kind==RoomKind::Shop)
                Say(Event::Shop,LINE({L"お店だ！何か買っていこうか。",L"A shop! Shall we buy something?"},{L"こんなところにお店があるんだね。",L"A shop, down here of all places."}),
                    Fmt(T(L"ダンジョンの中でお店を見つけました（いま{0}ゴールド持っています）",L"You found a shop in the dungeon (you have {0} gold)"),{std::to_wstring(player.gold)}),3);
            else if(room.kind==RoomKind::Camp)
                Say(Event::Camp,LINE({L"焚き火がある。ちょっと休んでいこうよ。",L"A campfire. Let's rest a little."}),
                    T(L"焚き火のある部屋を見つけました。そこで休めます",L"You found a room with a campfire where you can rest"),2);
            else if(room.kind==RoomKind::Crystal)
                Say(Event::Crystal,LINE({L"わあ…きれい。水晶が光ってる。",L"Wow... so pretty. The crystals are glowing."},
                                        {L"見て、きらきらしてる…！",L"Look, it's all sparkling...!"}),
                    T(L"光る水晶がいっぱいの、とてもきれいな部屋に入りました",L"You walked into a beautiful room full of glowing crystals"),4);
            else if(room.kind==RoomKind::Dark)
                Say(Event::Room,LINE({L"暗い…足元に気をつけてね。",L"It's dark... watch your step."}),
                    T(L"真っ暗な部屋に入りました。まわりしか見えません",L"You walked into a pitch-dark room and can only see around you"),2);
            else
            {
                int foes=0;
                for(const auto& m:monsters)if(Visible(m.x,m.y)&&RoomAt(m.x,m.y)==here)++foes;
                const bool chest=std::any_of(chests.begin(),chests.end(),[&](const Chest& c){return RoomAt(c.x,c.y)==here;});
                bool stairs=false;
                for(int y=room.y;y<room.y+room.h;++y)for(int x=room.x;x<room.x+room.w;++x)stairs|=At(x,y)==Tile::Stairs;
                std::wstring facts=T(L"新しい部屋に入りました。",L"You walked into a new room. ");
                if(stairs)facts+=T(L"下りの階段があります。",L"The stairs down are here. ");
                if(chest)facts+=T(L"宝箱があります。",L"There is a chest. ");
                if(foes)facts+=Fmt(T(L"モンスターが{0}匹います。",L"{0} monster(s) are here. "),{std::to_wstring(foes)});
                if(stairs)Say(Event::Room,LINE({L"階段、見つけた！",L"Found the stairs!"},{L"あっ、階段だ。",L"Oh, the stairs."}),facts,2);
                else if(chest)Say(Event::Room,LINE({L"宝箱がある！",L"There's a chest!"},{L"何か入ってるかな？",L"I wonder what's inside?"}),facts,1);
                else if(foes>=3)Say(Event::Room,LINE({L"モンスターが多いね…気をつけて。",L"Lots of monsters... be careful."}),facts,1);
                else Say(Event::Room,LINE({L"ここは静かだね。",L"It's quiet here."},{L"次はどっちに行く？",L"Which way next?"},{L"広い部屋だね。",L"What a big room."}),facts,0);
            }
        }
        for(auto& m:monsters)
            if(!m.awake&&Visible(m.x,m.y))
            {
                m.awake=true;
                if(m.boss)Say(Event::Boss,LINE({L"大きいのがいる…！気をつけて！",L"Something big is here... careful!"},{L"ドラゴン…！負けないよ！",L"A dragon...! I won't lose!"}),
                              Fmt(T(L"ダンジョンの{0}階で、ボスのドラゴンを見つけました。倒さないと階段を使えません",L"On floor {0} you found the boss, a dragon. The stairs stay closed until it is beaten"),
                                  {std::to_wstring(floor)}),6);
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
            Note(Fmt(T(L"レベル{0}になった！",L"Level {0}!"),{std::to_wstring(player.level)}));
            Say(Event::LevelUp,Fmt(LINE({L"レベル{0}になったよ！",L"I'm level {0} now!"},{L"やった、レベル{0}！強くなった気がする。",L"Yay, level {0}! I feel stronger."}),
                                   {std::to_wstring(player.level)}),
                Fmt(T(L"ダンジョンでレベルが上がって、レベル{0}になりました",L"You levelled up in the dungeon and are now level {0}"),{std::to_wstring(player.level)}),4);
        }
    }

    void Game::Hit(Monster& m,int damage,const wchar_t* how,bool magic)
    {
        m.hp-=damage;
        m.awake=true;
        Note(Fmt(T(L"{0}、{1}に{2}のダメージ。",L"{0} {1} for {2}."),{how,m.name,std::to_wstring(damage)}));
        if(m.hp>0)
        {
            // A slime struck and still standing may split in two (once; the new one joins as
            // the monsters act, beside it).
            if(m.id==L"Slime"&&!m.split&&m.hp>=2&&Random(1,100)<=40)
                for(const auto& [ox,oy]:{std::pair{1,0},{-1,0},{0,1},{0,-1}})
                    if(Free(m.x+ox,m.y+oy)&&std::none_of(born.begin(),born.end(),[&](const Monster& b){return b.x==m.x+ox&&b.y==m.y+oy;}))
                    {
                        Monster half=m;
                        half.x=m.x+ox;half.y=m.y+oy;
                        half.hp=half.maxHp=m.hp/2;
                        m.hp-=half.hp;
                        m.split=half.split=true;
                        half.xp=std::max(1,m.xp/2);
                        born.push_back(half);
                        Note(Fmt(T(L"{0}が分裂した！",L"The {0} split in two!"),{m.name}));
                        break;
                    }
            return;
        }
        // A skeleton falls apart, and may pull itself together again (once).
        if(m.id==L"Skeleton"&&!m.risen&&Random(1,100)<=50)
        {
            m.risen=true;
            m.hp=std::max(1,m.maxHp/2);
            Note(Fmt(T(L"{0}は崩れ落ちた…が、また組み上がった！",L"The {0} fell apart... and pulled itself together again!"),{m.name}));
            return;
        }
        ++kills;
        if(m.stolen>0)
        {
            player.gold+=m.stolen;
            Note(Fmt(T(L"盗まれた{0}ゴールドを取り返した！",L"Got back the {0} gold it stole!"),{std::to_wstring(m.stolen)}));
            if(Random(1,100)<=60)Say(Event::Kill,LINE({L"返してもらったよ！",L"Got it back!"},{L"ふふん、取り返した！",L"Hah, it's mine again!"}),
                                     Fmt(T(L"ゴールドを盗んだ{0}を倒して、取り返しました",L"You defeated the {0} that stole your gold and got it back"),{m.name}),2);
        }
        Note(Fmt(T(L"{0}を倒した！",L"Defeated the {0}!"),{m.name}));
        if(record)
        {
            ++record->kills[m.id];
            const auto book=MonsterBook();
            if(std::all_of(book.begin(),book.end(),[&](const auto& b){return record->kills.count(b.first)>0;}))Earn("hunter");
        }
        if(magic&&++magicKills>=20)Earn("mage");
        // Now and then it leaves something to eat, picked up when there is room.
        if(Random(1,100)<=8&&static_cast<int>(player.bag.size())<BagSize)
        {
            const Item food=Meat(false);
            player.bag.push_back(food);
            Note(Fmt(T(L"{0}は{1}を落とした。拾った。",L"The {0} dropped a {1}. Picked it up."),{m.name,food.name}));
        }
        if(Random(1,100)<=45)
        {
            const int gold=Random(2,4)+floor*2;
            player.gold+=gold;
            Note(Fmt(T(L"{0}ゴールド手に入れた。",L"Got {0} gold."),{std::to_wstring(gold)}));
        }
        if(m.boss)
        {
            Say(Event::BossDown,LINE({L"やった、倒した！これで階段に行けるね。",L"We did it! Now the stairs are open."},
                                     {L"ドラゴンに勝ったよ！すごいよね、私たち！",L"We beat the dragon! Aren't we amazing!"}),
                Fmt(T(L"ダンジョンの{0}階で、話し相手と一緒にボスのドラゴンを倒しました",L"On floor {0} you and the person you talk with defeated the boss dragon"),{std::to_wstring(floor)}),7);
            Earn("dragon");
            const Item prize=Treasure(true);
            Found(prize,"boss");
            Note(Fmt(T(L"宝物「{0}」を手に入れた！（持ち帰ってデスクトップで眺められる）",L"Won a treasure: {0}! (to look at on the desktop)"),{prize.name}));
        }
        else if(m.house&&houseWoke&&std::none_of(monsters.begin(),monsters.end(),[&](const Monster& o){return o.house&&&o!=&m&&o.hp>0;}))
        {
            houseWoke=false;
            Earn("house");
            Say(Event::Kill,LINE({L"全部やっつけた！ふう…。",L"Got every last one! Phew..."}),
                T(L"モンスターハウスのモンスターを、全部倒しました",L"You defeated every monster in the monster house"),5);
        }
        else if(Random(1,100)<=20)
            Say(Event::Kill,LINE({L"やったね！",L"Nice!"},{L"よし、倒した！",L"Got it!"},{L"ふう、片付いた。",L"Phew, done."}),
                Fmt(T(L"{0}を倒しました",L"You defeated a {0}"),{m.name}),0);
        GainXp(m.xp);
    }

    // Hurt by a monster or a trap. Close enough to the user, she holds on once at 1 HP.
    void Game::Hurt(int damage,const std::wstring& by)
    {
        const int before=player.hp;
        player.hp-=damage;
        hurtOnFloor=true;
        if(player.hp<=0)
        {
            if(bond>=4&&!saved)
            {
                saved=true;
                player.hp=1;
                Note(T(L"…なんとか踏みとどまった！",L"...she held on somehow!"));
                Say(Event::Saved,LINE({L"まだ…倒れない。あなたがいてくれるから！",L"Not yet... I won't fall. You're with me!"}),
                    Fmt(T(L"{0}の攻撃で倒れそうになったけれど、話し相手との絆のおかげで、HP1で踏みとどまりました",
                          L"The {0} nearly beat you, but your bond with the person you talk with kept you standing at 1 HP"),{by}),8);
                return;
            }
            player.hp=0;
            state=State::Dead;
            Say(Event::Dead,LINE({L"うう…もうだめ…。",L"Ugh... I can't go on..."},{L"ごめんね…ここまでみたい…。",L"I'm sorry... this is as far as I go..."}),
                Fmt(T(L"ダンジョンの{0}階で、{1}にやられて倒れてしまいました",L"You were beaten by the {1} on floor {0} of the dungeon"),{std::to_wstring(floor),by}),9);
            return;
        }
        if(before>player.MaxHp()*3/10&&player.hp<=player.MaxHp()*3/10)
            Say(Event::LowHp,LINE({L"ちょっと危ないかも…回復しよう。",L"This is getting dangerous... let's heal."},{L"いたた…HPが少ないよ。",L"Ouch... my HP is low."}),
                Fmt(T(L"HPが残り{0}/{1}まで減って、危なくなってきました",L"Your HP is down to {0}/{1}, things are getting dangerous"),
                    {std::to_wstring(player.hp),std::to_wstring(player.MaxHp())}),5);
    }

    void Game::RestAtFire(Fire& fire)
    {
        if(fire.used){Note(T(L"焚き火は、もう燃え尽きている。",L"The campfire has burnt out."));return;}
        if(std::any_of(monsters.begin(),monsters.end(),[&](const Monster& m){return m.awake&&Visible(m.x,m.y);}))
        {
            Note(T(L"モンスターがいて、落ち着いて休めない。",L"Too many monsters about to rest."));
            return;
        }
        fire.used=true;
        player.hp=player.MaxHp();
        player.mp=player.MaxMp();
        ++rested;
        Note(T(L"焚き火で休んだ。HPとMPが回復した。",L"Rested by the fire. HP and MP restored."));
        if(bond>=3)
        {
            player.baseHp+=3;
            player.hp=player.MaxHp();
            Note(T(L"心まであたたまって、最大HPが3増えた。",L"Warmed to the heart: max HP +3."));
        }
        if(record&&++record->fires>=5)Earn("camp");
        std::wstring facts=Fmt(T(L"ダンジョンの{0}階で、話し相手と一緒に焚き火を囲んで休んでいます。ここまでに{1}匹のモンスターを倒し、レベル{2}です。"
                                 L"冒険のこと、話し相手のことなど、ゆっくり話しかけて（2〜3文）",
                                 L"On floor {0} of the dungeon you are resting by a campfire with the person you talk with. You have defeated {1} monsters and are level {2}. "
                                 L"Talk to them calmly about the adventure or about them (two or three sentences)"),
                               {std::to_wstring(floor),std::to_wstring(kills),std::to_wstring(player.level)});
        Say(Event::Camp,LINE({L"あったかいね…。こうして一緒に冒険できて、うれしいな。",L"It's warm... I'm glad we can go on adventures together like this."},
                             {L"ふう、ひと休み。ここまで来られたのは、あなたのおかげだよ。",L"Phew, a little break. We got this far thanks to you."}),facts,8);
    }

    // What lies where she has just stepped: gold, a key, a chest, goods, a trap. True when a pit
    // took her to the next floor.
    bool Game::StepOn(int x,int y)
    {
        if(const auto g=std::find_if(golds.begin(),golds.end(),[&](const Gold& g){return g.x==x&&g.y==y;});g!=golds.end())
        {
            player.gold+=g->amount;
            Note(Fmt(T(L"{0}ゴールド拾った。",L"Picked up {0} gold."),{std::to_wstring(g->amount)}));
            golds.erase(g);
        }
        if(const auto k=std::find(keysOnFloor.begin(),keysOnFloor.end(),std::pair{x,y});k!=keysOnFloor.end())
        {
            keysOnFloor.erase(k);
            ++player.keys;
            Note(T(L"鍵を拾った。",L"Picked up a key."));
            Say(Event::Key,LINE({L"鍵だ！どこかに開けられる扉があるはず。",L"A key! There must be a door it opens somewhere."}),
                T(L"ダンジョンで鍵を拾いました。この階のどこかに、鍵のかかった宝物庫があります",L"You picked up a key. Somewhere on this floor is a locked vault"),3);
        }
        if(const auto chest=std::find_if(chests.begin(),chests.end(),[&](const Chest& c){return c.x==x&&c.y==y;});chest!=chests.end())
        {
            if(chest->item.kind==ItemKind::Treasure)
            {
                const Item item=chest->item;
                chests.erase(chest);
                const bool fresh=record&&!record->treasures.count(item.id);
                const int room=RoomAt(x,y);
                Found(item,room>=0&&rooms[room].kind==RoomKind::Vault?"vault":"chest");
                Note(Fmt(T(L"宝物「{0}」を見つけた！（持ち帰ってデスクトップで眺められる）",L"Found a treasure: {0}! (to look at on the desktop)"),{item.name}));
                Say(Event::Treasure,Fmt(LINE({L"宝物の{0}だ…！きれい…。",L"The {0}, a real treasure...! So beautiful..."}),{item.name}),
                    Fmt(T(L"宝箱から、めったに見つからない宝物「{0}」が出てきました{1}",L"A chest held a rare treasure, the {0}{1}"),
                        {item.name,fresh?T(L"（初めて見つけました）",L" (the first you have found)"):T(L"",L"")}),6);
            }
            else if(static_cast<int>(player.bag.size())>=BagSize)Note(T(L"宝箱がある。でも持ち物がいっぱい。",L"A chest, but the bag is full."));
            else
            {
                const Item item=chest->item;
                chests.erase(chest);
                player.bag.push_back(item);
                Found(item);
                const int index=static_cast<int>(player.bag.size())-1;
                int* slot=item.kind==ItemKind::Weapon?&player.weapon:item.kind==ItemKind::Armor?&player.armor:item.kind==ItemKind::Accessory?&player.accessory:nullptr;
                const bool equip=slot&&*slot<0;
                if(equip)*slot=index;
                Note(Fmt(T(L"宝箱から{0}が出てきた。",L"The chest held a {0}."),{item.name}));
                Say(Event::Chest,Fmt(equip?LINE({L"宝箱だ！{0}が入ってた。さっそく装備するね。",L"A chest! There's a {0}. I'll put it on."})
                                          :LINE({L"宝箱だ！{0}が入ってた。",L"A chest! There's a {0}."},{L"{0}、もらっておくね。",L"I'll take the {0}."}),{item.name}),
                    Fmt(T(L"宝箱を開けたら、{0}が入っていました",L"You opened a chest and found a {0}"),{item.name}),1);
                // Close to the user, she finds a little more now and then.
                if(bond>=1&&Random(1,100)<=bond*7&&static_cast<int>(player.bag.size())<BagSize)
                {
                    Item extra=RandomItem();
                    extra.kind=ItemKind::Potion;extra.name=T(L"回復薬",L"Potion");extra.id=L"Potion";extra.hp=20;extra.atk=extra.def=extra.mag=extra.mp=extra.food=0;
                    player.bag.push_back(extra);
                    Note(T(L"おまけに回復薬も見つけた。",L"Found a potion too, as a bonus."));
                }
            }
        }
        if(const Ware* w=WareHere())
            Note(Fmt(T(L"{0}：{1}ゴールド（Enterで買う）",L"{0}: {1} gold (Enter: buy)"),{w->item.name,std::to_wstring(w->price)}));
        const auto trap=std::find_if(traps.begin(),traps.end(),[&](const Trap& t){return t.x==x&&t.y==y;});
        if(trap==traps.end())return false;
        trap->found=true;
        if(record&&++record->traps>=10)Earn("traps");
        const std::wstring f=std::to_wstring(floor);
        switch(trap->kind)
        {
        case TrapKind::Spikes:
        {
            const int damage=3+floor/2;
            Note(Fmt(T(L"トゲの罠だ！{0}のダメージ。",L"Spikes! {0} damage."),{std::to_wstring(damage)}));
            Say(Event::Trap,LINE({L"いたっ！トゲが出てきた！",L"Ow! Spikes!"},{L"痛い…罠だった…。",L"That hurt... a trap..."}),
                T(L"ダンジョンでトゲの罠を踏んで、ダメージを受けました",L"You stepped on a spike trap in the dungeon and got hurt"),4);
            Hurt(damage,T(L"トゲの罠",L"spike trap"));
            return false;
        }
        case TrapKind::Warp:
        {
            const int here=RoomAt(x,y);
            for(int tries=0;tries<60;++tries)
            {
                const int r=Random(0,static_cast<int>(rooms.size())-1);
                if(r==here||rooms[r].kind==RoomKind::Vault)continue;
                const int wx=Random(rooms[r].x,rooms[r].x+rooms[r].w-1),wy=Random(rooms[r].y,rooms[r].y+rooms[r].h-1);
                if(!Empty(wx,wy))continue;
                player.x=wx;player.y=wy;
                break;
            }
            Note(T(L"ワープの罠だ！どこかに飛ばされた。",L"A warp trap! Sent somewhere else."));
            Say(Event::Trap,LINE({L"わわっ、ここどこ？",L"Whoa, where am I?"},{L"飛ばされちゃった…。",L"I got sent flying..."}),
                T(L"ワープの罠を踏んで、同じ階の別の場所に飛ばされました",L"You stepped on a warp trap and were sent elsewhere on the floor"),4);
            return false;
        }
        case TrapKind::Pit:
            Note(T(L"落とし穴だ！下の階に落ちた。",L"A pit! She fell to the floor below."));
            Hurt(2,T(L"落とし穴",L"pit"));
            if(state==State::Dead)return false;
            ++floor;
            Generate();
            Say(Event::Trap,LINE({L"いたたた…落ちちゃった。",L"Ow ow ow... I fell."}),
                Fmt(T(L"落とし穴に落ちて、{0}階まで落ちてしまいました",L"You fell through a pit trap, down to floor {0}"),{std::to_wstring(floor)}),4);
            Arrived();
            return true;
        case TrapKind::Alarm:
            for(auto& m:monsters)m.awake=true;
            Note(T(L"警報の罠だ！フロア中のモンスターが目を覚ました。",L"An alarm trap! Every monster on the floor is awake."));
            Say(Event::Trap,LINE({L"うるさい音…みんな起きちゃった！",L"What a noise... everyone's awake now!"}),
                T(L"警報の罠を踏んで、階じゅうのモンスターが目を覚ましました",L"You stepped on an alarm trap and woke every monster on the floor"),4);
            return false;
        case TrapKind::Sleep:
            Note(T(L"眠りガスの罠だ！少しの間、眠ってしまった。",L"A sleep gas trap! She dozed off for a while."));
            Say(Event::Trap,LINE({L"ふわぁ…眠い…はっ！",L"Yawn... so sleepy... huh!"}),
                T(L"眠りガスの罠を踏んで、少し眠ってしまいました",L"You stepped on a sleep gas trap and dozed off for a moment"),4);
            for(int i=0;i<2&&state!=State::Dead;++i){++turns;MonstersAct();}
            return false;
        }
        return false;
    }

    bool Game::Move(int dx,int dy)
    {
        if(state!=State::Playing)return false;
        bolt.clear();flame.clear();
        player.dirX=dx;player.dirY=dy;
        const int x=player.x+dx,y=player.y+dy;
        const auto target=std::find_if(monsters.begin(),monsters.end(),[&](const Monster& m){return Occupies(m,x,y);});
        if(target!=monsters.end())
        {
            const int damage=std::max(1,player.Atk()+Random(0,player.Atk()/2)-target->def/2);
            Hit(*target,damage,T(L"剣で攻撃",L"Sword hits"),false);
            if(target->hp<=0)monsters.erase(target);
            EndTurn();
            return true;
        }
        if(const auto fire=std::find_if(fires.begin(),fires.end(),[&](const Fire& f){return f.x==x&&f.y==y;});fire!=fires.end())
        {
            if(fire->used||std::any_of(monsters.begin(),monsters.end(),[&](const Monster& m){return m.awake&&Visible(m.x,m.y);})){RestAtFire(*fire);return false;}
            RestAtFire(*fire);
            EndTurn();
            return true;
        }
        if(At(x,y)==Tile::Door)
        {
            if(player.keys<=0){Note(T(L"鍵がかかっている。",L"It's locked."));return false;}
            --player.keys;
            for(auto& t:tiles)if(t==Tile::Door)t=Tile::Floor;
            Note(T(L"鍵で扉を開けた。",L"Opened the door with the key."));
            Say(Event::Vault,LINE({L"開いた！宝物庫だよ、きっと。",L"It's open! A treasure vault, I bet."}),
                T(L"鍵で扉を開けて、宝物庫に入れるようになりました",L"You opened the door with the key: the vault is open"),5);
            Earn("vault");
            EndTurn();
            return true;
        }
        if(At(x,y)==Tile::Wall)return false;
        player.x=x;player.y=y;
        if(StepOn(x,y))return true;   // down a pit: a new floor
        if(state==State::Dead)return true;
        if(At(player.x,player.y)==Tile::Stairs)
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
        bolt.clear();flame.clear();
        if(player.mp<MagicCost){Note(T(L"MPが足りない。",L"Not enough MP."));return false;}
        player.mp-=MagicCost;
        ++casts;
        int x=player.x,y=player.y;
        for(int step=0;step<MagicRange;++step)
        {
            x+=player.dirX;y+=player.dirY;
            if(At(x,y)==Tile::Wall||At(x,y)==Tile::Door)break;
            bolt.push_back({x,y});
            const auto target=std::find_if(monsters.begin(),monsters.end(),[&](const Monster& m){return Occupies(m,x,y);});
            if(target!=monsters.end())
            {
                const int mag=player.Mag();
                Hit(*target,std::max(1,mag*2+Random(0,mag)-target->def/2),T(L"魔法が当たって",L"Magic hits"),true);
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
        bolt.clear();flame.clear();
        // Looking around: traps next to her come to light.
        for(auto& t:traps)
            if(!t.found&&std::abs(t.x-player.x)<=1&&std::abs(t.y-player.y)<=1)
            {
                t.found=true;
                Note(T(L"罠を見つけた！",L"Found a trap!"));
            }
        EndTurn();
        return true;
    }

    bool Game::Use(int index)
    {
        if(state!=State::Playing||index<0||index>=static_cast<int>(player.bag.size()))return false;
        bolt.clear();flame.clear();
        Item& item=player.bag[index];
        if(item.kind==ItemKind::Potion||item.kind==ItemKind::Ether||item.kind==ItemKind::Food)
        {
            if(item.kind==ItemKind::Potion)player.hp=std::min(player.MaxHp(),player.hp+item.hp);
            else if(item.kind==ItemKind::Ether)player.mp=std::min(player.MaxMp(),player.mp+item.mp);
            else{player.food=std::min(100,player.food+item.food);hungryTold=false;}
            Note(Fmt(item.kind==ItemKind::Food?T(L"{0}を食べた。",L"Ate the {0}."):T(L"{0}を使った。",L"Used the {0}."),{item.name}));
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

    bool Game::Buy()
    {
        if(state!=State::Playing)return false;
        const auto w=std::find_if(wares.begin(),wares.end(),[&](const Ware& w){return w.x==player.x&&w.y==player.y;});
        if(w==wares.end())return false;
        // Why not, in her words (the goods' question hides the log while she stands on them).
        if(player.gold<w->price)
        {
            Note(T(L"ゴールドが足りない。",L"Not enough gold."));
            Say(Event::Refused,Fmt(T(L"ゴールドが足りないみたい…。{0}ゴールドで、持ってるのは{1}ゴールドだよ。",L"We don't have enough gold... it's {0}, and we have {1}."),
                                   {std::to_wstring(w->price),std::to_wstring(player.gold)}),{},4);
            return false;
        }
        if(static_cast<int>(player.bag.size())>=BagSize)
        {
            Note(T(L"持ち物がいっぱい。",L"The bag is full."));
            Say(Event::Refused,T(L"持ち物がいっぱいで持てないよ。何か売ってから買おう（Shift＋数字キーで売れるよ）。",
                                 L"My bag is full, I can't carry it. Let's sell something first (Shift + a number key sells)."),{},4);
            return false;
        }
        player.gold-=w->price;
        player.bag.push_back(w->item);
        Found(w->item);
        Note(Fmt(T(L"{0}を{1}ゴールドで買った。",L"Bought the {0} for {1} gold."),{w->item.name,std::to_wstring(w->price)}));
        Say(Event::Bought,Fmt(LINE({L"{0}、買っちゃった。",L"Got the {0}."},{L"いい買い物したね！",L"A good buy!"}),{w->item.name}),
            Fmt(T(L"ダンジョンのお店で{0}を買いました",L"You bought a {0} at the dungeon shop"),{w->item.name}),1);
        wares.erase(w);
        if(record&&++record->bought>=5)Earn("patron");
        EndTurn();
        return true;
    }

    bool Game::InShop() const
    {
        const int r=RoomAt(player.x,player.y);
        return r>=0&&rooms[r].kind==RoomKind::Shop;
    }

    bool Game::Drop(int index)
    {
        if(state!=State::Playing||index<0||index>=static_cast<int>(player.bag.size()))return false;
        bolt.clear();flame.clear();
        const Item item=player.bag[index];
        player.bag.erase(player.bag.begin()+index);
        for(int* slot:{&player.weapon,&player.armor,&player.accessory})
        {
            if(*slot==index)*slot=-1;
            else if(*slot>index)--*slot;
        }
        player.hp=std::min(player.hp,player.MaxHp());
        player.mp=std::min(player.mp,player.MaxMp());
        if(InShop())
        {
            const int gold=std::max(1,Price(item)/2);
            player.gold+=gold;
            Note(Fmt(T(L"{0}を{1}ゴールドで売った。",L"Sold the {0} for {1} gold."),{item.name,std::to_wstring(gold)}));
            Say(Event::Sold,Fmt(LINE({L"{0}、{1}ゴールドになったよ。",L"The {0} got us {1} gold."}),{item.name,std::to_wstring(gold)}),{},1);
        }
        else Note(Fmt(T(L"{0}を捨てた。",L"Left the {0} behind."),{item.name}));
        EndTurn();
        return true;
    }

    void Game::Descend()
    {
        if(state!=State::AtStairs)return;
        if(!hurtOnFloor)Earn("untouched");
        ++floor;
        Generate();
        Arrived();
    }

    void Game::StepOffStairs()
    {
        if(state==State::AtStairs)state=State::Playing;
    }

    void Game::End()
    {
        if(!record)return;
        ++record->runs;
        Earn("first");
        if(daily)Earn("daily");
    }

    void Game::EndTurn()
    {
        ++turns;
        // Hungrier by the turn (gently): empty, she no longer heals and slowly weakens, never below 1 HP.
        if(turns%12==0&&player.food>0)
        {
            --player.food;
            // She says what would help: the food she carries (eaten with its number key), or
            // to look for some (chests, shops).
            const auto food=std::find_if(player.bag.begin(),player.bag.end(),[](const Item& i){return i.kind==ItemKind::Food;});
            const std::wstring key=food==player.bag.end()?std::wstring():std::to_wstring((food-player.bag.begin()+1)%10);
            if(player.food==20&&!hungryTold)
            {
                hungryTold=true;
                Say(Event::Hungry,food!=player.bag.end()?Fmt(T(L"おなかすいてきちゃった…。{0}、食べてもいい？（{1}キー）",L"I'm getting hungry... can I eat the {0}? (key {1})"),{food->name,key})
                                                        :T(L"おなかすいてきちゃった…。宝箱かお店で、骨付き肉を探そう。",L"I'm getting hungry... let's look for some meat in a chest or a shop."),
                    food!=player.bag.end()?Fmt(T(L"ダンジョンの中で、おなかがすいてきました（満腹度20%）。持ち物の{0}を食べたいと伝えて",L"You are getting hungry in the dungeon (20% full). Say you would like to eat the {0} you carry"),{food->name})
                                          :T(L"ダンジョンの中で、おなかがすいてきました（満腹度20%）。食べ物を持っていないので、宝箱やお店で探したいと伝えて",
                                             L"You are getting hungry in the dungeon (20% full). You carry no food: say you would like to find some in a chest or a shop"),4);
            }
            if(player.food==0)
                Say(Event::Starving,food!=player.bag.end()?Fmt(T(L"おなかぺこぺこ…。{0}を食べさせて（{1}キー）。",L"I'm starving... let me eat the {0} (key {1})."),{food->name,key})
                                                          :T(L"おなかぺこぺこ…。食べ物を見つけるまで、少しずつ力が抜けちゃう。",L"I'm starving... until we find food, I'll keep getting weaker."),
                    T(L"おなかがぺこぺこで、力が出なくなってきました（HPが少しずつ減ります）",L"You are starving and losing strength (HP slowly drops)"),5);
        }
        if(player.food>0)
        {
            // A little back now and then, so a careful player is not stuck.
            if(turns%10==0)player.hp=std::min(player.MaxHp(),player.hp+1);
            if(turns%8==0)player.mp=std::min(player.MaxMp(),player.mp+1);
        }
        else if(turns%4==0&&player.hp>1)--player.hp;
        MonstersAct();
        if(state!=State::Dead)Reveal();
    }

    void Game::MonstersAct()
    {
        // Those split off last turn join in (they act from the next).
        const size_t acting=monsters.size();
        for(auto& b:born)if(Free(b.x,b.y))monsters.push_back(b);
        born.clear();
        for(size_t i=0;i<acting&&state!=State::Dead;++i)
        {
            if(monsters[i].hp<=0)continue;
            Monster& m=monsters[i];
            if(!m.awake)continue;
            // How far she is from its body's edge (a big one's body is wider than a tile).
            const int dx=player.x-m.x,dy=player.y-m.y;
            const int ax=std::max(0,std::abs(dx)-m.size),ay=std::max(0,std::abs(dy)-m.size),gap=ax+ay;
            const bool inWall=At(m.x,m.y)==Tile::Wall;   // a ghost passing through: it cannot strike from there
            // A goblin with her gold runs: two steps a turn, away from her, and after a few
            // turns it is gone for good (with the gold).
            if(m.stolen>0)
            {
                if(++m.fleeing>8)
                {
                    Note(Fmt(T(L"{0}は{1}ゴールドを持って逃げていった…。",L"The {0} got away with {1} gold..."),{m.name,std::to_wstring(m.stolen)}));
                    m.hp=0;
                    continue;
                }
                for(int step=0;step<2;++step)Flee(m);
                continue;
            }
            // A bat flutters about now and then instead of what it meant to do.
            if(m.id==L"Bat"&&Random(1,100)<=40)
            {
                Wander(m);
                continue;
            }
            // The dragon breathes fire along a straight, open line (2-5 tiles), now and then.
            if(m.boss&&--m.breath<=0&&(ax==0||ay==0)&&gap>=2&&gap<=5&&Random(1,100)<=50)
            {
                // From the edge of its body straight at her.
                const int sx=ay==0?(dx>0?1:-1):0,sy=ax==0?(dy>0?1:-1):0;
                std::vector<std::pair<int,int>> line;
                bool open=true;
                for(int k=1;k<=gap;++k)
                {
                    const int x=sx?m.x+sx*(m.size+k):player.x,y=sy?m.y+sy*(m.size+k):player.y;
                    if(At(x,y)==Tile::Wall||At(x,y)==Tile::Door){open=false;break;}
                    line.push_back({x,y});
                }
                if(open)
                {
                    flame=line;
                    m.breath=4;
                    const int damage=std::max(2,m.atk+Random(0,m.atk/2)-player.Def()/3);
                    Note(Fmt(T(L"{0}が炎を吐いた！{1}のダメージ。",L"The {0} breathes fire for {1}!"),{m.name,std::to_wstring(damage)}));
                    if(Random(1,100)<=50)Say(Event::Trap,LINE({L"あつっ！炎だ！",L"Hot! Fire!"},{L"まっすぐ並ばないようにしよう！",L"Let's not stand in line with it!"}),
                                             Fmt(T(L"ボスのドラゴンが炎を吐いて、{0}のダメージを受けました",L"The boss dragon breathed fire at you for {0} damage"),{std::to_wstring(damage)}),2);
                    Hurt(damage,m.name);
                    continue;
                }
            }
            if(gap==1&&!inWall)
            {
                // An orc gathers itself for a heavy blow first (a step away and it is lost).
                if(m.id==L"Orc"&&!m.charging&&Random(1,100)<=35)
                {
                    m.charging=true;
                    Note(Fmt(T(L"{0}が力をためている…！",L"The {0} is gathering its strength...!"),{m.name}));
                    if(Random(1,100)<=40)Say(Event::Trap,LINE({L"何か来る…！離れたほうがいいかも。",L"Something's coming... maybe step back."}),
                                             Fmt(T(L"{0}が力をためています",L"The {0} is gathering strength for a heavy blow"),{m.name}),1);
                    continue;
                }
                // A goblin snatches gold when it can, then runs.
                if(m.id==L"Goblin"&&player.gold>0&&Random(1,100)<=20)
                {
                    const int amount=std::min(player.gold,std::max(1,player.gold*Random(20,40)/100));
                    player.gold-=amount;
                    m.stolen=amount;
                    Note(Fmt(T(L"{0}に{1}ゴールド盗まれた！",L"The {0} stole {1} gold!"),{m.name,std::to_wstring(amount)}));
                    Say(Event::Stolen,LINE({L"あっ、ゴールド返して！",L"Hey, give that back!"},{L"泥棒ー！待てー！",L"Thief! Come back here!"}),
                        Fmt(T(L"{0}に{1}ゴールドを盗まれました。倒せば取り返せます",L"A {0} stole {1} gold; defeating it gets it back"),{m.name,std::to_wstring(amount)}),4);
                    continue;
                }
                // Close to the user, she slips past a blow now and then.
                if(bond>=2&&Random(1,100)<=bond*2)
                {
                    m.charging=false;
                    Note(Fmt(T(L"{0}の攻撃を、ひらりとかわした！",L"Dodged the {0}'s attack!"),{m.name}));
                    if(Random(1,100)<=30)Say(Event::Dodged,LINE({L"見えてたよ！",L"Saw that coming!"},{L"当たらないよっ。",L"Missed me!"}),
                                             Fmt(T(L"{0}の攻撃をかわしました",L"You dodged the {0}'s attack"),{m.name}),0);
                    continue;
                }
                int damage=std::max(1,m.atk+Random(0,m.atk/3)-player.Def()/2);
                if(m.charging)
                {
                    m.charging=false;
                    damage=damage*9/5;
                    Note(Fmt(T(L"{0}の渾身の一撃！{1}のダメージ。",L"The {0} lands a crushing blow for {1}!"),{m.name,std::to_wstring(damage)}));
                }
                else Note(Fmt(T(L"{0}の攻撃。{1}のダメージ。",L"The {0} attacks for {1}."),{m.name,std::to_wstring(damage)}));
                Hurt(damage,m.name);
                continue;
            }
            m.charging=false;
            // Slow kinds take a step only every other turn.
            if((m.id==L"Slime"||m.id==L"Golem")&&(turns&1))continue;
            // A step toward her: along the longer way first, else the other. A ghost goes
            // through walls (never off the map, nor onto anyone).
            const int sx=dx>0?1:dx<0?-1:0,sy=dy>0?1:dy<0?-1:0;
            const std::pair<int,int> first=std::abs(dx)>=std::abs(dy)?std::pair{sx,0}:std::pair{0,sy};
            const std::pair<int,int> second=std::abs(dx)>=std::abs(dy)?std::pair{0,sy}:std::pair{sx,0};
            for(const auto& [mx,my]:{first,second})
                if((mx||my)&&(m.id==L"Ghost"?Passable(m.x+mx,m.y+my):m.size>0?FreeFor(m,m.x+mx,m.y+my):Free(m.x+mx,m.y+my))){m.x+=mx;m.y+=my;break;}
        }
        // Gone (a goblin that got away).
        monsters.erase(std::remove_if(monsters.begin(),monsters.end(),[](const Monster& m){return m.hp<=0;}),monsters.end());
    }

    // A ghost's way: through walls, but inside the map's edge and onto nobody.
    bool Game::Passable(int x,int y) const
    {
        if(x<1||y<1||x>=Width-1||y>=Height-1||(x==player.x&&y==player.y))return false;
        if(At(x,y)!=Tile::Wall)return Free(x,y);
        return std::none_of(monsters.begin(),monsters.end(),[&](const Monster& o){return Occupies(o,x,y);});
    }

    // Whether monster m (a big one too) could stand with its middle on (x, y): all its body on
    // open floor, clear of her, the others, campfires and chests.
    bool Game::FreeFor(const Monster& m,int x,int y) const
    {
        for(int oy=-m.size;oy<=m.size;++oy)
            for(int ox=-m.size;ox<=m.size;++ox)
            {
                const int tx=x+ox,ty=y+oy;
                const Tile t=At(tx,ty);
                if(t==Tile::Wall||t==Tile::Door||(tx==player.x&&ty==player.y))return false;
                if(std::any_of(monsters.begin(),monsters.end(),[&](const Monster& o){return &o!=&m&&Occupies(o,tx,ty);}))return false;
                if(std::any_of(fires.begin(),fires.end(),[&](const Fire& f){return f.x==tx&&f.y==ty;}))return false;
                if(std::any_of(chests.begin(),chests.end(),[&](const Chest& c){return c.x==tx&&c.y==ty;}))return false;
            }
        return true;
    }

    // A step away from her (the one that leaves it furthest), if there is one.
    void Game::Flee(Monster& m)
    {
        int best=std::abs(player.x-m.x)+std::abs(player.y-m.y),bx=0,by=0;
        for(const auto& [ox,oy]:{std::pair{1,0},{-1,0},{0,1},{0,-1}})
        {
            const int d=std::abs(player.x-m.x-ox)+std::abs(player.y-m.y-oy);
            if(d>best&&Free(m.x+ox,m.y+oy)){best=d;bx=ox;by=oy;}
        }
        m.x+=bx;m.y+=by;
    }

    // A step any way that is open.
    void Game::Wander(Monster& m)
    {
        std::vector<std::pair<int,int>> open;
        for(const auto& [ox,oy]:{std::pair{1,0},{-1,0},{0,1},{0,-1}})if(Free(m.x+ox,m.y+oy))open.push_back({ox,oy});
        if(open.empty())return;
        const auto [ox,oy]=open[Random(0,static_cast<int>(open.size())-1)];
        m.x+=ox;m.y+=oy;
    }
    #undef LINE
}
