#include "Gomoku.hpp"
#include <algorithm>
#include <chrono>
#include <sstream>

namespace MikuMikuDesktopPerch::Gomoku
{
    namespace
    {
        using Board=std::array<int,Points>;
        constexpr int Dirs[4][2]={{0,1},{1,0},{1,1},{1,-1}};   // (row, column) steps

        bool Inside(int r,int c){return r>=0&&r<Size&&c>=0&&c<Size;}
        int P(int r,int c){return r*Size+c;}

        // The run of who's stones through point along dir (point itself counted as who's).
        int Run(const Board& b,int point,int who,int d,int* from=nullptr)
        {
            const int r=point/Size,c=point%Size,dr=Dirs[d][0],dc=Dirs[d][1];
            int back=0,ahead=0;
            for(int k=1;Inside(r-k*dr,c-k*dc)&&b[P(r-k*dr,c-k*dc)]==who;++k)++back;
            for(int k=1;Inside(r+k*dr,c+k*dc)&&b[P(r+k*dr,c+k*dc)]==who;++k)++ahead;
            if(from)*from=back;
            return back+ahead+1;
        }

        // A run of five wins: exactly five for black, five or more for white.
        bool Wins(int run,bool isBlack){return isBlack?run==5:run>=5;}

        // The fours through point along dir, with who's stone on point: each set of stones a
        // stone short of a winning five (a straight four is one), and whether one is straight
        // (two ways to five).
        int Fours(Board& b,int point,int who,bool isBlack,int d,bool* straight=nullptr)
        {
            const int r=point/Size,c=point%Size,dr=Dirs[d][0],dc=Dirs[d][1];
            std::vector<unsigned> sets;
            bool open=false;
            for(int k=-4;k<=4;++k)
            {
                const int qr=r+k*dr,qc=c+k*dc;
                if(k==0||!Inside(qr,qc)||b[P(qr,qc)]!=-1)continue;
                b[P(qr,qc)]=who;
                int back=0;
                const int run=Run(b,point,who,d,&back);
                // The five holds point and q: its stones (offsets -4..4 from point) less q.
                if(Wins(run,isBlack)&&k>=-back&&k<run-back)
                {
                    unsigned set=0;
                    for(int j=-back;j<run-back;++j)if(j!=k&&j>=-4&&j<=4)set|=1u<<(j+4);
                    if(std::find(sets.begin(),sets.end(),set)!=sets.end())open=true;
                    else sets.push_back(set);
                }
                b[P(qr,qc)]=-1;
            }
            if(straight)*straight=open;
            return static_cast<int>(sets.size());
        }

        // An open three through point along dir: one more stone there makes a straight four.
        bool OpenThree(Board& b,int point,int who,bool isBlack,int d)
        {
            const int r=point/Size,c=point%Size,dr=Dirs[d][0],dc=Dirs[d][1];
            for(int k=-3;k<=3;++k)
            {
                const int qr=r+k*dr,qc=c+k*dc;
                if(k==0||!Inside(qr,qc)||b[P(qr,qc)]!=-1)continue;
                b[P(qr,qc)]=who;
                bool straight=false;
                Fours(b,point,who,isBlack,d,&straight);
                // Black's four may not run to six.
                if(straight&&isBlack&&Run(b,point,who,d)>4)straight=false;
                b[P(qr,qc)]=-1;
                if(straight)return true;
            }
            return false;
        }

        Forbidden WhyNotOn(Board& b,int point,int who)
        {
            if(b[static_cast<size_t>(point)]!=-1)return Forbidden::None;
            b[point]=who;
            Forbidden why=Forbidden::None;
            bool five=false,six=false;
            for(int d=0;d<4;++d)
            {
                const int run=Run(b,point,who,d);
                if(run==5)five=true;
                if(run>=6)six=true;
            }
            if(!five)
            {
                int fours=0,threes=0;
                for(int d=0;d<4;++d)
                {
                    const int f=Fours(b,point,who,true,d);
                    fours+=f;
                    if(f==0&&OpenThree(b,point,who,true,d))++threes;
                }
                why=six?Forbidden::Overline:fours>=2?Forbidden::DoubleFour:threes>=2?Forbidden::DoubleThree:Forbidden::None;
            }
            b[point]=-1;
            return why;
        }

