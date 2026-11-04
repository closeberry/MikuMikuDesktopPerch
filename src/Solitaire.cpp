#include "Solitaire.hpp"
#include "Text.hpp"
#include <algorithm>
#include <chrono>
#include <map>
#include <unordered_map>

namespace MikuMikuDesktopPerch::Solitaire
{
    struct Geometry
    {
        std::vector<Spot> spots;
        std::array<std::vector<std::uint8_t>,TileCount> above,left,right;
    };

    namespace
    {
        using Faces=std::array<std::int8_t,TileCount>;

        // A block of tiles, in half tiles.
        void Block(std::vector<Spot>& out,int x0,int y0,int across,int down,int z)
        {
            for(int y=0;y<down;++y)for(int x=0;x<across;++x)out.push_back({x0+2*x,y0+2*y,z});
        }

        std::vector<Spot> Turtle()
        {
            std::vector<Spot> s;
            // The ground: rows of 12, 8, 10, 12, 12, 10, 8, 12, then the head and the tail.
            static const int rows[8][2]={{1,12},{3,8},{2,10},{1,12},{1,12},{2,10},{3,8},{1,12}};
            for(int r=0;r<8;++r)Block(s,2*rows[r][0],2*r,rows[r][1],1,0);
            s.push_back({0,7,0});
            s.push_back({26,7,0});
            s.push_back({28,7,0});
            Block(s,8,2,6,6,1);
            Block(s,10,4,4,4,2);
            Block(s,12,6,2,2,3);
            s.push_back({13,7,4});
            return s;
        }

        std::vector<Spot> Pyramid()
        {
            std::vector<Spot> s;
            Block(s,0,0,12,8,0);
            Block(s,4,4,8,4,1);
            Block(s,6,6,6,2,2);
            Block(s,10,6,2,2,3);
            return s;
        }

        std::vector<Spot> TwinTowers()
        {
            std::vector<Spot> s;
            Block(s,0,0,14,6,0);
            for(int x:{0,16})
            {
                Block(s,x+2,2,4,4,1);
                Block(s,x+3,3,3,3,2);
                Block(s,x+4,4,2,2,3);
                s.push_back({x+5,5,4});
            }
            return s;
        }

        // The rest, as blocks of tiles: x, y (half tiles), across, down (tiles), layer.
        struct Blocks {int x,y,across,down,z;};
        constexpr Blocks Castle[]={{0,0,14,2,0},{0,12,14,2,0},{0,4,2,4,0},{24,4,2,4,0},{0,0,14,1,1},{0,14,14,1,1},{0,2,1,6,1},{26,2,1,6,1},{0,0,2,2,2},{24,0,2,2,2},{0,12,2,2,2},{24,12,2,2,2},{2,2,1,1,1},{24,2,1,1,1},{2,12,1,1,1},{24,12,1,1,1},{10,6,4,2,0},{12,6,2,2,1}};
        constexpr Blocks Bridge[]={{0,0,4,8,0},{20,0,4,8,0},{8,6,6,2,0},{0,2,3,6,1},{22,2,3,6,1},{6,6,8,2,1},{0,4,2,4,2},{24,4,2,4,2}};
        constexpr Blocks Cross[]={{0,4,14,4,0},{10,0,4,2,0},{10,12,4,2,0},{2,6,12,2,1},{12,2,2,2,1},{12,10,2,2,1},{6,6,8,2,2},{12,4,2,1,2},{12,10,2,1,2},{8,6,6,2,3},{12,4,2,1,3},{12,10,2,1,3},{12,6,2,2,4}};
        constexpr Blocks Diamond[]={{8,0,6,1,0},{6,2,8,1,0},{2,4,12,1,0},{0,6,14,1,0},{0,8,14,1,0},{2,10,12,1,0},{6,12,8,1,0},{8,14,6,1,0},{12,2,2,1,1},{8,4,6,1,1},{4,6,10,1,1},{4,8,10,1,1},{8,10,6,1,1},{12,12,2,1,1},{12,4,2,1,2},{8,6,6,1,2},{8,8,6,1,2},{12,10,2,1,2},{10,6,4,1,3},{10,8,4,1,3},{12,6,2,1,4},{12,8,2,1,4}};
        constexpr Blocks Butterfly[]={{4,0,4,1,0},{16,0,4,1,0},{2,2,5,1,0},{16,2,5,1,0},{6,2,3,1,1},{16,2,3,1,1},{0,4,6,1,0},{16,4,6,1,0},{4,4,4,1,1},{16,4,4,1,1},{4,6,4,1,0},{16,6,4,1,0},{8,6,2,1,1},{16,6,2,1,1},{4,8,4,1,0},{16,8,4,1,0},{8,8,2,1,1},{16,8,2,1,1},{0,10,6,1,0},{16,10,6,1,0},{4,10,4,1,1},{16,10,4,1,1},{2,12,5,1,0},{16,12,5,1,0},{6,12,3,1,1},{16,12,3,1,1},{4,14,4,1,0},{16,14,4,1,0},{12,0,2,8,0},{12,2,2,6,1},{12,6,2,2,2}};
        constexpr Blocks Mountains[]={{0,0,14,6,0},{0,2,4,4,1},{2,4,2,2,2},{10,2,4,4,1},{12,4,2,2,2},{20,2,4,4,1},{22,4,2,2,2}};
        constexpr Blocks Stairs[]={{0,4,2,4,0},{4,4,2,4,0},{8,4,2,4,0},{8,4,2,4,1},{12,4,2,4,0},{12,4,2,4,1},{16,4,2,4,0},{16,4,2,4,1},{16,4,2,4,2},{20,4,2,4,0},{20,4,2,4,1},{20,4,2,4,2},{20,4,2,4,3},{24,4,2,4,0},{24,4,2,4,1},{24,4,2,4,2},{24,4,2,4,3},{24,4,2,4,4}};
        constexpr Blocks Cloister[]={{0,0,14,1,0},{0,14,14,1,0},{0,2,1,6,0},{26,2,1,6,0},{0,0,14,1,1},{0,14,14,1,1},{0,2,1,6,1},{26,2,1,6,1},{0,0,14,1,2},{0,14,14,1,2},{0,2,1,6,2},{26,2,1,6,2},{10,4,4,4,0},{10,6,4,2,1}};

