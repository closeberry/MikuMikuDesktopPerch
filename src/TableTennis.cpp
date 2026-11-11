#include "TableTennis.hpp"
#include <algorithm>
#include <cmath>

namespace MikuMikuDesktopPerch::TableTennis
{
    namespace
    {
        constexpr float SwingWindow=0.16f;    // seconds a swing can meet the ball
        constexpr float SwingAgain=0.45f;     // seconds before the next swing
        constexpr float ServeHeight=0.35f;    // the ball held up for a serve
        constexpr float PointPause=1.4f;      // seconds between a point and the next serve

        // The ball falls faster with topspin, slower with backspin.
        float Fall(const Rules& r,float spin){return r.gravity*(1+(spin>=0?0.4f:0.6f)*spin);}
        // The speed along the table kept at a bounce: topspin kicks on, backspin holds up.
        float Grip(float spin){return spin>=0?1+0.12f*spin:1+0.45f*spin;}
        constexpr float CutCurve=6.5f;        // how much a cut bends across (metres a second, each second)
        float SpinOf(Stroke s){return s==Stroke::Drive?0.6f:s==Stroke::Cut?-0.8f:0.15f;}
        float SpeedOf(Stroke s){return s==Stroke::Drive?5.6f:s==Stroke::Cut?3.8f:10.5f;}

        enum class Met {Nothing,TableYou,TableHer,Net,Floor};

        // The ball h seconds on: its flight, the net, the table, the floor. strength: how fast
        // it came at what it met.
        Met Advance(Ball& b,float h,const Rules& r,float& strength)
        {
            const Vec3 before=b.p;
            const float a=Fall(r,b.spin);
            b.p.x+=b.v.x*h+0.5f*b.curve*h*h;
            b.v.x+=b.curve*h;
            b.p.y+=b.v.y*h;
            b.p.z+=b.v.z*h-0.5f*a*h*h;
            b.v.z-=a*h;
            strength=std::sqrt(b.v.x*b.v.x+b.v.y*b.v.y+b.v.z*b.v.z);
            if((before.y<0)!=(b.p.y<0)&&before.y!=b.p.y)
            {
                const float t=(0-before.y)/(b.p.y-before.y);
                const float x=before.x+(b.p.x-before.x)*t,z=before.z+(b.p.z-before.z)*t;
                if(std::abs(x)<=r.width/2+0.15f&&z<r.net+r.ball*0.5f&&z>-0.03f)
                {
                    b.p={x,before.y<0?-0.012f:0.012f,z};
                    b.v.y*=-0.12f;
                    b.v.x*=0.3f;
                    b.v.z*=0.3f;
                    b.curve=0;
                    return Met::Net;
                }
            }
            if(std::abs(b.p.x)<=r.width/2&&std::abs(b.p.y)<=r.length/2&&b.p.z<r.ball&&before.z>=r.ball-0.002f&&b.v.z<0)
            {
                const float in=-b.v.z;
                b.p.z=r.ball;
                if(in<0.25f)
                {
                    // Rolling on the top.
                    b.v.z=0;
                    const float keep=std::max(0.0f,1-2.5f*h);
                    b.v.x*=keep;
                    b.v.y*=keep;
                    return Met::Nothing;
                }
                b.v.z=in*r.bounce;
                const float grip=Grip(b.spin);
                b.v.x*=grip;
                b.v.y*=grip;
                b.spin*=0.4f;
                // A bending ball breaks on the way it bent, and bends less from there.
                b.v.x+=b.curve*0.10f;
                b.curve*=0.5f;
                strength=in;
                return b.p.y<0?Met::TableYou:Met::TableHer;
            }
            if(b.p.z<r.ball-r.height&&b.v.z<0)
            {
                const float in=-b.v.z;
                b.p.z=r.ball-r.height;
                b.v.z=in<0.4f?0:in*0.6f;
                b.v.x*=in<0.4f?std::max(0.0f,1-2.5f*h):0.8f;
                b.v.y*=in<0.4f?std::max(0.0f,1-2.5f*h):0.8f;
                b.spin=0;
                b.curve=0;
                strength=in;
                return in<0.4f?Met::Nothing:Met::Floor;
            }
            return Met::Nothing;
        }

