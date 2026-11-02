#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch::Chess
{
    // The rules of the chess mini game (docs/Chess.md): you against her on the usual board.
    // Squares are numbered from a1: 8 * rank + file (a1 0, h1 7, a8 56, h8 63); colours are
    // White and Black, who is which being the table's (ColourOf).
    constexpr int White=0,Black=1;
    constexpr int You=0,Her=1;
    constexpr int Squares=64;
    enum Kind : int {None=0,Pawn,Knight,Bishop,Rook,Queen,King};

    inline int FileOf(int square){return square&7;}
    inline int RankOf(int square){return square>>3;}

    struct Move
    {
        int from=-1,to=-1;
        int promote=None;   // the kind a pawn becomes
        bool operator==(const Move& o) const {return from==o.from&&to==o.to&&promote==o.promote;}
        bool operator!=(const Move& o) const {return !(*this==o);}
        bool Valid() const {return to>=0;}
    };

    class Engine;

    // A position, with the moves played onto it kept so that they can be taken back.
    class Position
    {
    public:
        void Start();
        // A position in FEN (diagnostics); false when it cannot be read.
        bool FromFen(const std::string& fen);

        // What is on a square: 0, or the kind plus 8 times its colour.
        int At(int square) const {return board[static_cast<size_t>(square)];}
        int KindAt(int square) const {return board[static_cast<size_t>(square)]&7;}
        int ColourAt(int square) const {return board[static_cast<size_t>(square)]?board[static_cast<size_t>(square)]>>3:-1;}
        int ToMove() const {return side;}
        int KingOf(int colour) const {return king[static_cast<size_t>(colour)];}
        int EnPassant() const {return enPassant;}
        int Rights() const {return rights;}
        int Halfmoves() const {return halfmoves;}
        std::uint64_t Key() const {return key;}
        bool Attacked(int square,int by) const;
        bool InCheck() const {return Attacked(king[static_cast<size_t>(side)],side^1);}
        // Neither side can ever mate (king alone, or with one knight or one bishop; or bishops
        // all on squares of one colour).
        bool TooLittle() const;

        void Legal(std::vector<Move>& out) const;
        void Play(const Move& m);
        void Undo();

        // The leaf count of the legal move tree, depth moves deep (diagnostics).
        std::uint64_t Perft(int depth);
        // The key and kings agree with the board (diagnostics).
        bool Consistent() const;

    private:
        std::array<std::uint8_t,Squares> board{};
        int side=White,rights=0,enPassant=-1,halfmoves=0;
        std::array<int,2> king{};
        std::uint64_t key=0;
        struct Undone {Move m;int captured=0,rights=0,enPassant=-1,halfmoves=0;std::uint64_t key=0;};
        std::vector<Undone> undo;

        void Pseudo(std::vector<Move>& out,bool captures) const;
        std::uint64_t Rekey() const;
        void Put(int square,int piece);
        void Pass();
        void UndoPass();
        friend class Engine;
    };

    // YourMove: you move (Play). HerTurn: she moves (Step). GameOver: mate or a draw.
    enum class Phase {YourMove,HerTurn,GameOver};
    enum class Ending {None,Checkmate,Stalemate,Repetition,FiftyMoves,TooLittle};

    // Her move as her search chose it, with its worth for her (centipawns; near Mate when she
    // sees a mate, near -Mate when she sees hers).
    struct Found {Move move;int score=0;int depth=0;std::uint64_t nodes=0;};
    constexpr int Mate=30000;

    // The last move, as the board shows it: what moved, what it took and from where (en passant:
    // not where it went), the rook of a castling.
    struct Played
    {
        Move move;
        int who=-1,moved=None,captured=None,capturedAt=-1,rookFrom=-1,rookTo=-1;
        bool check=false;
        std::wstring name;   // in the usual letters (Nf3, exd5, O-O, e8=Q+)
    };

    class Table
    {
    public:
        Table();
        ~Table();
        Table(const Table&)=delete;
        Table& operator=(const Table&)=delete;
        // care: how carefully she plays (1: her best; less: now and then a looser move).
        // yourColour: who has white. seconds: how long she may think at most.
        void NewGame(std::uint32_t seed,double care,int yourColour,double seconds=1.0);

        Phase Now() const {return phase;}
        int ColourOf(int who) const {return who==You?yours:yours^1;}
        int WhoHas(int colour) const {return colour==yours?You:Her;}
        const Position& Board() const {return position;}
        int Moves() const {return static_cast<int>(played);}
        const Played& Last() const {return last;}
        bool InCheck() const {return position.InCheck();}
        // Your legal moves from a square (four for each promotion).
        std::vector<Move> MovesFrom(int square) const;
        bool CanPlay(const Move& m) const;
        void Play(const Move& m);

        // HerTurn: she starts to think (on a thread of its own) if she has not yet.
        void Think();
        bool Thought() const;
        // Her move (waits for her thinking).
        Move HerNext();
        void Step();
        // Your move played as she would play it, thinking this long (diagnostics).
        void PlayForYou(double seconds);
        void PlayRandomlyForYou();

        int Winner() const {return winner;}
        Ending HowEnded() const {return ending;}
        const Found& LastThought() const {return thought;}
        bool Consistent() const;
        // The game goes on from a position in FEN (diagnostics).
        bool FromFen(const std::string& fen);

    private:
        std::mt19937 rng;
        double care=1,seconds=1.0;
        int yours=White;
        Position position;
        Phase phase=Phase::YourMove;
        size_t played=0;
        Played last;
        std::vector<Move> legal;
        std::vector<std::uint64_t> keys;     // the key before each move, and now
        int winner=-1;
        Ending ending=Ending::None;
        Found thought;
        std::future<Found> thinking;
        bool planned=false;
        std::shared_ptr<std::atomic<bool>> stop;
        std::shared_ptr<void> memory;        // her search's table, kept between moves

        void Lay(const Move& m);
        void Settle();
        double CareNow() const;
        void Halt();
        Found Choose(double carefully,double secondsNow);
        friend std::wstring CheckRules();
    };

    // Diagnostics (--chess-test): the move counts from known positions, the rules' corners and
    // her finding mates; the first line counts the failures.
    std::wstring CheckRules();
    // Diagnostics (--chess-play-test): plays count games, her against herself (thinking seconds
    // a move) and against random play; checks the position stays consistent and says how the
    // games ended.
    std::wstring CheckPlaying(int count,std::uint32_t seed,double seconds);
}
