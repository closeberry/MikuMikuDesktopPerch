#include "AirHockey.hpp"
#include <algorithm>
#include <cmath>

namespace MikuMikuDesktopPerch::AirHockey
{
    namespace
    {
        Vec operator+(Vec a,Vec b){return {a.x+b.x,a.y+b.y};}
        Vec operator-(Vec a,Vec b){return {a.x-b.x,a.y-b.y};}
        Vec operator*(Vec a,float k){return {a.x*k,a.y*k};}
        float Dot(Vec a,Vec b){return a.x*b.x+a.y*b.y;}
        float Length(Vec a){return std::sqrt(Dot(a,a));}
        Vec Unit(Vec a,Vec otherwise={0,1}){const float l=Length(a);return l>1e-6f?a*(1/l):otherwise;}
        // +1 for her end (+y), -1 for yours.
        float End(int side){return side==Her?1.0f:-1.0f;}
        // A coordinate folded back into [-limit, limit] as off walls there.
        float Fold(float x,float limit)
        {
            if(limit<=0)return 0;
            const float span=4*limit;
            float u=std::fmod(x+limit,span);
            if(u<0)u+=span;
            return u<=2*limit?u-limit:3*limit-u;
        }
    }

    Game::Game(const Rules& r):rules(r){NewGame();}

    Vec Game::Home(int side) const {return {0,End(side)*(rules.length/2-0.15f)};}

    void Game::NewGame(int first)
    {
        score={};
        winner=-1;
        lastScorer=-1;
        longestRally=0;
        fastest=0;
        for(int side:{You,Her}){mallet[side]=Home(side);malletVelocity[side]={};}
        Serve(first);
    }

    // The puck at rest on side's half, to play after a moment.
    void Game::Serve(int side,bool afterGoal)
    {
        serving=side;
        goalServe=afterGoal;
        puck={0,End(side)*rules.length*0.22f};
        puckVelocity={};
        serveLeft=1.0;
        phase=Phase::Serve;
        rally=0;
        lastHitter=-1;
        still=0;
    }

    void Game::SetPuckSpeed(float speed)
    {
        rules.puckSpeed=std::max(0.5f,speed);
        const float now=Length(puckVelocity);
        if(now>rules.puckSpeed)puckVelocity=puckVelocity*(rules.puckSpeed/now);
    }

    void Game::SetPieceSize(float scale)
    {
        scale=std::clamp(scale,0.5f,2.0f);
        rules.puck=0.035f*scale;
        rules.mallet={0.06f*scale,0.06f*scale};
        rules.goal=0.34f+0.22f*(scale-1);
        // Mallets back inside their halves; a puck in play is put down again where it is not in
        // the way of a grown mallet or wall.
        for(int side:{You,Her})
        {
            const float R=rules.mallet[side],W=rules.width/2-R,L=rules.length/2-R,s=End(side);
            mallet[side]={std::clamp(mallet[side].x,-W,W),std::clamp(mallet[side].y*s,R,L)*s};
        }
        if(phase==Phase::Play)Serve(puck.y>0?Her:You);
    }

    void Game::HoldMallet(int side,Vec at)
    {
        const float R=rules.mallet[side],W=rules.width/2-R,L=rules.length/2-R,s=End(side);
        at.x=std::clamp(at.x,-W,W);
        at.y=std::clamp(at.y*s,R,L)*s;
        held[side]=true;
        heldFrom[side]=mallet[side];
        heldAt[side]=at;
    }