        std::uint32_t Next(std::uint32_t& r)
        {
            r^=r<<13;r^=r>>17;r^=r<<5;
            return r;
        }
    }

    Game::Game(const Rules& rules,std::uint32_t seed):rules(rules),random(seed*2654435761u|1u)
    {
        NewGame(You);
    }

    float Game::Random(float lo,float hi){return lo+(hi-lo)*static_cast<float>(Next(random)%100000)/100000.0f;}

    void Game::NewGame(int first)
    {
        score={};
        winner=-1;
        firstServer=first;
        hits=rally=longestRally=0;
        lastWinner=-1;
        lastReason=Reason::None;
        clock=0;
        racket[You]={0.2f,-rules.length/2-0.3f};
        racket[Her]={-0.2f,rules.length/2+0.3f};
        racketVelocity={};
        racketMove={};
        swings={};
        events.clear();
        StartServe();
    }

    void Game::StartServe()
    {
        // Two serves each; one each from 10 all.
        const int total=score[You]+score[Her],even=2*(rules.toWin-1);
        server=(firstServer+(total<even?total/2:even/2+(total-even)))%2;
        phase=Phase::Serve;
        serveLeft=0.7;
        stage=0;
        lastHitter=-1;
        rally=0;
        marked=false;
        sinceHit=0;
        for(auto& s:swings)s.live=false;
        HoldBall();
    }

    // The server holds the ball up a little before its racket, behind its end of the table.
    void Game::HoldBall()
    {
        const float own=server==You?-1.0f:1.0f,end=rules.length/2+0.03f;
        float y=racket[server].y-own*0.10f;
        y=server==You?std::min(y,-end):std::max(y,end);
        ball.p={std::clamp(racket[server].x,-rules.width/2+0.05f,rules.width/2-0.05f),y,ServeHeight};
        ball.v={};
        ball.spin=0;
        ball.curve=0;
    }

    Vec Game::Keep(int side,Vec at) const
    {
        const float W=rules.width/2+0.7f,back=rules.length/2+0.9f;
        at.x=std::clamp(at.x,-W,W);
        at.y=side==You?std::clamp(at.y,-back,-0.15f):std::clamp(at.y,0.15f,back);
        return at;
    }

    void Game::MoveRacket(int side,Vec target,float dt)
    {
        target=Keep(side,target);
        if(dt<=0)return;
        Vec& p=racket[side];
        if(racketSpeed[side]>=20)
        {
            // As the mouse: there at once; how it moves, evened out over a moment.
            const Vec raw{(target.x-p.x)/dt,(target.y-p.y)/dt};
            const float ease=1-std::exp(-dt/0.05f);
            racketVelocity[side].x+=(raw.x-racketVelocity[side].x)*ease;
            racketVelocity[side].y+=(raw.y-racketVelocity[side].y)*ease;
            p=target;
            return;
        }
        // As an arm: followed as by a spring that just does not overshoot, so each move fades
        // in and out.
        Vec& v=racketMove[side];
        constexpr float W=16,most=70;
        const int steps=std::max(1,static_cast<int>(std::ceil(dt*240)));
        const float h=dt/steps;
        for(int i=0;i<steps;++i)
        {
            Vec acc{W*W*(target.x-p.x)-2*W*v.x,W*W*(target.y-p.y)-2*W*v.y};
            const float al=std::hypot(acc.x,acc.y);
            if(al>most){acc.x*=most/al;acc.y*=most/al;}
            v.x+=acc.x*h;v.y+=acc.y*h;
            const float vl=std::hypot(v.x,v.y);
            if(vl>racketSpeed[side]){v.x*=racketSpeed[side]/vl;v.y*=racketSpeed[side]/vl;}
            p.x+=v.x*h;p.y+=v.y*h;
        }
        p=Keep(side,p);
        racketVelocity[side]=v;
    }

