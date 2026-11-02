#define NOMINMAX
#include "Chess.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>

namespace MikuMikuDesktopPerch::Chess
{
    namespace
    {
        constexpr int WhiteShort=1,WhiteLong=2,BlackShort=4,BlackLong=8;
        constexpr int MaxPly=64,TopDepth=7;
        constexpr int Infinity=32000;
        constexpr int Value[7]={0,100,320,330,500,900,20000};

        // The rights a move loses by leaving or landing on a corner or a king's square.
        constexpr int Kept(int square)
        {
            switch(square)
            {
            case 0:return 15&~WhiteLong;
            case 4:return 15&~(WhiteShort|WhiteLong);
            case 7:return 15&~WhiteShort;
            case 56:return 15&~BlackLong;
            case 60:return 15&~(BlackShort|BlackLong);
            case 63:return 15&~BlackShort;
            }
            return 15;
        }

        struct Keys
        {
            std::uint64_t piece[16][Squares];
            std::uint64_t side;
            std::uint64_t rights[16];
            std::uint64_t file[8];
            Keys()
            {
                std::mt19937_64 r(0x43484553u);
                for(auto& row:piece)for(auto& k:row)k=r();
                side=r();
                for(auto& k:rights)k=r();
                for(auto& k:file)k=r();
            }
        };
        const Keys& TheKeys(){static const Keys k;return k;}

        constexpr int KnightSteps[8][2]={{1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2}};
        constexpr int RookWays[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
        constexpr int BishopWays[4][2]={{1,1},{1,-1},{-1,1},{-1,-1}};
        inline bool OnBoard(int f,int r){return f>=0&&f<8&&r>=0&&r<8;}
    }

    // ---- The position ----

    void Position::Put(int square,int piece)
    {
        board[static_cast<size_t>(square)]=static_cast<std::uint8_t>(piece);
    }

    std::uint64_t Position::Rekey() const
    {
        const Keys& k=TheKeys();
        std::uint64_t out=0;
        for(int s=0;s<Squares;++s)if(board[static_cast<size_t>(s)])out^=k.piece[board[static_cast<size_t>(s)]][s];
        if(side==Black)out^=k.side;
        out^=k.rights[rights];
        if(enPassant>=0)out^=k.file[FileOf(enPassant)];
        return out;
    }

    void Position::Start()
    {
        FromFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    }

    bool Position::FromFen(const std::string& fen)
    {
        std::istringstream in(fen);
        std::string rows,turn,castle,pass;
        int half=0;
        if(!(in>>rows>>turn>>castle>>pass))return false;
        in>>half;
        board={};
        undo.clear();
        int f=0,r=7;
        bool kings[2]={false,false};
        for(char c:rows)
        {
            if(c=='/'){--r;f=0;continue;}
            if(c>='1'&&c<='8'){f+=c-'0';continue;}
            int kind=None;
            switch(c|0x20)
            {
            case 'p':kind=Pawn;break;
            case 'n':kind=Knight;break;
            case 'b':kind=Bishop;break;
            case 'r':kind=Rook;break;
            case 'q':kind=Queen;break;
            case 'k':kind=King;break;
            default:return false;
            }
            if(!OnBoard(f,r))return false;
            const int colour=(c&0x20)?Black:White;
            Put(r*8+f,kind+colour*8);
            if(kind==King){king[static_cast<size_t>(colour)]=r*8+f;kings[colour]=true;}
            ++f;
        }
        if(!kings[0]||!kings[1])return false;
        side=turn=="b"?Black:White;
        rights=0;
        for(char c:castle)rights|=c=='K'?WhiteShort:c=='Q'?WhiteLong:c=='k'?BlackShort:c=='q'?BlackLong:0;
        enPassant=pass.size()==2?(pass[1]-'1')*8+(pass[0]-'a'):-1;
        halfmoves=half;
        key=Rekey();
        return true;
    }

    bool Position::Attacked(int square,int by) const
    {
        const int f=FileOf(square),r=RankOf(square);
        // A pawn of by's strikes toward the far side, so it stands a rank back from there.
        const int back=by==White?-1:1;
        for(int df:{-1,1})
            if(OnBoard(f+df,r+back)&&board[static_cast<size_t>((r+back)*8+f+df)]==Pawn+by*8)return true;
        for(const auto& s:KnightSteps)
            if(OnBoard(f+s[0],r+s[1])&&board[static_cast<size_t>((r+s[1])*8+f+s[0])]==Knight+by*8)return true;
        for(int df=-1;df<=1;++df)
            for(int dr=-1;dr<=1;++dr)
                if((df||dr)&&OnBoard(f+df,r+dr)&&board[static_cast<size_t>((r+dr)*8+f+df)]==King+by*8)return true;
        for(const auto& w:RookWays)
            for(int x=f+w[0],y=r+w[1];OnBoard(x,y);x+=w[0],y+=w[1])
                if(const int p=board[static_cast<size_t>(y*8+x)])
                {
                    if(p==Rook+by*8||p==Queen+by*8)return true;
                    break;
                }
        for(const auto& w:BishopWays)
            for(int x=f+w[0],y=r+w[1];OnBoard(x,y);x+=w[0],y+=w[1])
                if(const int p=board[static_cast<size_t>(y*8+x)])
                {
                    if(p==Bishop+by*8||p==Queen+by*8)return true;
                    break;
                }
        return false;
    }

