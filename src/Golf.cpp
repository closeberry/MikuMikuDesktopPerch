#define NOMINMAX
#include "Golf.hpp"
#include <windows.h>
#include "Text.hpp"
#include <algorithm>
#include <cmath>
#include <cwchar>

namespace MikuMikuDesktopPerch::Golf
{
    namespace
    {
        constexpr float Pi=3.14159265f,Deg=Pi/180,G=9.81f;
        // The air on the ball: half the air's density times its cross-section, over its mass.
        constexpr float AirK=0.5f*1.2f*Pi*BallRadius*BallRadius/0.04593f;

        float Smooth(float t){t=std::clamp(t,0.0f,1.0f);return t*t*(3-2*t);}

        std::uint32_t Hash(std::int32_t x,std::int32_t z,std::uint32_t seed)
        {
            std::uint32_t h=static_cast<std::uint32_t>(x)*374761393u+static_cast<std::uint32_t>(z)*668265263u+seed*2246822519u;
            h=(h^(h>>13))*1274126177u;
            return h^(h>>16);
        }
        // Smooth value noise, 0 to 1.
        float Noise(float x,float z,std::uint32_t seed)
        {
            const float fx=std::floor(x),fz=std::floor(z);
            const std::int32_t ix=static_cast<std::int32_t>(fx),iz=static_cast<std::int32_t>(fz);
            const float u=x-fx,v=z-fz;
            const float su=u*u*(3-2*u),sv=v*v*(3-2*v);
            auto at=[&](std::int32_t a,std::int32_t b){return (Hash(a,b,seed)&0xFFFF)/65535.0f;};
            const float a=at(ix,iz),b=at(ix+1,iz),c=at(ix,iz+1),d=at(ix+1,iz+1);
            return (a+(b-a)*su)+((c+(d-c)*su)-(a+(b-a)*su))*sv;
        }

        // How the ground takes the ball: how much of its speed into the ground comes back, how
        // much of its speed along the ground a bounce takes, how hard it is to roll on.
        struct Ground {float bounce,grab,roll;};
        Ground GroundOf(Lie lie)
        {
            switch(lie)
            {
            case Lie::Green:return {0.25f,0.26f,0.065f};
            case Lie::Rough:return {0.18f,0.55f,0.30f};
            case Lie::Bunker:return {0.04f,0.90f,1.20f};
            default:return {0.30f,0.32f,0.13f};   // fairway, tee
            }
        }

        constexpr Club Clubs[ClubCount]={
            {L"1W",L"1W",10.5f,69.0f,2900},
            {L"3W",L"3W",15.0f,62.0f,3700},
            {L"5I",L"5I",22.0f,53.0f,5000},
            {L"7I",L"7I",30.0f,47.0f,6600},
            {L"9I",L"9I",39.0f,41.0f,8000},
            {L"PW",L"PW",45.0f,37.0f,8800},
            {L"SW",L"SW",56.0f,30.0f,9600},
            {L"PT",L"PT",2.0f,0,0}};

        // Lie: speed and spin the ball leaves with, from there, with a club.
        void LieEffect(Lie lie,int club,float& speed,float& spin)
        {
            speed=spin=1;
            if(lie==Lie::Rough){speed=0.88f;spin=0.5f;}
            else if(lie==Lie::Bunker){if(club==Wedge){speed=0.84f;spin=0.75f;}else{speed=0.6f;spin=0.5f;}}
            if(club==Driver&&lie!=Lie::Tee)speed*=0.93f;
        }
    }