    void Game::Step(double seconds,Vec yourTarget,Vec herTarget)
    {
        const float dt=static_cast<float>(std::clamp(seconds,0.0,0.1));
        clock+=dt;
        if(phase==Phase::Serve&&(serveLeft-=dt)<=0)phase=Phase::Play;
        // Steps short enough that nothing goes through anything: the puck and the mallets move
        // less than half the puck's radius in each.
        float travel=Length(puckVelocity)*dt+std::max(rules.malletSpeed[0],rules.malletSpeed[1])*dt;
        for(int side:{You,Her})if(held[side])travel=std::max(travel,Length(puckVelocity)*dt+Length(heldAt[side]-heldFrom[side]));
        const int steps=std::clamp(static_cast<int>(std::ceil(travel/(0.4f*rules.puck))),1,64);
        const float h=dt/steps;
        for(int side:{You,Her})if(held[side]&&dt>0)malletVelocity[side]=(heldAt[side]-heldFrom[side])*(1/dt);
        for(int i=0;i<steps;++i)
        {
            for(int side:{You,Her})
            {
                if(!held[side]){MoveMallet(side,side==You?yourTarget:herTarget,h);continue;}
                // Where the hand takes it, a step at a time.
                mallet[side]=heldFrom[side]+(heldAt[side]-heldFrom[side])*(static_cast<float>(i+1)/steps);
            }
            if(phase==Phase::Play)MovePuck(h);
            if(phase==Phase::Over)break;
            Collide(You);
            Collide(Her);
        }
        held={};
        if(phase!=Phase::Play)return;
        puckVelocity=puckVelocity*std::exp(-rules.drag*dt);
        const float speed=Length(puckVelocity);
        fastest=std::max(fastest,speed);
        // Off the table, or inside a mallet: what must never be (counted for the tests).
        const float W=rules.width/2,L=rules.length/2;
        if(std::abs(puck.x)>W+1e-3f||(std::abs(puck.y)>L+1e-3f&&std::abs(puck.x)>rules.goal/2))++offTable;
        for(int side:{You,Her})if(Length(puck-mallet[side])<rules.puck+rules.mallet[side]-0.005f)++overlaps;
        // Stopped (or crawling) on her side for long: put in front of you.
        if(speed<0.06f&&puck.y>0)still+=dt;
        else still=0;
        if(still>4){++restarts;Serve(You);}
    }

    void Game::MoveMallet(int side,Vec target,float h)
    {
        const float R=rules.mallet[side],W=rules.width/2-R,L=rules.length/2-R,s=End(side);
        // Kept on its own half, clear of the centre line and inside the walls.
        auto keep=[&](Vec p){
            p.x=std::clamp(p.x,-W,W);
            const float y=std::clamp(p.y*s,R,L);
            p.y=y*s;
            return p;
        };
        target=keep(target);
        const Vec from=mallet[side],to=target-from;
        const float far=Length(to);
        Vec next;
        if(side==You)
        {
            // Yours follows the mouse, only as fast as a hand can take it.
            const float go=std::min(far,rules.malletSpeed[You]*h);
            next=far>1e-6f?from+to*(go/far):from;
        }
        else
        {
            // Hers speeds up and slows down (an arm moving it), to stop at the target.
            const float a=rules.herAcceleration;
            const Vec want=far>1e-6f?to*(std::min(rules.malletSpeed[Her],std::sqrt(2*a*far))/far):Vec{};
            Vec change=want-malletVelocity[side];
            const float most=a*h,c=Length(change);
            if(c>most)change=change*(most/c);
            next=from+(malletVelocity[side]+change)*h;
        }
        next=keep(next);
        malletVelocity[side]=(next-from)*(1/h);
        mallet[side]=next;
    }

