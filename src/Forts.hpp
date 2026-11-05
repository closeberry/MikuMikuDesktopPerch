#pragma once
#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch::Forts
{
    // The rules of Three Forts (docs/Forts.md): you and her, three forts between you. A fort with
    // only your soldiers at it scores at the start of your turn; the first to Target wins.
    constexpr int You=0,Her=1;
    constexpr int FortCount=3,PerFort=3;
    constexpr int Target=8;
    constexpr int StartHand=4,MaxHand=10,MaxMagic=8;
    constexpr int DeckSize=30;
    constexpr int TurnLimit=40;          // turns of both together; then the higher score wins
    constexpr std::array<int,FortCount> FortValue{1,2,1};

    enum Keyword : std::uint8_t {Guard=1,Haste=2,Fly=4,Ranged=8,Tough=16,Banner=32,Scout=64};

    enum class Effect : std::uint8_t
    {
        None,
        DrawOnDeath,    // when it falls: draw
        DrawOnPlay,     // draw value cards
        HealAlly,       // an ally back to full
        ShieldFort,     // a fort: your soldiers take value less until your next turn
        RallyFort,      // a fort: your soldiers' attack up by value this turn
        BuffFort,       // the others at its fort: +value/+value
        SummonNext,     // a rabbit at the fort next to it
        Ambush,         // an ally to any fort, ready to act
        RaiseAll,       // all your other soldiers' attack up by value
        HitEnemy,       // an enemy takes value
        Freeze,         // an enemy does not act next turn
        Push,           // an enemy to the fort next to it
        Storm           // a fort: each enemy there takes value
    };

    enum class Needs : std::uint8_t {Nothing,Ally,Enemy,Fort};

    struct Card
    {
        const wchar_t* name;
        const wchar_t* nameEn;
        const wchar_t* text;
        const wchar_t* textEn;
        std::int8_t cost,attack,health;
        bool spell;
        std::uint8_t keywords;
        Effect effect;
        std::int8_t value;
        std::int8_t faction;             // 0 common, 1 knights, 2 forest, 3 mages, -1 not in a deck
    };

    constexpr int DeckKinds=3;           // 0 knights, 1 forest, 2 mages
    int CardKinds();
    const Card& CardOf(int id);
    // The magic a card gives when given up: by what it costs, 1 to 3.
    int GivesMagic(const Card& card);
    // The deck of a kind (DeckSize cards), and its name.
    std::vector<int> DeckOf(int kind);
    const wchar_t* DeckName(int kind,bool english);
    // One deck is collected: a card of its own for each game you win, and yours to play from
    // UnlockAt of them on, with those and the common cards, which you always have.
    constexpr int CollectedDeck=2,UnlockAt=10;
    // Those to be collected.
    std::vector<int> CollectableCards();
    // Your deck with the ones you have.
    std::vector<int> CollectedDeckCards(const std::vector<int>& owned);
    // Which card a win brings, added to owned (-1: you have them all).
    int Collect(std::vector<int>& owned,std::mt19937& dice);
    Needs NeedsOf(const Card& card);

    struct Unit
    {
        std::int16_t uid=0;
        std::int8_t card=0,attack=0,health=0,maxHealth=0,rally=0;
        bool ready=false,moved=false,frozen=false;
    };

    struct Side
    {
        std::array<std::array<Unit,PerFort>,FortCount> units{};
        std::array<std::int8_t,FortCount> count{};
        std::array<std::int8_t,MaxHand> hand{};
        std::array<bool,FortCount> shield{};
        std::int8_t handSize=0,magic=0,maxMagic=0,score=0,deckLeft=0,pending=0;
        std::int8_t aim=-1;              // the fort this side means to hold next (-1: none, or a bluff)
        std::int8_t deck=0;              // which deck it plays (known to both)
        bool spare=false,offered=false;  // the one extra magic of the second to play; a card given up this turn
    };

    struct State
    {
        std::array<Side,2> side{};
        std::int16_t nextUid=1,turns=0;
        std::int8_t who=You,winner=-1;   // winner: 2 for a draw
        bool over=false;
    };

    enum class ActionKind : std::uint8_t {End,Play,Move,Attack,Offer,Spare};

    struct Action
    {
        ActionKind kind=ActionKind::End;
        std::int8_t hand=-1;             // Play, Offer: the place in the hand
        std::int8_t fort=-1;             // Play: the fort a soldier goes to, or a Fort spell's fort
        std::int8_t dest=-1;             // Move, Ambush, Push, SummonNext: the fort it goes to
        std::int16_t unit=-1;            // Move, Attack: the soldier acting
        std::int16_t target=-1;          // Attack, or the Ally/Enemy a card is aimed at
    };

    enum class EventKind : std::uint8_t {TurnStart,Scored,Drew,Placed,Spell,Moved,Hit,Fell,Healed,Buffed,Frozen,Shield,Offered,Spare};

    struct Event
    {
        EventKind kind=EventKind::TurnStart;
        std::int8_t who=0,fort=-1,dest=-1,card=-1,amount=0,back=0;
        std::int16_t unit=-1,target=-1;
    };

    std::vector<Action> Legal(const State& state);
    // Carries an action out. Cards drawn are counted in Side::pending (the Table hands them over).
    void Apply(State& state,const Action& action,std::vector<Event>* events=nullptr);
    const Unit* FindUnit(const State& state,int uid,int* side=nullptr,int* fort=nullptr);
    // Whether only this side stands at the fort, and what it would score there.
    bool Holds(const State& state,int who,int fort);
    int FortScore(const State& state,int who,int fort);

    enum class Phase {Redraw,Yours,Hers,Over};

    // How hard she plays: from Levels-1, her best, down to 0, where she is still learning. It
    // follows how the games with you end, so that they stay close (AfterGame).
    constexpr int Levels=10;
    struct Strength
    {
        double level=1;
        int games=0;
        int Level() const;
    };
    // result: 1 you won, -1 she won, 0 drawn; margin: the points between you. gaveUp: you stopped
    // midway (it counts only when you were behind, after some turns: pass result -1 then).
    void AfterGame(Strength& strength,int result,int margin,bool gaveUp=false);

    class Table
    {
    public:
        // level: how hard she plays (Levels-1: her best). Style 0 for you is always at her best.
        // yourCards: the cards of your deck, when not the whole of it.
        void NewGame(std::uint32_t seed,int yourDeck,int herDeck,int level=Levels-1,int first=You,const std::vector<int>* yourCards=nullptr);

        Phase Now() const;
        const State& Board() const {return state;}
        int Deck(int who) const {return deckKind[who];}
        int First() const {return first;}
        // What she let you see of her plan (the fort; -1: nothing said).
        int HerAim() const {return shownAim;}

        // Yours: which cards of your hand to change (a bit each), once, at the start.
        void Redraw(unsigned mask);
        unsigned SuggestRedraw(int who) const;
        std::vector<Action> Actions() const {return Legal(state);}
        void Do(const Action& action);
        // Hers: her next action, carried out and returned.
        Action HerStep();
        // Whether all she has left to do this turn is to end it, as far as she has planned.
        bool HerAboutToEnd() const {return planDone&&plan.empty()&&looseTurn==state.turns;}
        // Your move by a given way of play (diagnostics); style 0 as she plays, 1 greedy, 2 anyhow.
        Action PlayForYou(int style=0);

        std::vector<Event> TakeEvents();
        bool Consistent() const;
        // Which cards each side has drawn so far (diagnostics).
        const std::array<std::vector<int>,2>& Seen() const {return seen;}
        // Diagnostics: set the board up for a test.
        State& Edit() {return state;}
        void ClearRedraw() {redrawDone=true;}
        void StepAs(int who,int style);

    private:
        State state;
        std::array<std::vector<int>,2> decks;
        std::array<std::vector<int>,2> seen;
        std::array<int,2> deckKind{};
        std::vector<Event> events;
        std::mt19937 rng,think;
        int level=Levels-1;
        int first=You,shownAim=-1,looseTurn=-1;
        bool looseNow=false,clumsyNow=false,redrawDone=false,planDone=false;
        std::vector<Action> plan;

        void TakeDrawn();
        void DrawTo(int who,int count);
        double CareNow() const;
        void AfterHerTurn(int who);
        void Begin();
    };

    // Her choice for the side to move, knowing only what it can see (the other's hand is hidden
    // from it). loose: she takes the next-best turn.
    Action ChooseAction(const State& state,bool loose,std::mt19937& rng);
    std::vector<Action> ChoosePlan(const State& state,bool loose,std::mt19937& rng);
    // A turn of one still learning: each step the look of the board, misjudged by noise.
    std::vector<Action> ClumsyPlan(const State& state,double noise,std::mt19937& rng);
    Action ChooseGreedy(const State& state);
    Action ChooseAnyhow(const State& state,std::mt19937& rng);

    // Diagnostics (--forts-test): set boards played out; the first line counts the failures.
    std::wstring CheckRules();
    // Diagnostics (--forts-play-test): games of hers against each style and against herself, who
    // won, by deck, by card, how long they took.
    std::wstring CheckPlaying(int count,std::uint32_t seed);
    // Diagnostics (--forts-level-test): how she does at each level against each style, and where
    // the level settles against each.
    std::wstring CheckLevels(int count,std::uint32_t seed);
}
