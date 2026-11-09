#pragma once
#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch::PageOne
{
    // The rules of the Page One mini game (docs/PageOne.md): you and her, seven cards each, cards
    // played onto the pile by suit or rank until one has none; the first to Target points wins.
    // Cards are numbered as in res/blackjack/cards.jpg: suit * 13 + rank - 1 (suits spade, heart,
    // diamond, club; rank 1 the ace .. 13 the king); the back is 52, the joker 53.
    constexpr int Cards=54,Back=52,Joker=53;
    constexpr int You=0,Her=1;
    constexpr int HandSize=7;
    constexpr int Target=150;            // points to win the game
    constexpr int CaughtDraw=2;          // cards drawn for not saying "page one"
    constexpr int TurnLimit=300;         // a hand this long is void

    inline bool IsJoker(int card){return card==Joker;}
    inline int RankOf(int card){return IsJoker(card)?0:card%13+1;}
    inline int SuitOf(int card){return IsJoker(card)?-1:card/13;}

    // What a card does: 2 makes the next draw two (answered with a 2 or the joker), J skips the
    // other, the ace turns the order back (with two, the same as a skip: you play again), 8 is
    // wild (you name the suit), the joker is wild and makes the next draw four.
    enum class Kind {Plain,DrawTwo,Skip,Reverse,Wild,DrawFour};
    Kind KindOf(int card);
    inline bool IsWild(int card){const Kind k=KindOf(card);return k==Kind::Wild||k==Kind::DrawFour;}
    // What a card left in the hand counts for the one who went out.
    int Points(int card);

    // How a hand ended.
    struct Outcome
    {
        int winner=-1;                   // who went out (-1: the hand was void)
        int points=0;
        int lastCard=-1;                 // the card the winner went out with
        int drawnAtEnd=0;                // cards the other drew for that card (a 2 or the joker)
        std::array<int,2> left{};        // points left in each hand
    };

    // Dealing: the cards are to be dealt (Step). YourTurn, HerTurn: to play a card or draw.
    // YourDrawn, HerDrawn: one card drawn: to play it or keep it. HandOver: the hand scored
    // (NextHand goes on). GameOver: someone has reached the target.
    enum class Phase {Dealing,YourTurn,YourDrawn,HerTurn,HerDrawn,HandOver,GameOver};

    enum class Did {None,Played,Drew,Kept,Penalty,Caught};

    class Table
    {
    public:
        // care: how carefully she plays (1: her best; less: now and then a looser choice).
        void NewGame(std::uint32_t seed,double care=1);
        void NextHand();

        Phase Now() const {return phase;}
        int HandNumber() const {return handNumber;}
        int Dealer() const {return dealer;}
        int Score(int who) const {return scores[who];}
        const std::vector<int>& HandOf(int who) const {return hands[who];}
        const std::vector<int>& Stock() const {return stock;}
        const std::vector<int>& Pile() const {return pile;}
        int Top() const {return pile.empty()?-1:pile.back();}
        int Suit() const {return suit;}              // the suit to follow (a wild's named one)
        int Penalty() const {return penalty;}        // cards the one to play draws unless answering
        int ToAct() const {return toAct;}
        int Moves() const {return moves;}            // grows with each card dealt, drawn or played
        int Turns() const {return turns;}
        const Outcome& Result() const {return outcome;}
        int JustDrawn() const {return justDrawn;}    // the card last drawn by the one to play (-1)
        Did LastDid(int who) const {return did[who];}
        int LastPlayed(int who) const {return lastPlayed[who];}
        int Refills() const {return refills;}        // the pile shuffled back into the stock, this hand
        // Whether who said "page one" as they came down to one card (still holding one).
        bool Called(int who) const {return called[who]&&hands[who].size()==1;}

        // For the one whose turn it is (after a draw, only the card drawn).
        bool CanPlay(int card) const;
        bool CanPlayAny() const;
        bool CanDraw() const;       // in YourTurn/HerTurn: draw one, or the penalty
        bool CanKeep() const;       // in YourDrawn/HerDrawn
        // Playing card (suit: the suit named with a wild; call: "page one" said with it, as it
        // leaves one card).
        void Play(int card,int suit=-1,bool call=false);
        void Draw();
        void Keep();
        // Whether who may point out that the other came down to one card without saying "page
        // one" (only on who's own turn, before they play or draw); the other draws CaughtDraw.
        bool CanCatch(int who) const;
        void Catch(int who);

        // Dealing, HerTurn, HerDrawn: one step (the deal, or her choice carried out).
        void Step();
        // What she will do at the next Step: the card she plays (-1: she draws, or keeps the one
        // drawn), the suit she names with a wild, whether she says "page one"; or that she points
        // out yours first.
        int HerPlay(int& suit,bool& call);
        bool HerCatches();
        // Your move as her own judgment would play it (diagnostics); style 0 as she plays,
        // 1 greedy (the most points first), 2 anyhow.
        void PlayForYou(int style=0);

        bool Consistent() const;
        // The cards to come, the next first (diagnostics). Dealt: one by one, starting with the
        // one who is not the dealer, then the first card turned up, then the stock.
        void StackDeck(const std::vector<int>& next);
        // A position set by hand (diagnostics): the hands, the pile (its top last), who is to
        // play, the stock (the next drawn first); the cards not named go under the stock, or
        // under the pile with restToPile.
        void Arrange(const std::vector<int>& you,const std::vector<int>& her,const std::vector<int>& pile,int toAct,
                     const std::vector<int>& stock={},bool restToPile=false);

    private:
        std::mt19937 rng,think;
        std::vector<int> stock,pile,stacked;
        std::array<std::vector<int>,2> hands;
        std::array<int,2> scores{};
        std::array<Did,2> did{};
        std::array<int,2> lastPlayed{-1,-1};
        std::array<bool,2> called{},forgot{};
        // The suits each seemed not to hold: they drew while it was to be followed.
        std::array<std::array<bool,4>,2> lacks{};
        int dealer=You,toAct=You,suit=0,penalty=0,justDrawn=-1,refills=0;
        bool jokerPending=false;
        int handNumber=0,moves=0,turns=0;
        double care=1;
        int plannedAt=-1,plannedCard=-1,plannedSuit=0;
        bool plannedCall=true,plannedCatch=false;
        int catchPlannedAt=-1;
        Outcome outcome;
        Phase phase=Phase::Dealing;

        int Other(int who) const {return 1-who;}
        Phase TurnOf(int who) const {return who==You?Phase::YourTurn:Phase::HerTurn;}
        bool Drawn() const {return phase==Phase::YourDrawn||phase==Phase::HerDrawn;}
        bool Playing() const {return phase==Phase::YourTurn||phase==Phase::HerTurn||Drawn();}
        void Deal();
        int TakeFromStock();
        void PassTurn();
        void Settle(int winner);
        double CareNow() const;
        int ChoosePlay(int who,double carefully,int& named);
        int BestSuit(int who,int except) const;
    };

    // Diagnostics (--pageone-test): set hands played (following, wilds, draw two and its answer,
    // the joker, skips, "page one" and being caught, going out, the stock refilled); the first
    // line counts the failures.
    std::wstring CheckRules();
    // Diagnostics (--pageone-play-test): count games of hers against each style of play and
    // against herself, every card accounted for; who won.
    std::wstring CheckPlaying(int count,std::uint32_t seed);
}