    bool Game::Swing(int side,Stroke stroke)
    {
        auto& s=swings[side];
        if(s.age<SwingAgain||phase==Phase::Over)return false;
        s={};
        s.age=0;
        s.stroke=stroke;
        s.live=true;
        return true;
    }

    bool Game::SwingAimed(int side,Stroke stroke,Vec target,float error)
    {
        if(!Swing(side,stroke))return false;
        swings[side].aimed=true;
        swings[side].target=target;
        swings[side].error=error;
        return true;
    }

    void Game::Step(double dt,Vec yourTarget,Vec herTarget)
    {
        const float step=static_cast<float>(dt);
        clock+=dt;
        MoveRacket(You,yourTarget,step);
        MoveRacket(Her,herTarget,step);
        for(auto& s:swings)s.age+=step;
        if(phase==Phase::Serve)
        {
            serveLeft-=dt;
            HoldBall();
            if(swings[server].live&&serveLeft<=0)Serve(server);
            for(auto& s:swings)if(s.age>SwingWindow)s.live=false;
            return;
        }
        const int n=std::max(1,static_cast<int>(std::ceil(dt*480)));
        const float h=step/n;
        for(int i=0;i<n;++i)
        {
            MoveBall(h);
            if(phase!=Phase::Rally)continue;
            sinceHit+=h;
            Meet(You,h);
            Meet(Her,h);
        }
        for(auto& s:swings)if(s.age>SwingWindow&&!s.within)s.live=false;
        if(phase==Phase::Rally&&sinceHit>15)
        {
            ++wrong;   // a ball that never came down
            Award(1-lastHitter,Reason::Out);
        }
        if(phase==Phase::Point)
        {
            pointLeft-=dt;
            if(pointLeft<=0)
            {
                if(winner>=0)phase=Phase::Over;
                else StartServe();
            }
        }
    }

    void Game::MoveBall(float h)
    {
        float strength=0;
        const Met met=Advance(ball,h,rules,strength);
        if(!std::isfinite(ball.p.x)||!std::isfinite(ball.p.y)||!std::isfinite(ball.p.z))
        {
            ++wrong;
            ball={};
            if(phase==Phase::Rally)Award(1-lastHitter,Reason::Out);
            return;
        }
        const bool rallying=phase==Phase::Rally;
        const int striker=lastHitter,other=1-lastHitter;
        switch(met)
        {
        case Met::Net:
            events.push_back({Event::Net,striker,lastStroke,strength});
            if(rallying)Award(other,Reason::Net);
            break;
        case Met::TableYou:
        case Met::TableHer:
        {
            const int side=met==Met::TableYou?You:Her;
            events.push_back({Event::Bounce,side,lastStroke,strength});
            if(!rallying)break;
            if(stage==0)
            {
                if(side==striker)stage=1;
                else Award(other,Reason::Out);   // a serve straight over
            }
            else if(stage==1)
            {
                if(side==other)stage=2;
                else Award(other,Reason::Out);   // down on the striker's own half
            }
            else Award(striker,Reason::Missed);
            break;
        }
        case Met::Floor:
            events.push_back({Event::Floor,0,lastStroke,strength});
            if(rallying)Award(stage==2?striker:other,stage==2?Reason::Missed:Reason::Out);
            break;
        default:
            if(rallying&&(std::abs(ball.p.y)>7||std::abs(ball.p.x)>5))Award(stage==2?striker:other,stage==2?Reason::Missed:Reason::Out);
            break;
        }
    }

