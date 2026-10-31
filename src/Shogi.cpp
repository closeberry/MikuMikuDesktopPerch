#include "Shogi.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>

namespace MikuMikuDesktopPerch::Shogi
{
    namespace
    {
        // Directions as (row, column) steps, row 0 being her side: 0 up (your forward), then
        // round to the right.
        constexpr int Dr[8]={-1,-1,0,1,1,1,0,-1},Dc[8]={0,1,1,1,0,-1,-1,-1};
        constexpr unsigned N=1,NE=2,E=4,SE=8,S=16,SW=32,W=64,NW=128;
        constexpr unsigned GoldSteps=N|NE|NW|E|W|S;
        int Opposite(int d){return (d+4)&7;}
        unsigned Turned(unsigned m){return ((m<<4)|(m>>4))&255u;}
        bool Inside(int r,int c){return r>=0&&r<Size&&c>=0&&c<Size;}

        std::uint64_t Mix(std::uint64_t& s)
        {
            std::uint64_t z=(s+=0x9E3779B97F4A7C15ull);
            z=(z^(z>>30))*0xBF58476D1CE4E5B9ull;
            z=(z^(z>>27))*0x94D049BB133111EBull;
            return z^(z>>31);
        }

        struct Tables
        {
            std::array<std::array<int,8>,Squares> next{};             // the square a step away, or -1
            std::array<std::array<std::array<int,2>,Squares>,2> knight{};
            std::array<std::array<unsigned,16>,2> steps{},slides{};   // by owner and kind
            std::array<std::array<std::uint64_t,Squares>,32> piece{};
            std::array<std::array<std::array<std::uint64_t,19>,HandKinds>,2> held{};
            std::uint64_t turn=0;
            Tables()
            {
                for(int sq=0;sq<Squares;++sq)
                {
                    const int r=sq/Size,c=sq%Size;
                    for(int d=0;d<8;++d)next[sq][d]=Inside(r+Dr[d],c+Dc[d])?(r+Dr[d])*Size+c+Dc[d]:-1;
                    for(int who=0;who<2;++who)
                        for(int k=0;k<2;++k)
                        {
                            const int rr=r+(who==You?-2:2),cc=c+(k?1:-1);
                            knight[who][sq][k]=Inside(rr,cc)?rr*Size+cc:-1;
                        }
                }
                std::array<unsigned,16> step{},slide{};
                step[Pawn]=N;
                slide[Lance]=N;
                step[Silver]=N|NE|NW|SE|SW;
                step[Gold]=step[Tokin]=step[ProLance]=step[ProKnight]=step[ProSilver]=GoldSteps;
                slide[Bishop]=NE|SE|SW|NW;
                slide[Rook]=N|E|S|W;
                step[King]=255;
                step[Horse]=N|E|S|W;slide[Horse]=slide[Bishop];
                step[Dragon]=NE|SE|SW|NW;slide[Dragon]=slide[Rook];
                for(int k=0;k<16;++k)
                {
                    steps[You][k]=step[k];slides[You][k]=slide[k];
                    steps[Her][k]=Turned(step[k]);slides[Her][k]=Turned(slide[k]);
                }
                std::uint64_t seed=0x5106A1ull;
                for(auto& row:piece)for(auto& z:row)z=Mix(seed);
                for(auto& who:held)for(auto& kind:who)for(auto& z:kind)z=Mix(seed);
                turn=Mix(seed);
            }
        };
        const Tables& Geo(){static const Tables t;return t;}

        // The last n rows on the far side for who.
        bool LastRanks(int who,int sq,int n){const int r=sq/Size;return who==You?r<n:r>=Size-n;}
        int Distance(int a,int b){return std::max(std::abs(a/Size-b/Size),std::abs(a%Size-b%Size));}

        // What pieces are worth (centipawns), on the board and in the hand.
        constexpr int Value[16]={0,100,330,400,520,580,850,1000,0,560,560,560,560,0,1100,1300};
        constexpr int HandValue[HandKinds]={0,110,350,430,580,620,950,1100};
        // Near the other's king (by distance, 0-8), and near one's own.
        constexpr int Near[16][9]={
            {},
            {0,20,12,4},                 // pawn
            {0,25,20,10,4},              // lance
            {0,25,20,10,4},              // knight
            {0,50,35,18,6},              // silver
            {0,60,40,20,8},              // gold
            {0,20,18,14,10,6,3},         // bishop
            {0,20,18,14,10,6,3},         // rook
            {},
            {0,60,40,20,8},{0,60,40,20,8},{0,60,40,20,8},{0,60,40,20,8},
            {},
            {0,60,45,30,18,10,5},        // horse
            {0,60,45,30,18,10,5}};       // dragon
        constexpr int Guard[16][9]={
            {},
            {0,10,8},
            {0,8,5},
            {0,8,5},
            {0,35,28,12},
            {0,45,32,12},
            {},
            {0,-40,-20},
            {},
            {0,45,32,12},{0,45,32,12},{0,45,32,12},{0,45,32,12},
            {},
            {0,30,25,15,5},
            {0,-10}};
        constexpr int Tempo=30;
        constexpr int Infinity=32000;
        constexpr int MaxPly=96;
        constexpr int Decisive=15000;   // a perpetual check, lost for the one checking
        constexpr int MoveLimit=512;    // a draw after this many moves
        constexpr int TopDepth=8;       // how deep she reads at most (fairly strong, not unbeatable)
    }

    // ---- The position ----

    void Position::Start(int first)
    {
        FromSfen("lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1");
        side=first;
        key=Rekey();
    }