    bool Position::TooLittle() const
    {
        int minors=0,knights=0;
        int bishopColours=0;
        for(int s=0;s<Squares;++s)
        {
            const int k=KindAt(s);
            if(k==Pawn||k==Rook||k==Queen)return false;
            if(k==Knight){++minors;++knights;}
            else if(k==Bishop){++minors;bishopColours|=1<<((FileOf(s)+RankOf(s))&1);}
        }
        if(minors<=1)return true;
        return !knights&&bishopColours!=3;
    }

    void Position::Pseudo(std::vector<Move>& out,bool captures) const
    {
        out.clear();
        const int me=side,other=side^1;
        for(int s=0;s<Squares;++s)
        {
            const int p=board[static_cast<size_t>(s)];
            if(!p||p>>3!=me)continue;
            const int kind=p&7,f=FileOf(s),r=RankOf(s);
            if(kind==Pawn)
            {
                const int up=me==White?1:-1,last=me==White?7:0,home=me==White?1:6;
                auto add=[&](int to){
                    if(RankOf(to)==last)for(int k:{Queen,Knight,Rook,Bishop})out.push_back({s,to,k});
                    else out.push_back({s,to,None});
                };
                const int ahead=s+up*8;
                if(!board[static_cast<size_t>(ahead)]&&(!captures||RankOf(ahead)==last))
                {
                    add(ahead);
                    if(r==home&&!board[static_cast<size_t>(ahead+up*8)])out.push_back({s,ahead+up*8,None});
                }
                for(int df:{-1,1})
                {
                    if(f+df<0||f+df>7)continue;
                    const int to=ahead+df,q=board[static_cast<size_t>(to)];
                    if((q&&q>>3==other)||to==enPassant)add(to);
                }
            }
            else if(kind==Knight)
            {
                for(const auto& k:KnightSteps)
                {
                    if(!OnBoard(f+k[0],r+k[1]))continue;
                    const int to=(r+k[1])*8+f+k[0],q=board[static_cast<size_t>(to)];
                    if(q?q>>3==other:!captures)out.push_back({s,to,None});
                }
            }
            else if(kind==King)
            {
                for(int df=-1;df<=1;++df)
                    for(int dr=-1;dr<=1;++dr)
                    {
                        if((!df&&!dr)||!OnBoard(f+df,r+dr))continue;
                        const int to=(r+dr)*8+f+df,q=board[static_cast<size_t>(to)];
                        if(q?q>>3==other:!captures)out.push_back({s,to,None});
                    }
                if(!captures)
                {
                    const int shortRight=me==White?WhiteShort:BlackShort,longRight=me==White?WhiteLong:BlackLong;
                    if((rights&(shortRight|longRight))&&!Attacked(s,other))
                    {
                        if((rights&shortRight)&&!board[static_cast<size_t>(s+1)]&&!board[static_cast<size_t>(s+2)]&&!Attacked(s+1,other))
                            out.push_back({s,s+2,None});
                        if((rights&longRight)&&!board[static_cast<size_t>(s-1)]&&!board[static_cast<size_t>(s-2)]&&!board[static_cast<size_t>(s-3)]&&!Attacked(s-1,other))
                            out.push_back({s,s-2,None});
                    }
                }
            }
            else
            {
                auto slide=[&](const int (*ways)[2]){
                    for(int i=0;i<4;++i)
                        for(int x=f+ways[i][0],y=r+ways[i][1];OnBoard(x,y);x+=ways[i][0],y+=ways[i][1])
                        {
                            const int to=y*8+x,q=board[static_cast<size_t>(to)];
                            if(q)
                            {
                                if(q>>3==other)out.push_back({s,to,None});
                                break;
                            }
                            if(!captures)out.push_back({s,to,None});
                        }
                };
                if(kind==Rook||kind==Queen)slide(RookWays);
                if(kind==Bishop||kind==Queen)slide(BishopWays);
            }
        }
    }

    void Position::Legal(std::vector<Move>& out) const
    {
        Position p=*this;
        std::vector<Move> all;
        p.Pseudo(all,false);
        out.clear();
        const int me=side;
        for(const Move& m:all)
        {
            p.Play(m);
            if(!p.Attacked(p.king[static_cast<size_t>(me)],me^1))out.push_back(m);
            p.Undo();
        }
    }

    void Position::Play(const Move& m)
    {
        const Keys& k=TheKeys();
        const int mover=board[static_cast<size_t>(m.from)],kind=mover&7,me=side;
        Undone u;
        u.m=m;
        u.rights=rights;
        u.enPassant=enPassant;
        u.halfmoves=halfmoves;
        u.key=key;
        int capturedAt=m.to;
        if(kind==Pawn&&m.to==enPassant)capturedAt=m.to+(me==White?-8:8);
        u.captured=board[static_cast<size_t>(capturedAt)];
        if(enPassant>=0)key^=k.file[FileOf(enPassant)];
        key^=k.rights[rights];
        if(u.captured){key^=k.piece[u.captured][capturedAt];Put(capturedAt,0);}
        key^=k.piece[mover][m.from];
        Put(m.from,0);
        const int landing=m.promote?m.promote+me*8:mover;
        Put(m.to,landing);
        key^=k.piece[landing][m.to];
        if(kind==King)
        {
            king[static_cast<size_t>(me)]=m.to;
            if(std::abs(m.to-m.from)==2)
            {
                const int rookFrom=m.to>m.from?m.from+3:m.from-4,rookTo=(m.from+m.to)/2,rook=Rook+me*8;
                Put(rookFrom,0);
                Put(rookTo,rook);
                key^=k.piece[rook][rookFrom]^k.piece[rook][rookTo];
            }
        }
        rights&=Kept(m.from)&Kept(m.to);
        enPassant=kind==Pawn&&std::abs(m.to-m.from)==16?(m.from+m.to)/2:-1;
        halfmoves=kind==Pawn||u.captured?0:halfmoves+1;
        key^=k.rights[rights];
        if(enPassant>=0)key^=k.file[FileOf(enPassant)];
        key^=k.side;
        side=me^1;
        undo.push_back(u);
    }

