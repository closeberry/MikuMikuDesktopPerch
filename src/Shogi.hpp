#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch::Shogi
{
    // The rules of the shogi mini game (docs/Shogi.md): you against her on a 9 by 9 board.
    // Squares are numbered row by row, Size * row + column; row 0 is her side of the board
    // (your pieces move toward it), column 0 at your left.
    constexpr int You=0,Her=1;
    constexpr int Size=9,Squares=Size*Size;

    // Kinds of piece; a promoted one is its kind plus Promoted.
    enum Kind : int {None=0,Pawn,Lance,Knight,Silver,Gold,Bishop,Rook,King,
                     Promoted=8,Tokin=Pawn+Promoted,ProLance,ProKnight,ProSilver,Horse=Bishop+Promoted,Dragon};
    constexpr int HandKinds=8;   // a hand is counted by kind, Pawn to Rook (index 0 unused)

    inline int Base(int kind){return kind>King?kind-Promoted:kind;}
    inline bool CanPromote(int kind){return kind>=Pawn&&kind<=Rook&&kind!=Gold;}

    struct Move
    {
        int from=-1;       // the square it leaves (-1: dropped from the hand)
        int to=-1;         // (-1: no move)
        int drop=None;     // the kind dropped
        bool promote=false;
        bool operator==(const Move& o) const {return from==o.from&&to==o.to&&drop==o.drop&&promote==o.promote;}
        bool operator!=(const Move& o) const {return !(*this==o);}
        bool Valid() const {return to>=0;}
    };

    class Engine;

    // A position: the board, both hands and who is to move, with the moves played onto it
    // kept so that they can be taken back.
    class Position
    {
    public:
        // The opening position, first to move.
        void Start(int first);
        // A position in SFEN, the side moving first (b) as You (diagnostics).
        bool FromSfen(const std::string& sfen);

        // What is on a square: 0, or the kind plus 16 times its owner.
        int At(int square) const {return board[static_cast<size_t>(square)];}
        int KindAt(int square) const {return board[static_cast<size_t>(square)]&15;}
        int OwnerAt(int square) const {return board[static_cast<size_t>(square)]?board[static_cast<size_t>(square)]>>4:-1;}
        int Hand(int who,int kind) const {return hand[static_cast<size_t>(who)][static_cast<size_t>(kind)];}
        int ToMove() const {return side;}
        int KingOf(int who) const {return king[static_cast<size_t>(who)];}
        std::uint64_t Key() const {return key;}
        // The side to move is in check.
        bool InCheck() const {return Attacked(king[static_cast<size_t>(side)],side^1);}
        bool Attacked(int square,int by) const;

        // Every legal move for the side to move (a pawn dropped to mate is not one).
        void Legal(std::vector<Move>& out) const;
        bool IsLegal(const Move& m) const;
        void Play(const Move& m);
        void Undo();
        // The kind the last move took (0: none).
        int Captured() const {return undo.empty()?0:undo.back().captured&15;}

        // The leaf count of the legal move tree, depth moves deep (diagnostics).
        std::uint64_t Perft(int depth);
        // The key and kings agree with the board (diagnostics).
        bool Consistent() const;

    private:
        std::array<std::uint8_t,Squares> board{};
        std::array<std::array<int,HandKinds>,2> hand{};
        int side=You;
        std::array<int,2> king{};
        std::uint64_t key=0;
        struct Undone {Move m;int captured=0;std::uint64_t key=0;};
        std::vector<Undone> undo;

        // Pseudo-legal moves: every one (all), or without a pawn's, bishop's or rook's choice
        // not to promote (search); only captures (and checks escaped elsewhere) with captures.
        void Pseudo(std::vector<Move>& out,bool all,bool captures) const;
        // After m is played: the mover's king is safe (and a pawn drop does not mate).
        bool Safe(int mover) const;
        bool DropMates(const Move& m);
        bool AnyLegal();
        std::uint64_t Rekey() const;
        void Pass();
        friend class Engine;
    };

    // YourMove: you move (Play). HerTurn: she moves (Step). GameOver: a mate, a resignation or
    // a repetition.
    enum class Phase {YourMove,HerTurn,GameOver};
    enum class Ending {None,Mate,Resigned,Repetition,PerpetualCheck,MoveLimit};

    // Her move as her search chose it, with its worth for her (centipawns; near Mate when she
    // sees a mate, near -Mate when she sees hers).
    struct Found {Move move;int score=0;int depth=0;std::uint64_t nodes=0;};
    constexpr int Mate=30000;

    class Table
    {
    public:
        Table();
        ~Table();
        Table(const Table&)=delete;
        Table& operator=(const Table&)=delete;
        // care: how carefully she plays (1: her best; less: now and then a looser move).
        // first: who moves first (sente). seconds: how long she may think at most.
        void NewGame(std::uint32_t seed,double care,int first,double seconds=1.0);

        Phase Now() const {return phase;}
        int First() const {return first;}
        const Position& Board() const {return position;}
        int Moves() const {return static_cast<int>(played.size());}
        Move LastMove() const {return played.empty()?Move{}:played.back();}
        int LastWho() const {return played.empty()?-1:lastWho;}
        // The kind the last move took, as it stood (0: none).
        int LastCaptured() const {return lastCaptured;}
        // The kind the last move moved, as it stood before (a drop: the kind dropped).
        int LastMoved() const {return lastMoved;}
        bool InCheck() const {return position.InCheck();}
        // Your legal moves from a square, or of a kind dropped from your hand.
        std::vector<Move> MovesFrom(int square) const;
        std::vector<Move> DropsOf(int kind) const;
        bool CanPlay(const Move& m) const;
        void Play(const Move& m);
        void Resign(int who);

        // HerTurn: she starts to think (on a thread of its own) if she has not yet.
        void Think();
        bool Thought() const;
        // Her move (waits for her thinking); none when she resigns.
        Move HerNext();
        void Step();
        // Your move played as she would play it, thinking this long (diagnostics).
        void PlayForYou(double seconds);
        void PlayRandomlyForYou();

        int Winner() const {return winner;}
        Ending HowEnded() const {return ending;}
        // Her last thought: its worth for her, and how deep she read.
        const Found& LastThought() const {return thought;}
        // A move in words (▲７六歩), seen from the side moving first; same: "同" when it goes
        // where the move before went.
        std::wstring Name(const Move& m,int who,int kindBefore,bool same) const;
        std::wstring LastName() const;

        bool Consistent() const;

    private:
        std::mt19937 rng;
        double care=1,seconds=1.0;
        int first=You;
        Position position;
        Phase phase=Phase::YourMove;
        std::vector<Move> played;
        std::vector<Move> legal;             // the legal moves now
        std::vector<std::uint64_t> keys;     // the key before each move, and now
        std::vector<bool> checks;            // whether each of those positions was in check
        int lastWho=-1,lastCaptured=0,lastMoved=0;
        int winner=-1;
        Ending ending=Ending::None;
        Found thought;
        std::future<Found> thinking;
        bool planned=false;
        std::shared_ptr<std::atomic<bool>> stop;
        std::shared_ptr<void> memory;          // her search's table, kept between moves

        void Lay(const Move& m);
        void Settle();
        Found Choose(int who,double carefully,double secondsNow);
        double CareNow() const;
        void Halt();
        friend std::wstring CheckRules();
    };

    // Diagnostics (--shogi-test): the move counts from the opening, set positions checked for
    // the rules (two pawns on a file, a pawn dropped to mate, promotion, repetition) and her
    // finding mates; the first line counts the failures.
    std::wstring CheckRules();
    // Diagnostics (--shogi-play-test): plays count games, her against herself (thinking
    // seconds a move) and against random play; checks the position stays consistent and says
    // how the games ended.
    std::wstring CheckPlaying(int count,std::uint32_t seed,double seconds);
}