    bool Position::FromSfen(const std::string& sfen)
    {
        board.fill(0);
        for(auto& h:hand)h.fill(0);
        undo.clear();
        king={-1,-1};
        std::istringstream in(sfen);
        std::string placed,turn,held;
        in>>placed>>turn>>held;
        auto kindOf=[](char ch){
            switch(ch){case 'p':return Pawn;case 'l':return Lance;case 'n':return Knight;case 's':return Silver;
                       case 'g':return Gold;case 'b':return Bishop;case 'r':return Rook;case 'k':return King;}
            return None;
        };
        int r=0,c=0;
        bool promoted=false;
        for(char ch:placed)
        {
            if(ch=='/'){++r;c=0;continue;}
            if(ch=='+'){promoted=true;continue;}
            if(ch>='1'&&ch<='9'){c+=ch-'0';continue;}
            const int who=std::isupper(static_cast<unsigned char>(ch))?You:Her;
            int kind=kindOf(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            if(!kind||r>=Size||c>=Size)return false;
            if(promoted)kind+=Promoted;
            promoted=false;
            board[static_cast<size_t>(r*Size+c)]=static_cast<std::uint8_t>(kind+16*who);
            if(kind==King)king[static_cast<size_t>(who)]=r*Size+c;
            ++c;
        }
        side=turn=="w"?Her:You;
        int count=0;
        for(char ch:held)
        {
            if(ch=='-')break;
            if(ch>='0'&&ch<='9'){count=count*10+(ch-'0');continue;}
            const int who=std::isupper(static_cast<unsigned char>(ch))?You:Her;
            const int kind=kindOf(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            if(kind<Pawn||kind>Rook)return false;
            hand[static_cast<size_t>(who)][static_cast<size_t>(kind)]+=count?count:1;
            count=0;
        }
        key=Rekey();
        return king[0]>=0&&king[1]>=0;
    }

    std::uint64_t Position::Rekey() const
    {
        const Tables& t=Geo();
        std::uint64_t k=side==Her?t.turn:0;
        for(int sq=0;sq<Squares;++sq)if(board[sq])k^=t.piece[board[sq]][sq];
        for(int who=0;who<2;++who)
            for(int kind=Pawn;kind<=Rook;++kind)k^=t.held[who][kind][std::min(hand[who][kind],18)];
        return k;
    }

    bool Position::Attacked(int sq,int by) const
    {
        const Tables& t=Geo();
        const auto& steps=t.steps[static_cast<size_t>(by)];
        const auto& slides=t.slides[static_cast<size_t>(by)];
        for(int d=0;d<8;++d)
        {
            const unsigned toward=1u<<Opposite(d);
            int n=t.next[sq][d];
            if(n<0)continue;
            int c=board[n];
            if(c)
            {
                if((c>>4)==by&&((steps[c&15]|slides[c&15])&toward))return true;
                continue;
            }
            for(n=t.next[n][d];n>=0;n=t.next[n][d])
            {
                c=board[n];
                if(!c)continue;
                if((c>>4)==by&&(slides[c&15]&toward))return true;
                break;
            }
        }
        for(int n:t.knight[static_cast<size_t>(by^1)][sq])if(n>=0&&board[n]==Knight+16*by)return true;
        return false;
    }

    void Position::Pseudo(std::vector<Move>& out,bool all,bool captures) const
    {
        const Tables& t=Geo();
        out.clear();
        const int me=side;
        auto add=[&](int from,int to,int kind){
            if(CanPromote(kind)&&(LastRanks(me,from,3)||LastRanks(me,to,3)))
            {
                out.push_back({from,to,None,true});
                const bool must=((kind==Pawn||kind==Lance)&&LastRanks(me,to,1))||(kind==Knight&&LastRanks(me,to,2));
                if(!must&&(all||!(kind==Pawn||kind==Bishop||kind==Rook)))out.push_back({from,to,None,false});
            }
            else out.push_back({from,to,None,false});
        };
        for(int sq=0;sq<Squares;++sq)
        {
            const int c=board[sq];
            if(!c||(c>>4)!=me)continue;
            const int kind=c&15;
            // Whether a slide goes on past to.
            auto target=[&](int to){
                const int d=board[to];
                if(d&&((d>>4)==me||(d&15)==King))return false;
                if(!captures||d)add(sq,to,kind);
                return !d;
            };
            const unsigned steps=t.steps[me][kind],slides=t.slides[me][kind];
            for(int d=0;d<8;++d)
            {
                if(steps&(1u<<d)){const int to=t.next[sq][d];if(to>=0)target(to);}
                if(slides&(1u<<d))for(int to=t.next[sq][d];to>=0&&target(to);to=t.next[to][d]){}
            }
            if(kind==Knight)for(int to:t.knight[me][sq])if(to>=0)target(to);
        }
        if(captures)return;
        unsigned pawnFiles=0;
        for(int sq=0;sq<Squares;++sq)if(board[sq]==Pawn+16*me)pawnFiles|=1u<<(sq%Size);
        for(int kind=Pawn;kind<=Rook;++kind)
        {
            if(!hand[me][kind])continue;
            for(int sq=0;sq<Squares;++sq)
            {
                if(board[sq])continue;
                if((kind==Pawn||kind==Lance)&&LastRanks(me,sq,1))continue;
                if(kind==Knight&&LastRanks(me,sq,2))continue;
                if(kind==Pawn&&(pawnFiles>>(sq%Size)&1))continue;
                out.push_back({-1,sq,kind,false});
            }
        }
    }

    void Position::Play(const Move& m)
    {
        const Tables& t=Geo();
        undo.push_back({m,0,key});
        const int me=side;
        if(m.from<0)
        {
            int& n=hand[me][m.drop];
            key^=t.held[me][m.drop][n]^t.held[me][m.drop][n-1];
            --n;
            board[m.to]=static_cast<std::uint8_t>(m.drop+16*me);
            key^=t.piece[board[m.to]][m.to];
        }
        else
        {
            const int c=board[m.from],kind=c&15;
            key^=t.piece[c][m.from];
            board[m.from]=0;
            if(const int taken=board[m.to])
            {
                undo.back().captured=taken;
                key^=t.piece[taken][m.to];
                const int b=Base(taken&15);
                int& n=hand[me][b];
                key^=t.held[me][b][n]^t.held[me][b][n+1];
                ++n;
            }
            const int now=(m.promote?kind+Promoted:kind)+16*me;
            board[m.to]=static_cast<std::uint8_t>(now);
            key^=t.piece[now][m.to];
            if(kind==King)king[me]=m.to;
        }
        side^=1;
        key^=t.turn;
    }

    void Position::Pass()
    {
        undo.push_back({Move{},0,key});
        side^=1;
        key^=Geo().turn;
    }

    void Position::Undo()
    {
        const Undone u=undo.back();
        undo.pop_back();
        side^=1;
        key=u.key;
        const Move& m=u.m;
        if(m.to<0)return;   // a pass
        const int me=side;
        if(m.from<0){board[m.to]=0;++hand[me][m.drop];return;}
        int kind=board[m.to]&15;
        if(m.promote)kind-=Promoted;
        board[m.from]=static_cast<std::uint8_t>(kind+16*me);
        board[m.to]=static_cast<std::uint8_t>(u.captured);
        if(u.captured)--hand[me][Base(u.captured&15)];
        if(kind==King)king[me]=m.from;
    }

    bool Position::Safe(int mover) const {return !Attacked(king[static_cast<size_t>(mover)],mover^1);}

    bool Position::AnyLegal()
    {
        std::vector<Move> moves;
        Pseudo(moves,true,false);
        const int me=side;
        for(const Move& m:moves)
        {
            Play(m);
            const bool ok=Safe(me);
            Undo();
            if(ok)return true;
        }
        return false;
    }

    bool Position::DropMates(const Move& m)
    {
        if(m.drop!=Pawn)return false;
        const int mover=side^1;
        if(Geo().next[m.to][mover==You?0:4]!=king[static_cast<size_t>(side)])return false;
        return !AnyLegal();
    }

    void Position::Legal(std::vector<Move>& out) const
    {
        out.clear();
        Position p=*this;
        std::vector<Move> moves;
        Pseudo(moves,true,false);
        const int me=side;
        for(const Move& m:moves)
        {
            p.Play(m);
            const bool ok=p.Safe(me)&&!p.DropMates(m);
            p.Undo();
            if(ok)out.push_back(m);
        }
    }

    bool Position::IsLegal(const Move& m) const
    {
        std::vector<Move> moves;
        Legal(moves);
        return std::find(moves.begin(),moves.end(),m)!=moves.end();
    }

    std::uint64_t Position::Perft(int depth)
    {
        std::vector<Move> moves;
        Legal(moves);
        if(depth<=1)return moves.size();
        std::uint64_t n=0;
        for(const Move& m:moves){Play(m);n+=Perft(depth-1);Undo();}
        return n;
    }

    bool Position::Consistent() const
    {
        if(key!=Rekey())return false;
        std::array<int,9> count{};
        for(int sq=0;sq<Squares;++sq)
        {
            if(!board[sq])continue;
            const int kind=board[sq]&15;
            ++count[static_cast<size_t>(Base(kind))];
            if(kind==King&&king[static_cast<size_t>(board[sq]>>4)]!=sq)return false;
        }
        for(int who=0;who<2;++who)
            for(int kind=Pawn;kind<=Rook;++kind)count[static_cast<size_t>(kind)]+=hand[who][kind];
        return count==std::array<int,9>{0,18,4,4,4,4,2,2,2};
    }

    // ---- Her search ----

    class Engine
    {
    public:
        struct Entry {std::uint64_t key=0;std::uint16_t move=0;std::int16_t score=0;std::int8_t depth=-1;std::uint8_t bound=0;};
        using Memory=std::vector<Entry>;
        static constexpr size_t MemorySize=1u<<20;

        Engine(const Position& p,const std::vector<std::uint64_t>& history,const std::vector<bool>& checks,Memory& memory,
               std::atomic<bool>& stop,double seconds,std::uint32_t noise)
            :pos(p),tt(memory),stop(stop),noise(noise)
        {
            keys=history;
            checked.assign(checks.begin(),checks.end());
            if(keys.empty()||keys.back()!=pos.key){keys.push_back(pos.key);checked.push_back(pos.InCheck());}
            start=Clock::now();
            deadline=start+std::chrono::microseconds(static_cast<long long>(seconds*1e6));
        }

        // Her best move by iterative deepening; loose: now and then the next best instead,
        // when it is not much worse (margin) and throws no mate away.
        Found Run(bool loose,int margin)
        {
            Found found;
            std::vector<Move> root;
            pos.Legal(root);
            if(root.empty()){found.score=-Mate;return found;}
            found.move=root[0];
            for(int depth=1;depth<=TopDepth;++depth)
            {
                Move best;
                int score=-Infinity;
                Root(root,depth,best,score);
                if(aborted)
                {
                    // A move found better before time ran out stands.
                    if(best.Valid()&&best!=root[0]&&score>found.score-30)found.move=best;
                    break;
                }
                found.move=best;
                found.score=score;
                found.depth=depth;
                std::stable_partition(root.begin(),root.end(),[&](const Move& m){return m==best;});
                if(std::abs(score)>=Mate-MaxPly)break;
                if(Seconds()>0.45*Budget())break;
            }
            if(loose&&found.depth>=2&&root.size()>1&&std::abs(found.score)<Mate-MaxPly)
            {
                // The best of the others, a little less deep, in a little more time.
                aborted=false;
                deadline=Clock::now()+std::chrono::microseconds(static_cast<long long>(Budget()*0.5e6));
                std::vector<Move> others(root.begin()+1,root.end());
                Move second;
                int score=-Infinity;
                Root(others,std::max(1,found.depth-1),second,score);
                if(!aborted&&second.Valid()&&score>=found.score-margin&&score>-Mate+MaxPly)found.move=second,found.score=score;
            }
            found.nodes=nodes;
            return found;
        }

    private:
        using Clock=std::chrono::steady_clock;
        enum : std::uint8_t {Exact=1,Lower=2,Upper=3};
        Position pos;
        Memory& tt;
        std::atomic<bool>& stop;
        std::uint32_t noise;
        std::vector<std::uint64_t> keys;
        std::vector<char> checked;
        Clock::time_point start,deadline;
        std::uint64_t nodes=0;
        bool aborted=false;
        std::array<std::vector<Move>,MaxPly+1> lists;
        std::array<std::vector<int>,MaxPly+1> ranks;
        std::array<std::array<Move,2>,MaxPly+1> killers{};
        std::array<std::array<int,89*Squares>,2> history{};

        double Seconds() const {return std::chrono::duration<double>(Clock::now()-start).count();}
        double Budget() const {return std::chrono::duration<double>(deadline-start).count();}
        void Tick()
        {
            if((++nodes&1023)==0&&(stop.load(std::memory_order_relaxed)||Clock::now()>deadline))aborted=true;
        }

        static std::uint16_t Code(const Move& m)
        {
            const int from=m.from>=0?m.from:Squares+m.drop;
            return static_cast<std::uint16_t>(from|(m.to<<7)|(m.promote?1<<14:0));
        }
        static int FromIndex(const Move& m){return m.from>=0?m.from:Squares+m.drop;}

        // Plays m for the side to move; false (and nothing played) when it leaves its king in
        // check or drops a pawn to mate.
        bool Make(const Move& m)
        {
            const int me=pos.side;
            pos.Play(m);
            if(!pos.Safe(me)||pos.DropMates(m)){pos.Undo();return false;}
            keys.push_back(pos.key);
            checked.push_back(pos.InCheck());
            return true;
        }
        void Unmake(){pos.Undo();keys.pop_back();checked.pop_back();}

        // The position came before with the same side to move: a draw, unless one side gave
        // check with every move since (that side loses).
        bool Repeated(int& value) const
        {
            const int n=static_cast<int>(keys.size())-1;
            for(int i=n-1;i>=0&&n-i<=64;--i)
            {
                if(!keys[i])return false;   // a pass between
                if((n-i)%2||n-i<4||keys[i]!=keys[n])continue;
                bool mine=true,theirs=true;
                for(int j=i+1;j<=n;++j)(((n-j)&1)?theirs:mine)&=checked[j]!=0;
                value=mine?Decisive:theirs?-Decisive:0;
                return true;
            }
            return false;
        }

        int Evaluate() const
        {
            const Tables& t=Geo();
            int score[2]={0,0};
            int inHand[2]={0,0};
            for(int who=0;who<2;++who)
                for(int kind=Pawn;kind<=Rook;++kind)
                {
                    const int n=pos.hand[who][kind];
                    score[who]+=HandValue[kind]*n;
                    inHand[who]+=n;
                }
            for(int sq=0;sq<Squares;++sq)
            {
                const int c=pos.board[sq];
                if(!c)continue;
                const int who=c>>4,kind=c&15;
                if(kind==King)
                {
                    // Up from its own side is bare.
                    const int up=who==You?Size-1-sq/Size:sq/Size;
                    if(up>=3)score[who]-=(up-2)*40;
                    continue;
                }
                score[who]+=Value[kind]+Near[kind][std::min(8,Distance(sq,pos.king[who^1]))]+Guard[kind][std::min(8,Distance(sq,pos.king[who]))];
            }
            // The squares round each king that the other attacks, the worse with pieces in hand.
            for(int who=0;who<2;++who)
            {
                const int k=pos.king[who];
                int hit=0;
                for(int d=0;d<8;++d)
                {
                    const int n=t.next[k][d];
                    if(n>=0&&pos.Attacked(n,who^1))hit+=pos.board[n]?2:3;
                }
                score[who]-=hit*(6+2*std::min(inHand[who^1],6));
            }
            const std::uint64_t h=(pos.key^noise)*0x9E3779B97F4A7C15ull;
            const int jitter=static_cast<int>(h>>59)-16;
            const int me=pos.side;
            return score[me]-score[me^1]+Tempo+jitter;
        }

        // Ranks moves for trying: the remembered one, captures (the most for the least),
        // promotions, the killers, then by history.
        void Rank(const std::vector<Move>& moves,std::vector<int>& out,std::uint16_t remembered,int ply) const
        {
            out.resize(moves.size());
            for(size_t i=0;i<moves.size();++i)
            {
                const Move& m=moves[i];
                int r;
                if(Code(m)==remembered)r=1000000;
                else if(m.from>=0&&pos.board[m.to])
                    r=100000+Value[pos.board[m.to]&15]*8-Value[pos.board[m.from]&15]/8+(m.promote?500:0);
                else if(m.promote)r=90000+Value[(pos.board[m.from]&15)+Promoted]-Value[pos.board[m.from]&15];
                else if(m==killers[ply][0])r=80000;
                else if(m==killers[ply][1])r=79000;
                else r=std::min(70000,history[pos.side][FromIndex(m)*Squares+m.to]);
                out[i]=r;
            }
        }
        static void PickNext(std::vector<Move>& moves,std::vector<int>& ranks,size_t i)
        {
            size_t best=i;
            for(size_t j=i+1;j<moves.size();++j)if(ranks[j]>ranks[best])best=j;
            if(best!=i){std::swap(moves[i],moves[best]);std::swap(ranks[i],ranks[best]);}
        }

        static int ToMemory(int s,int ply){return s>=Mate-MaxPly?s+ply:s<=-Mate+MaxPly?s-ply:s;}
        static int FromMemory(int s,int ply){return s>=Mate-MaxPly?s-ply:s<=-Mate+MaxPly?s+ply:s;}

        int Root(const std::vector<Move>& root,int depth,Move& best,int& bestScore)
        {
            int alpha=-Infinity;
            const int beta=Infinity;
            for(size_t i=0;i<root.size();++i)
            {
                const Move& m=root[i];
                if(!Make(m))continue;
                int v;
                if(i==0)v=-Search(depth-1,-beta,-alpha,1,true);
                else
                {
                    v=-Search(depth-1,-alpha-1,-alpha,1,true);
                    if(v>alpha&&!aborted)v=-Search(depth-1,-beta,-alpha,1,true);
                }
                Unmake();
                if(aborted)break;
                if(v>bestScore){bestScore=v;best=m;}
                if(v>alpha)alpha=v;
            }
            Entry& e=tt[pos.key&(MemorySize-1)];
            if(!aborted&&best.Valid())e={pos.key,Code(best),static_cast<std::int16_t>(ToMemory(bestScore,0)),static_cast<std::int8_t>(depth),Exact};
            return bestScore;
        }

        int Search(int depth,int alpha,int beta,int ply,bool nullOk)
        {
            Tick();
            if(aborted)return 0;
            const bool pv=beta-alpha>1;
            const bool inCheck=checked.back()!=0;
            if(int value=0;Repeated(value))return value;
            alpha=std::max(alpha,-Mate+ply);
            beta=std::min(beta,Mate-ply-1);
            if(alpha>=beta)return alpha;
            if(inCheck&&ply<MaxPly/2)++depth;
            if(depth<=0)return Quiesce(alpha,beta,ply);
            if(ply>=MaxPly-1)return Evaluate();
            Entry& e=tt[pos.key&(MemorySize-1)];
            std::uint16_t remembered=0;
            if(e.key==pos.key)
            {
                remembered=e.move;
                if(!pv&&e.depth>=depth)
                {
                    const int s=FromMemory(e.score,ply);
                    if(e.bound==Exact||(e.bound==Lower&&s>=beta)||(e.bound==Upper&&s<=alpha))return s;
                }
            }
            if(nullOk&&!pv&&!inCheck&&depth>=3&&Evaluate()>=beta)
            {
                pos.Pass();
                keys.push_back(0);
                checked.push_back(0);
                const int v=-Search(depth-3,-beta,-beta+1,ply+1,false);
                Unmake();
                if(aborted)return 0;
                if(v>=beta)return beta;
            }
            auto& moves=lists[ply];
            auto& ranks=this->ranks[ply];
            pos.Pseudo(moves,false,false);
            Rank(moves,ranks,remembered,ply);
            const int alpha0=alpha;
            int best=-Infinity,legal=0;
            Move bestMove;
            for(size_t i=0;i<moves.size();++i)
            {
                PickNext(moves,ranks,i);
                const Move m=moves[i];
                const bool quiet=!(m.from>=0&&pos.board[m.to])&&!m.promote;
                if(!Make(m))continue;
                ++legal;
                const bool gives=checked.back()!=0;
                int v;
                if(legal==1)v=-Search(depth-1,-beta,-alpha,ply+1,true);
                else
                {
                    const int r=depth>=3&&!inCheck&&!gives&&quiet&&legal>3?(legal>10?2:1):0;
                    v=-Search(depth-1-r,-alpha-1,-alpha,ply+1,true);
                    if(v>alpha&&r&&!aborted)v=-Search(depth-1,-alpha-1,-alpha,ply+1,true);
                    if(v>alpha&&v<beta&&!aborted)v=-Search(depth-1,-beta,-alpha,ply+1,true);
                }
                Unmake();
                if(aborted)return 0;
                if(v>best){best=v;bestMove=m;}
                if(v>alpha)
                {
                    alpha=v;
                    if(alpha>=beta)
                    {
                        if(quiet)
                        {
                            if(killers[ply][0]!=m){killers[ply][1]=killers[ply][0];killers[ply][0]=m;}
                            int& h=history[pos.side][FromIndex(m)*Squares+m.to];
                            h=std::min(60000,h+depth*depth);
                        }
                        break;
                    }
                }
            }
            if(!legal)return -Mate+ply;   // mated (no move at all loses too)
            const std::uint8_t bound=best>=beta?Lower:best>alpha0?Exact:Upper;
            if(e.key!=pos.key||e.depth<=depth)
                e={pos.key,Code(bestMove),static_cast<std::int16_t>(ToMemory(best,ply)),static_cast<std::int8_t>(depth),bound};
            return best;
        }

        // Captures only (every escape when in check), until it is quiet.
        int Quiesce(int alpha,int beta,int ply)
        {
            Tick();
            if(aborted)return 0;
            const bool inCheck=checked.back()!=0;
            if(ply>=MaxPly-1)return Evaluate();
            auto& moves=lists[ply];
            auto& ranks=this->ranks[ply];
            int best,stand=0;
            if(inCheck)
            {
                best=-Mate+ply;
                pos.Pseudo(moves,false,false);
            }
            else
            {
                stand=Evaluate();
                if(stand>=beta)return stand;
                alpha=std::max(alpha,stand);
                best=stand;
                pos.Pseudo(moves,false,true);
            }
            Rank(moves,ranks,0,ply);
            for(size_t i=0;i<moves.size();++i)
            {
                PickNext(moves,ranks,i);
                const Move m=moves[i];
                if(!inCheck&&m.from>=0&&stand+Value[pos.board[m.to]&15]+(m.promote?400:0)+150<=alpha)continue;
                if(!Make(m))continue;
                const int v=-Quiesce(-beta,-alpha,ply+1);
                Unmake();
                if(aborted)return 0;
                if(v>best)
                {
                    best=v;
                    if(v>alpha){alpha=v;if(alpha>=beta)break;}
                }
            }
            return best;
        }
    };

    namespace
    {
        Engine::Memory& MemoryOf(std::shared_ptr<void>& memory)
        {
            if(!memory)memory=std::make_shared<Engine::Memory>(Engine::MemorySize);
            return *static_cast<Engine::Memory*>(memory.get());
        }
    }

    // ---- The game ----

    Table::Table():stop(std::make_shared<std::atomic<bool>>(false)){}
    Table::~Table(){Halt();}

    void Table::Halt()
    {
        if(thinking.valid())
        {
            stop->store(true);
            thinking.wait();
            thinking=std::future<Found>();
        }
        stop->store(false);
        planned=false;
    }

    void Table::NewGame(std::uint32_t seed,double c,int f,double s)
    {
        Halt();
        rng.seed(seed);
        care=c;
        first=f;
        seconds=s;
        position.Start(first);
        played.clear();
        keys={position.Key()};
        checks={false};
        lastWho=-1;
        lastCaptured=lastMoved=0;
        winner=-1;
        ending=Ending::None;
        thought={};
        phase=Phase::YourMove;
        Settle();
    }

    std::vector<Move> Table::MovesFrom(int square) const
    {
        std::vector<Move> out;
        if(phase!=Phase::YourMove)return out;
        for(const Move& m:legal)if(m.from==square&&m.from>=0)out.push_back(m);
        return out;
    }

    std::vector<Move> Table::DropsOf(int kind) const
    {
        std::vector<Move> out;
        if(phase!=Phase::YourMove)return out;
        for(const Move& m:legal)if(m.from<0&&m.drop==kind)out.push_back(m);
        return out;
    }

    bool Table::CanPlay(const Move& m) const
    {
        return phase==Phase::YourMove&&std::find(legal.begin(),legal.end(),m)!=legal.end();
    }

    void Table::Play(const Move& m)
    {
        if(CanPlay(m))Lay(m);
    }

    void Table::Resign(int who)
    {
        if(phase==Phase::GameOver)return;
        Halt();
        phase=Phase::GameOver;
        winner=who^1;
        ending=Ending::Resigned;
    }

    void Table::Lay(const Move& m)
    {
        const int who=position.ToMove();
        lastMoved=m.from<0?m.drop:position.KindAt(m.from);
        lastCaptured=m.from>=0?position.KindAt(m.to):0;
        position.Play(m);
        played.push_back(m);
        lastWho=who;
        keys.push_back(position.Key());
        checks.push_back(position.InCheck());
        if(thinking.valid())thinking=std::future<Found>();
        planned=false;
        Settle();
    }

    // After a move (or at the start): a mate, the fourth time a position comes, or whose turn.
    void Table::Settle()
    {
        position.Legal(legal);
        if(legal.empty())
        {
            phase=Phase::GameOver;
            winner=position.ToMove()^1;
            ending=Ending::Mate;
            return;
        }
        const size_t n=keys.size()-1;
        int seen=0;
        size_t from=n;
        for(size_t i=0;i<=n;++i)if(keys[i]==keys[n]){if(!seen)from=i;++seen;}
        if(seen>=4)
        {
            // Whether one side gave check with every move since the position first came.
            bool all[2]={true,true};
            for(size_t j=from+1;j<=n;++j)
            {
                const int mover=first^static_cast<int>((j-1)&1);
                if(!checks[j])all[mover]=false;
            }
            phase=Phase::GameOver;
            if(all[You]!=all[Her]){winner=all[You]?Her:You;ending=Ending::PerpetualCheck;}
            else {winner=-1;ending=Ending::Repetition;}
            return;
        }
        if(played.size()>=MoveLimit){phase=Phase::GameOver;winner=-1;ending=Ending::MoveLimit;return;}
        phase=position.ToMove()==You?Phase::YourMove:Phase::HerTurn;
    }

    double Table::CareNow() const
    {
        // Well ahead, she minds it less.
        const int lead=thought.score;
        return std::max(0.0,care-(lead>=800?0.15:0)-(lead>=1600?0.15:0));
    }

    Found Table::Choose(int who,double carefully,double secondsNow)
    {
        (void)who;
        const bool loose=std::uniform_real_distribution<double>(0,1)(rng)<(1-carefully)*0.7;
        Engine engine(position,keys,checks,MemoryOf(memory),*stop,secondsNow,static_cast<std::uint32_t>(rng()));
        return engine.Run(loose,static_cast<int>(150+300*(1-carefully)));
    }

    void Table::Think()
    {
        if(phase!=Phase::HerTurn||planned)return;
        planned=true;
        const double carefully=CareNow();
        const bool loose=std::uniform_real_distribution<double>(0,1)(rng)<(1-carefully)*0.7;
        const std::uint32_t noise=static_cast<std::uint32_t>(rng());
        Engine::Memory& tt=MemoryOf(memory);
        thinking=std::async(std::launch::async,[p=position,k=keys,c=checks,&tt,mem=memory,st=stop,secs=seconds,loose,carefully,noise]{
            Engine engine(p,k,c,tt,*st,secs,noise);
            return engine.Run(loose,static_cast<int>(150+300*(1-carefully)));
        });
    }

    bool Table::Thought() const
    {
        return planned&&(!thinking.valid()||thinking.wait_for(std::chrono::seconds(0))==std::future_status::ready);
    }

    Move Table::HerNext()
    {
        if(phase!=Phase::HerTurn)return {};
        Think();
        if(thinking.valid())thought=thinking.get();
        // She resigns once she sees she is mated.
        if(thought.score<=-Mate+MaxPly)return {};
        return thought.move;
    }

    void Table::Step()
    {
        if(phase!=Phase::HerTurn)return;
        const Move m=HerNext();
        if(!m.Valid()){Resign(Her);return;}
        Lay(m);
    }

    void Table::PlayForYou(double secondsNow)
    {
        if(phase!=Phase::YourMove)return;
        const Found f=Choose(You,1,secondsNow);
        if(!f.move.Valid()||f.score<=-Mate+MaxPly){Resign(You);return;}
        Lay(f.move);
    }

    void Table::PlayRandomlyForYou()
    {
        if(phase!=Phase::YourMove||legal.empty())return;
        Lay(legal[std::uniform_int_distribution<size_t>(0,legal.size()-1)(rng)]);
    }

    std::wstring Table::Name(const Move& m,int who,int kindBefore,bool same) const
    {
        if(!m.Valid())return L"";
        static const wchar_t* const names[16]={L"",L"歩",L"香",L"桂",L"銀",L"金",L"角",L"飛",L"玉",L"と",L"成香",L"成桂",L"成銀",L"",L"馬",L"龍"};
        static const wchar_t files[]=L"１２３４５６７８９",ranks[]=L"一二三四五六七八九";
        const int file=first==You?Size-m.to%Size:m.to%Size+1,rank=first==You?m.to/Size+1:Size-m.to/Size;
        std::wstring out=who==first?L"▲":L"△";
        if(same)out+=L"同";
        else {out+=files[file-1];out+=ranks[rank-1];}
        const int kind=m.from<0?m.drop:kindBefore;
        out+=kind==King&&who==Her?L"王":names[kind&15];
        if(m.from<0)out+=L"打";
        else if(m.promote)out+=L"成";
        else if(CanPromote(kind)&&(LastRanks(who,m.from,3)||LastRanks(who,m.to,3)))out+=L"不成";
        return out;
    }

    std::wstring Table::LastName() const
    {
        if(played.empty())return L"";
        const size_t n=played.size();
        return Name(played.back(),lastWho,lastMoved,n>=2&&played[n-2].to==played.back().to);
    }

    bool Table::Consistent() const
    {
        if(!position.Consistent()||keys.back()!=position.Key())return false;
        return (phase==Phase::GameOver)==(ending!=Ending::None);
    }

    // ---- Diagnostics ----

    std::wstring CheckRules()
    {
        std::wostringstream lines;
        int failed=0;
        auto check=[&](bool ok,const std::wstring& what){if(!ok)++failed;lines<<what<<L": "<<(ok?L"ok":L"WRONG")<<L"\n";};
        auto sq=[](int r,int c){return r*Size+c;};
        {
            Position p;
            p.Start(You);
            const std::uint64_t expected[]={30,900,25470,719731};
            for(int d=1;d<=4;++d)
            {
                const std::uint64_t n=p.Perft(d);
                check(n==expected[d-1],L"moves from the opening, "+std::to_wstring(d)+L" deep: "+std::to_wstring(n));
            }
            check(p.Consistent(),L"the opening is consistent");
        }
        {
            Position p;
            p.FromSfen("lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPP1PPPP/1B5R1/LNSGKGSNL b P 1");
            check(p.IsLegal({-1,sq(4,4),Pawn,false}),L"a pawn dropped on a file without one");
            check(!p.IsLegal({-1,sq(4,5),Pawn,false}),L"no second pawn on a file");
        }
        {
            Position p;
            p.FromSfen("7nk/7p1/9/9/9/9/9/9/4K3L b P 1");
            check(!p.IsLegal({-1,sq(1,8),Pawn,false}),L"no pawn dropped to mate");
            check(p.IsLegal({-1,sq(2,8),Pawn,false}),L"a pawn dropped short of the king");
            check(!p.IsLegal({-1,sq(0,0),Pawn,false}),L"no pawn dropped on the last row");
        }
        {
            Position p;
            p.FromSfen("4k4/P8/N8/9/3S5/9/9/9/4K4 b - 1");
            std::vector<Move> moves;
            p.Legal(moves);
            auto from=[&](int s){int n=0,promoted=0;for(const Move& m:moves)if(m.from==s){++n;promoted+=m.promote;}return std::pair{n,promoted};};
            check(from(sq(1,0))==std::pair{1,1},L"a pawn on the last row must promote");
            check(from(sq(2,0))==std::pair{1,1},L"a knight on the last two rows must promote");
            const auto silver=from(sq(4,3));
            check(silver.first==5&&silver.second==0,L"a silver outside the far rows does not promote");
            p.FromSfen("4k4/9/9/3S5/9/9/9/9/4K4 b - 1");
            p.Legal(moves);
            const auto into=from(sq(3,3));
            check(into.first==8&&into.second==3,L"a silver going into the far rows may promote or not");
        }
        {
            Table t;
            t.NewGame(1,1,Her,2.0);
            t.position.FromSfen("4k4/9/9/9/9/9/4p4/9/4K4 w g 1");
            t.keys={t.position.Key()};
            t.checks={false};
            t.Settle();
            const Move m=t.HerNext();
            check(m==Move{-1,sq(7,4),Gold,false},L"she drops a gold to mate");
            check(t.LastThought().score>=Mate-MaxPly,L"and knows it mates");
            t.Step();
            check(t.Now()==Phase::GameOver&&t.Winner()==Her&&t.HowEnded()==Ending::Mate,L"a mate ends the game");
        }
        {
            Table t;
            t.NewGame(1,1,Her,2.0);
            // A gold dropped beside her rook's row.
            t.position.FromSfen("4k4/9/9/9/9/9/9/2r6/4K4 w g 1");
            t.keys={t.position.Key()};
            t.checks={false};
            t.Settle();
            const Move m=t.HerNext();
            check(t.LastThought().score>=Mate-MaxPly&&m.Valid(),L"she sees a mate along her rook's row");
        }
        {
            Table t;
            t.NewGame(1,1,You);
            const Move go[4]={{sq(8,4),sq(7,5)},{sq(0,4),sq(1,3)},{sq(7,5),sq(8,4)},{sq(1,3),sq(0,4)}};
            for(int i=0;i<12;++i)
            {
                if(i==11)check(t.Now()!=Phase::GameOver,L"the third time is not yet a repetition");
                t.Lay(go[i%4]);
            }
            check(t.Now()==Phase::GameOver&&t.HowEnded()==Ending::Repetition&&t.Winner()<0,L"the fourth time is a draw");
        }
        {
            Table t;
            t.NewGame(1,1,You);
            t.position.FromSfen("k8/9/9/9/9/1R7/9/9/8K b - 1");
            t.keys={t.position.Key()};
            t.checks={false};
            t.Settle();
            const Move go[4]={{sq(5,1),sq(5,0)},{sq(0,0),sq(0,1)},{sq(5,0),sq(5,1)},{sq(0,1),sq(0,0)}};
            for(int i=0;i<12&&t.Now()!=Phase::GameOver;++i)t.Lay(go[i%4]);
            check(t.Now()==Phase::GameOver&&t.HowEnded()==Ending::PerpetualCheck&&t.Winner()==Her,L"checking every move of a repetition loses");
        }
        {
            Table t;
            t.NewGame(1,1,You);
            t.Lay({sq(6,2),sq(5,2)});
            check(t.LastName()==L"▲７六歩",L"a move in words: "+t.LastName());
            t.Lay({sq(2,6),sq(3,6)});
            t.Lay({sq(7,1),sq(1,7),None,true});
            check(t.LastName()==L"▲２二角成",L"a capture with promotion in words: "+t.LastName());
            t.Lay({sq(0,6),sq(1,7)});
            check(t.LastName()==L"△同銀",L"the same square in words: "+t.LastName());
            check(t.Consistent(),L"the game is consistent");
        }
        std::wostringstream out;
        out<<failed<<L" failed\n"<<lines.str();
        return out.str();
    }

    std::wstring CheckPlaying(int count,std::uint32_t seed,double seconds)
    {
        std::wostringstream out,lines;
        int wrong=0;
        for(int against=0;against<2;++against)
        {
            int hers=0,yours=0,draws=0,moves=0,depths=0,thoughts=0;
            double longest=0;
            std::wstring endings;
            for(int g=0;g<count;++g)
            {
                Table t;
                t.NewGame(seed+g*7919u+against*104729u,1,g%2==0?You:Her,seconds);
                for(int step=0;step<MoveLimit+5&&t.Now()!=Phase::GameOver;++step)
                {
                    if(t.Now()==Phase::HerTurn)
                    {
                        const auto from=std::chrono::steady_clock::now();
                        t.Step();
                        longest=std::max(longest,std::chrono::duration<double>(std::chrono::steady_clock::now()-from).count());
                        depths+=t.LastThought().depth;
                        ++thoughts;
                    }
                    // Against her own play, the first moves at random (else every game is alike).
                    else if(against==1||step<4)t.PlayRandomlyForYou();
                    else t.PlayForYou(seconds);
                    if(!t.Consistent()){++wrong;break;}
                }
                if(t.Now()!=Phase::GameOver){++wrong;continue;}
                moves+=t.Moves();
                const int w=t.Winner();
                (w==Her?hers:w==You?yours:draws)+=1;
                static const wchar_t* const how[]={L"-",L"mate",L"resigned",L"repetition",L"perpetual check",L"move limit"};
                endings+=std::wstring(endings.empty()?L"":L", ")+how[static_cast<int>(t.HowEnded())];
            }
            lines<<(against==0?L"against her own play: ":L"against random play: ")<<count<<L" games, her "<<hers<<L", yours "<<yours<<L", draws "<<draws
                 <<L"; moves on average "<<(count?moves/count:0)<<L"; her depth on average "<<(thoughts?static_cast<double>(depths)/thoughts:0)
                 <<L"; her longest move "<<static_cast<int>(longest*1000)<<L" ms ("<<endings<<L")\n";
        }
        out<<wrong<<L" wrong\n"<<lines.str();
        return out.str();
    }
}
