#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace MikuMikuDesktopPerch::BlockTower
{
    // The block tower mini game's rules and its physics (docs/BlockTower.md): 54 wooden blocks
    // stacked three to a level, each level across the one below. In turn, one block is taken out
    // from below the top (by magic: it is slid along its length) and laid on the top; the one who
    // brings the tower down loses.
    //
    // The physics is in centimetres and seconds, the table's top at y 0 and the tower's middle at
    // x 0, z 0; +z is her side of the table. Even levels lie along z, odd ones along x.
    constexpr int Levels=18,PerLevel=3,Blocks=Levels*PerLevel;
    constexpr float Length=7.5f,Thick=1.5f,Width=2.5f;   // a block's size (each a little off it)
    constexpr int You=0,Her=1;
    // A block's length in her upper body's lengths, as drawn (the table round the tower is sized by it).
    constexpr float BlockShare=0.22f;

    // A block: where its middle is, how it is turned (a quaternion x, y, z, w), its own size
    // (length, thickness, width), and its place in the tower (level, slot across: 0 to 2;
    // level -1: off the tower, as while it is being moved).
    struct Block
    {
        std::array<float,3> at{};
        std::array<float,4> turn{0,0,0,1};
        std::array<float,3> size{Length,Thick,Width};
        int level=0,slot=0;
    };

    // Choose: the one to move chooses a block. Pull: it is being slid out (it may go back while
    // it is still mostly in). Place: out, it waits over the top for its slot, then is laid there.
    // Settle: laid; the tower comes to rest. Fallen: the tower came down (Loser).
    enum class Phase {Choose,Pull,Place,Settle,Fallen};

    struct World;

    // How a pull is driven (hers and yours alike): the place along its length the magic draws the
    // block toward goes there at most this fast (cm a second).
    constexpr float MostSpeed=7.0f;

    class Game
    {
    public:
        Game();
        ~Game();
        Game(const Game&)=delete;
        Game& operator=(const Game&)=delete;

        // first: who moves first. care: how carefully she plays (1: her best; less: now and then
        // a looser choice, a quicker pull).
        void NewGame(std::uint32_t seed,int first,double care=1);
        // The physics and the phases on by seconds (in fixed steps).
        void Advance(double seconds);

        Phase Now() const {return phase;}
        int ToMove() const {return toMove;}
        int Loser() const {return loser;}
        Phase FellDuring() const {return fellDuring;}   // (Fallen) what was going on as it fell
        int Turns() const {return turns;}             // turns played to their end
        int Height() const;                          // levels with a block on them
        int TopCount() const;                        // blocks on the top level
        const Block& BlockAt(int b) const {return blocks[static_cast<size_t>(b)];}
        bool Legal(int b) const;                     // may be taken now (by the one to move)
        // Why a block may not be taken (empty: it may): in the language shown.
        std::wstring WhyNot(int b) const;
        // The block under a ray (from, to; cm), -1: none.
        int Pick(const std::array<float,3>& from,const std::array<float,3>& to) const;

        // The block being moved (-1: none), how far it has come out along its length (cm, signed),
        // the way it goes out (+1/-1 along its length; 0: not yet), and how hard the magic pulls
        // on it now (0-1 of the most it can) and how much the rest has moved (cm, the most any
        // block is off where it rested).
        int Active() const {return active;}
        float Offset() const;
        float Resistance() const {return resistance;}
        float Wobble() const {return wobble;}
        // The block's way along its length (unit vector, cm) as it lay.
        std::array<float,3> Axis(int b) const;
        // Your pull: the block chosen (Choose, or Pull while the one being pulled may still go back),
        // then where along its length it should be (cm from where it lay).
        bool Choose(int b);
        void PullTo(float offset);
        float Wanted() const {return wanted;}
        bool CanPutBack() const;
        void PutBack();
        // Place: the free slots on the top (0-2), and the one chosen (laid as soon as it is over it).
        std::vector<int> FreeSlots() const;
        bool Place(int slot);
        int Chosen() const {return slotChosen;}
        // A slot's place over the top (middle and its way along, as the block would lie there).
        Block SlotBlock(int slot) const;
        bool Hovering() const;                     // out, waiting over the top for a slot

        // Her turn: she starts to choose (on a thread of its own); once Thought(), Begin pulls the
        // block she chose (and she lays it in the slot she chooses once it is out). Probes: the
        // blocks she tried in her head, best first.
        void Think();
        bool Thinking() const {return thinkingNow;}
        bool Thought() const {return thoughtDone.load();}
        void BeginHers();
        bool HersRunning() const {return hersRunning;}
        int HerChoice() const {return herChoice;}
        int HerChoiceNow() const {return herOrder.empty()?-1:herOrder[0];}   // (once Thought)

        // Knocks heard since the last call (how hard, 0-1): a block landing, the tower falling.
        std::vector<float> TakeKnocks();
        // Seconds since the tower fell (Fallen), or since the turn began.
        double PhaseSeconds() const {return phaseTime;}
        // What happened in the last turn: the block came out after sticking, the tower swayed hard.
        float TurnWobble() const {return turnWobble;}
        float TurnResistance() const {return turnResistance;}
        // Diagnostics: a whole turn for the one to move, with her judgment (or at random: wild),
        // played out in the physics at once.
        void PlayTurn(bool wild=false);
        // Diagnostics: the top of the tower pushed sideways (it falls, the one to move losing).
        void Shove();
        std::wstring Report() const;

    private:
        friend std::wstring CheckRules();
        friend std::wstring CheckPlaying(int count,std::uint32_t seed);
        // A pull tried in her head: how it went.
        struct Trial {int block=-1,side=1;bool fell=false,out=false;float wobble=0,resistance=0,seconds=0,load=0;double score=0;};
        // Her choice: the blocks to try, best first (block * 2 + 1 for the way out along +, 0 for -).
        static std::vector<int> Judge(const std::array<Block,Blocks>& blocks,const std::vector<int>& legal,int toMove,
                                      double care,std::uint32_t seed,float speed,std::vector<Trial>* report);
        void StartTurn();
        void Rest();
        void StepOnce(double h);
        void Drive(double h);
        void CheckFall();
        void Join();
        int ChooseSlot() const;
        void Fell(int who);
        std::unique_ptr<World> world;
        std::array<Block,Blocks> blocks{};
        std::array<std::array<float,3>,Blocks> rest{};   // where each lay as the turn began
        std::array<std::array<float,4>,Blocks> restTurn{};
        Phase phase=Phase::Choose,fellDuring=Phase::Choose;
        std::wstring fallSeen,settleNote;
        int settleTimeouts=0;
        double settleSeconds=0;       // which block showed the tower falling, and how
        int toMove=You,loser=-1,turns=0,active=-1,slotChosen=-1;
        int side=0;                  // the active block's way out (+1/-1; 0: not yet)
        float wanted=0,target=0;     // where the player wants it, where the magic draws it now
        float resistance=0,wobble=0,turnWobble=0,turnResistance=0;
        bool returning=false;        // the active block going back where it lay
        bool committed=false;        // drawn out past a third once: it may no longer go back
        int pending=-1;              // the block to pull once it is back
        double crossFrom=0;          // (floatTime) when it set off for its slot
        std::array<float,3> home{};  // the active block's middle as it lay
        std::array<float,4> homeTurn{0,0,0,1};
        int homeLevel=0,homeSlot=0;
        // Out and over the top: the path it floats along (seconds into it).
        double floatTime=0,phaseTime=0,calmFor=0,accumulated=0;
        std::array<float,3> outAt{};
        std::array<float,4> outTurn{0,0,0,1};
        bool laid=false;
        std::vector<float> knocks;
        mutable std::mt19937 random{1};
        double care=1;
        // Her turn.
        std::thread worker;
        std::atomic<bool> thoughtDone{false};
        bool thinkingNow=false,hersRunning=false;
        std::vector<int> herOrder,herSides;   // blocks to try, best first, and their ways out
        int herIndex=0;
        int herChoice=-1,herSide=1;
        float herSpeed=3;
        double stuckFor=0;
        int herSlot=-1;
        double herWait=0;
    };

    // Checks (--blocktower-test, --blocktower-play-test): the first line of each is the failures.
    std::wstring CheckRules();
    std::wstring CheckPlaying(int count,std::uint32_t seed);
}
