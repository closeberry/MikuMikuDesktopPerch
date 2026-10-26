#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch::Dungeon
{
    // The rules of the dungeon mini game (docs/Dungeon.md), apart from drawing: a turn-based
    // roguelike on a grid of rooms joined by short corridors. Each action that takes a turn
    // lets the monsters act after it.

    enum class Tile:std::uint8_t {Wall,Floor,Stairs};
    enum class ItemKind {Potion,Ether,Weapon,Armor,Accessory};

    struct Item
    {
        ItemKind kind=ItemKind::Potion;
        std::wstring name;
        int atk=0,def=0,mag=0,hp=0,mp=0;   // for equipment, what it adds; for potions, what they give back
        bool staff=false;
    };

    struct Monster
    {
        std::wstring name;
        int x=0,y=0,hp=1,maxHp=1,atk=1,def=0,xp=1;
        std::uint32_t color=0xFF888888;   // ARGB
        bool boss=false,awake=false;
    };

    struct Chest {int x=0,y=0;Item item;};
    struct Room {int x=0,y=0,w=0,h=0;};

    struct Player
    {
        int x=0,y=0,dirX=1,dirY=0;
        int hp=30,mp=10,level=1,xp=0;
        int baseHp=30,baseMp=10,baseAtk=5,baseDef=2,baseMag=4;
        std::vector<Item> bag;
        int weapon=-1,armor=-1,accessory=-1;   // index in bag, or -1
        int MaxHp() const;
        int MaxMp() const;
        int Atk() const;
        int Def() const;
        int Mag() const;
        bool Equipped(int index) const {return index>=0&&(index==weapon||index==armor||index==accessory);}
    };

    class Game
    {
    public:
        static constexpr int Width=68,Height=39,BagSize=10,MagicCost=3,MagicRange=7;
        enum class State {Playing,AtStairs,Dead};

        // A new adventure on floor 1; hero is the character's name (in her lines).
        void Start(std::uint32_t seed,const std::wstring& hero);

        // Actions: false when nothing happened (a wall, too little MP): no turn passes.
        bool Move(int dx,int dy);
        bool Cast();
        bool Rest();
        bool Use(int index);
        void Descend();          // on the stairs: the next floor
        void StepOffStairs();    // on the stairs, staying: back to playing
        // Turn to face (dx, dy) without a turn passing (the camera behind her turns with her).
        void Face(int dx,int dy){if(dx||dy){player.dirX=dx;player.dirY=dy;bolt.clear();}}

        State GetState() const {return state;}
        int Floor() const {return floor;}
        int Deepest() const {return floor;}
        int Turns() const {return turns;}
        int Kills() const {return kills;}
        Tile At(int x,int y) const;
        bool Seen(int x,int y) const;
        bool Visible(int x,int y) const;
        const Player& Hero() const {return player;}
        const std::vector<Monster>& Monsters() const {return monsters;}
        const std::vector<Chest>& Chests() const {return chests;}
        const std::vector<Room>& Rooms() const {return rooms;}
        const std::deque<std::wstring>& Log() const {return log;}
        // The last magic bolt's path (for a moment's flash), cleared by the next action.
        const std::vector<std::pair<int,int>>& Bolt() const {return bolt;}
        bool BossAlive() const;

    private:
        void Generate();
        void Reveal();
        void EndTurn();
        void MonstersAct();
        void Hit(Monster& m,int damage,const wchar_t* how);
        void GainXp(int xp);
        void Say(const std::wstring& line);   // the hero's own words, in the log
        void Note(const std::wstring& text);
        Item RandomItem();
        Monster RandomMonster(bool boss);
        int RoomAt(int x,int y) const;
        bool Free(int x,int y) const;
        int Random(int lo,int hi);   // lo..hi inclusive

        std::vector<Tile> tiles;
        std::vector<std::uint8_t> seen,visible;
        std::vector<Room> rooms;
        std::vector<Monster> monsters;
        std::vector<Chest> chests;
        std::vector<std::pair<int,int>> bolt;
        std::deque<std::wstring> log;
        Player player;
        std::wstring hero;
        State state=State::Playing;
        int floor=1,turns=0,kills=0;
        std::uint32_t random=1;
    };
}
