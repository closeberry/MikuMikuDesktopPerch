#pragma once
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch::OldMaid
{
    // The rules of the Old Maid (babanuki) mini game (docs/OldMaid.md): the 52 cards and the
    // joker dealt between you and her, pairs of a rank thrown out, then each in turn draws a
    // card from the other's hand (throwing out the pair it makes); the one left holding the
    // joker loses. Cards are numbered as in res/blackjack/cards.jpg: suit * 13 + rank - 1,
    // the back 52, the joker 53.
    constexpr int Back=52,Joker=53;
    constexpr int Cards=53;   // in play: the 52 and the joker

    inline int RankOf(int card){return card==Joker?0:card%13+1;}

    enum Side {You=0,Her=1};
    // YourDraw: you draw from her hand. HerDraw: she draws from yours. GameOver: one of you
    // has no cards left (the other holds the joker).
    enum class Phase {YourDraw,HerDraw,GameOver};

    class Table
    {
    public:
        // Deals and throws out the pairs; first draws first.
        void NewGame(std::uint32_t seed,Side first);
        // As NewGame, the cards dealt in this order (yours first, then in turn; diagnostics).
        void Deal(const std::vector<int>& order,Side first);

        Phase Now() const {return phase;}
        // Your hand in order of rank (the joker where it fell); hers as she holds it.
        const std::vector<int>& Hand(Side side) const {return hands[side];}
        // The cards thrown out, a pair at a time, in order.
        const std::vector<int>& Discards() const {return discards;}
        // A count that grows with each card drawn (and her hand shuffled).
        int Moves() const {return moves;}
        int Draws() const {return draws;}
        // The last draw: the card, who drew it, whether it made a pair.
        int LastCard() const {return lastCard;}
        Side LastDrawer() const {return lastDrawer;}
        bool LastPaired() const {return lastPaired;}
        // Who holds the joker (at the end: who lost).
        Side JokerHolder() const;
        Side Loser() const {return JokerHolder();}

        // The one whose turn it is takes the card at index from the other's hand.
        void Draw(int index);
        // Her pick from your hand (she cannot see it: any card).
        int HerPick();
        // Her hand shuffled (she does after it changes).
        void ShuffleHer();
        // Your draw at random (diagnostics).
        void PlayForYou();

        bool Consistent() const;

    private:
        std::mt19937 rng;
        std::vector<int> hands[2],discards;
        int moves=0,draws=0;
        int lastCard=-1;
        Side lastDrawer=You;
        bool lastPaired=false;
        Phase phase=Phase::GameOver;

        void ThrowPairs(Side side);
        void SortYours();
        void CheckEnd();
    };

    // Diagnostics (--oldmaid-test): set deals and draws checked (pairs thrown out at the deal,
    // a drawn card pairing, the joker kept, the game's end); the first line counts the failures.
    std::wstring CheckRules();
    // Diagnostics (--oldmaid-play-test): count games played at random, every card accounted for;
    // who lost and how many draws they took.
    std::wstring CheckPlaying(int count,std::uint32_t seed);
}
