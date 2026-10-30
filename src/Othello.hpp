#pragma once
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch::Othello
{
    // The rules of the Othello mini game (docs/Othello.md): you against her on an 8 by 8 board.
    // Squares are numbered row by row, 8 * row + column; row 0 is her side of the board, row 7
    // yours, column 0 on your left.
    constexpr int You=0,Her=1;
    constexpr int Squares=64;

    // Who lays a disc, where, and the discs it turned over.
    struct Move
    {
        int who=-1;
        int square=-1;
        std::vector<int> flipped;
    };

    // YourMove: lay a disc (Play). HerTurn: she lays hers (Step). A side with no square to lay
    // on passes by itself. GameOver: neither can lay.
    enum class Phase {YourMove,HerTurn,GameOver};

    class Table
    {
    public:
        // care: how carefully she plays (1: her best; less: now and then a looser move).
        // black: who has the black discs and lays first.
        void NewGame(std::uint32_t seed,double care,int black);

        Phase Now() const {return phase;}
        int Black() const {return black;}
        // Whose disc is on square (You, Her), or -1.
        int At(int square) const;
        int Count(int who) const;
        int Empties() const;
        // The squares the side to move can lay on (none at the game's end).
        std::vector<int> Legal() const;
        bool CanPlay(int square) const;
        // The discs a disc of who's laid on square would turn over (none: it cannot be laid there).
        std::vector<int> Flips(int who,int square) const;

        // The last disc laid, and a count that grows with each.
        const Move& LastMove() const {return last;}
        int Moves() const {return moves;}
        // The last pass (who passed), and a count that grows with each.
        int LastPass() const {return passer;}
        int Passes() const {return passes;}
        // GameOver: who has more discs (-1: as many each).
        int Winner() const;

        // YourMove: lay your disc on square (one of Legal()).
        void Play(int square);
        // HerTurn, before she lays: the square she will lay on, chosen now and kept for her
        // Step, so her hand can go out to it first.
        int HerNext();
        void Step();
        // Your move played as she would play it, at her best (diagnostics).
        void PlayForYou();
        // Your move played at random (diagnostics).
        void PlayRandomlyForYou();
        // The side to move lays on square, when it can (diagnostics).
        void LayForTest(int square);

        // The board as the bitboards say, the counts adding up (diagnostics).
        bool Consistent() const;

    private:
        std::mt19937 rng;
        double care=1;
        std::uint64_t discs[2]{};     // by who
        int black=You;
        Phase phase=Phase::YourMove;
        Move last;
        int moves=0;
        int passer=-1,passes=0;
        int planned=-1;               // HerNext's choice

        void Lay(int who,int square);
        // After who's disc: the other moves, or passes, or the game is over.
        void Advance(int who);
        int Choose(int who,double carefully);
        double CareNow() const;
    };

    // Diagnostics (--othello-test): counts the positions reached from the start to a depth
    // (known numbers) and checks the turning over; the first line counts the failures.
    std::wstring CheckRules();
    // Diagnostics (--othello-play-test): plays count games, her against a player of yours who
    // plays as she does at her best, and against one who plays at random; checks the board stays
    // consistent and says how the games ended.
    std::wstring CheckPlaying(int count,std::uint32_t seed);
}