        template<size_t N> std::vector<Spot> FromBlocks(const Blocks (&list)[N])
        {
            std::vector<Spot> s;
            for(const Blocks& b:list)Block(s,b.x,b.y,b.across,b.down,b.z);
            return s;
        }

        std::shared_ptr<const Geometry> GeometryOf(int layout)
        {
            static std::map<int,std::shared_ptr<const Geometry>> made;
            auto& g=made[layout];
            if(!g)
            {
                auto built=std::make_shared<Geometry>();
                built->spots=LayoutSpots(layout);
                const auto& s=built->spots;
                for(size_t i=0;i<s.size();++i)
                    for(size_t j=0;j<s.size();++j)
                    {
                        if(i==j)continue;
                        const int dx=s[j].x-s[i].x,dy=s[j].y-s[i].y;
                        if(s[j].z==s[i].z+1&&std::abs(dx)<2&&std::abs(dy)<2)built->above[i].push_back(static_cast<std::uint8_t>(j));
                        if(s[j].z==s[i].z&&std::abs(dy)<2&&dx==-2)built->left[i].push_back(static_cast<std::uint8_t>(j));
                        if(s[j].z==s[i].z&&std::abs(dy)<2&&dx==2)built->right[i].push_back(static_cast<std::uint8_t>(j));
                    }
                g=built;
            }
            return g;
        }

        bool IsFree(const Geometry& g,const Faces& f,int i)
        {
            if(f[static_cast<size_t>(i)]<0)return false;
            for(const auto j:g.above[static_cast<size_t>(i)])if(f[j]>=0)return false;
            bool leftOpen=true,rightOpen=true;
            for(const auto j:g.left[static_cast<size_t>(i)])if(f[j]>=0){leftOpen=false;break;}
            if(leftOpen)return true;
            for(const auto j:g.right[static_cast<size_t>(i)])if(f[j]>=0){rightOpen=false;break;}
            return rightOpen;
        }

        // The free tiles, and the pairs among them.
        int FreeTiles(const Geometry& g,const Faces& f,std::array<std::uint8_t,TileCount>& out)
        {
            int n=0;
            for(int i=0;i<TileCount;++i)if(IsFree(g,f,i))out[static_cast<size_t>(n++)]=static_cast<std::uint8_t>(i);
            return n;
        }

        void PairsOf(const Geometry& g,const Faces& f,std::vector<Pair>& out)
        {
            out.clear();
            std::array<std::uint8_t,TileCount> free{};
            const int n=FreeTiles(g,f,free);
            for(int i=0;i<n;++i)
                for(int j=i+1;j<n;++j)
                    if(GroupOf(f[free[static_cast<size_t>(i)]])==GroupOf(f[free[static_cast<size_t>(j)]]))out.push_back({free[static_cast<size_t>(i)],free[static_cast<size_t>(j)]});
        }