    void Position::Undo()
    {
        const Undone u=undo.back();
        undo.pop_back();
        const Move& m=u.m;
        side^=1;
        const int me=side,on=board[static_cast<size_t>(m.to)],kind=m.promote?Pawn:on&7;
        Put(m.from,kind+me*8);
        Put(m.to,0);
        if(u.captured)
        {
            const int at=kind==Pawn&&m.to==u.enPassant?m.to+(me==White?-8:8):m.to;
            Put(at,u.captured);
        }
        if(kind==King)
        {
            king[static_cast<size_t>(me)]=m.from;
            if(std::abs(m.to-m.from)==2)
            {
                const int rookFrom=m.to>m.from?m.from+3:m.from-4,rookTo=(m.from+m.to)/2;
                Put(rookTo,0);
                Put(rookFrom,Rook+me*8);
            }
        }
        rights=u.rights;
        enPassant=u.enPassant;
        halfmoves=u.halfmoves;
        key=u.key;
    }

    void Position::Pass()
    {
        const Keys& k=TheKeys();
        Undone u;
        u.rights=rights;
        u.enPassant=enPassant;
        u.halfmoves=halfmoves;
        u.key=key;
        undo.push_back(u);
        if(enPassant>=0)key^=k.file[FileOf(enPassant)];
        enPassant=-1;
        key^=k.side;
        side^=1;
    }

    void Position::UndoPass()
    {
        const Undone u=undo.back();
        undo.pop_back();
        side^=1;
        enPassant=u.enPassant;
        halfmoves=u.halfmoves;
        key=u.key;
    }

    std::uint64_t Position::Perft(int depth)
    {
        std::vector<Move> moves;
        Legal(moves);
        if(depth<=1)return depth==1?moves.size():1;
        std::uint64_t n=0;
        for(const Move& m:moves)
        {
            Play(m);
            n+=Perft(depth-1);
            Undo();
        }
        return n;
    }

    bool Position::Consistent() const
    {
        if(key!=Rekey())return false;
        for(int c:{White,Black})
            if(board[static_cast<size_t>(king[static_cast<size_t>(c)])]!=King+c*8)return false;
        return true;
    }

    // ---- Her judgement and search ----

    namespace
    {
        // How good a square is for each kind, seen from White (the first row here is rank 8).
        constexpr short PawnTable[64]={
             0,  0,  0,  0,  0,  0,  0,  0,
            50, 50, 50, 50, 50, 50, 50, 50,
            10, 10, 20, 30, 30, 20, 10, 10,
             5,  5, 10, 25, 25, 10,  5,  5,
             0,  0,  0, 20, 20,  0,  0,  0,
             5, -5,-10,  0,  0,-10, -5,  5,
             5, 10, 10,-20,-20, 10, 10,  5,
             0,  0,  0,  0,  0,  0,  0,  0};
        constexpr short KnightTable[64]={
           -50,-40,-30,-30,-30,-30,-40,-50,
           -40,-20,  0,  0,  0,  0,-20,-40,
           -30,  0, 10, 15, 15, 10,  0,-30,
           -30,  5, 15, 20, 20, 15,  5,-30,
           -30,  0, 15, 20, 20, 15,  0,-30,
           -30,  5, 10, 15, 15, 10,  5,-30,
           -40,-20,  0,  5,  5,  0,-20,-40,
           -50,-40,-30,-30,-30,-30,-40,-50};
        constexpr short BishopTable[64]={
           -20,-10,-10,-10,-10,-10,-10,-20,
           -10,  0,  0,  0,  0,  0,  0,-10,
           -10,  0,  5, 10, 10,  5,  0,-10,
           -10,  5,  5, 10, 10,  5,  5,-10,
           -10,  0, 10, 10, 10, 10,  0,-10,
           -10, 10, 10, 10, 10, 10, 10,-10,
           -10,  5,  0,  0,  0,  0,  5,-10,
           -20,-10,-10,-10,-10,-10,-10,-20};
        constexpr short RookTable[64]={
             0,  0,  0,  0,  0,  0,  0,  0,
             5, 10, 10, 10, 10, 10, 10,  5,
            -5,  0,  0,  0,  0,  0,  0, -5,
            -5,  0,  0,  0,  0,  0,  0, -5,
            -5,  0,  0,  0,  0,  0,  0, -5,
            -5,  0,  0,  0,  0,  0,  0, -5,
            -5,  0,  0,  0,  0,  0,  0, -5,
             0,  0,  0,  5,  5,  0,  0,  0};
        constexpr short QueenTable[64]={
           -20,-10,-10, -5, -5,-10,-10,-20,
           -10,  0,  0,  0,  0,  0,  0,-10,
           -10,  0,  5,  5,  5,  5,  0,-10,
            -5,  0,  5,  5,  5,  5,  0, -5,
             0,  0,  5,  5,  5,  5,  0, -5,
           -10,  5,  5,  5,  5,  5,  0,-10,
           -10,  0,  5,  0,  0,  0,  0,-10,
           -20,-10,-10, -5, -5,-10,-10,-20};
        constexpr short KingEarly[64]={
           -30,-40,-40,-50,-50,-40,-40,-30,
           -30,-40,-40,-50,-50,-40,-40,-30,
           -30,-40,-40,-50,-50,-40,-40,-30,
           -30,-40,-40,-50,-50,-40,-40,-30,
           -20,-30,-30,-40,-40,-30,-30,-20,
           -10,-20,-20,-20,-20,-20,-20,-10,
            20, 20,  0,  0,  0,  0, 20, 20,
            20, 30, 10,  0,  0, 10, 30, 20};
        constexpr short KingLate[64]={
           -50,-40,-30,-20,-20,-30,-40,-50,
           -30,-20,-10,  0,  0,-10,-20,-30,
           -30,-10, 20, 30, 30, 20,-10,-30,
           -30,-10, 30, 40, 40, 30,-10,-30,
           -30,-10, 30, 40, 40, 30,-10,-30,
           -30,-10, 20, 30, 30, 20,-10,-30,
           -30,-30,  0,  0,  0,  0,-30,-30,
           -50,-30,-30,-30,-30,-30,-30,-50};
        const short* const Tables[7]={nullptr,PawnTable,KnightTable,BishopTable,RookTable,QueenTable,nullptr};
    }