        constexpr double Five=1e6,OpenFour=1e5,Combo=9e4,Four=8000,Three=3000;

        // What a stone of who's on point is worth to who: fives, fours and threes it makes,
        // and the smaller shapes.
        double Worth(Board& b,int point,int who,bool isBlack)
        {
            b[point]=who;
            double v=0;
            int fours=0,threes=0;
            bool five=false;
            for(int d=0;d<4;++d)
            {
                int back=0;
                const int run=Run(b,point,who,d,&back);
                if(Wins(run,isBlack)){five=true;continue;}
                bool straight=false;
                const int f=Fours(b,point,who,isBlack,d,&straight);
                if(straight){v+=OpenFour;++fours;continue;}
                if(f>0){v+=Four*f;fours+=f;continue;}
                if(OpenThree(b,point,who,isBlack,d)){v+=Three;++threes;continue;}
                // Smaller shapes: the run and how many of its ends are free.
                const int r=point/Size,c=point%Size,dr=Dirs[d][0],dc=Dirs[d][1];
                const int er0=r-(back+1)*dr,ec0=c-(back+1)*dc,er1=r+(run-back)*dr,ec1=c+(run-back)*dc;
                const int free=(Inside(er0,ec0)&&b[P(er0,ec0)]==-1)+(Inside(er1,ec1)&&b[P(er1,ec1)]==-1);
                if(run>=3)v+=free?300:0;
                else if(run==2)v+=free==2?200:free==1?30:0;
                else v+=5*free;
            }
            b[point]=-1;
            if(five)return Five;
            if(fours>=2||(fours>=1&&threes>=1))v+=Combo;
            else if(threes>=2)v+=Combo/3;
            return v;
        }
    }

    void Table::NewGame(std::uint32_t seed,double c,int b)
    {
        rng.seed(seed);
        care=c;
        black=b;
        board.fill(-1);
        phase=black==You?Phase::YourMove:Phase::HerTurn;
        last=lastWho=-1;
        moves=0;
        winner=-1;
        line.clear();
        planned=-1;
    }

    Forbidden Table::WhyNot(int who,int point) const
    {
        if(who!=black||point<0||point>=Points)return Forbidden::None;
        Board b=board;
        return WhyNotOn(b,point,who);
    }

    bool Table::CanPlay(int point) const
    {
        return phase==Phase::YourMove&&point>=0&&point<Points&&board[static_cast<size_t>(point)]==-1&&WhyNot(You,point)==Forbidden::None;
    }

    int Table::ThreatOf(int who) const
    {
        Board b=board;
        for(int p=0;p<Points;++p)
        {
            if(b[p]!=-1)continue;
            b[p]=who;
            bool five=false;
            for(int d=0;d<4&&!five;++d)five=Wins(Run(b,p,who,d),who==black);
            b[p]=-1;
            if(five)return p;
        }
        return -1;
    }

    void Table::Lay(int who,int point)
    {
        board[static_cast<size_t>(point)]=who;
        last=point;
        lastWho=who;
        ++moves;
        planned=-1;
        for(int d=0;d<4;++d)
        {
            int back=0;
            const int run=Run(board,point,who,d,&back);
            if(Wins(run,who==black))
            {
                winner=who;
                line.clear();
                const int r=point/Size,c=point%Size;
                for(int k=-back;k<run-back;++k)line.push_back(P(r+k*Dirs[d][0],c+k*Dirs[d][1]));
                phase=Phase::GameOver;
                return;
            }
        }
        if(moves>=Points){phase=Phase::GameOver;return;}
        phase=who==You?Phase::HerTurn:Phase::YourMove;
    }

