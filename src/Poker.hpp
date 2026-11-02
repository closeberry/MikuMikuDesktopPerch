#pragma once
#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch::Poker
{
    // The rules of the poker mini game (docs/Poker.md): no-limit Texas hold'em, you and her alone.
    // Cards are numbered as in res/blackjack/cards.jpg: suit * 13 + rank - 1 (suits spade, heart,
    // diamond, club; rank 1 the ace .. 13 the king); the back is 52.
    constexpr int Cards=52,Back=52;
    constexpr int You=0,Her=1;
    constexpr int Hands=20;              // hands to a game, at most
    constexpr int StartChips=1000;
    constexpr int Unit=10;               // every amount is a whole number of these (the smallest chip)
    constexpr int HandsPerLevel=5;       // the blinds go up after so many hands

    inline int RankOf(int card){return card%13+1;}
    inline int SuitOf(int card){return card/13;}

    // Five to seven cards' best five, as a number: the higher the better, equal when they tie.
    int Score(const int* cards,int count);
    // Its kind: 0 high card, 1 one pair, 2 two pair, 3 three of a kind, 4 straight, 5 flush,
    // 6 full house, 7 four of a kind, 8 straight flush.
    inline int KindOf(int score){return score>>20;}
    // The five of the cards that make that number.
    std::vector<int> BestFive(const std::vector<int>& cards);

    // Dealing: the next cards are to come (Step; the hand's first, a street, or with all the chips
    // in, the rest one at a time and then the showdown). YourTurn, HerTurn: the one to act Folds,
    // CheckCalls or RaisesTo (hers come through Step). HandOver: the pot paid (NextHand goes on).
    // GameOver: one of you is out of chips, or the game's hands are played.
    enum class Phase {Dealing,YourTurn,HerTurn,HandOver,GameOver};

    enum class Did {None,Check,Call,Raise,AllIn,Fold,Blind};
    // What she is about to do: raiseTo is her whole bet this street after it. bluff: she is
    // betting on little.
    struct Order {Did did=Did::None;int raiseTo=0;bool bluff=false;};

    class Table
    {
    public:
        // care: how carefully she plays (1: her best; less: now and then a looser choice).
        void NewGame(std::uint32_t seed,double care=1);
        void NextHand();

        Phase Now() const {return phase;}
        int Hand() const {return hand;}       // 1 to Hands
        int Chips(int who) const {return chips[who];}
        int BetOf(int who) const {return bets[who];}
        int Pot() const {return pot+bets[0]+bets[1];}   // all that is in, this street's bets too
        int Collected() const {return pot;}             // without them
        int Button() const {return button;}             // the small blind, first to act before the flop
        int SmallBlind() const;
        int BigBlind() const {return SmallBlind()*2;}
        const std::array<std::vector<int>,2>& Holes() const {return holes;}
        const std::vector<int>& Board() const {return board;}
        int DeckLeft() const {return static_cast<int>(deck.size());}
        bool HerCardsShown() const {return shown;}
        // What each last did this street (a raise's or a call's amount: chips put in).
        Did LastDid(int who) const {return did[who];}
        int LastAmount(int who) const {return didAmount[who];}
        int Moves() const {return moves;}     // grows with each card dealt, chip bet or pot paid

        int ToCall(int who) const;
        bool CanRaise(int who) const;
        int MinRaiseTo(int who) const;
        int MaxRaiseTo(int who) const;
        // A raise of share of the pot (with the call counted in it), as a whole bet, in bounds.
        int RaiseToShare(int who,double share) const;

        // For the one to act.
        void Fold();
        void CheckCall();
        void RaiseTo(int total);
        // Dealing or HerTurn: one step (cards dealt, the showdown, or her choice carried out).
        void Step();
        // HerTurn: what she will do at the next Step (chosen once).
        const Order& HerOrder();
        // Your move as her own judgment would play it (diagnostics); style 0 as she plays,
        // 1 always calls, 2 tight, 3 wild.
        void PlayForYou(int style=0);

        // The hand over: who took the pot (-1: split), whether by a fold, and your chips won.
        int Winner() const {return winner;}
        bool ByFold() const {return byFold;}
        int HandGain() const {return chips[You]-startChips;}
        int GameGain() const {return chips[You]-StartChips;}
        int Won() const {return wonPot;}      // the pot as it was paid
        // How strong her cards look to her now, against any two (0-1; for her face, her words).
        double HerStrength();
        // Whether her last bet or raise was on little.
        bool Bluffing() const {return bluffing;}

        // Whether every card and chip is accounted for.
        bool Consistent() const;
        // The cards to come, the next first (diagnostics). Dealt: the big blind's, the button's,
        // the big blind's, the button's, then the board.
        void StackDeck(const std::vector<int>& next);

    private:
        std::mt19937 rng,think;
        std::vector<int> deck,board,stacked;
        std::array<std::vector<int>,2> holes;
        std::array<int,2> chips{StartChips,StartChips},bets{},didAmount{},raises{},earlyRaises{};
        std::array<Did,2> did{};
        std::array<bool,2> acted{};
        int pot=0,lastRaise=0;
        int hand=1,button=You,toAct=You,startChips=StartChips;
        int moves=0,winner=-1,wonPot=0;
        bool byFold=false,shown=false,bluffing=false,opened=false,aggressor[2]{};
        double care=1;
        bool ordered=false;
        Order order;
        int strengthFor=-1;
        double strength=0;
        Phase phase=Phase::Dealing;

        int Other(int who) const {return 1-who;}
        void Put(int who,int amount);
        void DealHand();
        void DealStreet();
        void StartBetting(int first);
        void Next(int who);
        void CloseStreet();
        void Pay(int who);
        void Showdown();
        double CareNow() const;
        double Equity(int who,int trials,bool againstAny);
        bool Draws(int who) const;
        Order Choose(int who,double carefully);
    };

    // Diagnostics (--poker-test): how often each kind comes up over every seven cards (known
    // numbers), set hands checked; the first line counts the failures.
    std::wstring CheckRules();
    // Diagnostics (--poker-play-test): count games of hers against each style of play and against
    // herself, every card and chip accounted for; who won.
    std::wstring CheckPlaying(int count,std::uint32_t seed);
}
