#pragma once
#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch::Gomoku
{
    // The rules of the gomoku mini game (docs/Gomoku.md): you against her on a 15 by 15 board,
    // five in a row wins; black (who moves first) may not make a double three, a double four or
    // six or more in a row. Points are numbered row by row, Size * row + column; row 0 is her
    // side of the board, column 0 at your left.
    constexpr int You=0,Her=1;
    constexpr int Size=15,Points=Size*Size;

    enum class Forbidden {None,DoubleThree,DoubleFour,Overline};

    // YourMove: lay a stone (Play). HerTurn: she lays hers (Step). GameOver: five in a row, or
    // the board full.
    enum class Phase {YourMove,HerTurn,GameOver};

    class Table
    {
    public:
        // care: how carefully she plays (1: her best; less: now and then a looser move).
        // black: who has black and lays first.
        void NewGame(std::uint32_t seed,double care,int black);

        Phase Now() const {return phase;}
        int Black() const {return black;}
        // Whose stone is on point (You, Her), or -1.
        int At(int point) const {return board[static_cast<size_t>(point)];}
        int Stones() const {return moves;}
        // Why a stone of who's may not go on point (black only; None: it may, or it is not empty).
        Forbidden WhyNot(int who,int point) const;
        bool CanPlay(int point) const;

        // The last stone laid, and a count that grows with each.
        int LastPoint() const {return last;}
        int LastWho() const {return lastWho;}
        int Moves() const {return moves;}
        // GameOver: who made five (-1: the board filled with no one doing so), and the five.
        int Winner() const {return winner;}
        const std::vector<int>& Line() const {return line;}
        // The longest row of who's through point now that would win next (a stone short of
        // five, the five's free point): her lines and yours (the window's words). -1: none.
        int ThreatOf(int who) const;

        void Play(int point);
        // HerTurn, before she lays: the point she will lay on, chosen now and kept for her Step.
        int HerNext();
        void Step();
        // Your move played as she would play it, at her best (diagnostics).
        void PlayForYou();
        void PlayRandomlyForYou();

        // The board and the count agree, the game over only with five or a full board (diagnostics).
        bool Consistent() const;

    private:
        std::mt19937 rng;
        double care=1;
        std::array<int,Points> board{};
        int black=You;
        Phase phase=Phase::YourMove;
        int last=-1,lastWho=-1;
        int moves=0;
        int winner=-1;
        std::vector<int> line;
        int planned=-1;

        void Lay(int who,int point);
        int Choose(int who,double carefully);
        double CareNow() const;
        friend std::wstring CheckRules();
    };

    // Diagnostics (--gomoku-test): set positions checked for five, the forbidden moves and her
    // replies to threats; the first line counts the failures.
    std::wstring CheckRules();
    // Diagnostics (--gomoku-play-test): plays count games, her against a player of yours who plays
    // as she does at her best, and against one who plays at random; checks the board stays
    // consistent and says how the games ended.
    std::wstring CheckPlaying(int count,std::uint32_t seed);
}
