#pragma once
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch::Blackjack
{
    // The rules of the blackjack mini game (docs/Blackjack.md): she deals, you bet chips and
    // play your hand against hers. Cards are numbered suit * 13 + rank - 1 (suits spade, heart,
    // diamond, club; rank 1 the ace .. 13 the king), as in res/blackjack/cards.jpg; the back is 52.
    constexpr int Cards=52,Back=52;
    constexpr int Rounds=10;             // hands to a game
    constexpr int StartChips=200;
    constexpr int Bets[]={10,20,50,100};   // (a blackjack's 3 to 2 comes out whole)

    inline int RankOf(int card){return card%13+1;}
    inline int SuitOf(int card){return card/13;}
    // A hand's best total (aces 11 while that does not go over 21) and whether an ace counts 11.
    int Total(const std::vector<int>& cards,bool* soft=nullptr);
    inline bool Natural(const std::vector<int>& cards){return cards.size()==2&&Total(cards)==21;}

    enum class Result {None,Win,Blackjack,Push,Lose,Bust};

    struct Hand
    {
        std::vector<int> cards;
        int bet=0;
        bool doubled=false,done=false,splitAces=false,split=false;
        Result result=Result::None;
        int paid=0;                      // chips back (the bet and what it won; 0 when lost)
    };

    // Betting: choose the bet and Deal. YourTurn: Hit, Stand, Double or Split the hand in play.
    // HerTurn: she turns her hidden card and draws to 17 (Step each). RoundOver: the hands
    // settled (NextRound goes on). GameOver: the game's ten hands played.
    enum class Phase {Betting,YourTurn,HerTurn,RoundOver,GameOver};

    class Table
    {
    public:
        void NewGame(std::uint32_t seed);
        void NextRound();

        Phase Now() const {return phase;}
        int Chips() const {return chips;}
        int Round() const {return round;}     // 1 to Rounds
        const std::vector<Hand>& Hands() const {return hands;}
        int Active() const {return active;}   // the hand in play
        const std::vector<int>& Dealer() const {return dealer;}
        bool HoleShown() const {return holeShown;}
        int ShoeLeft() const {return static_cast<int>(shoe.size());}
        const std::vector<int>& Discards() const {return discards;}   // the cards played, in order
        int Shuffles() const {return shuffles;}
        // What happened last, and a count that grows with each card dealt or turned.
        int Moves() const {return moves;}
        // The round's chips won (less what was bet), when it is over.
        int RoundGain() const {return roundGain;}
        int GameGain() const {return chips-StartChips+refills*StartChips;}
        int Refills() const {return refills;}

        bool CanDeal(int bet) const;
        bool CanHit() const;
        bool CanDouble() const;
        bool CanSplit() const;

        void Deal(int bet);
        void Hit();
        void Stand();
        void Double();
        void Split();
        // HerTurn: one step of hers (her hidden card turned, a card drawn, or the round settled).
        void Step();
        // Your move as the usual basic strategy plays it (diagnostics).
        void PlayForYou();
        // The cards to come, the next first (diagnostics).
        void StackShoe(const std::vector<int>& next);

        bool Consistent() const;

    private:
        std::mt19937 rng;
        std::vector<int> shoe,discards;
        std::vector<Hand> hands;
        std::vector<int> dealer;
        int active=0;
        bool holeShown=false;
        int chips=StartChips,refills=0;
        int round=1;
        int moves=0,shuffles=0;
        int roundGain=0;
        Phase phase=Phase::Betting;

        int Draw();
        void Shuffle();
        // The hand in play done: the next hand, or her turn.
        void NextHand();
        void Settle();
    };

    // Diagnostics (--blackjack-test): set deals checked (blackjacks, busts, doubling, splitting,
    // her drawing to 17); the first line counts the failures.
    std::wstring CheckRules();
    // Diagnostics (--blackjack-play-test): count games played with the basic strategy, every
    // card and chip accounted for; how the hands ended.
    std::wstring CheckPlaying(int count,std::uint32_t seed);
}
