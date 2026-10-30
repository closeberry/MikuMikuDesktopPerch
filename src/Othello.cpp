#include "Othello.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <sstream>

namespace MikuMikuDesktopPerch::Othello
{
    namespace
    {
        using Board=std::uint64_t;
        constexpr Board NotLeft=~0x0101010101010101ULL;    // every square but column 0
        constexpr Board NotRight=~0x8080808080808080ULL;   // every square but column 7
        constexpr int Directions[8]={1,-1,8,-8,9,7,-7,-9};

        // Every square one step toward dir (off the board's edges dropped).
        Board Shift(Board b,int dir)
        {
            switch(dir)
            {
            case 1:return (b<<1)&NotLeft;
            case -1:return (b>>1)&NotRight;
            case 8:return b<<8;
            case -8:return b>>8;
            case 9:return (b<<9)&NotLeft;
            case 7:return (b<<7)&NotRight;
            case -7:return (b>>7)&NotLeft;
            default:return (b>>9)&NotRight;
            }
        }

        // The squares mine can lay on against theirs.
        Board MovesOf(Board mine,Board theirs)
        {
            const Board empty=~(mine|theirs);
            Board moves=0;
            for(int dir:Directions)
            {
                Board x=Shift(mine,dir)&theirs;
                for(int i=0;i<5;++i)x|=Shift(x,dir)&theirs;
                moves|=Shift(x,dir)&empty;
            }
            return moves;
        }

        // The discs of theirs a disc of mine laid on square turns over.
        Board FlipsOf(Board mine,Board theirs,int square)
        {
            const Board at=1ULL<<square;
            Board flips=0;
            for(int dir:Directions)
            {
                Board line=0,x=Shift(at,dir);
                while(x&theirs){line|=x;x=Shift(x,dir);}
                if(x&mine)flips|=line;
            }
            return flips;
        }

        int Count(Board b){return std::popcount(b);}

        // How much a square is worth to hold: the corners most, the squares next to an empty
        // corner least (they give it away).
        constexpr int Weights[64]={
            100,-20,10, 5, 5,10,-20,100,
            -20,-50,-2,-2,-2,-2,-50,-20,
             10, -2,-1,-1,-1,-1, -2, 10,
              5, -2,-1,-1,-1,-1, -2,  5,
              5, -2,-1,-1,-1,-1, -2,  5,
             10, -2,-1,-1,-1,-1, -2, 10,
            -20,-50,-2,-2,-2,-2,-50,-20,
            100,-20,10, 5, 5,10,-20,100};
        constexpr int Corners[4]={0,7,56,63};
        // Each corner's neighbours (its C and X squares).
        constexpr int NextTo[4][3]={{1,8,9},{6,15,14},{57,48,49},{62,55,54}};

        constexpr int Won=100000;   // a finished game's score, past any judged one

        // The position as seen by mine, the side to move: squares held and the moves each has.
        int Judge(Board mine,Board theirs)
        {
            int score=0;
            for(Board b=mine;b;b&=b-1)score+=Weights[std::countr_zero(b)];
            for(Board b=theirs;b;b&=b-1)score-=Weights[std::countr_zero(b)];
            // A corner taken: the squares next to it are no longer a risk.
            for(int c=0;c<4;++c)
            {
                if(!((mine|theirs)>>Corners[c]&1))continue;
                for(int n:NextTo[c])
                {
                    if(mine>>n&1)score+=5-Weights[n];
                    if(theirs>>n&1)score-=5-Weights[n];
                }
            }
            score+=8*(Count(MovesOf(mine,theirs))-Count(MovesOf(theirs,mine)));
            return score;
        }

        int Final(Board mine,Board theirs)
        {
            const int d=Count(mine)-Count(theirs);
            return d>0?Won+d:d<0?-Won+d:0;
        }

        // Negamax with alpha-beta from mine's side; depth in moves (a pass is not counted).
        int Search(Board mine,Board theirs,int depth,int alpha,int beta,bool passed)
        {
            const Board moves=MovesOf(mine,theirs);
            if(!moves)
            {
                if(passed||!MovesOf(theirs,mine))return Final(mine,theirs);
                return -Search(theirs,mine,depth,-beta,-alpha,true);
            }
            if(depth<=0)return Judge(mine,theirs);
            // The better squares first (more cut off).
            int order[32],n=0;
            for(Board b=moves;b;b&=b-1)order[n++]=std::countr_zero(b);
            std::sort(order,order+n,[](int a,int b){return Weights[a]>Weights[b];});
            int best=-Won*2;
            for(int i=0;i<n;++i)
            {
                const Board f=FlipsOf(mine,theirs,order[i]);
                const int v=-Search(theirs&~f,mine|f|(1ULL<<order[i]),depth-1,-beta,-alpha,false);
                if(v>best)best=v;
                if(v>alpha)alpha=v;
                if(alpha>=beta)break;
            }
            return best;
        }

