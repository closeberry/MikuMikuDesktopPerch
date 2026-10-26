#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace MikuMikuDesktopPerch::Dungeon
{
    // The rules of the dungeon mini game (docs/Dungeon.md), apart from drawing: a turn-based
    // roguelike on a grid of rooms joined by short corridors. Each action that takes a turn
    // lets the monsters act after it.

    // Door: the way into a locked room, opened with a key (a wall until then).
    enum class Tile:std::uint8_t {Wall,Floor,Stairs,Door};
    // Food fills her up; a treasure goes straight into the records (never into the bag).
    enum class ItemKind {Potion,Ether,Weapon,Armor,Accessory,Food,Treasure};
    // Spikes hurt, Warp sends her to another room, Pit drops her to the next floor, Alarm wakes
    // every monster on the floor, Sleep has her doze while the monsters move.
    enum class TrapKind {Spikes,Warp,Pit,Alarm,Sleep};
    // Shop: goods for gold; Vault: locked, better chests inside; House: full of sleeping
    // monsters; Dark: seen only around her; Camp: a campfire to rest at; Crystal: a pretty room.
    enum class RoomKind {Normal,Shop,Vault,House,Dark,Camp,Crystal};

    struct Item
    {
        ItemKind kind=ItemKind::Potion;
        std::wstring name;
        int atk=0,def=0,mag=0,hp=0,mp=0;   // for equipment, what it adds; for potions, what they give back
        bool staff=false;
        int food=0;                        // for food, how much it fills her up
        std::wstring id;                   // the same in either language (the records' key)
    };

    struct Monster
    {
        std::wstring name,id;
        int x=0,y=0,hp=1,maxHp=1,atk=1,def=0,xp=1;
        std::uint32_t color=0xFF888888;   // ARGB
        bool boss=false,awake=false,house=false;
    };

    struct Chest {int x=0,y=0;Item item;};
    struct Room {int x=0,y=0,w=0,h=0;RoomKind kind=RoomKind::Normal;};
    struct Trap {int x=0,y=0;TrapKind kind=TrapKind::Spikes;bool found=false;};
    struct Ware {int x=0,y=0;Item item;int price=0;};   // goods on a shop's floor
    struct Gold {int x=0,y=0,amount=0;};
    struct Fire {int x=0,y=0;bool used=false;};          // a campfire (in the way, rested at by walking into it)

    struct Player
    {
        int x=0,y=0,dirX=1,dirY=0;
        int hp=30,mp=10,level=1,xp=0;
        int baseHp=30,baseMp=10,baseAtk=5,baseDef=2,baseMag=4;
        int gold=0,food=100,keys=0;
        std::vector<Item> bag;
        int weapon=-1,armor=-1,accessory=-1;   // index in bag, or -1
        int MaxHp() const;
        int MaxMp() const;
        int Atk() const;
        int Def() const;
        int Mag() const;
        bool Equipped(int index) const {return index>=0&&(index==weapon||index==armor||index==accessory);}
    };

    // What she has done over all her adventures (characters\<model>.dungeon.txt): the deepest
    // floor, today's dungeon, the monsters defeated, things found, and the titles earned.
    struct Record
    {
        int best=0,runs=0,traps=0,bought=0,fires=0;
        std::wstring dailyDate;   // YYYY-MM-DD of dailyBest
        int dailyBest=0;
        std::map<std::wstring,int> kills;   // by monster id
        std::set<std::wstring> items,treasures,titles;
        void Load(const std::wstring& path);
        bool Save(const std::wstring& path) const;
    };

    // The titles, in the order the records list them.
    struct Title {const char* id;const wchar_t* ja;const wchar_t* en;const wchar_t* hintJa;const wchar_t* hintEn;};
    const std::vector<Title>& Titles();
    // The monsters and items there are to find (ids and names in the language in use), for the records.
    std::vector<std::pair<std::wstring,std::wstring>> MonsterBook();
    std::vector<std::pair<std::wstring,std::wstring>> ItemBook();
    std::vector<std::pair<std::wstring,std::wstring>> TreasureBook();

    // Something she has words for. The window shows line (or asks the LLM, giving it facts);
    // weight: how much it matters (a heavier one wins when several come at once).
    enum class Event {Arrive,Room,House,Boss,BossDown,LevelUp,Chest,LowHp,Trap,Camp,Shop,Bought,Key,Vault,Treasure,Hungry,Starving,Dead,Kill,Crystal,Title,Saved,Dodged};
    struct Talk {Event event=Event::Room;std::wstring line,facts;int weight=1;};

    class Game
    {
    public:
        static constexpr int Width=68,Height=39,BagSize=10,MagicCost=3,MagicRange=7;
        enum class State {Playing,AtStairs,Dead};

        // A new adventure on floor 1. hero: the character's name (in her lines); bond: how
        // close she and the user are (0-4: a little help, never a hindrance); daily: today's
        // dungeon (seed from the date: the same floors all day). The record is kept up to date
        // as she goes, and must outlive the game.
        void Start(std::uint32_t seed,const std::wstring& hero,int bond,bool daily,Record& record);

        // Actions: false when nothing happened (a wall, too little MP): no turn passes.
        bool Move(int dx,int dy);
        bool Cast();
        bool Rest();             // waits a turn, looking for traps around her
        bool Use(int index);
        bool Buy();              // the goods she stands on
        void Descend();          // on the stairs: the next floor
        void StepOffStairs();    // on the stairs, staying: back to playing
        void End();              // the adventure is over (back home or beaten): the records count it        // Turn to face (dx, dy) without a turn passing (the camera behind her turns with her).
        void Face(int dx,int dy){if(dx||dy){player.dirX=dx;player.dirY=dy;bolt.clear();}}

        State GetState() const {return state;}
        int Floor() const {return floor;}
        int Turns() const {return turns;}
        int Kills() const {return kills;}
        bool Daily() const {return daily;}
        Tile At(int x,int y) const;
        bool Seen(int x,int y) const;
        bool Visible(int x,int y) const;
        const Player& Hero() const {return player;}
        const std::vector<Monster>& Monsters() const {return monsters;}
        const std::vector<Chest>& Chests() const {return chests;}
        const std::vector<Room>& Rooms() const {return rooms;}
        const std::vector<Trap>& Traps() const {return traps;}
        const std::vector<Ware>& Wares() const {return wares;}
        const std::vector<Gold>& Golds() const {return golds;}
        const std::vector<Fire>& Fires() const {return fires;}
        const std::vector<std::pair<int,int>>& Keys() const {return keysOnFloor;}
        const Ware* WareHere() const;
        int RoomAt(int x,int y) const;
        const std::deque<std::wstring>& Log() const {return log;}
        // What she would say since this was last called (oldest first).
        std::vector<Talk> TakeTalks(){return std::exchange(talks,{});}
        // The last magic bolt's path (for a moment's flash), cleared by the next action.
        const std::vector<std::pair<int,int>>& Bolt() const {return bolt;}
        bool BossAlive() const;
        // Titles earned in this adventure (ids), newest last.
        const std::vector<std::string>& NewTitles() const {return newTitles;}
        int Rested() const {return rested;}   // campfires rested at

    private:
        void Generate();
        void Reveal();
        void EndTurn();
        void MonstersAct();
        void Hit(Monster& m,int damage,const wchar_t* how,bool magic);
        void Hurt(int damage,const std::wstring& by);
        void GainXp(int xp);
        void Say(Event event,const std::wstring& line,const std::wstring& facts,int weight);
        void Note(const std::wstring& text);
        void Arrived();
        void Found(const Item& item);
        void Earn(const char* title);
        bool StepOn(int x,int y);
        void RestAtFire(Fire& fire);
        Item RandomItem(int tierBonus=0);
        Item Treasure();
        Monster RandomMonster(bool boss);
        bool Free(int x,int y) const;    // nothing standing there (monsters may walk it)
        bool Empty(int x,int y) const;   // nothing at all there (a place to put something)
        int Random(int lo,int hi);       // lo..hi inclusive
        template<class T> const T& Pick(const std::vector<T>& from){return from[Random(0,static_cast<int>(from.size())-1)];}

        std::vector<Tile> tiles;
        std::vector<std::uint8_t> seen,visible;
        std::vector<Room> rooms;
        std::vector<std::uint8_t> roomsEntered;
        std::vector<Monster> monsters;
        std::vector<Chest> chests;
        std::vector<Trap> traps;
        std::vector<Ware> wares;
        std::vector<Gold> golds;
        std::vector<Fire> fires;
        std::vector<std::pair<int,int>> keysOnFloor;
        std::vector<std::pair<int,int>> bolt;
        std::deque<std::wstring> log;
        std::vector<Talk> talks;
        std::vector<std::string> newTitles;
        Player player;
        std::wstring hero;
        Record* record=nullptr;
        State state=State::Playing;
        int floor=1,turns=0,kills=0,bond=0,rested=0;
        int magicKills=0,casts=0;
        bool daily=false,hurtOnFloor=false,saved=false,houseWoke=false,hungryTold=false,lowTold=false;
        std::uint32_t seed=1,random=1;
    };
}