    void Table::Play(int point)
    {
        if(!CanPlay(point))return;
        Lay(You,point);
    }

    double Table::CareNow() const {return care;}

    int Table::Choose(int who,double carefully)
    {
        if(moves==0)return P(Size/2,Size/2);
        Board b=board;
        const int other=1-who;
        std::vector<std::pair<double,int>> ranked;
        for(int p=0;p<Points;++p)
        {
            if(b[p]!=-1)continue;
            // Only near the stones.
            bool near=false;
            const int r=p/Size,c=p%Size;
            for(int dr=-2;dr<=2&&!near;++dr)
                for(int dc=-2;dc<=2&&!near;++dc)
                    if(Inside(r+dr,c+dc)&&b[P(r+dr,c+dc)]!=-1)near=true;
            if(!near)continue;
            if(who==black&&WhyNotOn(b,p,who)!=Forbidden::None)continue;
            double v=Worth(b,p,who,who==black);
            // Where the other would play: worth taking from them (less where black may not go).
            double theirs=Worth(b,p,other,other==black);
            if(other==black&&theirs<Five&&WhyNotOn(b,p,other)!=Forbidden::None)theirs*=0.2;
            v+=0.85*theirs;
            // A little toward the middle.
            v+=2.0-0.1*(std::abs(r-Size/2)+std::abs(c-Size/2));
            ranked.push_back({v,p});
        }
        if(ranked.empty())
        {
            for(int p=0;p<Points;++p)if(b[p]==-1&&(who!=black||WhyNotOn(b,p,who)==Forbidden::None))return p;
            return -1;
        }
        std::stable_sort(ranked.begin(),ranked.end(),[](const auto& a,const auto& z){return a.first>z.first;});
        // Now and then the next-best point, never when the best wins or stops a loss.
        if(ranked.size()>1&&ranked[0].first<Three*2&&std::uniform_real_distribution<double>(0,1)(rng)<(1-carefully)*0.6)
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
        const int p=HerNext();
        if(p>=0)Lay(Her,p);
        else phase=Phase::GameOver;
    }

    void Table::PlayForYou()
    {
        if(phase!=Phase::YourMove)return;
        const int p=Choose(You,1);
        if(p>=0)Lay(You,p);
        else phase=Phase::GameOver;
    }

    void Table::PlayRandomlyForYou()
    {
        if(phase!=Phase::YourMove)return;
        std::vector<int> free;
        for(int p=0;p<Points;++p)if(CanPlay(p))free.push_back(p);
        if(free.empty()){phase=Phase::GameOver;return;}
        Lay(You,free[std::uniform_int_distribution<size_t>(0,free.size()-1)(rng)]);
    }

    bool Table::Consistent() const
    {
        int n=0;
        for(int v:board)if(v!=-1)++n;
        if(n!=moves)return false;
        if(phase==Phase::GameOver)return winner>=0?line.size()>=5:true;
        return winner<0;
    }