        constexpr int Depth=6;       // moves looked ahead in the middle of the game
        constexpr int Exact=12;      // with this many squares left or fewer, read to the end

        // Each square mine can lay on, with its worth, the best first.
        std::vector<std::pair<int,int>> Rank(Board mine,Board theirs)
        {
            std::vector<std::pair<int,int>> out;
            const int empties=64-Count(mine|theirs);
            const int depth=empties<=Exact?64:Depth;
            for(Board b=MovesOf(mine,theirs);b;b&=b-1)
            {
                const int s=std::countr_zero(b);
                const Board f=FlipsOf(mine,theirs,s);
                out.push_back({-Search(theirs&~f,mine|f|(1ULL<<s),depth-1,-Won*2,Won*2,false),s});
            }
            std::stable_sort(out.begin(),out.end(),[](const auto& a,const auto& b){return a.first>b.first;});
            return out;
        }

        std::uint64_t Perft(Board mine,Board theirs,int depth,bool passed)
        {
            if(depth==0)return 1;
            const Board moves=MovesOf(mine,theirs);
            if(!moves)
            {
                if(passed)return 1;
                return Perft(theirs,mine,depth-1,true);
            }
            std::uint64_t n=0;
            for(Board b=moves;b;b&=b-1)
            {
                const int s=std::countr_zero(b);
                const Board f=FlipsOf(mine,theirs,s);
                n+=Perft(theirs&~f,mine|f|(1ULL<<s),depth-1,false);
            }
            return n;
        }
    }

    void Table::NewGame(std::uint32_t seed,double c,int b)
    {
        rng.seed(seed);
        care=c;
        black=b;
        // White on the middle's top left and bottom right (as the board is seen from your side),
        // black on the other two.
        const Board whites=(1ULL<<27)|(1ULL<<36),blacks=(1ULL<<28)|(1ULL<<35);
        discs[black]=blacks;
        discs[1-black]=whites;
        phase=black==You?Phase::YourMove:Phase::HerTurn;
        last={};
        moves=0;
        passer=-1;
        passes=0;
        planned=-1;
    }

    int Table::At(int square) const
    {
        if(discs[You]>>square&1)return You;
        if(discs[Her]>>square&1)return Her;
        return -1;
    }

    int Table::Count(int who) const {return std::popcount(discs[who]);}
    int Table::Empties() const {return 64-std::popcount(discs[You]|discs[Her]);}

    std::vector<int> Table::Legal() const
    {
        std::vector<int> out;
        if(phase==Phase::GameOver)return out;
        const int who=phase==Phase::YourMove?You:Her;
        for(Board b=MovesOf(discs[who],discs[1-who]);b;b&=b-1)out.push_back(std::countr_zero(b));
        return out;
    }

    bool Table::CanPlay(int square) const
    {
        return phase==Phase::YourMove&&square>=0&&square<64&&(MovesOf(discs[You],discs[Her])>>square&1);
    }

    std::vector<int> Table::Flips(int who,int square) const
    {
        std::vector<int> out;
        if(square<0||square>=64||((discs[You]|discs[Her])>>square&1))return out;
        for(Board b=FlipsOf(discs[who],discs[1-who],square);b;b&=b-1)out.push_back(std::countr_zero(b));
        return out;
    }

    int Table::Winner() const
    {
        const int you=Count(You),her=Count(Her);
        return you>her?You:her>you?Her:-1;
    }

    void Table::Lay(int who,int square)
    {
        const Board f=FlipsOf(discs[who],discs[1-who],square);
        discs[who]|=f|(1ULL<<square);
        discs[1-who]&=~f;
        last.who=who;
        last.square=square;
        last.flipped.clear();
        for(Board b=f;b;b&=b-1)last.flipped.push_back(std::countr_zero(b));
        ++moves;
        planned=-1;
        Advance(who);
    }

    void Table::Advance(int who)
    {
        const int other=1-who;
        if(MovesOf(discs[other],discs[who]))phase=other==You?Phase::YourMove:Phase::HerTurn;
        else if(MovesOf(discs[who],discs[other]))
        {
            passer=other;
            ++passes;
            phase=who==You?Phase::YourMove:Phase::HerTurn;
        }
        else phase=Phase::GameOver;
    }

    void Table::Play(int square)
    {
        if(!CanPlay(square))return;
        Lay(You,square);
    }

    double Table::CareNow() const
    {
        // Well ahead late in the game, she minds it less.
        if(Empties()>24)return care;
        const int lead=Count(Her)-Count(You);
        return std::max(0.0,care-(lead>=10?0.15:0)-(lead>=20?0.15:0));
    }

    int Table::Choose(int who,double carefully)
    {
        const auto ranked=Rank(discs[who],discs[1-who]);
        if(ranked.empty())return -1;
        // Now and then the next-best square (never one that throws a won ending away).
        if(ranked.size()>1&&std::uniform_real_distribution<double>(0,1)(rng)<(1-carefully)*0.7&&
           !(ranked[0].first>=Won&&ranked[1].first<Won))
            return ranked[1].second;
        return ranked[0].second;
    }

