#pragma once
#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch::Hanafuda
{
    // The rules of the hanafuda mini game (docs/Hanafuda.md): koi-koi for two, apart from
    // drawing.

    // The 48 cards, four to a month, month m's at 4m to 4m+3, the highest first:
    //   0 pine: crane (bright), red poem ribbon, two chaff
    //   1 plum: bush warbler (animal), red poem ribbon, two chaff
    //   2 cherry: curtain (bright), red poem ribbon, two chaff
    //   3 wisteria: cuckoo (animal), red ribbon, two chaff
    //   4 iris: bridge (animal), red ribbon, two chaff
    //   5 peony: butterflies (animal), blue ribbon, two chaff
    //   6 bush clover: boar (animal), red ribbon, two chaff
    //   7 susuki: moon (bright), geese (animal), two chaff
    //   8 chrysanthemum: sake cup (animal, and chaff too), blue ribbon, two chaff
    //   9 maple: deer (animal), blue ribbon, two chaff
    //  10 willow: the poet in the rain (bright), swallow (animal), red ribbon, lightning (chaff)
    //  11 paulownia: phoenix (bright), three chaff
    constexpr int Cards=48,Months=12;
    enum class Kind {Bright,Animal,Ribbon,Chaff};
    constexpr int Crane=0,Curtain=8,Moon=28,SakeCup=32,RainMan=40,Phoenix=44;
    constexpr int Boar=24,Deer=36,Butterflies=20;
    constexpr std::array<int,3> PoemRibbons{1,5,9},BlueRibbons{21,33,37};

    inline int MonthOf(int card){return card/4;}
    Kind KindOf(int card);
    const wchar_t* CardName(int card);   // in the language now (Text.hpp)
    const wchar_t* MonthName(int month); // the month's plant

    // A yaku made by the cards taken: its name, how many cards count, the points (mon).
    struct Yaku {const wchar_t* ja;const wchar_t* en;int cards=0,points=0;};
    std::vector<Yaku> YakuOf(const std::vector<int>& taken);
    int PointsOf(const std::vector<Yaku>& yaku);

    // The table (docs/Hanafuda.md): you against her, twelve months (rounds), the winner of a
    // round dealing the next. She plays her own hand from what she can see.
    constexpr int You=0,Her=1;
    constexpr int Rounds=12;
    constexpr int HandSize=8;

    struct Seat
    {
        int score=0;                  // mon over the game
        std::vector<int> hand;        // in order
        std::vector<int> taken;       // in the order taken
        int koi=0;                    // koi-koi called this round
        int yakuPoints=0;             // the yaku's points when last decided on (stopping needs more)
    };

    // A card going somewhere, for the window to show: played from a hand or turned up from the
    // pile, laid on the field or taking cards of its month from it.
    struct Move
    {
        int who=You;
        int card=-1;
        bool fromPile=false;
        std::vector<int> took;        // the field cards it took (none: laid on the field)
    };

    struct RoundEnd
    {
        int winner=-1;                // -1: no one stopped before the hands ran out
        bool dealt=false;             // won at the deal (four of a month, or four pairs, in hand)
        std::vector<Yaku> yaku;
        int base=0;                   // the yaku's points
        bool doubledSeven=false;      // seven or more: doubled
        bool doubledKoi=false;        // the other had called koi-koi: doubled
        int points=0;                 // what the winner gets
        std::vector<int> hand;        // a deal's win: the hand shown
    };

    // YourPlay: play a card from your hand. YourPick: it matches two on the field, pick one.
    // YourDraw: turn up the pile's top (Draw). YourDrawPick: that card matches two, pick one.
    // YourKoi: you made (or bettered) a yaku: koi-koi or stop. HerTurn: she plays, then turns
    // up (Step each). RoundOver: the round's end is shown (NextRound goes on). GameOver.
    enum class Phase {YourPlay,YourPick,YourDraw,YourDrawPick,YourKoi,HerTurn,RoundOver,GameOver};

    class Table
    {
    public:
        // care: how carefully she plays (1: her best; less: now and then a looser card or call).
        void NewGame(std::uint32_t seed,double care);
        void NextRound();

        Phase Now() const {return phase;}
        const Seat& Of(int who) const {return seats[who];}
        const std::vector<int>& Field() const {return field;}
        int PileLeft() const {return static_cast<int>(pile.size());}
        int Round() const {return round;}   // 1 to Rounds
        int Dealer() const {return dealer;}
        const RoundEnd& Ended() const {return ended;}
        // The last move, and a count that grows with each.
        const Move& LastMove() const {return last;}
        int Moves() const {return moves;}
        // The card waiting for a pick (YourPick, YourDrawPick), else -1.
        int Pending() const {return pending;}
        // Field indices of the cards of card's month.
        std::vector<int> Matches(int card) const;
        // Her last call (koi-koi or stop, Japanese and English) and a count that grows with each.
        const wchar_t* HerCallJa() const {return callJa;}
        const wchar_t* HerCallEn() const {return callEn;}
        int HerCalls() const {return calls;}
        // Her turn, before she plays: the hand index she will play, chosen now and kept for
        // her Step, so it can be shown before it goes; -1 when she is to turn up the pile next.
        int HerNextPlay();
        bool HerPlayed() const {return herPlayed;}

        // YourPlay: index in your hand.
        void Play(int index);
        // YourPick, YourDrawPick: a field index among Matches(Pending()).
        void Pick(int fieldIndex);
        void Draw();
        void Koi(bool goOn);
        // HerTurn: her card from her hand, then the pile's.
        void Step();
        // Your step played as she would play it, at her best (diagnostics).
        void PlayForYou();

        // Every card accounted for once (diagnostics).
        bool Consistent() const;

    private:
        std::mt19937 rng;
        double care=1;
        std::array<Seat,2> seats;
        std::vector<int> field,pile;
        int round=1,dealer=You;
        Phase phase=Phase::YourPlay;
        RoundEnd ended;
        Move last;
        int moves=0;
        int pending=-1;
        bool herPlayed=false;
        int planned=-1;               // HerNextPlay's choice (-1: not chosen yet)
        int plannedPick=-1;           // and the field card it takes when it could take either of two
        const wchar_t* callJa=L"";
        const wchar_t* callEn=L"";
        int calls=0;
        bool gameOver=false;

        void Deal();
        // who lays card (from the hand or the pile): takes from the field (pick: the field index
        // when two could be taken) or lays it there.
        void Lay(int who,int card,bool fromPile,int pick);
        // After who's turn (the pile's card laid): a yaku made or bettered, or the hands done.
        void TurnDone(int who);
        void Stop(int who);
        void NoOne();
        void HerSays(const wchar_t* ja,const wchar_t* en){callJa=ja;callEn=en;++calls;}
        bool Roll(double chance){return std::uniform_real_distribution<double>(0,1)(rng)<chance;}
        // How she plays (or you, in PlayForYou): the hand index and the field card it takes
        // when two could be; the field card a turned-up one takes; whether to go on.
        int ChoosePlay(int who,double carefully,int& pick);
        int ChoosePick(int who,int card,const std::vector<int>& matches);
        bool WantsKoi(int who,double carefully);
        double CareNow() const;
        // What who can see: own hand, the field, both piles taken (the rest is unseen).
        std::array<bool,Cards> SeenBy(int who) const;
        double Gain(int who,const std::vector<int>& cards) const;
        double LeaveCost(int who,int card) const;
    };

    // Diagnostics (--hanafuda-play-test): plays count games, her against a player of yours who
    // plays as she does at her best, and against one who plays at random; checks the cards stay
    // accounted for, and says how the rounds ended.
    std::wstring CheckPlaying(int count,std::uint32_t seed);
    // Diagnostics (--hanafuda-test): the yaku of piles of cards written one a line (card numbers,
    // => the expected points), the first line counting the failures.
    std::wstring CheckYaku(const std::wstring& cases);
}