    std::wstring CheckRules()
    {
        std::wostringstream lines;
        int failed=0;
        auto check=[&](bool ok,const wchar_t* what){if(!ok)++failed;lines<<what<<L": "<<(ok?L"ok":L"WRONG")<<L"\n";};
        // A table with black stones (yours) and white ones (hers) set, black to move.
        auto setUp=[](Table& t,std::initializer_list<std::pair<int,int>> blacks,std::initializer_list<std::pair<int,int>> whites){
            t.NewGame(1,1,You);
            for(auto [r,c]:blacks){t.board[static_cast<size_t>(P(r,c))]=You;++t.moves;}
            for(auto [r,c]:whites){t.board[static_cast<size_t>(P(r,c))]=Her;++t.moves;}
        };
        Table t;
        setUp(t,{{7,3},{7,4},{7,5},{7,6}},{{0,0},{0,2},{0,4},{0,6}});
        check(t.WhyNot(You,P(7,7))==Forbidden::None,L"black's five may be laid");
        t.Play(P(7,7));
        check(t.Now()==Phase::GameOver&&t.Winner()==You&&t.Line().size()==5,L"black's five wins");
        setUp(t,{{7,2},{7,3},{7,4},{7,6},{7,7}},{{0,0},{0,2},{0,4},{0,6},{0,8}});
        check(t.WhyNot(You,P(7,5))==Forbidden::Overline,L"black's six is forbidden");
        setUp(t,{{7,5},{7,6},{5,7},{6,7}},{{0,0},{0,2},{0,4},{0,6}});
        check(t.WhyNot(You,P(7,7))==Forbidden::DoubleThree,L"black's double three is forbidden");
        setUp(t,{{7,4},{7,5},{7,6},{4,7},{5,7},{6,7}},{{0,0},{0,2},{0,4},{0,6},{0,8},{0,10}});
        check(t.WhyNot(You,P(7,7))==Forbidden::DoubleFour,L"black's double four is forbidden");
        setUp(t,{{7,4},{7,5},{7,6},{5,7},{6,7}},{{0,0},{0,2},{0,4},{0,6},{0,8}});
        check(t.WhyNot(You,P(7,7))==Forbidden::None,L"black's four-three may be laid");
        setUp(t,{{7,5},{7,6},{5,7},{6,7}},{{7,4},{0,2},{0,4},{0,6}});
        check(t.WhyNot(You,P(7,7))==Forbidden::None,L"a three closed at one end is not open");
        // White may make six; her (white) win taken, and your four stopped.
        setUp(t,{{0,0},{0,2},{0,4},{0,6},{0,8}},{{7,2},{7,3},{7,4},{7,6},{7,7}});
        t.phase=Phase::HerTurn;
        check(t.HerNext()==P(7,5),L"she takes her win (white six)");
        setUp(t,{{7,3},{7,4},{7,5},{7,6}},{{7,2},{0,2},{0,4}});
        t.phase=Phase::HerTurn;
        check(t.HerNext()==P(7,7),L"she stops your four");
        setUp(t,{{7,4},{7,5},{7,6},{0,14}},{{3,3},{3,4},{0,4}});
        t.phase=Phase::HerTurn;
        {
            const int p=t.HerNext();
            check(p==P(7,3)||p==P(7,7)||p==P(7,2)||p==P(7,8),L"she stops your open three");
        }
        std::wostringstream out;
        out<<failed<<L" failed\n"<<lines.str();
        return out.str();
    }

    std::wstring CheckPlaying(int count,std::uint32_t seed)
    {
        std::wostringstream out,lines;
        int wrong=0;
        for(int against=0;against<2;++against)
        {
            int hers=0,yours=0,draws=0,stones=0;
            double longest=0;
            for(int g=0;g<count;++g)
            {
                Table t;
                t.NewGame(seed+g*7919u+against*104729u,1,g%2==0?You:Her);
                for(int step=0;step<Points+5&&t.Now()!=Phase::GameOver;++step)
                {
                    if(t.Now()==Phase::HerTurn)
                    {
                        const auto from=std::chrono::steady_clock::now();
                        t.Step();
                        longest=std::max(longest,std::chrono::duration<double>(std::chrono::steady_clock::now()-from).count());
                    }
                    // Against her own play, your first moves at random (else every game is the same).
                    else if(against==1||step<4)t.PlayRandomlyForYou();
                    else t.PlayForYou();
                    if(!t.Consistent()){++wrong;break;}
                }
                if(t.Now()!=Phase::GameOver){++wrong;continue;}
                stones+=t.Moves();
                const int w=t.Winner();
                (w==Her?hers:w==You?yours:draws)+=1;
            }
            lines<<(against==0?L"against her own play: ":L"against random play: ")<<count<<L" games, her "<<hers<<L", yours "<<yours<<L", draws "<<draws
                 <<L"; stones on average "<<(count?stones/count:0)<<L"; her longest move "<<static_cast<int>(longest*1000)<<L" ms\n";
        }
        out<<wrong<<L" wrong\n"<<lines.str();
        return out.str();
    }
}