    // A swing under way and the ball: struck as it passes nearest the racket (across the table;
    // the racket's height goes with the ball), once it has come down on the swinger's half.
    void Game::Meet(int side,float)
    {
        auto& s=swings[side];
        if(!s.live)return;
        if(lastHitter==side||stage!=2)return;
        // The swinging racket goes across to the ball by itself, up to so far.
        const float across=ball.p.x-racket[side].x,shift=std::clamp(across,-rules.assist[side],rules.assist[side]);
        const float d=std::hypot(across-shift,ball.p.y-racket[side].y);
        bool can=d<rules.reach[side]&&ball.p.z>-0.5f&&ball.p.z<1.3f;
        if(can&&!able[side])
        {
            // The racket is not there: the swing goes through the air.
            if(!s.counted){s.counted=true;++missed[side];}
            can=false;
        }
        if(can&&d<=s.nearest)
        {
            s.nearest=d;
            s.within=true;
            s.shift=shift;
        }
        else if(s.within)Strike(side,1-s.nearest/rules.reach[side]);
        if(s.live&&s.age>SwingWindow)
        {
            if(s.within)Strike(side,1-s.nearest/rules.reach[side]);
            s.live=false;
        }
    }

    // The ball struck by side's racket: quality 1 in the racket's middle, 0 at its edge.
    void Game::Strike(int side,float quality)
    {
        auto& s=swings[side];
        const float way=side==You?1.0f:-1.0f,loose=1-std::clamp(quality,0.0f,1.0f);
        Vec target;
        float speed=SpeedOf(s.stroke),bend=0;
        if(s.aimed)
        {
            if(s.stroke==Stroke::Cut)bend=Random(0,1)<0.5f?-CutCurve:CutCurve;
            const float e=s.error*(0.6f+0.8f*loose);
            target={s.target.x+Random(-e,e),s.target.y+Random(-e,e)};
        }
        else
        {
            // By the racket: a ball met right of its middle goes right, and the ball goes the
            // way the racket is moving (across: its direction; forward: deeper and faster).
            const Vec v=racketVelocity[side];
            const float forward=way*v.y,off=ball.p.x-(racket[side].x+s.shift);
            float across=ball.p.x*0.3f+std::clamp(v.x*0.10f,-0.6f,0.6f)+off*1.2f,depth;
            if(s.stroke==Stroke::Drive)
            {
                depth=0.85f+std::clamp(forward*0.08f,-0.3f,0.35f);
                speed+=std::clamp(forward*0.4f,-0.6f,1.8f);
            }
            else if(s.stroke==Stroke::Cut)
            {
                depth=0.6f+std::clamp(forward*0.06f,-0.2f,0.3f);
                // A cut bends the way the racket was drawn across the ball (still: in toward the middle).
                bend=(std::abs(v.x)>0.4f?v.x>0:ball.p.x<0)?CutCurve:-CutCurve;
            }
            else depth=0.9f+std::clamp(forward*0.05f,-0.15f,0.25f)+Random(-0.15f,0.15f);
            across+=Random(-1,1)*0.22f*loose;
            depth+=Random(-1,1)*0.22f*loose;
            target={std::clamp(across,-rules.width/2-0.2f,rules.width/2+0.2f),way*std::max(depth,0.2f)};
        }
        const Stroke stroke=s.stroke;
        s.live=false;
        s.within=false;
        Launch(side,stroke,target,speed,bend);
        lastHitter=side;
        lastStroke=stroke;
        stage=1;
        ++rally;
        ++hits;
        sinceHit=0;
        events.push_back({Event::Hit,side,stroke,speed});
        SetMark();
    }