    float Dot(const V3& a,const V3& b){return a.x*b.x+a.y*b.y+a.z*b.z;}
    V3 Cross(const V3& a,const V3& b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
    float Length(const V3& a){return std::sqrt(Dot(a,a));}
    V3 Normalized(const V3& a){const float l=Length(a);return l>1e-9f?a*(1/l):V3{};}
    float Flat(const V3& a,const V3& b){return std::hypot(a.x-b.x,a.z-b.z);}

    const wchar_t* LieName(Lie lie)
    {
        switch(lie)
        {
        case Lie::Tee:return T(L"ティー",L"Tee");
        case Lie::Fairway:return T(L"フェアウェイ",L"Fairway");
        case Lie::Rough:return T(L"ラフ",L"Rough");
        case Lie::Bunker:return T(L"バンカー",L"Bunker");
        case Lie::Green:return T(L"グリーン",L"Green");
        case Lie::Water:return T(L"池",L"Water");
        default:return T(L"OB",L"Out of bounds");
        }
    }

    const Club& ClubOf(int club){return Clubs[std::clamp(club,0,ClubCount-1)];}
    std::wstring ClubName(int club)
    {
        switch(club)
        {
        case 0:return T(L"ドライバー",L"Driver");
        case 1:return T(L"3番ウッド",L"3 wood");
        case 2:return T(L"5番アイアン",L"5 iron");
        case 3:return T(L"7番アイアン",L"7 iron");
        case 4:return T(L"9番アイアン",L"9 iron");
        case 5:return T(L"ピッチングウェッジ",L"Pitching wedge");
        case 6:return T(L"サンドウェッジ",L"Sand wedge");
        default:return T(L"パター",L"Putter");
        }
    }

    float Course::Ellipse::At(float px,float pz) const
    {
        if(rx<=0||rz<=0)return 1e9f;
        const float dx=px-x,dz=pz-z,c=std::cos(turn),s=std::sin(turn);
        const float a=(dx*c-dz*s)/rx,b=(dx*s+dz*c)/rz;
        return std::sqrt(a*a+b*b);
    }

    Course::Course(std::uint32_t s,int p):seed(s),par(std::clamp(p,3,5))
    {
        std::mt19937 r(s*2654435761u+17u);
        auto U=[&](float a,float b){return std::uniform_real_distribution<float>(a,b)(r);};
        length=par==3?U(140,180):par==5?U(430,470):U(320,370);
        bend=par==3?U(-5,5):U(-26,26);
        bendFrom=length*U(0.35f,0.48f);
        bendTo=length*U(0.62f,0.76f);
        fairwayFrom=par==3?length-35:U(70,95);
        fairwayWidth=U(26,33);
        for(float& h:hills)h=U(0,2*Pi);
        green.x=LineX(length);green.z=length;
        green.rx=U(11,14);green.rz=U(13,16);green.turn=U(-0.4f,0.4f);
        for(int i=0;i<50;++i)
        {
            const float a=U(0,2*Pi),d=std::sqrt(U(0,1))*0.62f;
            pin={green.x+d*green.rx*std::cos(a),0,green.z+d*green.rz*std::sin(a)};
            if(green.At(pin.x,pin.z)<0.65f)break;
        }
        {
            const float a=U(0,2*Pi),slope=U(0.008f,0.02f);
            greenTilt[0]=slope*std::cos(a);greenTilt[1]=slope*std::sin(a);
        }
        // Bunkers round the green (front left, front right, or beside it) and one at the drive's landing.
        const int round=par==3?3:2;
        const float start=U(0,2*Pi);
        for(int i=0;i<round;++i)
        {
            const float a=start+i*2*Pi/round+U(-0.3f,0.3f);
            if(std::sin(a)>0.75f)continue;   // not behind the green's far side mostly
            Ellipse b;
            const float rx=green.rx*std::abs(std::cos(a))+green.rz*std::abs(std::sin(a));
            b.x=green.x+std::cos(a)*(rx+U(3.5f,5.0f));
            b.z=green.z+std::sin(a)*(rx+U(3.5f,5.0f));
            b.rx=U(4.5f,6.5f);b.rz=U(3.0f,4.5f);b.turn=a+Pi/2;
            bunkers.push_back(b);
        }
        if(par>=4)
            for(float at:par==5?std::initializer_list<float>{U(205,235),U(310,340)}:std::initializer_list<float>{U(200,232)})
            {
                Ellipse b;
                const float side=U(0,1)<0.5f?-1.0f:1.0f;
                b.z=at;b.x=LineX(at)+side*(fairwayWidth/2+U(-3,3));
                b.rx=U(5,7);b.rz=U(9,13);b.turn=U(-0.2f,0.2f);
                bunkers.push_back(b);
            }
        // A pond: before the green of a short hole, beside the fairway of a longer one.
        if(U(0,1)<0.6f)
        {
            if(par==3){pond.z=length-green.rz-U(14,20);pond.x=LineX(pond.z)+U(-6,6);pond.rx=U(16,22);pond.rz=U(7,10);}
            else
            {
                const float side=U(0,1)<0.5f?-1.0f:1.0f;
                pond.z=U(140,length-110);pond.x=LineX(pond.z)+side*(fairwayWidth/2+U(8,14));
                pond.rx=U(11,16);pond.rz=U(18,30);
            }
            pond.turn=U(-0.3f,0.3f);
            waterLevel=BaseHeight(pond.x,pond.z)-0.9f;
        }
        minX=std::min(0.0f,bend)-62;
        maxX=std::max(0.0f,bend)+62;
        // Trees along the sides, in the rough beyond the fairway (none on the greens, bunkers, water).
        for(float z=-14;z<length+34;z+=U(5,9))
            for(float side:{-1.0f,1.0f})
            {
                const float x=LineX(z)+side*U(fairwayWidth/2+15,52);
                if(green.At(x,z)<1.8f||pond.At(x,z)<1.4f||(std::abs(x)<14&&z<16))continue;
                bool clear=true;
                for(const Ellipse& b:bunkers)if(b.At(x,z)<1.6f)clear=false;
                if(!clear)continue;
                trees.push_back({x,z,U(8,14),U(2.6f,4.2f),U(0,1)<0.5f?0:1});
            }
    }

    float Course::LineX(float z) const {return bend*Smooth((z-bendFrom)/std::max(1.0f,bendTo-bendFrom));}

    float Course::BaseHeight(float x,float z) const
    {
        float h=2.0f*std::sin(0.013f*x+hills[0])*std::sin(0.009f*z+hills[1])+1.2f*std::sin(0.021f*z+hills[2]);
        h+=2.2f*(Noise(x/45+hills[3],z/45,seed)-0.5f)+0.5f*(Noise(x/14,z/14+hills[4],seed+7)-0.5f);
        // The rough rises a little toward the sides (the hole lies in a shallow valley).
        const float off=std::abs(x-LineX(z));
        h+=0.0016f*off*off*Smooth((off-fairwayWidth*0.5f)/20);
        return h;
    }

    float Course::Height(float x,float z) const
    {
        float h=BaseHeight(x,z);
        // The tee box: flat, a little raised.
        {
            const float w=(1-Smooth((std::abs(x)-5)/3))*(1-Smooth((std::abs(z)-7)/3));
            if(w>0)h+=(BaseHeight(0,0)+0.35f-h)*w;
        }
        // The green: a tilted plane with gentle swells, raised a little over the ground round it.
        if(const float e=green.At(x,z);e<1.6f)
        {
            const float plane=BaseHeight(green.x,green.z)+0.5f+greenTilt[0]*(x-green.x)+greenTilt[1]*(z-green.z)+
                              0.14f*(Noise(x/7,z/7,seed+11)-0.5f);
            h+=(plane-h)*(1-Smooth((e-1.0f)/0.55f));
        }
        for(const Ellipse& b:bunkers)
            if(const float e=b.At(x,z);e<1.3f)
                h-=e<1?0.55f*(1-e*e):0.0f;
        if(pond.rx>0)
            if(const float e=pond.At(x,z);e<1.6f)
            {
                const float bank=waterLevel+0.25f+3.0f*(e-1);
                if(e<1)h=std::min(h,waterLevel-1.2f*(1-e)+0.05f);
                else h=std::min(h,std::max(bank,waterLevel+0.25f))*(1-Smooth((e-1.3f)/0.3f))+h*Smooth((e-1.3f)/0.3f);
            }
        return h;
    }

    V3 Course::Normal(float x,float z) const
    {
        constexpr float d=0.25f;
        const float hx=Height(x+d,z)-Height(x-d,z),hz=Height(x,z+d)-Height(x,z-d);
        return Normalized({-hx,2*d,-hz});
    }

    Lie Course::LieAt(float x,float z) const
    {
        if(z<-15||z>length+35||std::abs(x-LineX(z))>54)return Lie::Out;
        if(pond.rx>0&&pond.At(x,z)<1.05f&&Height(x,z)<waterLevel)return Lie::Water;
        if(green.At(x,z)<=1)return Lie::Green;
        for(const Ellipse& b:bunkers)if(b.At(x,z)<=1)return Lie::Bunker;
        if(std::abs(x)<=5&&std::abs(z)<=7)return Lie::Tee;
        if(green.At(x,z)<=1.28f)return Lie::Fairway;   // the fringe
        const float half=fairwayWidth*0.5f*(1+0.12f*std::sin(z*0.045f+hills[5]));
        if(z>=fairwayFrom&&z<=length&&std::abs(x-LineX(z))<=half)return Lie::Fairway;
        return Lie::Rough;
    }

    void Course::Surface(float x,float z,float& fairway,float& greenOut,float& sand,float& water,float& tee) const
    {
        const float half=fairwayWidth*0.5f*(1+0.12f*std::sin(z*0.045f+hills[5]));
        const float off=std::abs(x-LineX(z));
        fairway=(1-Smooth((off-half+0.8f)/1.6f))*Smooth((z-fairwayFrom+0.8f)/1.6f)*(1-Smooth((z-length+0.8f)/1.6f));
        const float e=green.At(x,z);
        fairway=std::max(fairway,1-Smooth((e-1.28f)/0.08f));
        greenOut=1-Smooth((e-1.0f)/0.06f);
        sand=0;
        for(const Ellipse& b:bunkers)sand=std::max(sand,1-Smooth((b.At(x,z)-0.98f)/0.06f));
        water=pond.rx>0?Smooth((waterLevel-Height(x,z))/0.25f)*(1-Smooth((pond.At(x,z)-1.0f)/0.1f)):0.0f;
        tee=(1-Smooth((std::abs(x)-5)/0.4f))*(1-Smooth((std::abs(z)-7)/0.4f));
    }

    V3 WindVector(const Wind& w){return {w.speed*std::sin(w.toward),0,w.speed*std::cos(w.toward)};}

    float PutterRange(float distance)
    {
        for(float r:{3.0f,6.0f,10.0f,15.0f,20.0f,30.0f,45.0f,60.0f})if(r>=distance*1.25f)return r;
        return 60;
    }

    Flight Fly(const Course& course,V3 from,Lie lie,const Shot& shot,const Wind& wind,bool keepPath,float step)
    {
        Flight f;
        const Club& club=ClubOf(shot.club);
        const V3 up{0,1,0},pin=course.Pin();
        V3 p=from;
        p.y=course.Height(p.x,p.z)+BallRadius;
        float speedK=1,spinK=1;
        LieEffect(lie,shot.club,speedK,spinK);
        const float power=std::max(0.0f,shot.power);
        float speed,launch,back,side,aim=shot.aim;
        if(shot.club==Putter)
        {
            // Rolled: as fast as rolls range * power on level green.
            const float range=shot.range>0?shot.range:10.0f;
            speed=std::sqrt(2*GroundOf(Lie::Green).roll*G*range*power)*(lie==Lie::Green?1.0f:1.08f);
            launch=0;back=side=0;
            aim+=shot.accuracy*2.0f*Deg;
        }
        else
        {
            speed=club.speed*power*speedK;
            // Over full power the strike suffers: higher, less control (the timing errs too).
            launch=(club.loft*(shot.club<=1?0.95f:0.86f)+(power<0.6f?(0.6f-power)*6:0))*Deg;
            back=club.spin*(0.55f+0.45f*power)*spinK;
            side=shot.accuracy*2300*std::min(1.0f,speed/55);
            aim+=shot.accuracy*3.5f*Deg;
        }
        const V3 d{std::sin(aim),0,std::cos(aim)};
        V3 v=d*(std::cos(launch)*speed)+up*(std::sin(launch)*speed);
        V3 spin=Normalized(Cross(d,up))*(back*2*Pi/60)+up*(side*2*Pi/60);
        const V3 w=WindVector(wind);
        bool rolling=launch==0,landed=false;
        V3 lastLand=p;
        if(keepPath)f.path.push_back(p);
        const int steps=static_cast<int>(40/step);
        for(int i=0;i<steps;++i)
        {
            const V3 before=p;
            float ground=course.Height(p.x,p.z);
            if(!rolling)
            {
                // In the air: gravity, drag against the air, the spin's lift (Magnus).
                const V3 air=v-w*std::clamp((p.y-ground)/3,0.0f,1.0f);
                const float va=Length(air);
                V3 a{0,-G,0};
                if(va>0.1f)
                {
                    const float s=BallRadius*Length(spin)/va;
                    // Lift grows with the spin, and so does the drag.
                    const float cl=std::min(0.25f,0.9f*s+0.05f),cd=0.21f+0.25f*std::min(s,0.4f);
                    a=a-air*(AirK*cd*va);
                    if(Length(spin)>1)a=a+Normalized(Cross(spin,air))*(AirK*cl*va*va);
                }
                spin=spin*std::exp(-step/14);
                v=v+a*step;
                p=p+v*step;
                ground=course.Height(p.x,p.z);
                if(p.y-BallRadius<=ground)
                {
                    const Lie at=course.LieAt(p.x,p.z);
                    if(!landed){landed=true;f.carry=Flat(from,p);f.landing=static_cast<int>(f.path.size());}
                    if(at==Lie::Out){f.out=true;f.rest=p;f.lie=at;if(keepPath)f.path.push_back(p);return f;}
                    if(at==Lie::Water){f.water=true;f.rest=p;f.lie=at;f.drop=lastLand;if(keepPath)f.path.push_back(p);return f;}
                    const V3 n=course.Normal(p.x,p.z);
                    const float vn=Dot(v,n);
                    p.y=ground+BallRadius;
                    if(vn<0)
                    {
                        const Ground g=GroundOf(at);
                        const V3 along=v-n*vn;
                        if(-vn>1.3f)
                        {
                            // A bounce: back up by the ground's give, slowed along it (more on a
                            // green the more it spins back).
                            float grab=g.grab;
                            if(at!=Lie::Rough&&at!=Lie::Bunker)grab+=std::min(0.45f,Length(spin)*60/(2*Pi)/(at==Lie::Green?15000.0f:20000.0f));
                            v=along*(1-std::min(grab,0.95f))-n*(vn*g.bounce);
                            spin=spin*0.4f;
                        }
                        else{v=along;rolling=true;}
                    }
                }
                else if(course.LieAt(p.x,p.z)!=Lie::Water)lastLand=p;
            }
            else
            {
                // On the ground: down the slope, held back by the grass.
                const Lie at=course.LieAt(p.x,p.z);
                if(at==Lie::Out){f.out=true;f.rest=p;f.lie=at;if(keepPath)f.path.push_back(p);return f;}
                if(at==Lie::Water){f.water=true;f.rest=p;f.lie=at;f.drop=lastLand;if(keepPath)f.path.push_back(p);return f;}
                lastLand=p;
                const V3 n=course.Normal(p.x,p.z);
                v=v-n*Dot(v,n);
                const V3 slope=V3{0,-G,0}-n*(-G*n.y);
                const float mu=GroundOf(at).roll*(at==Lie::Fairway&&course.LieAt(p.x,p.z)==Lie::Fairway?1.0f:1.0f);
                const float s=Length(v);
                if(s<0.04f&&Length(slope)<mu*1.25f*G*n.y)
                {
                    f.rest=p;f.lie=at;
                    if(keepPath)f.path.push_back(p);
                    return f;
                }
                const V3 next=v+(slope-(s>1e-4f?v*(1/s):V3{})*(mu*G*n.y))*step;
                v=s>0.04f&&Dot(next,v)<0?V3{}:next;
                p=p+v*step;
                p.y=course.Height(p.x,p.z)+BallRadius;
                if(!landed){landed=true;f.carry=0;f.landing=static_cast<int>(f.path.size());}
            }
            // The cup: passed over slowly enough, or fallen into.
            {
                const float sx=p.x-before.x,sz=p.z-before.z,len2=sx*sx+sz*sz;
                float t=len2>1e-12f?((pin.x-before.x)*sx+(pin.z-before.z)*sz)/len2:0;
                t=std::clamp(t,0.0f,1.0f);
                const float cx=before.x+sx*t-pin.x,cz=before.z+sz*t-pin.z;
                const float above=p.y-course.Height(p.x,p.z);
                if(cx*cx+cz*cz<CupRadius*CupRadius&&Length(v)<1.8f&&above<0.12f)
                {
                    f.holed=true;
                    f.rest=pin;
                    f.lie=Lie::Green;
                    if(keepPath){f.path.push_back({pin.x,pin.y-0.03f,pin.z});f.path.push_back({pin.x,pin.y-0.08f,pin.z});}
                    return f;
                }
            }
            if(keepPath)f.path.push_back(p);
        }
        f.rest=p;
        f.lie=course.LieAt(p.x,p.z);
        return f;
    }

    std::vector<int> ClubsFor(Lie lie,float toPin)
    {
        if(lie==Lie::Green)return {Putter};
        std::vector<int> clubs;
        for(int c=0;c<Putter;++c)clubs.push_back(c);
        if(lie==Lie::Fairway&&toPin<25)clubs.push_back(Putter);
        return clubs;
    }

    int ClubFor(Lie lie,float toPin,const Course& course,V3 ball,const Wind& wind)
    {
        if(lie==Lie::Green)return Putter;
        const float aim=std::atan2(course.Pin().x-ball.x,course.Pin().z-ball.z);
        int best=Wedge;
        for(int c=Wedge;c>=0;--c)
        {
            if(c==Driver&&lie!=Lie::Tee)continue;
            Shot s;s.club=c;s.aim=aim;s.power=1;
            const Flight f=Fly(course,ball,lie,s,wind,false,1/30.0f);
            best=c;
            if(f.carry>=toPin*0.92f||Flat(ball,f.rest)>=toPin)break;
        }
        return best;
    }

    float StrokesLeft(const Course& course,V3 p,Lie lie)
    {
        const float d=Flat(p,course.Pin());
        if(lie==Lie::Green)return d<0.3f?1.0f:1.0f+0.95f*(1-std::exp(-d/4))+d/80;
        float s=2.25f+d/165;
        if(d<18)s=std::min(s,2.2f+d/60);
        if(lie==Lie::Rough)s+=0.25f;
        else if(lie==Lie::Bunker)s+=d>40?0.7f:0.45f;
        return s;
    }

    namespace
    {
        // How many strokes the shot leaves (this one counted): penalties for water and out of bounds.
        float Outcome(const Course& course,V3 from,Lie lie,const Flight& f)
        {
            if(f.holed)return 1;
            if(f.out)return 2+StrokesLeft(course,from,lie);
            if(f.water)return 2+StrokesLeft(course,f.drop,Lie::Rough);
            return 1+StrokesLeft(course,f.rest,f.lie);
        }
    }

    Shot Choose(const Course& course,V3 from,Lie lie,const Wind& wind)
    {
        const V3 pin=course.Pin();
        const float d=Flat(from,pin),toward=std::atan2(pin.x-from.x,pin.z-from.z);
        Shot best;
        float bestScore=1e9f;
        auto score=[&](const Shot& s,std::initializer_list<std::pair<float,float>> variants,float step){
            float sum=0,weight=0;
            for(const auto& [acc,w]:variants)
            {
                Shot t=s;
                t.accuracy=acc;
                sum+=Outcome(course,from,lie,Fly(course,from,lie,t,wind,false,step))*w;
                weight+=w;
            }
            return sum/weight;
        };
        for(const int club:ClubsFor(lie,d))
        {
            if(club==Putter)
            {
                Shot s;
                s.club=Putter;
                s.range=PutterRange(d);
                // Coarse, then finer round the best.
                Shot coarse;float coarseScore=1e9f;
                for(float a=-6;a<=6.01f;a+=1.0f)
                    for(float pw=0.1f;pw<=1.001f;pw+=0.04f)
                    {
                        s.aim=toward+a*Deg;s.power=pw;
                        const float sc=score(s,{{0.0f,1.0f}},1/60.0f);
                        if(sc<coarseScore){coarseScore=sc;coarse=s;}
                    }
                for(float a=-1;a<=1.01f;a+=0.2f)
                    for(float pw=-0.04f;pw<=0.041f;pw+=0.01f)
                    {
                        s.aim=coarse.aim+a*Deg;s.power=std::clamp(coarse.power+pw,0.05f,1.0f);
                        const float sc=score(s,{{0.0f,2.0f},{0.25f,1.0f},{-0.25f,1.0f}},1/60.0f);
                        if(sc<bestScore){bestScore=sc;best=s;}
                    }
                continue;
            }
            if(club==Driver&&lie!=Lie::Tee)continue;
            // Only clubs that can get about there (or the longest, when none can).
            Shot full;full.club=club;full.aim=toward;full.power=1;
            const Flight f=Fly(course,from,lie,full,wind,false,1/30.0f);
            const float reach=Flat(from,f.rest);
            if(reach<d*0.45f&&club>Driver+1)continue;
            if(f.carry>d*1.6f&&club<Wedge)continue;
            for(float a=-12;a<=12.01f;a+=2.0f)
                for(float pw=0.35f;pw<=1.001f;pw+=0.065f)
                {
                    Shot s;s.club=club;s.aim=toward+a*Deg;s.power=pw;
                    const float sc=score(s,{{0.0f,2.0f},{0.35f,1.0f},{-0.35f,1.0f}},1/30.0f);
                    if(sc<bestScore){bestScore=sc;best=s;}
                }
        }
        return best;
    }

    void Execute(Shot& shot,float care,std::mt19937& random)
    {
        std::normal_distribution<float> n(0,1);
        const float loose=1-std::clamp(care,0.0f,1.0f);
        if(shot.club==Putter)
        {
            shot.power=std::clamp(shot.power*(1+n(random)*(0.05f+0.08f*loose)),0.02f,1.0f);
            shot.accuracy=n(random)*(0.35f+0.5f*loose);
        }
        else
        {
            shot.power=std::clamp(shot.power*(1+n(random)*(0.03f+0.05f*loose)),0.1f,1.03f);
            shot.accuracy=n(random)*(0.22f+0.35f*loose);
        }
    }

    std::wstring Check(int holes,std::uint32_t seed)
    {
        std::wstring out;
        int wrong=0;
        wchar_t line[300];
        // The clubs on the first hole's tee, straight, no wind.
        {
            const Course c(seed,4);
            out+=L"clubs from the tee (full power, no wind): carry, total\n";
            for(int k=0;k<Putter;++k)
            {
                Shot s;s.club=k;s.power=1;s.aim=0;
                const Flight f=Fly(c,c.Tee(),Lie::Tee,s,{},true);
                float top=0;
                for(const V3& p:f.path)top=std::max(top,p.y);
                swprintf_s(line,L"  %-4ls %6.1f m %6.1f m  apex %5.1f m  %.1f s in the air\n",ClubOf(k).en,f.carry,Flat(c.Tee(),f.rest),top,f.landing/60.0f);
                out+=line;
            }
            // Putts on level-ish green: the range.
            for(float r:{3.0f,10.0f,30.0f})
            {
                Shot s;s.club=Putter;s.range=r;s.power=1;
                const V3 from=c.GreenMiddle();
                s.aim=0.7f;
                const Flight f=Fly(c,from,Lie::Green,s,{},false);
                swprintf_s(line,L"  putt range %.0f m: rolled %.1f m\n",r,Flat(from,f.rest));
                out+=line;
            }
        }
        int strokes=0,pars=0;
        double thinking=0;
        for(int h=0;h<holes;++h)
        {
            const int par=3+h%3;
            const Course c(seed+static_cast<std::uint32_t>(h)*7919u,par);
            std::wstring problem;
            if(c.LieAt(0,0)!=Lie::Tee)problem+=L" tee not tee;";
            if(c.LieAt(c.Pin().x,c.Pin().z)!=Lie::Green)problem+=L" pin off the green;";
            std::mt19937 random(seed+h);
            V3 ball=c.Tee();
            Lie lie=Lie::Tee;
            int n=0;
            bool holed=false;
            std::wstring way;
            while(n<12&&!holed)
            {
                LARGE_INTEGER t0,t1,rate;
                QueryPerformanceCounter(&t0);
                Shot s=Choose(c,ball,lie,{3,0.6f});
                QueryPerformanceCounter(&t1);
                QueryPerformanceFrequency(&rate);
                thinking=std::max(thinking,static_cast<double>(t1.QuadPart-t0.QuadPart)/rate.QuadPart);
                Execute(s,0.85f,random);
                const Flight f=Fly(c,ball,lie,s,{3,0.6f},false);
                ++n;
                swprintf_s(line,L" %ls%.0f%%->",ClubOf(s.club).en,s.power*100);
                way+=line;
                if(f.holed){holed=true;way+=L"in";break;}
                if(f.out){++n;way+=L"OB";continue;}
                if(f.water){++n;ball=f.drop;lie=Lie::Rough;way+=L"water";continue;}
                ball=f.rest;lie=f.lie;
                swprintf_s(line,L"%ls %.1fm",LieName(lie),Flat(ball,c.Pin()));
                way+=line;
            }
            if(!holed)problem+=L" not holed in 12;";
            if(!problem.empty())++wrong;
            strokes+=n;pars+=par;
            swprintf_s(line,L"hole %d (seed %u) par %d, %.0f m, water %d, bunkers: %d strokes:%ls%ls\n",h+1,c.Seed(),par,c.Length(),c.HasWater()?1:0,n,way.c_str(),problem.empty()?L"":(L" WRONG"+problem).c_str());
            out+=line;
        }
        swprintf_s(line,L"%d wrong\n%d holes: %d strokes against par %d; longest think %.2f s\n",wrong,holes,strokes,pars,thinking);
        return line+out;
    }
}