        // Faces laid on the spots in use, by taking pairs of spots off from the top in a random
        // order: the same order then clears them. pairs: the faces to lay, two by two.
        bool LayAway(const Geometry& g,Faces& f,std::vector<std::array<int,2>> pairs,std::mt19937& random,std::vector<Pair>& order)
        {
            std::shuffle(pairs.begin(),pairs.end(),random);
            Faces used=f;   // any face >= 0: the spot is in use
            Faces result=f;
            order.clear();
            for(const auto& p:pairs)
            {
                std::array<std::uint8_t,TileCount> free{};
                const int n=FreeTiles(g,used,free);
                if(n<2)return false;
                std::uniform_int_distribution<int> any(0,n-1);
                const int a=any(random);
                int b=any(random);
                while(b==a)b=any(random);
                const int ta=free[static_cast<size_t>(a)],tb=free[static_cast<size_t>(b)];
                result[static_cast<size_t>(ta)]=static_cast<std::int8_t>(p[0]);
                result[static_cast<size_t>(tb)]=static_cast<std::int8_t>(p[1]);
                used[static_cast<size_t>(ta)]=used[static_cast<size_t>(tb)]=-1;
                order.push_back({ta,tb});
            }
            f=result;
            return true;
        }

        // The tiles left in pairs: faces of a group shuffled and paired off.
        std::vector<std::array<int,2>> PairUp(std::vector<int> faces,std::mt19937& random)
        {
            std::shuffle(faces.begin(),faces.end(),random);
            std::map<int,std::vector<int>> groups;
            for(int f:faces)groups[GroupOf(f)].push_back(f);
            std::vector<std::array<int,2>> out;
            for(const auto& [group,members]:groups)
                for(size_t i=0;i+1<members.size();i+=2)out.push_back({members[i],members[i+1]});
            return out;
        }

        void Remove(Faces& f,const Pair& p){f[static_cast<size_t>(p.a)]=f[static_cast<size_t>(p.b)]=-1;}

        // Pairs taken at random to the end: how many were taken, and how many tiles stayed. tight:
        // of a few pairs tried, each takes the one that leaves the fewest to take, as one out to
        // stop the other does (else any pair, as one out to clear them all might).
        void Playout(const Geometry& g,Faces f,std::mt19937& random,int& takes,int& left,bool tight=false)
        {
            std::vector<Pair> pairs,after;
            pairs.reserve(64);
            after.reserve(64);
            takes=0;
            while(true)
            {
                PairsOf(g,f,pairs);
                if(pairs.empty())break;
                Pair chosen=pairs[random()%pairs.size()];
                if(tight&&pairs.size()>1)
                {
                    size_t fewest=1000;
                    for(int i=0;i<3;++i)
                    {
                        const Pair tried=pairs[random()%pairs.size()];
                        Faces next=f;
                        Remove(next,tried);
                        PairsOf(g,next,after);
                        if(after.size()<fewest){fewest=after.size();chosen=tried;}
                    }
                }
                Remove(f,chosen);
                ++takes;
            }
            left=0;
            for(const auto v:f)if(v>=0)++left;
        }

        // Exact play to the end: for the one to move, 1 a win (the other is left with tiles and no
        // pair), 0 a draw (all taken), -1 a loss. The position alone decides it, as both may take
        // the same pairs. Gives up past the limits.
        struct Reader
        {
            const Geometry& g;
            std::unordered_map<std::string,signed char> known;
            std::chrono::steady_clock::time_point until;
            size_t nodes=0,maxNodes=0;
            bool gaveUp=false;

            static std::string Key(const Faces& f)
            {
                std::string k(18,'\0');
                for(int i=0;i<TileCount;++i)if(f[static_cast<size_t>(i)]>=0)k[static_cast<size_t>(i/8)]=static_cast<char>(k[static_cast<size_t>(i/8)]|(1<<(i%8)));
                return k;
            }
            int Value(const Faces& f)
            {
                if(gaveUp)return 0;
                std::string key=Key(f);
                if(const auto found=known.find(key);found!=known.end())return found->second;
                if(++nodes>maxNodes||((nodes&1023)==0&&std::chrono::steady_clock::now()>until)){gaveUp=true;return 0;}
                std::vector<Pair> pairs;
                PairsOf(g,f,pairs);
                int best=-1;
                if(pairs.empty())
                {
                    bool any=false;
                    for(const auto v:f)if(v>=0){any=true;break;}
                    best=any?-1:0;
                }
                for(const Pair& p:pairs)
                {
                    Faces after=f;
                    Remove(after,p);
                    const int v=-Value(after);
                    if(gaveUp)return 0;
                    if(v>best)best=v;
                    if(best==1)break;
                }
                known.emplace(std::move(key),static_cast<signed char>(best));
                return best;
            }
        };