    class Engine
    {
    public:
        struct Entry {std::uint64_t key=0;std::uint16_t move=0;std::int16_t score=0;std::int8_t depth=-1;std::uint8_t bound=0;};
        using Memory=std::vector<Entry>;
        static constexpr size_t MemorySize=1u<<20;

        Engine(const Position& p,const std::vector<std::uint64_t>& history,Memory& memory,
               std::atomic<bool>& stop,double seconds,std::uint32_t noise)
            :pos(p),tt(memory),stop(stop),noise(noise)
        {
            keys=history;
            if(keys.empty()||keys.back()!=pos.key)keys.push_back(pos.key);
            pos.undo.reserve(pos.undo.size()+MaxPly+8);
            start=Clock::now();
            deadline=start+std::chrono::microseconds(static_cast<long long>(seconds*1e6));
        }

        // Her best move by iterative deepening; loose: now and then the next best instead, when
        // it is not much worse (margin) and throws no mate away.
        Found Run(bool loose,int margin)
        {
            Found found;
            std::vector<Move> root;
            pos.Legal(root);
            if(root.empty()){found.score=pos.InCheck()?-Mate:0;return found;}
            found.move=root[0];
            for(int depth=1;depth<=TopDepth;++depth)
            {
                Move best;
                int score=-Infinity;
                Root(root,depth,best,score);
                if(aborted)
                {
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
                const std::vector<Move> others(root.begin()+1,root.end());
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
        Clock::time_point start,deadline;
        std::uint64_t nodes=0;
        bool aborted=false;
        std::array<std::vector<Move>,MaxPly+2> lists;
        std::array<std::vector<int>,MaxPly+2> ranks;
        std::array<std::array<Move,2>,MaxPly+2> killers{};
        std::array<std::array<int,Squares*Squares>,2> history{};

        double Seconds() const {return std::chrono::duration<double>(Clock::now()-start).count();}
        double Budget() const {return std::chrono::duration<double>(deadline-start).count();}
        void Tick()
        {
            if((++nodes&1023)==0&&(stop.load(std::memory_order_relaxed)||Clock::now()>deadline))aborted=true;
        }

        static std::uint16_t Code(const Move& m){return static_cast<std::uint16_t>(m.from|(m.to<<6)|(m.promote<<12));}

        // Plays m for the side to move; false (and nothing played) when it leaves its king in check.
        bool Make(const Move& m)
        {
            const int me=pos.side;
            pos.Play(m);
            if(pos.Attacked(pos.king[static_cast<size_t>(me)],me^1)){pos.Undo();return false;}
            keys.push_back(pos.key);
            return true;
        }
        void Unmake(){pos.Undo();keys.pop_back();}

        bool DrawnNow() const
        {
            if(pos.halfmoves>=100||pos.TooLittle())return true;
            const int n=static_cast<int>(keys.size())-1;
            for(int i=n-2;i>=0&&i>=n-pos.halfmoves;i-=2)
                if(keys[static_cast<size_t>(i)]==keys[static_cast<size_t>(n)])return true;
            return false;
        }

        // The worth of the position for the side to move: what stands on the board and where,
        // the bishop pair, the pawns' shape, rooks on open files, the kings' cover.
        int Judge() const
        {
            std::array<std::array<int,8>,2> pawnsOnFile{};
            std::array<int,2> bishops{},heavy{};
            for(int s=0;s<Squares;++s)
            {
                const int p=pos.board[static_cast<size_t>(s)];
                if(!p)continue;
                const int c=p>>3,k=p&7;
                if(k==Pawn)
                    ++pawnsOnFile[c][FileOf(s)];
                else if(k==Bishop)++bishops[c];
                if(k==Knight||k==Bishop)heavy[c]+=1;
                else if(k==Rook)heavy[c]+=2;
                else if(k==Queen)heavy[c]+=4;
            }
            const int phase=std::min(24,heavy[0]+heavy[1]);   // 24: the opening's pieces, 0: only kings and pawns
            int sum[2]={0,0};
            for(int s=0;s<Squares;++s)
            {
                const int p=pos.board[static_cast<size_t>(s)];
                if(!p)continue;
                const int c=p>>3,k=p&7,f=FileOf(s),r=RankOf(s);
                const int mine=c==White?(7-r)*8+f:r*8+f;   // the tables' first row is the far side
                int v=Value[k];
                if(k==King)v=(KingEarly[mine]*phase+KingLate[mine]*(24-phase))/24;
                else v+=Tables[k][mine];
                if(k==Pawn)
                {
                    const int own=c==White?r:7-r;
                    const bool isolated=(f==0||!pawnsOnFile[c][f-1])&&(f==7||!pawnsOnFile[c][f+1]);
                    if(isolated)v-=12;
                    if(pawnsOnFile[c][f]>1)v-=10;
                    // Passed: nothing of theirs ahead on its file or beside it.
                    bool passed=true;
                    for(int g=std::max(0,f-1);g<=std::min(7,f+1)&&passed;++g)
                        for(int ahead=own+1;ahead<=6&&passed;++ahead)
                            if(pos.board[static_cast<size_t>((c==White?ahead:7-ahead)*8+g)]==Pawn+(c^1)*8)passed=false;
                    if(passed)v+=6*own*own/2;
                }
                else if(k==Rook)
                {
                    if(!pawnsOnFile[c][f])v+=pawnsOnFile[c^1][f]?10:22;
                }
                sum[c]+=v;
            }
            for(int c:{White,Black})
            {
                if(bishops[c]>=2)sum[c]+=30;
                // The pawns before a castled king, while pieces remain to attack it.
                const int ks=pos.king[static_cast<size_t>(c)],kf=FileOf(ks),kr=c==White?RankOf(ks):7-RankOf(ks);
                if(phase>=10&&kr<=1&&(kf<=2||kf>=5))
                {
                    int cover=0;
                    for(int g=std::max(0,kf-1);g<=std::min(7,kf+1);++g)
                    {
                        const int y1=c==White?kr+1:7-(kr+1),y2=c==White?kr+2:7-(kr+2);
                        if(pos.board[static_cast<size_t>(y1*8+g)]==Pawn+c*8)cover+=10;
                        else if(pos.board[static_cast<size_t>(y2*8+g)]==Pawn+c*8)cover+=5;
                    }
                    sum[c]+=cover;
                }
            }
            const int edge=static_cast<int>((pos.key^noise)&7)-3;   // a little of her own mood
            const int white=sum[White]-sum[Black];
            return (pos.side==White?white:-white)+edge+8;           // and the move is worth something
        }

        // Ranks moves for trying: the remembered one, captures (the most for the least),
        // promotions, the killers, then by history.
        void Rank(const std::vector<Move>& moves,std::vector<int>& out,std::uint16_t remembered,int ply) const
        {
            out.resize(moves.size());
            for(size_t i=0;i<moves.size();++i)
            {
                const Move& m=moves[i];
                const int victim=pos.board[static_cast<size_t>(m.to)]&7;
                const int attacker=pos.board[static_cast<size_t>(m.from)]&7;
                int r;
                if(Code(m)==remembered)r=1000000;
                else if(m.promote)r=m.promote==Queen?200000:-1000;
                else if(victim||(attacker==Pawn&&m.to==pos.enPassant))r=100000+Value[victim?victim:Pawn]*8-Value[attacker]/8;
                else if(m==killers[static_cast<size_t>(ply)][0])r=80000;
                else if(m==killers[static_cast<size_t>(ply)][1])r=79000;
                else r=std::min(70000,history[static_cast<size_t>(pos.side)][static_cast<size_t>(m.from*Squares+m.to)]);
                out[i]=r;
            }
        }
        static void PickNext(std::vector<Move>& moves,std::vector<int>& order,size_t i)
        {
            size_t best=i;
            for(size_t j=i+1;j<moves.size();++j)if(order[j]>order[best])best=j;
            if(best!=i){std::swap(moves[i],moves[best]);std::swap(order[i],order[best]);}
        }

        static int ToMemory(int s,int ply){return s>=Mate-MaxPly?s+ply:s<=-Mate+MaxPly?s-ply:s;}
        static int FromMemory(int s,int ply){return s>=Mate-MaxPly?s-ply:s<=-Mate+MaxPly?s+ply:s;}

        void Root(const std::vector<Move>& root,int depth,Move& best,int& bestScore)
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
        }

        int Search(int depth,int alpha,int beta,int ply,bool nullOk)
        {
            Tick();
            if(aborted)return 0;
            if(DrawnNow())return 0;
            if(ply>=MaxPly)return Judge();
            const bool inCheck=pos.InCheck();
            if(inCheck)++depth;
            if(depth<=0)return Quiet(alpha,beta,ply,0);
            const bool pv=beta-alpha>1;
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
            // With a pawn or more in hand even after passing, this is good enough.
            if(nullOk&&!pv&&!inCheck&&depth>=3&&HasPieces(pos.side))
            {
                const int before=Judge();
                if(before>=beta)
                {
                    pos.Pass();
                    keys.push_back(pos.key);
                    const int r=depth>=6?3:2;
                    const int v=-Search(depth-1-r,-beta,-beta+1,ply+1,false);
                    keys.pop_back();
                    pos.UndoPass();
                    if(aborted)return 0;
                    if(v>=beta&&v<Mate-MaxPly)return beta;
                }
            }
            // Too far behind to catch up in the last move or two without taking something.
            const int futile=!pv&&!inCheck&&depth<=2?Judge()+(depth==1?150:350):Infinity;
            auto& moves=lists[static_cast<size_t>(ply)];
            auto& order=ranks[static_cast<size_t>(ply)];
            pos.Pseudo(moves,false);
            Rank(moves,order,remembered,ply);
            const int from=alpha;
            int best=-Infinity,legal=0;
            std::uint16_t bestCode=0;
            for(size_t i=0;i<moves.size();++i)
            {
                PickNext(moves,order,i);
                const Move m=moves[i];
                const bool quiet=!pos.board[static_cast<size_t>(m.to)]&&!m.promote&&!((pos.board[static_cast<size_t>(m.from)]&7)==Pawn&&m.to==pos.enPassant);
                if(quiet&&legal>0&&futile<=alpha)continue;
                if(!Make(m))continue;
                ++legal;
                int v;
                if(legal==1)v=-Search(depth-1,-beta,-alpha,ply+1,true);
                else
                {
                    const int r=quiet&&depth>=3&&legal>=4&&!inCheck?(legal>=12&&depth>=5?2:1):0;
                    v=-Search(depth-1-r,-alpha-1,-alpha,ply+1,true);
                    if(v>alpha&&r&&!aborted)v=-Search(depth-1,-alpha-1,-alpha,ply+1,true);
                    if(v>alpha&&v<beta&&!aborted)v=-Search(depth-1,-beta,-alpha,ply+1,true);
                }
                Unmake();
                if(aborted)return 0;
                if(v>best){best=v;bestCode=Code(m);}
                if(v>alpha)alpha=v;
                if(alpha>=beta)
                {
                    if(quiet)
                    {
                        auto& k=killers[static_cast<size_t>(ply)];
                        if(k[0]!=m){k[1]=k[0];k[0]=m;}
                        int& h=history[static_cast<size_t>(pos.side)][static_cast<size_t>(m.from*Squares+m.to)];
                        h=std::min(60000,h+depth*depth);
                    }
                    break;
                }
            }
            if(!legal)return inCheck?-Mate+ply:0;
            const std::uint8_t bound=best>=beta?Lower:best<=from?Upper:Exact;
            e={pos.key,bestCode,static_cast<std::int16_t>(ToMemory(best,ply)),static_cast<std::int8_t>(depth),bound};
            return best;
        }

        bool HasPieces(int colour) const
        {
            for(int s=0;s<Squares;++s)
            {
                const int p=pos.board[static_cast<size_t>(s)];
                if(p&&p>>3==colour&&(p&7)!=Pawn&&(p&7)!=King)return true;
            }
            return false;
        }

        // Taking until nothing more is worth taking (every way out, when in check).
        int Quiet(int alpha,int beta,int ply,int deep)
        {
            Tick();
            if(aborted)return 0;
            if(ply>=MaxPly)return Judge();
            const bool inCheck=deep<4&&pos.InCheck();
            int best=-Infinity;
            if(!inCheck)
            {
                best=Judge();
                if(best>=beta)return best;
                if(best>alpha)alpha=best;
            }
            auto& moves=lists[static_cast<size_t>(ply)];
            auto& order=ranks[static_cast<size_t>(ply)];
            pos.Pseudo(moves,!inCheck);
            Rank(moves,order,0,ply);
            int legal=0;
            for(size_t i=0;i<moves.size();++i)
            {
                PickNext(moves,order,i);
                const Move m=moves[i];
                if(!inCheck)
                {
                    const int victim=pos.board[static_cast<size_t>(m.to)]&7;
                    // Even winning it all back would not reach alpha.
                    if(!m.promote&&best+Value[victim?victim:Pawn]+150<alpha)continue;
                    if(m.promote&&m.promote!=Queen)continue;
                }
                if(!Make(m))continue;
                ++legal;
                const int v=-Quiet(-beta,-alpha,ply+1,deep+1);
                Unmake();
                if(aborted)return 0;
                if(v>best)best=v;
                if(v>alpha)alpha=v;
                if(alpha>=beta)break;
            }
            if(inCheck&&!legal)return -Mate+ply;
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

        // The move in the usual letters, before it is played from p; legal: p's legal moves.
        std::wstring NameOf(const Position& p,const Move& m,const std::vector<Move>& legal)
        {
            const int kind=p.KindAt(m.from);
            if(kind==King&&std::abs(m.to-m.from)==2)return m.to>m.from?L"O-O":L"O-O-O";
            static const wchar_t letters[]=L" PNBRQK";
            const wchar_t file=static_cast<wchar_t>(L'a'+FileOf(m.from)),rank=static_cast<wchar_t>(L'1'+RankOf(m.from));
            const bool takes=p.At(m.to)||(kind==Pawn&&m.to==p.EnPassant());
            std::wstring out;
            if(kind==Pawn)
            {
                if(takes)out+=file;
            }
            else
            {
                out+=letters[kind];
                bool same=false,sameFile=false,sameRank=false;
                for(const Move& o:legal)
                    if(o.to==m.to&&o.from!=m.from&&p.KindAt(o.from)==kind)
                    {
                        same=true;
                        sameFile=sameFile||FileOf(o.from)==FileOf(m.from);
                        sameRank=sameRank||RankOf(o.from)==RankOf(m.from);
                    }
                if(same)
                {
                    if(!sameFile)out+=file;
                    else if(!sameRank)out+=rank;
                    else {out+=file;out+=rank;}
                }
            }
            if(takes)out+=L'x';
            out+=static_cast<wchar_t>(L'a'+FileOf(m.to));
            out+=static_cast<wchar_t>(L'1'+RankOf(m.to));
            if(m.promote){out+=L'=';out+=letters[m.promote];}
            return out;
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

    void Table::NewGame(std::uint32_t seed,double c,int yourColour,double s)
    {
        Halt();
        rng.seed(seed);
        care=c;
        yours=yourColour;
        seconds=s;
        position.Start();
        played=0;
        last={};
        keys={position.Key()};
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
        for(const Move& m:legal)if(m.from==square)out.push_back(m);
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

    void Table::Lay(const Move& m)
    {
        Played p;
        p.move=m;
        p.who=WhoHas(position.ToMove());
        p.moved=position.KindAt(m.from);
        p.name=NameOf(position,m,legal);
        p.capturedAt=m.to;
        if(p.moved==Pawn&&m.to==position.EnPassant())p.capturedAt=m.to+(position.ToMove()==White?-8:8);
        p.captured=position.KindAt(p.capturedAt);
        if(!p.captured)p.capturedAt=-1;
        if(p.moved==King&&std::abs(m.to-m.from)==2)
        {
            p.rookFrom=m.to>m.from?m.from+3:m.from-4;
            p.rookTo=(m.from+m.to)/2;
        }
        position.Play(m);
        ++played;
        keys.push_back(position.Key());
        p.check=position.InCheck();
        last=std::move(p);
        if(thinking.valid())thinking=std::future<Found>();
        planned=false;
        Settle();
        if(last.check)last.name+=ending==Ending::Checkmate?L"#":L"+";
    }

    // After a move (or at the start): mate, a draw, or whose turn.
    void Table::Settle()
    {
        position.Legal(legal);
        if(legal.empty())
        {
            phase=Phase::GameOver;
            if(position.InCheck())
            {
                winner=WhoHas(position.ToMove()^1);
                ending=Ending::Checkmate;
            }
            else ending=Ending::Stalemate;
            return;
        }
        const int seen=static_cast<int>(std::count(keys.begin(),keys.end(),keys.back()));
        if(seen>=3)ending=Ending::Repetition;
        else if(position.Halfmoves()>=100)ending=Ending::FiftyMoves;
        else if(position.TooLittle())ending=Ending::TooLittle;
        if(ending!=Ending::None){phase=Phase::GameOver;return;}
        phase=WhoHas(position.ToMove())==You?Phase::YourMove:Phase::HerTurn;
    }

    double Table::CareNow() const
    {
        // Well ahead, she minds it less.
        const int lead=thought.score;
        return std::max(0.0,care-(lead>=500?0.15:0)-(lead>=1000?0.15:0));
    }

    Found Table::Choose(double carefully,double secondsNow)
    {
        const bool loose=std::uniform_real_distribution<double>(0,1)(rng)<(1-carefully)*0.7;
        Engine engine(position,keys,MemoryOf(memory),*stop,secondsNow,static_cast<std::uint32_t>(rng()));
        return engine.Run(loose,static_cast<int>(50+160*(1-carefully)));
    }

    void Table::Think()
    {
        if(phase!=Phase::HerTurn||planned)return;
        planned=true;
        const double carefully=CareNow();
        const bool loose=std::uniform_real_distribution<double>(0,1)(rng)<(1-carefully)*0.7;
        const std::uint32_t noise=static_cast<std::uint32_t>(rng());
        Engine::Memory& tt=MemoryOf(memory);
        thinking=std::async(std::launch::async,[p=position,k=keys,&tt,mem=memory,st=stop,secs=seconds,loose,carefully,noise]{
            Engine engine(p,k,tt,*st,secs,noise);
            return engine.Run(loose,static_cast<int>(50+160*(1-carefully)));
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
        return thought.move;
    }

    void Table::Step()
    {
        if(phase!=Phase::HerTurn)return;
        const Move m=HerNext();
        if(m.Valid())Lay(m);
    }

    void Table::PlayForYou(double secondsNow)
    {
        if(phase!=Phase::YourMove)return;
        const Found f=Choose(1,secondsNow);
        if(f.move.Valid())Lay(f.move);
    }

    void Table::PlayRandomlyForYou()
    {
        if(phase!=Phase::YourMove||legal.empty())return;
        Lay(legal[std::uniform_int_distribution<size_t>(0,legal.size()-1)(rng)]);
    }

    bool Table::FromFen(const std::string& fen)
    {
        Halt();
        if(!position.FromFen(fen))return false;
        keys={position.Key()};
        played=0;
        last={};
        winner=-1;
        ending=Ending::None;
        Settle();
        return true;
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
        auto sq=[](const char* name){return (name[1]-'1')*8+(name[0]-'a');};
        auto mv=[&](const char* a,const char* b,int promote=None){return Move{sq(a),sq(b),promote};};
        struct Count {const char* name;const char* fen;std::vector<std::uint64_t> expected;};
        const std::vector<Count> counts={
            {"start","rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",{20,400,8902,197281,4865609}},
            {"kiwipete","r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",{48,2039,97862,4085603}},
            {"position 3","8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",{14,191,2812,43238,674624}},
            {"position 4","r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",{6,264,9467,422333}},
            {"position 5","rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",{44,1486,62379,2103487}},
            {"position 6","r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",{46,2079,89890,3894594}},
        };
        for(const auto& c:counts)
        {
            Position p;
            check(p.FromFen(c.fen),std::wstring(L"read ")+std::wstring(c.name,c.name+strlen(c.name)));
            for(size_t d=0;d<c.expected.size();++d)
            {
                const std::uint64_t n=p.Perft(static_cast<int>(d+1));
                check(n==c.expected[d],std::wstring(c.name,c.name+strlen(c.name))+L", "+std::to_wstring(d+1)+L" deep: "+std::to_wstring(n));
            }
            check(p.Consistent(),L"and it is consistent");
        }
        auto setUp=[&](Table& t,const char* fen,int yours){
            t.NewGame(1,1,yours,2.0);
            t.position.FromFen(fen);
            t.keys={t.position.Key()};
            t.ending=Ending::None;
            t.winner=-1;
            t.Settle();
        };
        {
            Table t;
            setUp(t,"rnbqkbnr/pppp1ppp/8/4p3/6P1/5P2/PPPPP2P/RNBQKBNR b KQkq - 0 2",White);
            const Move m=t.HerNext();
            check(m==mv("d8","h4"),L"she finds the fool's mate");
            check(t.LastThought().score>=Mate-MaxPly,L"and knows it mates");
            t.Step();
            check(t.Now()==Phase::GameOver&&t.Winner()==Her&&t.HowEnded()==Ending::Checkmate,L"a mate ends the game");
            check(t.Last().name==L"Qh4#",L"a mate in letters: "+t.Last().name);
        }
        {
            Table t;
            setUp(t,"6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1",Black);
            check(t.Now()==Phase::HerTurn,L"she has white when you have black");
            check(t.HerNext()==mv("a1","a8"),L"she finds a back-rank mate");
        }
        {
            Table t;
            setUp(t,"7k/5Q2/6K1/8/8/8/8/8 b - - 0 1",White);
            check(t.Now()==Phase::GameOver&&t.HowEnded()==Ending::Stalemate&&t.Winner()<0,L"no move and no check is a stalemate");
        }
        {
            Table t;
            setUp(t,"8/8/4k3/8/8/2BK4/8/8 w - - 0 1",White);
            check(t.HowEnded()==Ending::TooLittle,L"king and bishop against king is a draw");
            setUp(t,"8/8/4kb2/8/8/2BK4/8/8 w - - 0 1",White);
            check(t.HowEnded()==Ending::TooLittle,L"bishops on one colour of square are a draw");
            setUp(t,"8/8/4k1b1/8/8/2BK4/8/8 w - - 0 1",White);
            check(t.HowEnded()==Ending::None,L"bishops on different colours play on");
            setUp(t,"8/8/4k3/8/8/2NK2N1/8/8 w - - 0 1",White);
            check(t.HowEnded()==Ending::None,L"two knights play on");
        }
        {
            Table t;
            t.NewGame(1,1,White,1.0);
            const Move go[4]={mv("g1","f3"),mv("g8","f6"),mv("f3","g1"),mv("f6","g8")};
            for(int i=0;i<8;++i)
            {
                if(i==7)check(t.HowEnded()==Ending::None,L"twice is not yet a repetition");
                t.Lay(go[i%4]);
            }
            check(t.HowEnded()==Ending::Repetition&&t.Winner()<0,L"the third time is a draw");
        }
        {
            Table t;
            setUp(t,"7k/8/8/8/8/8/R7/K7 w - - 99 80",White);
            t.Lay(mv("a2","b2"));
            check(t.HowEnded()==Ending::FiftyMoves,L"fifty moves without a pawn or a capture is a draw");
        }
        {
            Table t;
            setUp(t,"r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",White);
            t.Lay(mv("e1","g1"));
            check(t.Last().rookFrom==sq("h1")&&t.Last().rookTo==sq("f1")&&t.Board().At(sq("f1"))==Rook,L"castling moves the rook");
            check(t.Last().name==L"O-O",L"castling in letters: "+t.Last().name);
            t.Lay(mv("e8","c8"));
            check(t.Last().rookFrom==sq("a8")&&t.Last().rookTo==sq("d8")&&t.Last().name==L"O-O-O",L"castling long");
            Position p;
            p.FromFen("4k3/8/8/8/8/5r2/8/4K2R w K - 0 1");
            std::vector<Move> moves;
            p.Legal(moves);
            check(std::find(moves.begin(),moves.end(),mv("e1","g1"))==moves.end(),L"no castling through an attacked square");
        }
        {
            Table t;
            setUp(t,"4k3/8/8/8/5p2/8/4P3/4K3 w - - 0 1",White);
            t.Lay(mv("e2","e4"));
            check(t.Board().EnPassant()==sq("e3"),L"a pawn's double step can be taken in passing");
            t.Lay(mv("f4","e3"));
            check(t.Last().capturedAt==sq("e4")&&t.Last().captured==Pawn&&t.Board().At(sq("e4"))==0,L"taken in passing");
            check(t.Last().name==L"fxe3",L"in letters: "+t.Last().name);
        }
        {
            Table t;
            setUp(t,"7k/P7/8/8/8/8/8/K7 w - - 0 1",White);
            const auto from=t.MovesFrom(sq("a7"));
            check(from.size()==4,L"a pawn on the last rank has four choices");
            t.Play(mv("a7","a8",Knight));
            check(t.Board().At(sq("a8"))==Knight&&t.Last().name==L"a8=N",L"promotion to a knight: "+t.Last().name);
        }
        {
            Table t;
            setUp(t,"7k/8/8/8/8/8/1N3N2/K7 w - - 0 1",White);
            t.Lay(mv("b2","d3"));
            check(t.Last().name==L"Nbd3",L"which knight, by file: "+t.Last().name);
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
        constexpr int MoveLimit=600;
        for(int against=0;against<2;++against)
        {
            int hers=0,yours=0,draws=0,moves=0,depths=0,thoughts=0;
            double longest=0;
            std::wstring endings;
            for(int g=0;g<count;++g)
            {
                Table t;
                t.NewGame(seed+g*7919u+against*104729u,1,g%2==0?White:Black,seconds);
                for(int step=0;step<MoveLimit&&t.Now()!=Phase::GameOver;++step)
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
                static const wchar_t* const how[]={L"-",L"mate",L"stalemate",L"repetition",L"fifty moves",L"too little"};
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