    void Game::Bounce(Vec& p,Vec& v,float r,bool goals,float bounce)
    {
        const float W=rules.width/2,L=rules.length/2,mouth=rules.goal/2;
        auto wall=[&](float speed){events.push_back({Event::Wall,std::min(1.0f,speed/rules.puckSpeed),0});};
        if(p.x-r<-W){p.x=-W+r;if(v.x<0){wall(-v.x);v.x=-v.x*bounce;}}
        if(p.x+r>W){p.x=W-r;if(v.x>0){wall(v.x);v.x=-v.x*bounce;}}
        for(int side:{You,Her})
        {
            const float s=End(side);
            if(p.y*s+r<=L)continue;
            if(!goals||std::abs(p.x)>mouth)
            {
                // The end wall beside the mouth (a goal's whole end when goals do not count).
                if(std::abs(p.x)>mouth||!goals)
                {
                    p.y=s*(L-r);
                    if(v.y*s>0){wall(std::abs(v.y));v.y=-v.y*bounce;}
                }
                continue;
            }
            // In the mouth: off its posts.
            for(float px:{-mouth,mouth})
            {
                const Vec post{px,s*L},d=p-post;
                const float l=Length(d);
                if(l>=r||l<1e-6f)continue;
                const Vec n=d*(1/l);
                p=post+n*r;
                const float vn=Dot(v,n);
                if(vn<0){wall(-vn);v=v-n*((1+bounce)*vn);}
            }
        }
    }

    void Game::MovePuck(float h)
    {
        puck=puck+puckVelocity*h;
        Bounce(puck,puckVelocity,rules.puck,true,rules.wallBounce);
        // All the way into a goal.
        if(std::abs(puck.y)>rules.length/2+rules.puck)
        {
            const int scorer=puck.y<0?Her:You;
            ++score[scorer];
            lastScorer=scorer;
            events.push_back({Event::Goal,1,scorer});
            longestRally=std::max(longestRally,rally);
            if(score[scorer]>=rules.toWin)
            {
                winner=scorer;
                phase=Phase::Over;
                puckVelocity={};
            }
            else Serve(scorer==Her?You:Her,true);   // in front of the side that let it in
        }
    }

    void Game::Collide(int side)
    {
        const float R=rules.mallet[side],r=rules.puck;
        const Vec d=puck-mallet[side];
        const float l=Length(d);
        if(l>=R+r)return;
        const Vec n=Unit(d,{0,-End(side)});
        if(phase!=Phase::Play)
        {
            // Before the serve the puck stays: the mallet keeps off it.
            mallet[side]=puck-n*(R+r);
            return;
        }
        puck=mallet[side]+n*(R+r);
        // The mallet is far heavier: the puck bounces off it as off a moving wall.
        const Vec relative=puckVelocity-malletVelocity[side];
        const float vn=Dot(relative,n);
        if(vn<0)
        {
            puckVelocity=puckVelocity-n*((1+rules.malletBounce)*vn);
            const float speed=Length(puckVelocity);
            if(speed>rules.puckSpeed)puckVelocity=puckVelocity*(rules.puckSpeed/speed);
            events.push_back({Event::Hit,std::min(1.0f,-vn/rules.puckSpeed),side});
            if(lastHitter!=side){++rally;lastHitter=side;}
        }
        // Squeezed against a wall: the puck stays on the table, the mallet gives way.
        Vec v=puckVelocity;
        Bounce(puck,v,r,true,rules.wallBounce);
        puckVelocity=v;
        if(Length(puck-mallet[side])<R+r)
        {
            const Vec back=Unit(mallet[side]-puck,{0,End(side)});
            mallet[side]=puck+back*(R+r);
        }
    }

    Brain::Brain(int s,const Style& manner,std::uint32_t seed):side(s),style(manner),random(seed?seed:0x9E3779B9u){}

    float Brain::Random01()
    {
        random^=random<<13;random^=random>>17;random^=random<<5;
        return (random&0xFFFFFF)/16777216.0f;
    }

    Brain::Seen Brain::Late(const Game& game) const
    {
        const double at=game.Clock()-style.reaction;
        for(auto it=seen.rbegin();it!=seen.rend();++it)if(it->at<=at)return *it;
        return seen.empty()?Seen{game.Clock(),game.Puck(),game.PuckVelocity()}:seen.front();
    }

    float Brain::CrossAt(const Game& game,Vec p,Vec v,float y,float& seconds) const
    {
        seconds=-1;
        if(std::abs(v.y)<1e-4f)return p.x;
        const float t=(y-p.y)/v.y;
        if(t<0)return p.x;
        seconds=t;
        return Fold(p.x+v.x*t,game.Rule().width/2-game.Rule().puck);
    }