        struct Choice {Pair pair;int certain=0;};

        // Her choice. She is out to leave the other with tiles and no pair: all taken is only a
        // draw. What she can read to the end in the time, she reads; among pairs that come to the
        // same, or when it can't be read yet, each is tried out to the end, both sides taking
        // tightly, and she takes the one that stops the other most often, leaning to those that
        // leave few pairs open. care: now and then the next-best pair (never one that throws away
        // what she has read).
        Choice Choose(std::shared_ptr<const Geometry> g,Faces f,std::uint32_t seed,double care,double seconds)
        {
            std::mt19937 random(seed);
            std::vector<Pair> pairs;
            PairsOf(*g,f,pairs);
            Choice out;
            if(pairs.empty())return out;
            std::shuffle(pairs.begin(),pairs.end(),random);
            out.pair=pairs[0];
            const auto start=std::chrono::steady_clock::now();
            const auto after=[](const Faces& from,const Pair& p){Faces next=from;Remove(next,p);return next;};
            int left=0;
            for(const auto v:f)if(v>=0)++left;
            bool read=false;
            if(left<=64)
            {
                Reader reader{*g};
                reader.until=start+std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds*0.6));
                reader.maxNodes=300000;
                std::vector<int> values;
                int best=-1;
                for(const Pair& p:pairs)
                {
                    const int v=-reader.Value(after(f,p));
                    if(reader.gaveUp)break;
                    values.push_back(v);
                    best=std::max(best,v);
                    if(best==1)break;
                }
                if(!reader.gaveUp)
                {
                    read=true;
                    if(best!=0)out.certain=best;
                    if(best==1){out.pair=pairs[values.size()-1];return out;}
                    // A draw at best, or a loss: of those, the one that gives the other most
                    // chances to go wrong.
                    std::vector<Pair> same;
                    for(size_t i=0;i<values.size();++i)if(values[i]==best)same.push_back(pairs[i]);
                    pairs=std::move(same);
                    out.pair=pairs[0];
                }
            }
            if(pairs.size()==1)return out;
            // Playouts, round after round, until the time is up. A stop is worth 5, a draw 2.
            std::vector<int> worth(pairs.size(),0);
            std::vector<size_t> open(pairs.size(),0);
            {
                std::vector<Pair> scratch;
                for(size_t i=0;i<pairs.size();++i){PairsOf(*g,after(f,pairs[i]),scratch);open[i]=scratch.size();}
            }
            int rounds=0;
            const auto until=start+std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
            do
            {
                for(size_t i=0;i<pairs.size();++i)
                {
                    int takes=0,stay=0;
                    Playout(*g,after(f,pairs[i]),random,takes,stay,true);
                    if(stay==0)worth[i]+=2;
                    else if(takes%2==0)worth[i]+=5;   // even: it is the other who has no pair
                }
                ++rounds;
            }
            while(rounds<4000&&std::chrono::steady_clock::now()<until);
            std::vector<double> score(pairs.size());
            for(size_t i=0;i<pairs.size();++i)score[i]=static_cast<double>(worth[i])/rounds-0.04*static_cast<double>(open[i]);
            std::vector<size_t> order(pairs.size());
            for(size_t i=0;i<order.size();++i)order[i]=i;
            std::sort(order.begin(),order.end(),[&](size_t x,size_t y){return score[x]>score[y];});
            std::uniform_real_distribution<double> chance(0,1);
            const size_t pick=!read&&order.size()>=2&&chance(random)<(1-care)*0.7?order[1]:order[0];
            out.pair=pairs[pick];
            return out;
        }
    }

    int LayoutCount(){return 11;}

    const wchar_t* LayoutName(int layout)
    {
        switch(layout)
        {
        case 1:return T(L"ピラミッド",L"Pyramid");
        case 2:return T(L"双子の塔",L"Twin towers");
        case 3:return T(L"城",L"Castle");
        case 4:return T(L"橋",L"Bridge");
        case 5:return T(L"十字",L"Cross");
        case 6:return T(L"菱形",L"Diamond");
        case 7:return T(L"蝶",L"Butterfly");
        case 8:return T(L"山脈",L"Mountains");
        case 9:return T(L"階段",L"Stairs");
        case 10:return T(L"回廊",L"Cloister");
        default:return T(L"亀",L"Turtle");
        }
    }

    const std::vector<Spot>& LayoutSpots(int layout)
    {
        static const std::vector<Spot> layouts[]={Turtle(),Pyramid(),TwinTowers(),FromBlocks(Castle),FromBlocks(Bridge),FromBlocks(Cross),
                                                  FromBlocks(Diamond),FromBlocks(Butterfly),FromBlocks(Mountains),FromBlocks(Stairs),FromBlocks(Cloister)};
        return layouts[std::clamp(layout,0,LayoutCount()-1)];
    }

    Table::~Table(){Join();}

    void Table::Join()
    {
        if(worker.joinable())worker.join();
        thinkingNow=false;
    }

    const std::vector<Spot>& Table::Spots() const {return LayoutSpots(layout);}

    void Table::NewGame(std::uint32_t seed,Mode newMode,int newLayout,int newFirst,double newCare)
    {
        Join();
        random.seed(seed);
        mode=newMode;
        layout=std::clamp(newLayout,0,LayoutCount()-1);
        first=toMove=mode==Mode::Together?newFirst:You;
        care=newCare;
        phase=toMove==You?Phase::YourMove:Phase::HerTurn;
        winner=-1;
        taken=shuffles=hints=0;
        lastWho=-1;
        last={};
        lastFaces={-1,-1};
        history.clear();
        thoughtDone=false;
        next={};
        certain=0;
        ++version;
        geometry=GeometryOf(layout);
        std::vector<int> all;
        for(int kind=0;kind<Seasons;++kind)for(int i=0;i<4;++i)all.push_back(kind);
        for(int face=Seasons;face<FaceCount;++face)all.push_back(face);
        for(int attempt=0;;++attempt)
        {
            faces.fill(0);
            if(LayAway(*geometry,faces,PairUp(all,random),random,solution))break;
            if(attempt>2000)
            {
                // (Never seen.) The tiles anywhere: the game still goes on, and may be shuffled.
                std::shuffle(all.begin(),all.end(),random);
                for(size_t i=0;i<all.size();++i)faces[i]=static_cast<std::int8_t>(all[i]);
                solution.clear();
                break;
            }
        }
        Ended();
    }

    bool Table::Free(int tile) const {return tile>=0&&tile<TileCount&&geometry&&IsFree(*geometry,faces,tile);}

    std::vector<Pair> Table::Pairs() const
    {
        std::vector<Pair> out;
        if(geometry)PairsOf(*geometry,faces,out);
        return out;
    }

    int Table::Left() const
    {
        int n=0;
        for(const auto f:faces)if(f>=0)++n;
        return n;
    }

    bool Table::Stuck() const {return Left()>0&&Pairs().empty();}

    void Table::Ended()
    {
        if(phase==Phase::GameOver)return;
        if(!Pairs().empty())return;
        if(mode==Mode::Together)
        {
            // All taken is a draw; otherwise the one to move cannot take, and the other has won.
            winner=Left()==0?-1:toMove==You?Her:You;
            phase=Phase::GameOver;
        }
        else if(Left()==0)
        {
            winner=You;
            phase=Phase::GameOver;
        }
    }

    bool Table::Take(const Pair& pair)
    {
        if(phase==Phase::GameOver||!pair.Valid()||pair.a==pair.b||!Free(pair.a)||!Free(pair.b))return false;
        const int fa=faces[static_cast<size_t>(pair.a)],fb=faces[static_cast<size_t>(pair.b)];
        if(GroupOf(fa)!=GroupOf(fb))return false;
        history.push_back({pair,{fa,fb}});
        faces[static_cast<size_t>(pair.a)]=faces[static_cast<size_t>(pair.b)]=-1;
        last=pair;
        lastFaces={fa,fb};
        lastWho=toMove;
        ++taken;
        ++version;
        thoughtDone=false;
        next={};
        if(mode==Mode::Together)
        {
            toMove^=1;
            phase=toMove==You?Phase::YourMove:Phase::HerTurn;
        }
        Ended();
        return true;
    }

    bool Table::CanUndo() const {return mode==Mode::Alone&&phase!=Phase::GameOver&&!history.empty();}

    bool Table::Undo()
    {
        if(!CanUndo())return false;
        const auto [pair,was]=history.back();
        history.pop_back();
        faces[static_cast<size_t>(pair.a)]=static_cast<std::int8_t>(was[0]);
        faces[static_cast<size_t>(pair.b)]=static_cast<std::int8_t>(was[1]);
        --taken;
        last=history.empty()?Pair{}:history.back().first;
        lastFaces=history.empty()?std::array<int,2>{-1,-1}:history.back().second;
        ++version;
        return true;
    }

    bool Table::Shuffle()
    {
        if(mode!=Mode::Alone||phase==Phase::GameOver||Left()<2)return false;
        std::vector<int> left;
        for(const auto f:faces)if(f>=0)left.push_back(f);
        Faces laid=faces;
        std::vector<Pair> order;
        for(int attempt=0;attempt<4000;++attempt)
        {
            laid=faces;
            if(LayAway(*geometry,laid,PairUp(left,random),random,order))
            {
                faces=laid;
                solution=order;
                history.clear();
                last={};
                lastFaces={-1,-1};
                ++shuffles;
                ++version;
                return true;
            }
        }
        return false;
    }

    Pair Table::Hint()
    {
        std::vector<Pair> pairs=Pairs();
        if(pairs.empty())return {};
        ++hints;
        std::shuffle(pairs.begin(),pairs.end(),random);
        Pair best=pairs[0];
        double bestScore=1e9;
        for(const Pair& p:pairs)
        {
            Faces after=faces;
            Remove(after,p);
            double sum=0;
            for(int i=0;i<10;++i)
            {
                int takes=0,left=0;
                Playout(*geometry,after,random,takes,left);
                sum+=left;
            }
            if(sum<bestScore){bestScore=sum;best=p;}
        }
        return best;
    }

    void Table::Think()
    {
        if(phase!=Phase::HerTurn||thinkingNow)return;
        Join();
        thoughtDone=false;
        thinkingNow=true;
        const auto g=geometry;
        const Faces f=faces;
        const std::uint32_t seed=random();
        const double careNow=care,seconds=thinkSeconds;
        worker=std::thread([this,g,f,seed,careNow,seconds]{
            const Choice c=Choose(g,f,seed,careNow,seconds);
            next=c.pair;
            if(c.certain!=0)certain=c.certain;
            thoughtDone=true;
        });
    }

    void Table::Step()
    {
        if(phase!=Phase::HerTurn)return;
        if(!thinkingNow&&!thoughtDone)Think();
        Join();
        const Pair chosen=next;
        thoughtDone=false;
        if(!Take(chosen))
        {
            const auto pairs=Pairs();
            if(!pairs.empty())Take(pairs[0]);
        }
    }

    void Table::PlayForYou(bool wild)
    {
        if(phase!=Phase::YourMove)return;
        const auto pairs=Pairs();
        if(pairs.empty())return;
        if(wild){Take(pairs[random()%pairs.size()]);return;}
        const Choice c=Choose(geometry,faces,random(),1.0,0.03);
        Take(c.pair.Valid()?c.pair:pairs[0]);
    }

    std::wstring CheckRules()
    {
        int failures=0;
        std::wstring out;
        auto check=[&](bool ok,const std::wstring& what){if(!ok){++failures;out+=L"FAIL: "+what+L"\n";}else out+=L"ok: "+what+L"\n";};
        for(int layout=0;layout<LayoutCount();++layout)
        {
            const std::wstring name=std::to_wstring(layout);
            const auto& spots=LayoutSpots(layout);
            check(spots.size()==TileCount,L"layout "+name+L" has 144 tiles ("+std::to_wstring(spots.size())+L")");
            bool apart=true;
            for(size_t i=0;i<spots.size();++i)
                for(size_t j=i+1;j<spots.size();++j)
                    if(spots[i].z==spots[j].z&&std::abs(spots[i].x-spots[j].x)<2&&std::abs(spots[i].y-spots[j].y)<2)apart=false;
            check(apart,L"layout "+name+L": no two tiles in one place");
            // Each deal clears by its own order, whatever the layout.
            int cleared=0,wrongFaces=0;
            constexpr int Deals=200;
            for(int seed=1;seed<=Deals;++seed)
            {
                Table t;
                t.NewGame(static_cast<std::uint32_t>(seed*7+layout),Mode::Alone,layout,You,1);
                std::array<int,FaceCount> count{};
                for(int i=0;i<TileCount;++i)++count[static_cast<size_t>(t.FaceAt(i))];
                for(int f=0;f<FaceCount;++f)if(count[static_cast<size_t>(f)]!=(f<Seasons?4:1))++wrongFaces;
                bool all=t.Solution().size()==TileCount/2;
                for(const Pair& p:t.Solution())if(!t.Take(p))all=false;
                if(all&&t.Left()==0&&t.Now()==Phase::GameOver&&t.Winner()==You)++cleared;
            }
            check(wrongFaces==0,L"layout "+name+L": 34 kinds four each, eight single tiles");
            check(cleared==Deals,L"layout "+name+L": every deal clears ("+std::to_wstring(cleared)+L"/200)");
        }
        // Free: only the top of the turtle at first in the middle, and the ends of the rows.
        {
            Table t;
            t.NewGame(3,Mode::Alone,0,You,1);
            const auto& s=t.Spots();
            int top=-1,under=-1,head=-1,tail=-1,row=-1,middle=-1;
            for(int i=0;i<TileCount;++i)
            {
                const Spot& p=s[static_cast<size_t>(i)];
                if(p.z==4)top=i;
                if(p.z==3&&p.x==12&&p.y==6)under=i;
                if(p.z==0&&p.x==0)head=i;
                if(p.z==0&&p.x==28)tail=i;
                if(p.z==0&&p.x==2&&p.y==0)row=i;
                if(p.z==0&&p.x==4&&p.y==0)middle=i;
            }
            check(t.Free(top),L"the top tile is free");
            check(!t.Free(under),L"the one under it is not");
            check(t.Free(head)&&t.Free(tail),L"the head and the tail are free");
            check(t.Free(row),L"a row's end is free");
            check(!t.Free(middle),L"the row's next is not");
        }
        // Seasons with seasons, flowers with flowers; a pair is refused when not free or not alike.
        {
            check(GroupOf(Seasons)==GroupOf(Seasons+3)&&GroupOf(Flowers)==GroupOf(Flowers+3)&&GroupOf(Seasons)!=GroupOf(Flowers)&&GroupOf(0)!=GroupOf(1),L"season goes with season, flower with flower");
            Table t;
            t.NewGame(5,Mode::Alone,0,You,1);
            int kept=t.Version();
            bool refused=!t.Take({0,0});
            std::array<int,TileCount> blocked{};
            int nb=0;
            for(int i=0;i<TileCount;++i)if(!t.Free(i))blocked[static_cast<size_t>(nb++)]=i;
            refused=refused&&!t.Take({blocked[0],blocked[1]});
            check(refused&&t.Version()==kept,L"refused: the same tile twice, tiles not free");
            // Undo puts all back.
            std::vector<int> before;
            for(int i=0;i<TileCount;++i)before.push_back(t.FaceAt(i));
            const Pair p=t.Pairs().at(0);
            t.Take(p);
            check(t.Left()==TileCount-2&&t.CanUndo(),L"a pair taken");
            t.Undo();
            bool same=true;
            for(int i=0;i<TileCount;++i)if(t.FaceAt(i)!=before[static_cast<size_t>(i)])same=false;
            check(same&&t.Left()==TileCount,L"undo puts it back");
            // Shuffling keeps the same tiles and can be cleared.
            for(int i=0;i<30&&!t.Pairs().empty();++i)t.Take(t.Pairs()[0]);
            std::array<int,FaceCount> a{},b{};
            for(int i=0;i<TileCount;++i)if(t.FaceAt(i)>=0)++a[static_cast<size_t>(t.FaceAt(i))];
            const bool shuffled=t.Shuffle();
            for(int i=0;i<TileCount;++i)if(t.FaceAt(i)>=0)++b[static_cast<size_t>(t.FaceAt(i))];
            check(shuffled&&a==b,L"a shuffle keeps the same tiles");
            bool clears=true;
            const auto order=t.Solution();
            for(const Pair& q:order)if(!t.Take(q))clears=false;
            check(clears&&t.Left()==0,L"a shuffled board clears");
            check(t.Now()==Phase::GameOver&&t.Winner()==You,L"cleared is won");
        }
        // Together: turns go back and forth; the one with no pair loses, an empty board included.
        {
            Table t;
            t.NewGame(9,Mode::Together,1,You,1);
            check(t.Now()==Phase::YourMove,L"you first");
            const auto order=t.Solution();
            int mover=You;
            bool turns=true;
            for(const Pair& q:order)
            {
                if(t.Now()==Phase::GameOver)break;
                if(t.ToMove()!=mover||!t.Take(q))turns=false;
                mover^=1;
            }
            check(turns,L"turns alternate");
            check(t.Now()==Phase::GameOver&&t.Left()==0&&t.Winner()==-1,L"all 72 pairs taken is a draw");
            check(!t.CanUndo()&&!t.Shuffle(),L"no undo or shuffle together");
        }
        // Reading: an end she can read to is taken right.
        {
            Table t;
            t.NewGame(11,Mode::Together,0,Her,1);
            t.SetThinkSeconds(0.02);
            for(int guard=0;guard<80&&t.Now()!=Phase::GameOver;++guard)
            {
                if(t.Now()==Phase::HerTurn)t.Step();
                else t.PlayForYou(true);
            }
            check(t.Now()==Phase::GameOver,L"a game together comes to an end");
        }
        // No pair with tiles left: the one to move has lost.
        {
            bool found=false;
            for(std::uint32_t seed=1;seed<40&&!found;++seed)
            {
                Table t;
                t.NewGame(seed,Mode::Together,0,You,1);
                std::mt19937 any(seed);
                while(t.Now()!=Phase::GameOver){const auto pairs=t.Pairs();t.Take(pairs[any()%pairs.size()]);}
                if(t.Left()>0)
                {
                    found=true;
                    check(t.Pairs().empty()&&t.Winner()==(t.LastWho()==You?You:Her),L"stuck with tiles left: the last to take has won");
                }
            }
            check(found,L"random games do get stuck");
        }
        return std::to_wstring(failures)+L" wrong\n"+out;
    }

    std::wstring CheckPlaying(int count,std::uint32_t seed)
    {
        int wrong=0;
        std::wstring out;
        std::mt19937 random(seed);
        // Alone, at random: how far one gets, and how the hint helps.
        for(int layout=0;layout<LayoutCount();++layout)
        {
            int clearedRandom=0,clearedHints=0,leftRandom=0,leftHints=0;
            for(int g=0;g<count;++g)
            {
                Table t;
                t.NewGame(random(),Mode::Alone,layout,You,1);
                while(t.Now()!=Phase::GameOver&&!t.Stuck()){const auto pairs=t.Pairs();t.Take(pairs[random()%pairs.size()]);}
                if(t.Now()==Phase::GameOver)++clearedRandom;
                leftRandom+=t.Left();
                Table h;
                h.NewGame(random(),Mode::Alone,layout,You,1);
                while(h.Now()!=Phase::GameOver&&!h.Stuck())if(!h.Take(h.Hint())){++wrong;break;}
                if(h.Now()==Phase::GameOver)++clearedHints;
                leftHints+=h.Left();
            }
            out+=Fmt(L"alone, layout {0}: at random cleared {1}/{2} (tiles left {3}); by hints cleared {4}/{2} (tiles left {5})\n",
                     {std::to_wstring(layout),std::to_wstring(clearedRandom),std::to_wstring(count),std::to_wstring(leftRandom/std::max(1,count)),std::to_wstring(clearedHints),std::to_wstring(leftHints/std::max(1,count))});
        }
        // Together: her against herself, against one taking at random, and against one who only
        // tries to clear them all (a hint each turn).
        for(const int style:{0,1,2})
            for(int layout=0;layout<LayoutCount();++layout)
            {
                int her=0,you=0,draws=0,firstWins=0,takes=0;
                for(int g=0;g<count;++g)
                {
                    Table t;
                    const int first=g%2?Her:You;
                    t.NewGame(random(),Mode::Together,layout,first,1);
                    t.SetThinkSeconds(style==0?0.02:0.04);
                    int guard=0;
                    while(t.Now()!=Phase::GameOver&&++guard<200)
                    {
                        if(t.Now()==Phase::HerTurn)t.Step();
                        else if(style==2)
                        {
                            const auto pairs=t.Pairs();
                            Pair best=pairs[0];
                            double fewest=1e9;
                            Faces now{};
                            for(int i=0;i<TileCount;++i)now[static_cast<size_t>(i)]=static_cast<std::int8_t>(t.FaceAt(i));
                            const auto g2=GeometryOf(layout);
                            for(const Pair& p:pairs)
                            {
                                Faces after=now;
                                Remove(after,p);
                                double sum=0;
                                for(int k=0;k<4;++k){int a=0,b=0;Playout(*g2,after,random,a,b);sum+=b;}
                                if(sum<fewest){fewest=sum;best=p;}
                            }
                            t.Take(best);
                        }
                        else t.PlayForYou(style==1);
                    }
                    if(t.Now()!=Phase::GameOver){++wrong;continue;}
                    if(t.Winner()==Her)++her;
                    else if(t.Winner()==You)++you;
                    else ++draws;
                    if(t.Winner()==first)++firstWins;
                    takes+=t.Taken();
                }
                out+=Fmt(L"together ({0}), layout {1}: she {2}, the other {3}, draws {4}, first won {5}, pairs taken {6}\n",
                         {style==0?L"against herself":style==1?L"against random":L"against a clearer",std::to_wstring(layout),std::to_wstring(her),std::to_wstring(you),std::to_wstring(draws),std::to_wstring(firstWins),std::to_wstring(takes/std::max(1,count))});
            }
        return std::to_wstring(wrong)+L" wrong\n"+out;
    }
}