    // The ball sent from where it is to come down at target: as flat as clears the net, a
    // smash only a little slower than it is (a low ball smashed goes into the net).
    void Game::Launch(int,Stroke stroke,Vec target,float speed,float curve)
    {
        const float spin=SpinOf(stroke),a=Fall(rules,spin);
        const float dx=target.x-ball.p.x,dy=target.y-ball.p.y,d=std::max(std::hypot(dx,dy),0.3f);
        float T=d/speed;
        const float limit=T*(stroke==Stroke::Smash?1.12f:3.0f);
        const float part=std::abs(dy)>1e-3f?std::clamp((0-ball.p.y)/dy,0.0f,1.0f):0.5f;
        float up=0;
        for(;;)
        {
            up=(rules.ball-ball.p.z)/T+a*T/2;
            const float t=T*part,over=ball.p.z+up*t-a*t*t/2;
            if(over>=rules.net+rules.ball+0.03f||T>=limit)break;
            T=std::min(T*1.04f,limit);
        }
        // Bending on its way, it still comes down at the target.
        ball.v={(dx-0.5f*curve*T*T)/T,dy/T,up};
        ball.spin=spin;
        ball.curve=curve;
    }

    // The serve: down on the server's own half first, then over the net to the target.
    void Game::Serve(int side)
    {
        auto& s=swings[side];
        const float way=side==You?1.0f:-1.0f;
        const Stroke stroke=s.stroke==Stroke::Cut?Stroke::Cut:Stroke::Drive;
        Vec target;
        if(s.aimed)target={s.target.x+Random(-s.error,s.error),s.target.y+Random(-s.error,s.error)};
        else
        {
            const Vec v=racketVelocity[side];
            target={ball.p.x*0.3f+std::clamp(v.x*0.10f,-0.5f,0.5f)+Random(-0.06f,0.06f),way*(0.8f+std::clamp(way*v.y*0.06f,-0.2f,0.3f))};
        }
        target.x=std::clamp(target.x,-rules.width/2+0.06f,rules.width/2-0.06f);
        target.y=way*std::clamp(way*target.y,0.45f,rules.length/2-0.12f);
        const float spin=stroke==Stroke::Cut?-0.5f:0.3f,a=Fall(rules,spin),a1=Fall(rules,spin*0.4f),grip=Grip(spin);
        const Vec3 from=ball.p;
        const float y1=-way*rules.length*0.25f,part=(y1-from.y)/(target.y-from.y);
        const Vec first{from.x+(target.x-from.x)*part,y1};
        const float d1=std::hypot(first.x-from.x,first.y-from.y),want=std::hypot(target.x-first.x,target.y-first.y);
        const float toNet=want*(0-first.y)/(target.y-first.y);
        float best=1e9f,bestT=0.25f;
        for(float T=0.10f;T<=0.8f;T+=0.004f)
        {
            const float up0=(rules.ball-from.z)/T+a*T/2,in=a*T-up0;
            if(in<0.3f)continue;
            const float up=in*rules.bounce,along=d1/T*grip,t=toNet/along;
            if(rules.ball+up*t-a1*t*t/2<rules.net+rules.ball+0.03f)continue;
            const float off=std::abs(along*2*up/a1-want);
            if(off<best){best=off;bestT=T;}
        }
        ball.v={(first.x-from.x)/bestT,(first.y-from.y)/bestT,(rules.ball-from.z)/bestT+a*bestT/2};
        ball.spin=spin;
        ball.curve=0;
        s.live=false;
        phase=Phase::Rally;
        lastHitter=side;
        lastStroke=stroke;
        stage=0;
        rally=1;
        ++hits;
        sinceHit=0;
        events.push_back({Event::Hit,side,stroke,std::hypot(ball.v.x,ball.v.y)});
        SetMark();
    }

    void Game::Award(int side,Reason reason)
    {
        if(phase!=Phase::Rally)return;
        ++score[side];
        lastWinner=side;
        lastReason=reason;
        longestRally=std::max(longestRally,rally);
        phase=Phase::Point;
        pointLeft=PointPause;
        marked=false;
        if(score[side]>=rules.toWin&&score[side]-score[1-side]>=2)winner=side;
        Event e{Event::Point,side,lastStroke,0,reason};
        e.rally=rally;
        events.push_back(e);
    }

