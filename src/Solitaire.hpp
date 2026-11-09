#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace MikuMikuDesktopPerch::Solitaire
{
    // Mahjong solitaire's rules (docs/MahjongSolitaire.md): 144 tiles stacked in a layout. A tile
    // is free when nothing lies on it and its left or right side is open; two free tiles of a kind
    // are taken together. Alone, you try to clear them all; together with her, you take turns: the
    // one who cannot take a pair loses, and all taken is a draw.
    constexpr int TileCount=144;
    // A face: the 34 kinds (as in the mahjong atlas), then the seasons 春夏秋冬 (34-37) and the
    // flowers 梅蘭菊竹 (38-41). Any season goes with any season, any flower with any flower.
    constexpr int FaceCount=42,Seasons=34,Flowers=38;
    constexpr int GroupOf(int face){return face<Seasons?face:face<Flowers?34:35;}

    enum class Mode {Alone,Together};
    enum class Phase {YourMove,HerTurn,GameOver};
    constexpr int You=0,Her=1;

    // Where a tile lies, in half tiles across and down, and its layer.
    struct Spot {int x=0,y=0,z=0;};
    struct Pair
    {
        int a=-1,b=-1;
        bool Valid() const {return a>=0&&b>=0;}
    };

    constexpr int LayoutCountMax=11;
    int LayoutCount();
    const wchar_t* LayoutName(int layout);   // in the language shown
    const std::vector<Spot>& LayoutSpots(int layout);

    // Which tiles lie where and what stops them. Shared, made once per layout.
    struct Geometry;

    class Table
    {
    public:
        Table()=default;
        ~Table();
        Table(const Table&)=delete;
        Table& operator=(const Table&)=delete;

        // first: who takes first together (unused alone). care: how carefully she plays (0-1).
        void NewGame(std::uint32_t seed,Mode mode,int layout,int first,double care);
        // The same deal from the start again (the same first to take together); the counts
        // of pairs taken, shuffles and hints begin again too.
        void Restart();

        Mode GameMode() const {return mode;}
        int Layout() const {return layout;}
        Phase Now() const {return phase;}
        int ToMove() const {return toMove;}
        int First() const {return first;}
        int Winner() const {return winner;}              // You, Her, or -1 (going on, or a draw)
        const std::vector<Spot>& Spots() const;
        int FaceAt(int tile) const {return faces[static_cast<size_t>(tile)];}   // -1: taken
        bool Free(int tile) const;
        std::vector<Pair> Pairs() const;                 // the pairs that can be taken now
        int Left() const;                                // tiles still there
        bool Stuck() const;                              // tiles left, and no pair to take
        int Version() const {return version;}            // changes with the tiles
        int Taken() const {return taken;}                // pairs taken this game
        Pair Last() const {return last;}
        int LastWho() const {return lastWho;}
        int LastFaceA() const {return lastFaces[0];}
        int LastFaceB() const {return lastFaces[1];}

        // Takes the pair for the one to move; false when it may not be taken.
        bool Take(const Pair& pair);
        // Alone: takes the last pair back.
        bool CanUndo() const;
        bool Undo();
        // Alone: the tiles left dealt again on the spots they hold, so they can be cleared.
        bool Shuffle();
        int Shuffles() const {return shuffles;}
        // Alone: a pair worth taking (tried out a little further on); none when stuck.
        Pair Hint();
        int Hints() const {return hints;}
        // The pairs in the order the deal was made to clear in (for the checks).
        const std::vector<Pair>& Solution() const {return solution;}

        // Together: she starts to choose (on a thread of its own); then, once Thought(), her pair
        // is HerNext() and Step() takes it. Certain: what she has read to the end for sure, for
        // the one to move at that time: 1 they win, -1 they lose, 0 not read.
        void Think();
        bool Thinking() const {return thinkingNow;}
        bool Thought() const {return thoughtDone.load();}
        Pair HerNext() const {return next;}
        int Certain() const {return certain;}
        void Step();
        void SetThinkSeconds(double seconds){thinkSeconds=seconds;}
        // Diagnostics: your turn played as she would play it (or at random).
        void PlayForYou(bool wild);

    private:
        void Join();
        void Ended();
        std::shared_ptr<const Geometry> geometry;
        std::array<std::int8_t,TileCount> faces{},dealt{};   // (dealt: as the game began)
        std::vector<Pair> dealtSolution;
        Mode mode=Mode::Alone;
        Phase phase=Phase::YourMove;
        int layout=0,first=You,toMove=You,winner=-1,version=0,taken=0,lastWho=-1,shuffles=0,hints=0;
        Pair last;
        std::array<int,2> lastFaces{-1,-1};
        std::vector<std::pair<Pair,std::array<int,2>>> history;
        std::vector<Pair> solution;
        double care=1,thinkSeconds=0.25;
        std::mt19937 random{1};
        std::thread worker;
        std::atomic<bool> thoughtDone{false};
        bool thinkingNow=false;
        Pair next;
        std::atomic<int> certain{0};
    };

    // Checks (--solitaire-test, --solitaire-play-test): the first line of each is the failures.
    std::wstring CheckRules();
    std::wstring CheckPlaying(int count,std::uint32_t seed);
}
