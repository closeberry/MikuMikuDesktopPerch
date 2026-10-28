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

    // How many tiles from waiting (0: waiting, -1: a winning shape), for 3n+1 or 3n+2 tiles not
    // in melds: the least of four sets and a pair, seven pairs and the thirteen orphans (those
    // two only with no melds).
    int Shanten(const Counts& concealed,int melds);

    // The wall: the tiles nobody holds or has seen, drawn from at random.
    struct Unseen
    {
        Counts tiles{};
        std::array<int,3> red{};   // red fives among them, by suit
        static Unseen All();       // the 136 tiles, a red five in each suit
        int Size() const;
        // Taken out: one of the kind, red or not.
        void Take(int tile,bool redFive=false);
        // One at random, taken: whether it is a red five.
        int Draw(std::mt19937& rng,bool* redFive=nullptr);
    };

    // The table of the 2-player game (docs/Mahjong.md): you against her, an East and a South
    // round of two hands each (and the dealer's repeats). She plays her own hand from what she
    // can see.
    constexpr int You=0,Her=1;
    constexpr int Hands=4;        // East 1, East 2, South 1, South 2
    constexpr int DrawsEach=24;   // the live wall: 24 draws each, then the hand is drawn

    // A tile in play: its kind, and whether it is a red five.
    struct Piece {int kind=0;bool red=false;};
    struct RiverTile {Piece piece;bool sideways=false,taken=false;};
    // A wait after a riichi discard: the kind, what winning on it pays (by ron, or by tsumo when
    // furiten), and how many of it you cannot see (in the wall or her hand).
    struct RiichiWait {int kind=0,points=0,left=0;bool furiten=false;};   // sideways: riichi; taken: called by the other

    struct Seat
    {
        int points=25000;
        std::vector<Piece> hand;      // in order (the drawn tile apart)
        Piece drawn;                  // the tile just drawn
        bool hasDrawn=false;
        std::vector<Meld> melds;
        int meldRed=0;                // red fives in the melds
        std::vector<RiverTile> river;
        bool riichi=false,doubleRiichi=false,ippatsu=false;
        int draws=0;
        bool passedWin=false;         // let a winning tile go since the last discard (furiten)
        bool passedInRiichi=false;    // ... after riichi (furiten for good)
        std::vector<int> passedAfterRiichi;   // kinds the other let go after this seat's riichi
    };

    struct RoundEnd
    {
        bool draw=false;              // the wall ran out
        int winner=-1;
        bool tsumo=false;
        Score score;
        std::vector<Piece> hand;      // the winner's concealed tiles, the winning tile last
        std::vector<Meld> melds;
        std::vector<int> ura;         // revealed for a riichi win
        std::array<bool,2> tenpai{};  // at a draw
        std::vector<Piece> herHand;   // at a draw, her hand if she waits
        std::array<int,2> change{};   // points moved, by seat
    };

    // YourTurn: discard (or tsumo, kan, riichi). YourCall: her discard can be called (ron, pon,
    // kan) or let go. HerTurn: she draws, then discards (Step each). RoundOver: the hand's end
    // is shown (NextRound goes on). GameOver: the game's.
    enum class Phase {YourTurn,YourCall,HerTurn,RoundOver,GameOver};

    class Table
    {
    public:
        // care: how carefully she plays (1: her best; less: now and then a looser discard or call,
        // and less care against your riichi).
        void NewGame(std::uint32_t seed,double care);
        void NextRound();

        Phase Now() const {return phase;}
        const Seat& Of(int who) const {return seats[who];}
        const std::vector<int>& Dora() const {return dora;}
        int WallLeft() const {return wall;}
        int Honba() const {return honba;}
        int Sticks() const {return sticks;}
        int RoundNumber() const {return round;}   // 1 to Hands (East 1, East 2, South 1, South 2)
        int RoundWind() const {return round<=2?East:South;}
        int Dealer() const {return dealer;}
        int SeatWind(int who) const {return who==dealer?East:West;}
        const RoundEnd& Ended() const {return ended;}
        bool HerTurnDrawn() const {return herDrawn;}
        // Her turn, with a tile drawn (or after a call): the tile she will discard (an index as
        // Discard takes, her drawn tile at her hand's size), chosen now and kept for her Step,
        // so it can be shown before it goes; -1 when she has none to discard.
        int HerNextDiscard();
        // Her last call (Japanese and English) and a count that grows with each.
        const wchar_t* HerCallJa() const {return callJa;}
        const wchar_t* HerCallEn() const {return callEn;}
        int HerCalls() const {return calls;}

        // Yours, now.
        std::vector<int> Waits() const;           // what your 13 tiles wait on
        bool Furiten() const {return FuritenOf(You);}
        bool CanTsumo() const {return phase==Phase::YourTurn&&CanTsumoFor(You);}
        int TsumoPoints() const;                  // what your tsumo now would pay (0: none; ura dora not counted)
        std::vector<int> RiichiDiscards() const;  // hand indices (hand.size() for the drawn tile) that leave you waiting
        // Riichi with the tile at index discarded: what you would wait on, and what each pays
        // (riichi, dora and red fives counted; ura, ippatsu and the like not, as unknown yet).
        std::vector<RiichiWait> RiichiWaits(int index) const;
        std::vector<int> KanKinds() const;        // closed or added kans you can make now
        bool CanRon() const {return phase==Phase::YourCall&&CanRonFor(You);}
        bool CanPon() const;
        bool CanOpenKan() const;

        // index: in your hand, or hand.size() for the drawn tile.
        void Discard(int index,bool riichi=false);
        void Tsumo();
        void Kan(int kind);
        void Ron();
        void Pon();
        void OpenKan();
        void Pass();
        // HerTurn: her draw (perhaps a tsumo), then her discard (perhaps riichi).
        void Step();
        // Your turn or call played as she would play it, at her best (diagnostics).
        void PlayForYou();

        // Every tile accounted for (diagnostics): the wall and those seen make four of each.
        bool Consistent() const;

    private:
        std::mt19937 rng;
        double care=1;
        Unseen unseen;
        std::array<Seat,2> seats;
        std::vector<int> dora;
        int wall=0,honba=0,sticks=0,round=1,dealer=You;
        bool calledYet=false;          // a call this hand (no first-turn yaku after one)
        bool rinshan=false;            // the draw now is after a kan
        bool herDrawn=false;           // her turn: she has drawn (or called), and discards next
        int planned=-1;                // HerNextDiscard's choice (-1: not chosen yet)
        bool plannedRiichi=false;
        Phase phase=Phase::YourTurn;
        RoundEnd ended;
        const wchar_t* callJa=L"";
        const wchar_t* callEn=L"";
        int calls=0;
        std::array<int,3> indicatorRed{};   // red fives among the dora and ura indicators
        bool dealerStays=false,gameOver=false;
        int nextHonba=0;

        void Deal();
        int DrawIndicator();
        void Draw(int who);
        void KanDraw(int who);
        void DiscardFor(int who,int index,bool riichi);
        void AfterDiscard(int who,const Piece& tile);
        void HerDiscard();
        void PonFor(int who);
        void RonFor(int who);
        void HerSays(const wchar_t* ja,const wchar_t* en){callJa=ja;callEn=en;++calls;}
        bool Roll(double chance){return std::uniform_real_distribution<double>(0,1)(rng)<chance;}
        Situation SituationOf(int who,bool tsumo) const;
        Counts CountsOf(int who,bool withDrawn) const;
        int RedOf(int who,bool withDrawn) const;
        Counts RiverCounts(int who) const;
        bool FuritenOf(int who) const;
        bool CanTsumoFor(int who) const;
        bool CanRonFor(int who) const;            // on the other's last discard
        std::vector<int> RiichiDiscardsFor(int who) const;
        // How she plays (or you, in PlayForYou): the hand index to discard and whether with
        // riichi; whether to pon the other's last discard.
        int ChooseDiscard(int who,double carefully,bool& riichi);
        bool WantsPon(int who,double carefully);
        Counts SeenBy(int who) const;             // the tiles who can see: the own hand, the rivers, the melds, the indicators
        double Safety(int who,int kind) const;    // how safe kind is for who to discard against the other's riichi
        double CareNow() const;
        void Win(int who,bool tsumo,const Counts& concealed,const std::vector<Meld>& melds,int win,Situation s,std::vector<Piece> shown);
        void WallEmpty();
        void Settle();
    };

    // Diagnostics (--mahjong-play-test): plays count games, her against a player of yours who
    // plays as she does at her best, and against a simple one; checks the tiles stay accounted
    // for and the points add up, and says how the hands ended.
    std::wstring CheckPlaying(int count,std::uint32_t seed);

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
