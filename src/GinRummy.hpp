#pragma once
#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch::GinRummy
{
    // The rules of the gin rummy mini game (docs/GinRummy.md): you and her, ten cards each, the
    // first to 100 points. Cards are numbered as in res/blackjack/cards.jpg: suit * 13 + rank - 1
    // (suits spade, heart, diamond, club; rank 1 the ace .. 13 the king); the back is 52.
    constexpr int Cards=52,Back=52;
    constexpr int You=0,Her=1;
    constexpr int HandSize=10;
    constexpr int Target=100;            // points to win the game
    constexpr int KnockMax=10;           // the most unmatched points one may knock with
    constexpr int GinBonus=25,UndercutBonus=25;
    constexpr int StockEnd=2;            // the hand is void when only so many are left in the stock

    inline int RankOf(int card){return card%13+1;}
    inline int SuitOf(int card){return card/13;}
    // What an unmatched card counts: the ace 1, the number, the face cards 10.
    inline int Points(int card){return RankOf(card)>=10?10:RankOf(card);}

    // A hand sorted out so that the fewest points are left unmatched.
    struct Split
    {
        std::vector<std::vector<int>> melds;
        std::vector<int> dead;
        int deadwood=0;
    };
    Split BestSplit(const std::vector<int>& hand);
    int Deadwood(const std::vector<int>& hand);

    // How a hand ended.
    struct Outcome
    {
        int knocker=-1;                  // who knocked (-1: the stock ran out)
        int winner=-1;                   // who scored (-1: nobody)
        bool gin=false,undercut=false;
        int points=0;
        std::array<int,2> deadwood{};    // after laying off
        std::array<Split,2> split;       // as shown (the other's cards laid off are in laid)
        std::vector<int> laid;
    };

    // Dealing: the cards are to be dealt (Step). YourDraw, HerDraw: to take the stock's top or the
    // discard (in the first offer, the discard or pass). YourDiscard, HerDiscard: to discard,
    // knocking with it or not. HandOver: the hand scored (NextHand goes on). GameOver: someone has
    // reached the target.
    enum class Phase {Dealing,YourDraw,YourDiscard,HerDraw,HerDiscard,HandOver,GameOver};

    enum class Did {None,Stock,TookDiscard,Pass,Discard,Knock};

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
        const std::vector<int>& Discards() const {return discards;}
        int Top() const {return discards.empty()?-1:discards.back();}
        int Moves() const {return moves;}     // grows with each card dealt, drawn or discarded
        int Turns() const {return turns;}     // discards made this hand
        const Outcome& Result() const {return outcome;}
        // The card last drawn by the one to discard (-1: none).
        int JustDrawn() const {return justDrawn;}
        Did LastDid(int who) const {return did[who];}

        bool InOffer() const {return offer>0;}     // the first upcard being offered
        bool CanDrawStock() const;
        bool CanTakeDiscard() const;
        bool CanDiscard(int card) const;
        // Whether discarding card leaves few enough points to knock.
        bool CanKnockWith(int card) const;

        void DrawStock();
        void TakeDiscard();
        void Pass();
        void Discard(int card,bool knock=false);
        // Dealing, HerDraw, HerDiscard: one step (the deal, or her choice carried out).
        void Step();
        // What she will do at the next Step: draw: she takes the discard, or passes in the offer;
        // discard: the card, and whether she knocks with it.
        bool HerTakes();
        int HerDiscard(bool& knock);
        // Your move as her own judgment would play it (diagnostics); style 0 as she plays,
        // 1 greedy (the most points away, knocking at once), 2 anyhow.
        void PlayForYou(int style=0);

        bool Consistent() const;
        // The cards to come, the next first (diagnostics). Dealt: one by one, starting with the
        // one who is not the dealer, then the upcard, then the stock.
        void StackDeck(const std::vector<int>& next);

    private:
        std::mt19937 rng,think;
        std::vector<int> stock,discards,stacked;
        std::array<std::vector<int>,2> hands;
        std::array<std::vector<int>,2> tookUp;   // the cards each took from the discard
        std::array<int,2> scores{};
        std::array<Did,2> did{};
        int dealer=You,toAct=You,offer=0,justDrawn=-1,justTaken=-1;
        bool mustStock=false;
        int handNumber=0,moves=0,turns=0;
        double care=1;
        int plannedAt=-1,plannedCard=-1;
        bool plannedTake=false,plannedKnock=false;
        Outcome outcome;
        Phase phase=Phase::Dealing;

        int Other(int who) const {return 1-who;}
        void Deal();
        void Settle(int knocker);
        double CareNow() const;
        bool WantsDiscard(int who);
        int ChooseDiscard(int who,double carefully,bool& knock);
    };

    // Diagnostics (--ginrummy-test): set hands scored (melds, gin, undercut, laying off, the stock
    // running out); the first line counts the failures.
    std::wstring CheckRules();
    // Diagnostics (--ginrummy-play-test): count games of hers against each style of play and
    // against herself, every card accounted for; who won.
    std::wstring CheckPlaying(int count,std::uint32_t seed);
}