    int Table::HerNext()
    {
        if(phase!=Phase::HerTurn)return -1;
        if(planned<0)planned=Choose(Her,CareNow());
        return planned;
    }

    void Table::Step()
    {
        if(phase!=Phase::HerTurn)return;
        const int s=HerNext();
        if(s>=0)Lay(Her,s);
    }

    void Table::PlayForYou()
    {
        if(phase!=Phase::YourMove)return;
        const int s=Choose(You,1);
        if(s>=0)Lay(You,s);
    }

    void Table::LayForTest(int square)
    {
        const int who=phase==Phase::YourMove?You:Her;
        if(phase!=Phase::GameOver&&(MovesOf(discs[who],discs[1-who])>>square&1))Lay(who,square);
    }

    void Table::PlayRandomlyForYou()
    {
        const auto legal=Legal();
        if(phase!=Phase::YourMove||legal.empty())return;
        Lay(You,legal[std::uniform_int_distribution<size_t>(0,legal.size()-1)(rng)]);
    }

    bool Table::Consistent() const
    {
        if(discs[You]&discs[Her])return false;
        if(Count(You)+Count(Her)!=4+moves)return false;
        // The side to move can move; at the end neither can.
        const Board yours=MovesOf(discs[You],discs[Her]),hers=MovesOf(discs[Her],discs[You]);
        if(phase==Phase::YourMove)return yours!=0;
        if(phase==Phase::HerTurn)return hers!=0;
        return !yours&&!hers;
    }

    std::wstring CheckRules()
    {
        std::wostringstream out;
        int failed=0;
        // Positions from the start (black to move), as counted for Othello.
        const std::uint64_t known[]={1,4,12,56,244,1396,8200,55092,390216,3005288};
        const Board whites=(1ULL<<27)|(1ULL<<36),blacks=(1ULL<<28)|(1ULL<<35);
        std::wostringstream lines;
        for(int d=1;d<=9;++d)
        {
            const std::uint64_t n=Perft(blacks,whites,d,false);
            const bool ok=n==known[d];
            if(!ok)++failed;
            lines<<L"depth "<<d<<L": "<<n<<(ok?L" ok":L" WRONG, expected ")<<(ok?L"":std::to_wstring(known[d]))<<L"\n";
        }
        // A disc turning over a whole row and column and diagonal at once.
        {
            Table t;
            t.NewGame(1,1,You);
            // Black to move at the start: laying on square 19 (row 2, column 3) turns over 27.
            const auto f=t.Flips(You,19);
            const bool ok=f.size()==1&&f[0]==27;
            if(!ok)++failed;
            lines<<L"first move on 19 turns over 27: "<<(ok?L"ok":L"WRONG")<<L"\n";
        }
        out<<failed<<L" failed\n"<<lines.str();
        return out.str();
    }

    std::wstring CheckPlaying(int count,std::uint32_t seed)
    {
        std::wostringstream out,lines;
        int wrong=0;
        for(int against=0;against<2;++against)
        {
            int hers=0,yours=0,ties=0,passes=0,hersDiscs=0,yoursDiscs=0;
            double longest=0;
            for(int g=0;g<count;++g)
            {
                Table t;
                t.NewGame(seed+g*7919u+against*104729u,1,g%2==0?You:Her);
                for(int step=0;step<200&&t.Now()!=Phase::GameOver;++step)
                {
                    // Against her own play, the first moves at random (else every game is the same).
                    if(against==0&&step<4)
                    {
                        const auto legal=t.Legal();
                        std::mt19937 any(seed+g*31u+step);
                        t.LayForTest(legal[std::uniform_int_distribution<size_t>(0,legal.size()-1)(any)]);
                    }
                    else if(t.Now()==Phase::HerTurn)
                    {
                        const auto from=std::chrono::steady_clock::now();
                        t.Step();
                        longest=std::max(longest,std::chrono::duration<double>(std::chrono::steady_clock::now()-from).count());
                    }
                    else if(against==0)t.PlayForYou();
                    else t.PlayRandomlyForYou();
                    if(!t.Consistent()){++wrong;break;}
                }
                if(t.Now()!=Phase::GameOver){++wrong;continue;}
                passes+=t.Passes();
                hersDiscs+=t.Count(Her);
                yoursDiscs+=t.Count(You);
                const int w=t.Winner();
                (w==Her?hers:w==You?yours:ties)+=1;
            }
            lines<<(against==0?L"against her own play: ":L"against random play: ")<<count<<L" games, her "<<hers<<L", yours "<<yours<<L", ties "<<ties
                 <<L"; discs on average her "<<(count?hersDiscs/count:0)<<L", yours "<<(count?yoursDiscs/count:0)<<L"; passes "<<passes<<L"; her longest move "<<static_cast<int>(longest*1000)<<L" ms\n";
        }
        out<<wrong<<L" wrong\n"<<lines.str();
        return out.str();
    }
}
