#pragma once
#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch::Mahjong
{
    // The rules of the mahjong mini game (docs/Mahjong.md), apart from drawing: winning shapes,
    // yaku, fu and points, and the tiles a hand waits on.

    // Tiles by kind, 0 to 33: 1-9 characters (0-8), circles (9-17), bamboo (18-26), then the
    // winds East, South, West, North (27-30) and the dragons white, green, red (31-33).
    constexpr int Kinds=34;
    constexpr int East=27,South=28,West=29,North=30,White=31,Green=32,Red=33;
    using Counts=std::array<int,Kinds>;

    inline bool Honor(int t){return t>=East;}
    inline bool Terminal(int t){return t<East&&(t%9==0||t%9==8);}
    inline bool Simple(int t){return !Honor(t)&&!Terminal(t);}
    inline int Suit(int t){return Honor(t)?3:t/9;}

    // Written the usual short way: digits then their suit, "123m456p789s1122z" (z: 1-4 the winds
    // E S W N, 5-7 the dragons white, green, red), 0 for a red five. Tiles in order; red: how
    // many red fives. Empty when the text is not tiles.
    std::vector<int> ParseTiles(const std::wstring& text,int* red=nullptr);
    std::wstring TileText(int tile);
    std::wstring TilesText(const Counts& counts);

    // Chi: tile is the lowest of the run. Pon, Kan (open, called or added) and ClosedKan: the
    // tile of the set. (The 2-player game has no chi, but the counting allows it.)
    enum class MeldKind {Chi,Pon,Kan,ClosedKan};
    struct Meld {MeldKind kind=MeldKind::Pon;int tile=0;};

    // What the hand and the moment are, beyond the tiles.
    struct Situation
    {
        bool tsumo=false;
        bool riichi=false,doubleRiichi=false,ippatsu=false;
        bool lastTile=false;     // the wall's last tile: drawn (haitei) or discarded (houtei)
        bool rinshan=false;      // drawn after a kan
        bool chankan=false;      // robbed from an added kan
        bool firstDraw=false;    // the first draw, nothing called yet (tenhou for the dealer, chiihou otherwise)
        int roundWind=East,seatWind=East;   // the dealer sits East
        std::vector<int> dora,ura;          // the indicators (ura counts only with riichi)
        int red=0;                          // red fives among all the hand's tiles
        int honba=0;                        // 300 each, paid on top
    };

    struct YakuHan {const wchar_t* ja;const wchar_t* en;int han;};

    struct Score
    {
        bool win=false;           // a winning shape with a yaku
        std::vector<YakuHan> yaku;   // dora, ura and red fives last, by name
        int han=0,fu=0;
        bool yakuman=false;       // a real yakuman (not one counted to 13 han)
        int basic=0;              // the basic points (fu x 2^(han+2), limited)
        int points=0;             // what the other player pays, honba included
        const wchar_t* limitJa=L"";   // 満貫, 跳満, ... or empty
        const wchar_t* limitEn=L"";
    };

    // concealed: the tiles not in melds, the winning tile included (14, 11, 8, ... tiles).
    // win: the winning tile (one of them).
    Score Evaluate(const Counts& concealed,const std::vector<Meld>& melds,int win,const Situation& s);

    // Whether the tiles (3n+2 of them, melds aside) make a winning shape: four sets and a pair,
    // seven pairs or thirteen orphans (yaku aside).
    bool WinningShape(const Counts& concealed);
    // The kinds that would complete the tiles (3n+1 of them) into a winning shape. A kind the
    // hand already holds all four of is not a wait.
    std::vector<int> Waits(const Counts& concealed);
    // The tiles one may discard from 3n+2 tiles to be left waiting (for riichi).
    std::vector<int> TenpaiDiscards(const Counts& concealed);

    // Points as paid in the 2-player game: a dealer or not, the basic points, by ron or tsumo
    // (a tsumo is paid as the sum the three others would pay in a 4-player game).
    int Payment(int basic,bool dealer,bool tsumo,int honba);

    // The tiles nobody has seen yet (docs/Mahjong.md "見えない牌"): the wall and her hidden hand
    // are made of these, chosen only when they have to be.
    struct Unseen
    {
        Counts tiles{};
        std::array<int,3> red{};   // red fives among them, by suit
        static Unseen All();       // the 136 tiles, a red five in each suit
        int Size() const;
        // Seen now (drawn, discarded, shown): one of the kind, red or not.
        void Take(int tile,bool redFive=false);
        // One at random, taken: whether it is a red five.
        int Draw(std::mt19937& rng,bool* redFive=nullptr);
    };

    // How much a hand of hers is worth, by its basic points (docs/Mahjong.md "点数の帯"):
    // below 5200 for a non-dealer, 5200 to 7700, mangan, haneman to sanbaiman, yakuman.
    enum class Band {Cheap,Middle,Mangan,Haneman,Yakuman};
    Band BandOf(int basic);

    // A hand of hers, made from unseen tiles.
    struct Built
    {
        bool found=false;
        Counts concealed{};          // the tiles not in melds (the winning tile included)
        int win=-1;                  // the winning tile (-1: a hand that waits)
        std::array<bool,3> red{};    // red fives among the concealed tiles (the ron tile aside), by suit
        Score score;
        Band band=Band::Cheap;
    };
    // A winning hand worth about band: with her melds, on ron (the tile another discarded; -1
    // for a tsumo, its tile chosen among the unseen too). A ron is never on a wait she has
    // discarded (discards: her river). s: the moment (tsumo and red are set here; red counts the
    // red fives of her melds and the ron tile already). When no hand of the band can be made, a
    // lower band; found is false when none at all.
    Built BuildWin(const Unseen& unseen,const std::vector<Meld>& melds,int ron,const Counts& discards,
                   Situation s,Band band,std::mt19937& rng);
    // A hand that waits (for riichi, or shown at a draw), yaku aside. With discards, none of its
    // waits is among them.
    Built BuildTenpai(const Unseen& unseen,const std::vector<Meld>& melds,const Counts* discards,std::mt19937& rng);

    // Diagnostics (--mahjong-build-test): builds count hands in random games, checks each against
    // the tiles seen and the band asked for, and says how it went.
    std::wstring CheckBuilding(int count,std::uint32_t seed);

    // Diagnostics (--mahjong-test): checks hands written one a line, and says how each came out.
    //   <tiles> [options] => <expected>      a winning hand (its tiles not in melds, the winning
    //                                       tile last unless win= says)
    //   waits <tiles> => <tiles>|none        what the tiles wait on
    //   discards <tiles> => <tiles>|none     what may be discarded to be waiting
    // Options: win=<tile> tsumo riichi wriichi ippatsu last rinshan chankan first seat=E|S|W|N
    // round=E|S|W|N dora=<indicators> ura=<indicators> honba=<n> pon=<tile> kan=<tile>
    // ankan=<tile> chi=<lowest tile>. Expected: any of <n>han <n>fu <points>, or nowin.
    // Lines starting with # are comments. The first line of the result counts the failures.
    std::wstring CheckHands(const std::wstring& cases);
}