    Vec Brain::Think(const Game& game,double dt)
    {
        const Rules& rules=game.Rule();
        seen.push_back({game.Clock(),game.Puck(),game.PuckVelocity()});
        while(seen.size()>2&&seen[1].at<game.Clock()-style.reaction-0.5)seen.erase(seen.begin());
        // Worked out as if its own goal were at +y (hers is; yours is turned round).
        const float s=End(side),W=rules.width/2,L=rules.length/2,R=rules.mallet[side],r=rules.puck;
        auto local=[&](Vec p){return Vec{p.x,p.y*s};};
        auto world=[&](Vec p){return Vec{p.x,p.y*s};};
        const Vec me=local(game.Mallet(side));
        const Vec home{0,L-0.15f-0.08f*style.boldness};
        if(game.Now()==Phase::Over){strike=0;return world(home);}
        // The puck as it was a moment ago, carried on to now as it was going then.
        const Seen late=Late(game);
        Vec v=local(late.v),p=local(late.p);
        const float lag=static_cast<float>(game.Clock()-late.at);
        const float raw=p.x+v.x*lag,limit=W-r;
        if(std::abs(static_cast<int>(std::floor((raw+limit)/(2*limit))))%2)v.x=-v.x;   // carried off a side wall
        p={Fold(raw,limit),p.y+v.y*lag};
        if(game.Now()==Phase::Serve&&game.ServingSide()!=side){strike=0;return world(home);}

        if((aimFor-=dt)<=0)
        {
            // Straight at the far goal, or off a side wall into it.
            const float pick=Random01();
            const float err=(Random01()*2-1)*style.aimError;
            aim=pick<0.55f?Vec{err,-L}:Vec{(pick<0.78f?-1.0f:1.0f)*(2*W-2*r)+err,-L};
            aimFor=1.2+Random01();
        }
        const float speed=Length(v);
        const float reachY=L-(L-R)*style.reach;   // the nearest to the middle it goes out to strike
        const bool mine=p.y>std::max(reachY,0.04f),coming=v.y>0.25f;
        // A strike: in behind the puck, then through it toward the aim.
        if(strike>0||(mine&&(speed<1.3f+1.2f*style.boldness||!coming)&&game.Now()!=Phase::Over))
        {
            const Vec way=Unit(aim-p,{0,-1});
            const Vec behind=p-way*(R+r+0.05f);
            if(strike>0)
            {
                strike+=static_cast<float>(dt);
                if(strike>0.3f||!mine){strike=0;}
                return world(strikeFrom+way*0.3f);
            }
            if(me.y<p.y-0.01f)
            {
                // On the wrong side of it (toward the middle): round it, keeping clear.
                const float by=me.x<p.x?-1.0f:1.0f;
                return world({p.x+by*(R+r+0.08f),p.y+R+r+0.06f});
            }
            const Vec gap=me-behind;
            if(Length(gap)<0.04f+0.03f*style.boldness&&game.Now()==Phase::Play)
            {
                strike=static_cast<float>(dt);
                strikeFrom=p;
                return world(p+way*0.3f);
            }
            return world(behind);
        }
        strike=0;
        // Coming at its goal: in front of it, where the puck will cross.
        if(coming)
        {
            const float line=L-0.16f;
            float t=0;
            const float x=CrossAt(game,p,v,line,t);
            if(t>=0)return world({std::clamp(x,-rules.goal*0.8f,rules.goal*0.8f),line});
        }
        // Away: back in front of the goal, on the way from the puck to it (a straight shot
        // comes at the mallet without a move).
        const Vec goal{0,L};
        const Vec guard=goal+Unit(p-goal,{0,-1})*(L-home.y+0.05f);
        return world({std::clamp(guard.x,-rules.goal/2,rules.goal/2),std::min(guard.y,home.y)});
    }
}