    Path Game::Predict() const
    {
        Path path;
        if(phase!=Phase::Rally||lastHitter<0)return path;
        Ball b=ball;
        const int striker=lastHitter,other=1-lastHitter;
        int at=stage;
        constexpr float h=1/480.0f;
        float t=0,strength=0;
        if(at==2)
        {
            path.lands=true;
            path.bounce={b.p.x,b.p.y};
        }
        for(int i=0;i<480*3&&at<2;++i)
        {
            const Met met=Advance(b,h,rules,strength);
            t+=h;
            if(met==Met::Nothing)continue;
            if(met==Met::Net||met==Met::Floor)return path;
            const int side=met==Met::TableYou?You:Her;
            if(at==0&&side==striker)at=1;
            else if(at==1&&side==other)
            {
                at=2;
                path.lands=true;
                path.bounce={b.p.x,b.p.y};
                path.bounceIn=t;
            }
            else return path;
        }
        if(at<2)return path;
        for(int i=1;i<=480*2;++i)
        {
            const Met met=Advance(b,h,rules,strength);
            if(met!=Met::Nothing)break;
            if(i%4==0)path.after.push_back(b);
        }
        return path;
    }

    void Game::SetMark()
    {
        const Path path=Predict();
        marked=path.lands;
        mark=path.bounce;
    }

    Style LevelStyle(int level)
    {
        switch(level)
        {
        case 1:return {0.38f,2.4f,0.32f,0.22f,0.15f,0.15f};
        case 3:return {0.20f,4.8f,0.13f,0.05f,0.8f,0.8f};
        default:return {0.28f,3.4f,0.22f,0.12f,0.45f,0.45f};
        }
    }

    Brain::Brain(int side,const Style& style,std::uint32_t seed):side(side),style(style),random(seed*2246822519u|1u){}

    float Brain::Random(float lo,float hi){return lo+(hi-lo)*static_cast<float>(Next(random)%100000)/100000.0f;}

    Vec Brain::Think(Game& game,double)
    {
        const auto& r=game.Rule();
        const float own=side==Her?1.0f:-1.0f,L=r.length/2;
        const double now=game.Clock();
        home={0,own*(L+0.40f)};
        auto swing=[&]{
            if(style.plain)game.Swing(side,stroke);
            else game.SwingAimed(side,stroke,aim,style.error*(wild?3.5f:1.0f));
        };
        if(game.Now()==Phase::Serve)
        {
            planned=false;
            seenHits=game.Hits();
            if(game.Server()!=side)
            {
                serveAt=-1;
                height=0.2f;
                return home;
            }
            if(serveAt<0)
            {
                serveAt=now+std::max(game.ServeLeft(),0.0)+Random(0.5f,1.1f);
                spot={Random(-0.45f,0.45f),own*(L+0.22f)};
                aim={Random(-0.5f,0.5f),-own*Random(0.6f,1.05f)};
                stroke=Random(0,1)<0.3f?Stroke::Cut:Stroke::Drive;
            }
            const Vec at=game.Racket(side);
            if(now>=serveAt&&std::hypot(at.x-spot.x,at.y-spot.y)<0.15f)swing();
            height=0.3f;
            return spot;
        }
        serveAt=-1;
        if(game.Now()!=Phase::Rally||game.LastHitter()==side)
        {
            planned=false;
            seenHits=game.Hits();
            height=0.2f;
            return home;
        }
        if(game.Hits()!=seenHits)
        {
            // The other's stroke: seen a moment late.
            seenHits=game.Hits();
            seeAt=now+style.reaction;
            planned=comes=swung=false;
        }
        if(!planned&&now>=seeAt)
        {
            planned=true;
            const Path path=game.Predict();
            comes=false;
            // Where to meet it: near its own end of the table, at a good height.
            float best=-1e9f;
            int chosen=-1;
            for(size_t i=0;i<path.after.size()&&path.lands;++i)
            {
                const Ball& b=path.after[i];
                const float back=own*b.p.y;
                if(b.p.z<0.05f||back<0.25f||back>L+0.75f||std::abs(b.p.x)>r.width/2+0.6f)continue;
                const float worth=-std::abs(back-(L+0.38f))-1.2f*std::abs(b.p.z-0.24f);   // (well behind the table's end: there is room to stand beside it)
                if(worth>best){best=worth;chosen=static_cast<int>(i);}
            }
            if(chosen>=0)
            {
                const Ball& b=path.after[chosen];
                comes=true;
                hitAt=now+path.bounceIn+(chosen+1)/120.0;
                spot={b.p.x,b.p.y};
                height=b.p.z;
                if(b.p.z>0.30f&&Random(0,1)<style.smash)stroke=Stroke::Smash;
                else if((b.spin<-0.15f||b.p.z<0.14f)&&Random(0,1)<0.7f)stroke=Stroke::Cut;
                else stroke=Random(0,1)<0.12f?Stroke::Cut:Stroke::Drive;
                const float theirs=game.Racket(1-side).x;
                const float across=Random(0,1)<style.placing?(theirs>0?-1.0f:1.0f)*Random(0.3f,0.62f):Random(-0.55f,0.55f);
                const float depth=stroke==Stroke::Drive?Random(0.75f,1.15f):stroke==Stroke::Cut?Random(0.45f,0.9f):Random(0.8f,1.12f);
                aim={style.across<5?style.across:across,-own*depth};
                // Now and then a slip: late, or its aim well astray.
                wild=false;
                if(Random(0,1)<style.slip)
                {
                    if(Random(0,1)<0.5f)hitAt+=0.24;
                    else wild=true;
                }
            }
        }
        if(planned&&comes)
        {
            if(!swung&&now>=hitAt-0.05)
            {
                swung=true;
                swing();
            }
            return spot;
        }
        height=0.2f;
        return home;
    }

