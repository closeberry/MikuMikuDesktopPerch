#define NOMINMAX
#include "MonsterArt.hpp"
#include "ShapeBuilder.hpp"
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace MikuMikuDesktopPerch
{
    namespace
    {
        using namespace Art;

        float Smooth(float x){x=std::clamp(x,0.0f,1.0f);return x*x*(3-2*x);}

        // What a pose comes to: the stride (swing -1..1, hop 0..1), the attack (wind: drawing
        // back, then hit: the blow, each 0..1, one fading as the other comes), the flinch, and
        // the dragon's fire (breathIn: drawing back and taking breath, breathOut: breathing it
        // out, sweep: its head turned aside as it does, radians).
        struct Motion {float swing,hop,wind,hit,flinch,time;bool charging;float breathIn,breathOut,sweep;};

        Motion Of(const MonsterPose& p)
        {
            Motion k{};
            k.swing=std::sin(p.walk)*p.moving;
            k.hop=std::abs(std::sin(p.walk))*p.moving;
            if(p.strike>=0)
            {
                k.wind=Smooth(p.strike/0.35f)*(1-Smooth((p.strike-0.35f)/0.15f));
                k.hit=Smooth((p.strike-0.3f)/0.2f)*(1-Smooth((p.strike-0.6f)/0.4f));
            }
            k.flinch=p.flinch>=0?std::sin(XM_PI*std::min(1.0f,p.flinch)):0.0f;
            if(p.breath>=0)
            {
                const float b=p.breath;
                k.breathIn=Smooth(b/0.22f)*(1-Smooth((b-0.22f)/0.12f));
                k.breathOut=Smooth((b-0.24f)/0.1f)*(1-Smooth((b-0.82f)/0.18f));
                // To one side, across to the other, and back to the middle.
                k.sweep=0.45f*std::sin(XM_2PI*std::clamp((b-0.3f)/0.52f,0.0f,1.0f))*k.breathOut;
            }
            k.time=p.time;
            k.charging=p.charging;
            return k;
        }

        // The whole body moved: forward (lunge), up (rise), leaning forward from the feet (lean),
        // and knocked back a little by a blow.
        XMMATRIX Body(const Motion& k,float lunge,float rise,float lean)
        {
            return XMMatrixRotationX(lean-0.25f*k.flinch)*XMMatrixTranslation(0,rise,lunge-1.2f*k.flinch);
        }

        float Slime(Shapes& s,const Dungeon::Monster& m,const Motion& k)
        {
            // A soft drop of jelly: wide at the foot, a little tip on top, a face, a shine. It
            // hops along, squashing as it lands; it crouches, then leaps at her.
            const Colour body=Tint(m.color),foot=Tint(m.color,0.8f);
            const float squash=0.14f*k.wind;
            s.With(XMMatrixScaling(1+0.6f*squash,1-squash,1+0.6f*squash)*Body(k,3.2f*k.hit,1.3f*k.hop+1.6f*k.hit,0.2f*k.hit),[&]{
                s.Ellipsoid({0,1.0f,0},{3.9f,1.2f,3.9f},foot);
                s.Ellipsoid({0,2.4f,0},{3.4f,2.6f,3.4f},body);
                s.Limb({0,4.6f,0},{0,6.0f,-0.4f},1.0f,0.05f,body);
                s.Both([&](float o){
                    s.Ellipsoid({o*1.1f,3.0f,2.9f},{0.45f,0.62f,0.3f},Black);
                    s.Ellipsoid({o*1.0f,3.25f,3.12f},{0.14f,0.14f,0.08f},{1,1,1,Glow});
                });
                s.Ellipsoid({0,1.9f,3.15f},{0.7f,0.28f+0.25f*k.hit,0.2f},Black);
                s.Ellipsoid({-1.4f,4.1f,1.4f},{0.8f,0.4f,0.5f},{0.92f,1.0f,0.92f},{0,0,0.5f});
            });
            return 6.0f;
        }

        float Bat(Shapes& s,const Dungeon::Monster& m,const Motion& k)
        {
            // Hovering at her chest's height, beating its wings all the time, ears up, red eyes;
            // its shadow below. It dives at her.
            const Colour body=Tint(m.color,0.7f),wing=Tint(m.color,0.5f);
            const float flap=0.6f*std::sin(k.time*16.0f),bob=0.5f*std::sin(k.time*6.0f);
            s.Ellipsoid({0,0.05f,0},{2.4f,0.02f,2.0f},{0.08f,0.07f,0.09f});
            s.With(Body(k,3.6f*k.hit,bob-2.4f*k.hit+0.8f*k.wind,0.5f*k.hit-0.2f*k.wind),[&]{
                s.Ellipsoid({0,9.0f,0},{1.3f,1.7f,1.2f},body);
                s.Ellipsoid({0,11.0f,0.4f},{1.05f,1.0f,1.0f},body);
                s.Both([&](float o){
                    s.Limb({o*0.5f,11.6f,0.3f},{o*1.0f,13.2f,0.1f},0.38f,0.02f,body,6);
                    s.Ellipsoid({o*0.42f,11.15f,1.32f},{0.18f,0.16f,0.1f},{1.0f,0.25f,0.2f,Glow});
                    const XMFLOAT3 shoulder{o*1.0f,9.8f,0};
                    s.With(Shapes::About(shoulder,0,0,o*flap),[&]{
                        const XMFLOAT3 tips[5]={{o*3.4f,11.2f,-0.4f},{o*7.0f,12.0f,-0.8f},{o*6.0f,8.6f,-0.9f},{o*4.0f,7.4f,-0.6f},{o*2.0f,7.9f,-0.3f}};
                        for(int i=0;i+1<5;++i)s.Triangle(shoulder,tips[i],tips[i+1],wing);
                        s.Limb(shoulder,tips[0],0.22f,0.15f,body,5);s.Limb(tips[0],tips[1],0.15f,0.05f,body,5);
                    });
                    s.Limb({o*0.6f,7.6f,0},{o*0.7f,6.6f,0.2f},0.18f,0.1f,body,5);
                });
                s.Triangle({-0.3f,10.6f,1.3f},{0.3f,10.6f,1.3f},{0,10.1f,1.45f},White);
            });
            return 13.5f;
        }

        float Goblin(Shapes& s,const Dungeon::Monster& m,const Motion& k)
        {
            // Small and wiry, big pointed ears, a long nose, yellow eyes, a ragged tunic and a
            // knife. It scurries, arms pumping; it draws the knife back and stabs.
            const Colour skin=Tint(m.color),tunic{0.42f,0.30f,0.18f},belt{0.22f,0.15f,0.09f},metal{0.70f,0.72f,0.76f};
            s.With(Body(k,1.8f*k.hit,0.5f*k.hop,0.25f*k.hit-0.1f*k.wind),[&]{
                s.Both([&](float o){
                    s.With(Shapes::About({o*0.9f,5.2f,0},o*0.6f*k.swing,0),[&]{
                        s.Limb({o*0.9f,5.2f,0},{o*1.05f,0.9f,0.25f},0.62f,0.45f,skin);
                        s.Box({o*1.05f,0.45f,0.7f},{0.55f,0.45f,0.9f},belt);
                    });
                    // The knife arm (its right) draws back, then thrusts.
                    const float arm=o>0?0.7f*k.wind-1.5f*k.hit-0.5f*o*k.swing:-0.5f*o*k.swing;
                    s.With(Shapes::About({o*1.9f,8.1f,0},arm,0),[&]{
                        s.Limb({o*1.9f,8.1f,0},{o*2.5f,5.8f,1.0f},0.5f,0.42f,skin);
                        s.Ellipsoid({o*2.55f,5.5f,1.25f},{0.55f,0.55f,0.55f},skin);
                        if(o>0)
                        {
                            s.Box({2.55f,5.7f,2.6f},{0.12f,0.32f,1.2f},metal);
                            s.Box({2.55f,5.6f,1.4f},{0.3f,0.45f,0.18f},belt);
                        }
                    });
                    s.Limb({o*1.55f,10.2f,0},{o*3.8f,11.0f,-0.7f},0.6f,0.02f,skin,6);
                    s.Ellipsoid({o*0.62f,10.25f,1.62f},{0.24f,0.2f,0.12f},{1.0f,0.85f,0.2f,Glow});
                });
                s.Ellipsoid({0,6.6f,0},{2.0f,2.3f,1.5f},tunic);
                s.Limb({0,5.2f,0},{0,5.6f,0},2.05f,1.95f,belt);
                s.Ellipsoid({0,9.8f,0.3f},{1.8f,1.7f,1.6f},skin);
                s.Limb({0,9.7f,1.6f},{0,9.25f,2.7f},0.38f,0.05f,skin,6);
                s.Ellipsoid({0,9.0f,1.65f},{0.7f,0.18f,0.2f},Black);
            });
            return 11.6f;
        }

        float Skeleton(Shapes& s,const Dungeon::Monster& m,const Motion& k)
        {
            // Bare bones: a skull with dark sockets and a red glint, ribs, thin limbs, a rusty
            // sword. It rattles along stiff-legged; it raises the sword high and cuts down.
            const Colour bone=Tint(m.color),dim=Tint(m.color,0.8f),rust{0.55f,0.38f,0.26f};
            s.With(Body(k,1.6f*k.hit,0.3f*k.hop,0.2f*k.hit-0.1f*k.wind),[&]{
                s.Both([&](float o){
                    s.With(Shapes::About({o*0.9f,8.0f,0},o*0.5f*k.swing,0),[&]{
                        s.Limb({o*0.9f,8.0f,0},{o*1.0f,4.3f,0.35f},0.36f,0.3f,bone,6);
                        s.Limb({o*1.0f,4.3f,0.35f},{o*1.0f,0.7f,0},0.3f,0.26f,bone,6);
                        s.Ellipsoid({o*1.0f,4.3f,0.35f},{0.45f,0.45f,0.45f},dim);
                        s.Box({o*1.0f,0.35f,0.5f},{0.5f,0.3f,0.9f},bone);
                    });
                    const float arm=o>0?-2.5f*k.wind-0.8f*k.hit-0.4f*o*k.swing:-0.4f*o*k.swing;
                    s.With(Shapes::About({o*2.1f,13.0f,0},arm,0),[&]{
                        s.Limb({o*2.1f,13.0f,0},{o*2.5f,10.4f,0.7f},0.3f,0.26f,bone,6);
                        s.Limb({o*2.5f,10.4f,0.7f},{o*2.3f,8.4f,1.7f},0.26f,0.22f,bone,6);
                        if(o>0)
                        {
                            s.Box({2.3f,9.6f,2.8f},{0.14f,2.4f,0.32f},rust,{0.5f,0,0});
                            s.Box({2.3f,8.4f,1.75f},{0.6f,0.15f,0.2f},{0.3f,0.2f,0.12f});
                        }
                    });
                    s.Ellipsoid({o*0.5f,14.8f,1.38f},{0.38f,0.44f,0.2f},Black);
                    s.Ellipsoid({o*0.5f,14.75f,1.5f},{0.1f,0.1f,0.06f},{1.0f,0.2f,0.15f,Glow});
                });
                s.Box({0,8.2f,0},{1.4f,0.6f,0.7f},bone);
                s.Limb({0,8.6f,-0.2f},{0,13.0f,-0.2f},0.32f,0.3f,dim,6);
                for(int i=0;i<4;++i)s.Ellipsoid({0,9.9f+0.75f*i,0.35f},{1.75f-0.08f*i,0.17f,1.05f},bone,{0,0,0},12,4);
                s.Box({0,13.05f,0},{2.1f,0.25f,0.35f},bone);
                s.Limb({0,13.2f,0},{0,13.9f,0.1f},0.25f,0.25f,bone,6);
                s.Ellipsoid({0,14.7f,0.2f},{1.3f,1.45f,1.35f},bone);
                s.Box({0,13.45f-0.3f*k.hit,0.75f},{0.8f,0.32f,0.6f},dim);
                s.Triangle({-0.2f,14.1f,1.55f},{0.2f,14.1f,1.55f},{0,14.5f,1.5f},Black);
            });
            return 16.4f;
        }

        float Orc(Shapes& s,const Dungeon::Monster& m,const Motion& k)
        {
            // Big and heavy: a broad chest, a small brow over red eyes, tusks, and a club. It
            // stomps along; it heaves the club up (and holds it there gathering itself) and
            // brings it down.
            const Colour skin=Tint(m.color,0.9f),dark=Tint(m.color,0.6f),cloth{0.30f,0.24f,0.20f},tusk{0.92f,0.88f,0.75f},wood{0.40f,0.26f,0.14f};
            const float raised=std::max(k.wind,k.charging?1.0f:0.0f);
            s.With(Body(k,1.6f*k.hit,0.4f*k.hop,0.3f*k.hit-0.12f*raised),[&]{
                s.Both([&](float o){
                    s.With(Shapes::About({o*1.6f,8.4f,0},o*0.4f*k.swing,0),[&]{
                        s.Limb({o*1.6f,8.4f,0},{o*1.8f,1.0f,0.2f},1.25f,1.0f,cloth);
                        s.Box({o*1.8f,0.5f,0.7f},{0.95f,0.5f,1.4f},dark);
                    });
                    s.Ellipsoid({o*3.6f,13.4f,0},{1.5f,1.3f,1.4f},skin);
                    const float arm=o>0?-2.4f*raised-0.7f*k.hit-0.35f*o*k.swing:-0.35f*o*k.swing+0.3f*k.hit;
                    s.With(Shapes::About({o*3.9f,13.0f,0.2f},arm,0),[&]{
                        s.Limb({o*3.9f,13.0f,0.2f},{o*4.5f,8.8f,1.3f},1.05f,0.85f,skin);
                        s.Ellipsoid({o*4.6f,8.2f,1.5f},{1.0f,1.0f,1.0f},skin);
                        if(o>0)s.Limb({4.6f,7.8f,1.6f},{4.9f,12.8f,4.2f},0.35f,0.95f,wood);
                    });
                    s.Limb({o*0.75f,15.3f,2.15f},{o*0.95f,16.6f,2.45f},0.3f,0.02f,tusk,6);
                    s.Ellipsoid({o*0.7f,16.7f,1.95f},{0.22f,0.2f,0.12f},{1.0f,0.25f,0.15f,Glow});
                });
                s.Ellipsoid({0,8.8f,0},{3.0f,1.6f,2.2f},cloth);
                s.Ellipsoid({0,11.6f,0},{3.7f,3.4f,2.5f},skin);
                s.Ellipsoid({0,16.3f,0.7f},{1.9f,1.8f,1.9f},skin);
                s.Box({0,17.1f,2.05f},{1.4f,0.25f,0.3f},dark);
                s.Box({0,15.4f,1.6f},{1.2f,0.6f,0.9f},skin);
            });
            return 18.6f;
        }

        float Golem(Shapes& s,const Dungeon::Monster& m,const Motion& k)
        {
            // Rough blocks of stone, slightly askew, a glowing rune on the chest and glowing eyes.
            // It lumbers, swaying; it lifts both fists high and smashes them down.
            const Colour stone=Tint(m.color),dark=Tint(m.color,0.75f),glow{0.45f,0.95f,1.0f,Glow};
            s.With(XMMatrixRotationZ(0.06f*k.swing)*Body(k,1.2f*k.hit,0.25f*k.hop,0.2f*k.hit-0.08f*k.wind),[&]{
                s.Both([&](float o){
                    s.With(Shapes::About({o*1.9f,6.8f,0},o*0.3f*k.swing,0),[&]{
                        s.Box({o*1.9f,3.4f,0},{1.4f,3.4f,1.5f},dark,{0.05f*o,0,0.04f});
                        s.Box({o*2.0f,0.6f,0.6f},{1.6f,0.6f,2.0f},dark);
                    });
                    s.Box({o*5.0f,14.6f,0},{1.8f,1.6f,1.8f},stone,{0.1f,0.2f*o,0.15f*o});
                    s.With(Shapes::About({o*5.4f,13.4f,0.4f},-2.3f*k.wind-1.0f*k.hit-0.25f*o*k.swing,0),[&]{
                        s.Box({o*5.6f,10.0f,0.6f},{1.3f,3.2f,1.3f},dark,{-0.15f,0,0.06f*o});
                        s.Box({o*5.8f,5.8f,1.2f},{1.6f,1.4f,1.6f},stone,{0.2f,0.3f*o,0});
                    });
                    s.Box({o*0.6f,18.0f,1.85f},{0.32f,0.18f,0.1f},glow);
                });
                s.Box({0,8.6f,0},{3.2f,1.8f,2.2f},dark,{0,0.05f,0});
                s.Box({0,13.2f,0},{4.0f,3.3f,2.6f},stone,{0.04f,-0.04f,0.02f});
                s.Box({0,18.0f,0.6f},{1.7f,1.3f,1.4f},stone,{0.05f,0.08f,0});
                s.Box({0,13.4f,2.62f},{0.9f,0.15f,0.05f},glow);
                s.Box({0,13.4f,2.62f},{0.15f,1.0f,0.05f},glow);
            });
            return 19.6f;
        }

        float Ghost(Shapes& s,const Dungeon::Monster& m,const Motion& k)
        {
            // A pale glow drifting above the floor: a round head-body trailing into a wisp,
            // arms reaching out, dark hollow eyes and mouth. It bobs and sways; it rushes at her.
            const Colour spirit{((m.color>>16)&255)/255.0f*0.85f,((m.color>>8)&255)/255.0f*0.85f,(m.color&255)/255.0f*0.9f,Glow};
            const float bob=0.8f*std::sin(k.time*2.4f),sway=0.3f*std::sin(k.time*1.9f)+0.25f*k.swing;
            s.With(Body(k,3.4f*k.hit,bob+0.6f*k.wind,0.35f*k.hit-0.15f*k.wind),[&]{
                s.Ellipsoid({0,10.0f,0},{2.6f,3.0f,2.4f},spirit);
                s.With(Shapes::About({0,8.6f,0},-0.3f*k.hit,sway,0.15f*sway),[&]{
                    s.Limb({0,8.6f,0},{0.4f,5.0f,-1.2f},2.3f,1.2f,spirit);
                    s.Limb({0.4f,5.0f,-1.2f},{-0.6f,2.4f,-2.4f},1.2f,0.1f,spirit);
                });
                s.Both([&](float o){
                    s.With(Shapes::About({o*2.2f,10.0f,0.5f},0.5f*k.wind-0.6f*k.hit+0.15f*std::sin(k.time*3.0f+o),0),[&]{
                        s.Limb({o*2.2f,10.0f,0.5f},{o*3.8f,8.4f,2.9f},0.75f,0.3f,spirit);
                    });
                    s.Ellipsoid({o*0.9f,10.8f,2.1f},{0.5f,0.7f,0.3f},Black);
                });
                s.Ellipsoid({0,9.0f,2.25f},{0.55f,0.8f+0.5f*k.hit,0.3f},Black);
            });
            return 13.2f;
        }

        // A point along a cubic curve (a, b, c, d) at t.
        XMVECTOR Curve(FXMVECTOR a,FXMVECTOR b,FXMVECTOR c,GXMVECTOR d,float t)
        {
            const float u=1-t;
            return XMVectorAdd(XMVectorAdd(XMVectorScale(a,u*u*u),XMVectorScale(b,3*u*u*t)),XMVectorAdd(XMVectorScale(c,3*u*t*t),XMVectorScale(d,t*t*t)));
        }

        XMFLOAT3 F3(FXMVECTOR v){XMFLOAT3 f;XMStoreFloat3(&f,v);return f;}

        // A leg from the hip down to the ground and three toes with hooked claws, forward along
        // +z: thin, with a knee (bent forward or back) and an ankle.
        void Leg(Shapes& s,float o,XMFLOAT3 hip,XMFLOAT3 knee,XMFLOAT3 ankle,const Colour& c,const Colour& claw,float r)
        {
            const XMFLOAT3 foot{ankle.x,0.75f,ankle.z+0.6f};
            s.Limb(hip,knee,r,r*0.6f,c);
            s.Ellipsoid(knee,{r*0.62f,r*0.62f,r*0.62f},c,{0,0,0},8,6);
            s.Limb(knee,ankle,r*0.58f,r*0.42f,c);
            s.Limb(ankle,foot,r*0.42f,r*0.4f,c);
            for(int i=-1;i<=1;++i)
            {
                // Toes spread a little, the middle one longest, each ending in a claw curling down.
                const XMFLOAT3 tip{foot.x+o*0.0f+i*0.75f,0.35f,foot.z+(i==0?2.3f:1.8f)};
                const XMFLOAT3 hook{tip.x+i*0.1f,0.0f,tip.z+0.95f};
                s.Limb(foot,tip,0.34f,0.2f,c,6);
                s.Limb(tip,{tip.x,0.45f,tip.z+0.45f},0.22f,0.16f,claw,5);
                s.Limb({tip.x,0.45f,tip.z+0.45f},hook,0.16f,0.01f,claw,5);
            }
            // A spur behind the ankle.
            s.Limb({foot.x,0.6f,foot.z-0.4f},{foot.x,0.1f,foot.z-1.5f},0.18f,0.01f,claw,5);
        }

        float Dragon(Shapes& s,const Dungeon::Monster& m,const Motion& k,MonsterMouth* mouth)
        {
            // A proud dragon: its chest thrust out under belly plates, a long neck curving up
            // in an S to a horned head with a jaw that opens on teeth, thin legs with hooked
            // claws, spines down the back, a tail and great wings. It walks with its legs in
            // pairs, tail swaying, wings stirring and chest breathing. It rears back and bites
            // down at her, or rears, opens its jaws wide and breathes fire, sweeping its head
            // from side to side.
            const Colour scale=Tint(m.color,0.85f),dark=Tint(m.color,0.55f),belly{0.95f,0.68f,0.40f},horn{0.92f,0.86f,0.72f};
            const Colour wing{0.55f,0.10f,0.10f},eye{1.0f,0.85f,0.2f,Glow},claw{0.18f,0.14f,0.12f,Grimy},tooth{0.96f,0.94f,0.86f};
            const Colour throat{0.35f,0.03f,0.04f},tongue{0.75f,0.22f,0.25f};
            const float breathe=1+0.025f*std::sin(k.time*1.7f);
            // How the neck moves: drawn back (rearing, or taking in breath), thrust down at her
            // (the bite, the fire), turned (idly, or sweeping the fire), and the jaw open.
            const float rear=k.wind+k.breathIn,lunge=k.hit+0.55f*k.breathOut;
            const float turn=0.06f*std::sin(k.time*0.9f)+k.sweep;
            const float jaw=0.12f+0.15f*k.wind+0.45f*k.hit+0.3f*k.breathIn+0.55f*k.breathOut;
            s.With(Body(k,1.6f*k.hit+0.6f*k.breathOut,0.35f*k.hop,0.08f*k.hit-0.1f*k.wind-0.08f*k.breathIn),[&]{
                // The body, the chest high and thrust forward, and the plates under it.
                s.Ellipsoid({0,9.2f,-2.2f},{3.8f,3.6f,6.4f},scale,{-0.12f,0,0});
                s.Ellipsoid({0,10.6f,2.8f},{3.6f*breathe,4.2f*breathe,3.8f*breathe},scale);
                s.Ellipsoid({0,7.4f,-3.0f},{3.0f,2.2f,4.8f},belly);
                for(int i=0;i<7;++i)
                {
                    // Round the front of the chest and under it, each plate lying on it.
                    const float a=0.75f+0.3f*i;
                    s.Ellipsoid({0,10.6f+4.05f*breathe*std::cos(a),2.8f+3.7f*breathe*std::sin(a)},{2.5f-0.12f*i,0.55f,1.05f},belly,{a,0,0},10,5);
                }
                // The neck: an S from the shoulders up to the head, made of short pieces along a
                // curve so it bends smoothly; turned more toward its head.
                const XMVECTOR base=XMVectorSet(0,12.5f,4.8f,0);
                XMVECTOR head=XMVectorSet(0,20.5f+2.2f*rear-5.0f*lunge,10.0f-3.5f*rear+4.8f*lunge,0);
                XMVECTOR c1=XMVectorSet(0,17.5f,5.0f,0),c2=XMVectorAdd(head,XMVectorSet(0,-2.5f+1.5f*lunge,-3.5f+1.0f*lunge,0));
                auto turned=[&](FXMVECTOR p,float share){
                    const XMVECTOR d=XMVectorSubtract(p,base);
                    return XMVectorAdd(base,XMVector3Transform(d,XMMatrixRotationY(turn*share)));
                };
                head=turned(head,1.0f);c2=turned(c2,0.8f);c1=turned(c1,0.3f);
                constexpr int Pieces=10;
                XMVECTOR at[Pieces+1];
                for(int i=0;i<=Pieces;++i)at[i]=Curve(base,c1,c2,head,static_cast<float>(i)/Pieces);
                for(int i=0;i<Pieces;++i)
                {
                    const float r0=1.9f-0.95f*i/Pieces,r1=1.9f-0.95f*(i+1)/Pieces;
                    s.Limb(F3(at[i]),F3(at[i+1]),r0,r1,scale,10);
                    s.Ellipsoid(F3(at[i+1]),{r1,r1,r1},scale,{0,0,0},10,6);
                    // The plates down its throat, on the side toward the front and below.
                    const XMVECTOR along=XMVector3Normalize(XMVectorSubtract(at[i+1],at[i]));
                    const XMVECTOR front=XMVector3Normalize(XMVectorSet(0,-XMVectorGetZ(along),XMVectorGetY(along),0));
                    const XMVECTOR middle=XMVectorAdd(XMVectorLerp(at[i],at[i+1],0.5f),XMVectorScale(front,0.62f*(r0+r1)*0.5f));
                    s.Ellipsoid(F3(middle),{0.7f*r0,0.42f*r0,0.6f*r0},belly,{std::atan2(XMVectorGetY(along),-XMVectorGetZ(along)),turn*0.6f,0},8,5);
                    // A spine along the top.
                    if(i%2==0)
                    {
                        const XMVECTOR top=XMVectorSubtract(at[i],XMVectorScale(front,r0*0.9f));
                        s.Limb(F3(top),F3(XMVectorSubtract(top,XMVectorAdd(XMVectorScale(front,1.0f),XMVectorScale(along,0.6f)))),0.4f,0.02f,dark,5);
                    }
                }
                // The head, at the neck's end: looking down a little, more as it strikes or
                // breathes, up as it rears; turned with the neck.
                const float pitch=0.15f-0.35f*rear+0.4f*lunge+0.2f*k.breathOut;
                const XMMATRIX headFrame=XMMatrixRotationRollPitchYaw(pitch,turn*1.25f,0)*XMMatrixTranslationFromVector(head);
                s.With(headFrame,[&]{
                    s.Ellipsoid({0,0.5f,0},{1.45f,1.3f,2.0f},scale);
                    // The snout, long and narrow with a ridge down it, the inside of the mouth
                    // under it, teeth along it.
                    s.Ellipsoid({0,0.4f,2.6f},{0.8f,0.5f,2.2f},scale);
                    s.Limb({0,0.85f,0.4f},{0,0.65f,3.8f},0.32f,0.14f,scale,6);
                    s.Ellipsoid({0,0.1f,2.5f},{0.65f,0.18f,1.9f},throat);
                    s.Both([&](float o){
                        s.Ellipsoid({o*0.28f,0.72f,4.6f},{0.1f,0.06f,0.09f},Black);
                        // A glare: narrow eyes slanting down toward the snout under low brows,
                        // with slit pupils.
                        s.Ellipsoid({o*0.72f,1.3f,1.3f},{0.4f,0.13f,0.62f},dark,{0.3f,0,o*0.45f});
                        s.Ellipsoid({o*0.8f,1.08f,1.35f},{0.22f,0.075f,0.26f},eye,{0.15f,o*0.3f,o*0.4f});
                        s.Ellipsoid({o*0.88f,1.08f,1.4f},{0.04f,0.07f,0.05f},Black,{0.15f,o*0.3f,0});
                        s.Limb({o*0.7f,1.4f,-0.4f},{o*1.7f,3.3f,-3.6f},0.5f,0.02f,horn,6);
                        s.Limb({o*1.15f,0.55f,-0.7f},{o*2.2f,0.85f,-2.5f},0.3f,0.02f,horn,5);
                        for(int i=0;i<4;++i)
                            s.Limb({o*(0.6f-0.06f*i),0.05f,1.7f+0.7f*i},{o*(0.6f-0.06f*i),-0.4f,1.78f+0.7f*i},0.13f,0.02f,tooth,5);
                    });
                    // The lower jaw, opening from its hinge below the ears, with its own teeth
                    // and a tongue inside.
                    s.With(Shapes::About({0,-0.2f,-0.3f},jaw,0),[&]{
                        s.Ellipsoid({0,-0.38f,2.2f},{0.68f,0.24f,2.0f},scale);
                        s.Ellipsoid({0,-0.26f,2.1f},{0.55f,0.12f,1.7f},throat);
                        s.Ellipsoid({0,-0.18f,1.9f},{0.32f,0.08f,1.3f},tongue);
                        s.Both([&](float o){
                            for(int i=0;i<3;++i)
                                s.Limb({o*(0.52f-0.06f*i),-0.25f,1.8f+0.75f*i},{o*(0.52f-0.06f*i),0.12f,1.85f+0.75f*i},0.12f,0.02f,tooth,5);
                        });
                    });
                    // Where its fire comes out: between the jaws, the way the head faces.
                    if(mouth)
                    {
                        const XMMATRIX to=s.local*s.world;
                        XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(mouth->at),XMVector3TransformCoord(XMVectorSet(0,-0.2f,3.9f,0),to));
                        XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(mouth->way),XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0,0,1,0),to)));
                        mouth->sweep=k.sweep;
                    }
                });
                // The tail, swaying, ending in a spade.
                s.With(Shapes::About({0,8.0f,-7.5f},0,0.2f*std::sin(k.time*1.4f)+0.25f*k.swing),[&]{
                    const XMFLOAT3 tail[6]={{0,8.2f,-7.5f},{0,6.4f,-11.5f},{-1.0f,4.6f,-15.0f},{-2.6f,3.2f,-18.0f},{-4.2f,2.5f,-20.5f},{-5.6f,2.2f,-22.3f}};
                    const float tailR[6]={2.4f,1.8f,1.25f,0.8f,0.45f,0.15f};
                    for(int i=0;i+1<6;++i)
                    {
                        s.Limb(tail[i],tail[i+1],tailR[i],tailR[i+1],scale);
                        s.Limb({tail[i].x,tail[i].y+tailR[i]*0.8f,tail[i].z},{tail[i].x,tail[i].y+tailR[i]*0.8f+0.9f,tail[i].z-0.7f},0.35f,0.02f,dark,5);
                    }
                    s.Triangle({-5.6f,2.2f,-22.3f},{-4.6f,2.3f,-24.6f},{-7.4f,2.1f,-23.0f},dark);
                    s.Triangle({-5.6f,2.2f,-22.3f},{-7.4f,2.1f,-23.0f},{-6.9f,2.2f,-21.0f},dark);
                });
                for(int i=0;i<6;++i)
                {
                    const float z=1.5f-2.4f*i,y=12.6f-0.45f*i*(i>2?1.5f:1.0f);
                    s.Limb({0,y-0.3f,z},{0,y+1.5f-0.12f*i,z-0.8f},0.6f-0.05f*i,0.02f,dark,5);
                }
                s.Both([&](float o){
                    // Front legs bent forward at the knee, hind legs with a strong haunch and the
                    // ankle bent back; each pair swings as it walks.
                    s.With(Shapes::About({o*3.0f,8.5f,3.0f},o*0.35f*k.swing,0),[&]{
                        s.Ellipsoid({o*3.1f,8.4f,3.0f},{1.3f,1.8f,1.6f},scale);
                        Leg(s,o,{o*3.2f,8.0f,3.2f},{o*3.7f,4.6f,5.2f},{o*3.7f,1.5f,4.4f},scale,claw,1.05f);
                    });
                    s.With(Shapes::About({o*3.3f,8.5f,-5.4f},-o*0.35f*k.swing,0),[&]{
                        s.Ellipsoid({o*3.3f,8.0f,-5.2f},{1.6f,2.6f,2.4f},scale);
                        Leg(s,o,{o*3.5f,7.0f,-5.0f},{o*3.9f,4.2f,-3.0f},{o*3.8f,1.6f,-5.6f},scale,claw,1.15f);
                    });
                    // The wing: an arm out from the shoulder to the elbow, the hand on to the
                    // tip and fingers fanning back from the elbow, the skin stretched between.
                    // Each joint follows the one before it a little late, so the wing waves
                    // rather than swinging stiffly; raised as it rears.
                    const float raise=0.35f*k.wind+0.3f*k.breathIn,beat=k.time*1.3f;
                    const XMFLOAT3 shoulder{o*3.0f,13.0f,1.0f};
                    const XMMATRIX arm=Shapes::About(shoulder,0.04f*std::sin(beat-0.3f),0,o*(0.1f*std::sin(beat)+raise));
                    const XMMATRIX hand=Shapes::About({o*8.5f,18.5f,-0.5f},0,o*0.06f*std::sin(beat-1.0f),o*(0.14f*std::sin(beat-0.7f)+0.25f*raise))*arm;
                    auto at=[](XMFLOAT3 p,FXMMATRIX m){return F3(XMVector3TransformCoord(XMLoadFloat3(&p),m));};
                    const XMFLOAT3 elbow=at({o*8.5f,18.5f,-0.5f},arm),tip=at({o*14.0f,20.0f,-3.0f},hand);
                    // The skin's edge: by the body, and at the fingers' ends, billowing a little
                    // after the beat.
                    const float sag=0.35f*std::sin(beat-1.4f);
                    const XMFLOAT3 edge[4]={at({o*14.5f,12.5f,-6.0f},hand),at({o*11.0f,9.5f+sag,-7.5f},hand),
                                            at({o*7.0f,9.8f+sag,-7.0f},hand),at({o*3.6f,11.0f,-4.5f},arm)};
                    s.Limb(shoulder,elbow,0.6f,0.45f,dark);
                    s.Ellipsoid(elbow,{0.5f,0.5f,0.5f},dark,{0,0,0},8,5);
                    s.Limb(elbow,tip,0.45f,0.1f,dark);
                    s.Limb(tip,at({o*15.0f,21.2f,-3.4f},hand),0.2f,0.01f,horn,5);
                    for(int i=0;i<3;++i)s.Limb(elbow,edge[i],0.22f,0.06f,dark,5);
                    s.Triangle(shoulder,elbow,edge[3],wing);
                    s.Triangle(elbow,edge[2],edge[3],wing);
                    s.Triangle(elbow,edge[1],edge[2],wing);
                    s.Triangle(elbow,tip,edge[1],wing);
                    s.Triangle(tip,edge[0],edge[1],wing);
                });
            });
            return 23.0f;
        }
    }

    float AddMonster(std::vector<MascotRenderer::GeometryVertex>& out,const Dungeon::Monster& m,float x,float z,float yaw,const MonsterPose& pose,MonsterMouth* mouth)
    {
        // The dragon is huge where its body takes 3x3 tiles (its wings and tail still reach
        // past them); one squeezed into a single tile is drawn smaller.
        const float scale=m.boss?(m.size>0?1.45f:0.8f):1.0f;
        Shapes s{out,XMMatrixScaling(scale,scale,scale)*XMMatrixRotationY(yaw)*XMMatrixTranslation(x,0,z)};
        const Motion k=Of(pose);
        float height=0;
        if(m.boss)height=Dragon(s,m,k,mouth);
        else if(m.id==L"Slime")height=Slime(s,m,k);
        else if(m.id==L"Bat")height=Bat(s,m,k);
        else if(m.id==L"Goblin")height=Goblin(s,m,k);
        else if(m.id==L"Skeleton")height=Skeleton(s,m,k);
        else if(m.id==L"Orc")height=Orc(s,m,k);
        else if(m.id==L"Golem")height=Golem(s,m,k);
        else height=Ghost(s,m,k);
        return height*scale;
    }
}