    std::wstring CheckPlaying(int games,std::uint32_t seed)
    {
        std::wstring lines;
        int wrong=0;
        for(int level=1;level<=3;++level)
        {
            int wins[2]={},points=0,strokes=0,longest=0,ended=0;
            int why[2][4]={};   // by who got the point, the reason
            double seconds=0;
            for(int i=0;i<games;++i)
            {
                Game game({},seed*7919u+level*131u+i);
                game.NewGame(i%2);
                const Style hers=LevelStyle(level);
                game.SetRacketSpeed(Her,hers.speed);
                game.SetRacketSpeed(You,8.0f);
                Style person{0.28f,8.0f,0.2f,0.08f,0.4f,0.4f,true};
                Brain you(You,person,seed*104729u+i*2+1),her(Her,hers,seed*15485863u+i*2+2);
                const double dt=1/120.0;
                while(game.Now()!=Phase::Over&&game.Clock()<1800)
                {
                    const Vec a=you.Think(game,dt),b=her.Think(game,dt);
                    game.Step(dt,a,b);
                    for(const Event& e:game.TakeEvents())
                        if(e.kind==Event::Point)
                        {
                            ++points;
                            strokes+=e.rally;
                            ++why[e.side][static_cast<int>(e.reason)];
                        }
                }
                const bool over=game.Now()==Phase::Over;
                if(over){++ended;++wins[game.Winner()];}
                wrong+=game.Wrong()+(over?0:1);
                longest=std::max(longest,game.LongestRally());
                seconds+=game.Clock();
            }
            wchar_t line[400];
            swprintf_s(line,L"level %d: %d games (%d ended): her %d wins, you %d; %.1f strokes a point, longest rally %d, %.0f s a game\n"
                            L"  her points: you missed %d, your net %d, your out %d; your points: she missed %d, her net %d, her out %d\n",
                       level,games,ended,wins[Her],wins[You],points?static_cast<double>(strokes)/points:0.0,longest,games?seconds/games:0.0,
                       why[Her][3],why[Her][1],why[Her][2],why[You][3],why[You][1],why[You][2]);
            lines+=line;
        }
        return std::to_wstring(wrong)+L" wrong\n"+lines;
    }
}
