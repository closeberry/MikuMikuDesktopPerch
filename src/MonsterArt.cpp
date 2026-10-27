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
        // back, then hit: the blow, each 0..1, one fading as the other comes), and the flinch.
        struct Motion {float swing,hop,wind,hit,flinch,time;bool charging;};

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

        float Dragon(Shapes& s,const Dungeon::Monster& m,const Motion& k)
        {
            // Four legs, a long neck and tail, horns, spines down the back and great wings. It
            // walks with its legs in pairs, tail swaying, wings stirring; it rears its head back
            // and lunges down at her (biting, or breathing fire).
            const Colour scale=Tint(m.color,0.85f),dark=Tint(m.color,0.55f),belly{0.95f,0.75f,0.45f},horn{0.92f,0.86f,0.72f};
            const Colour wing{0.50f,0.12f,0.12f},eye{1.0f,0.85f,0.2f,Glow};
            s.With(Body(k,1.6f*k.hit,0.35f*k.hop,0.08f*k.hit-0.1f*k.wind),[&]{
                s.Ellipsoid({0,9.0f,-1.0f},{4.2f,4.0f,6.5f},scale);
                s.Ellipsoid({0,7.6f,0},{3.4f,2.8f,5.2f},belly);
                // The neck and head, rearing back then striking down.
                s.With(Shapes::About({0,11.0f,3.6f},-0.45f*k.wind+0.5f*k.hit,0.05f*std::sin(k.time*0.9f)),[&]{
                    s.Limb({0,11.0f,3.6f},{0,17.0f,8.0f},2.3f,1.4f,scale);
                    s.Ellipsoid({0,18.2f,9.6f},{2.0f,1.9f,2.6f},scale);
                    s.Box({0,17.5f,12.1f},{1.35f,1.0f,1.6f},scale,{0.1f,0,0});
                    s.With(Shapes::About({0,17.0f,10.6f},0.35f*k.hit,0),[&]{
                        s.Box({0,16.6f,12.0f},{1.2f,0.35f,1.5f},belly,{0.1f,0,0});
                    });
                    s.Both([&](float o){
                        s.Limb({o*0.9f,19.4f,9.0f},{o*1.7f,22.6f,6.4f},0.55f,0.02f,horn,6);
                        s.Ellipsoid({o*1.25f,18.8f,11.0f},{0.32f,0.3f,0.2f},eye);
                    });
                });
                // The tail, swaying.
                s.With(Shapes::About({0,8.0f,-7.0f},0,0.2f*std::sin(k.time*1.4f)+0.25f*k.swing),[&]{
                    const XMFLOAT3 tail[5]={{0,8.0f,-7.0f},{0,6.2f,-11.5f},{-1.5f,4.4f,-15.5f},{-3.2f,3.2f,-18.5f},{-4.6f,2.6f,-20.5f}};
                    const float tailR[5]={2.4f,1.7f,1.1f,0.6f,0.1f};
                    for(int i=0;i+1<5;++i)s.Limb(tail[i],tail[i+1],tailR[i],tailR[i+1],scale);
                });
                for(int i=0;i<7;++i)
                {
                    const float z=6.0f-2.4f*i,y=13.0f-0.55f*i*(i>3?1.6f:1.0f);
                    s.Limb({0,y-0.3f,z},{0,y+1.4f-0.12f*i,z-0.8f},0.6f-0.05f*i,0.02f,dark,5);
                }
                s.Both([&](float o){
                    s.With(Shapes::About({o*3.1f,7.5f,3.2f},o*0.35f*k.swing,0),[&]{
                        s.Limb({o*3.1f,7.5f,3.2f},{o*3.6f,1.0f,4.4f},1.5f,1.0f,scale);
                        s.Ellipsoid({o*3.6f,0.6f,5.0f},{1.2f,0.6f,1.6f},dark);
                    });
                    s.With(Shapes::About({o*3.4f,7.5f,-4.6f},-o*0.35f*k.swing,0),[&]{
                        s.Limb({o*3.4f,7.5f,-4.6f},{o*3.9f,1.0f,-4.2f},1.7f,1.1f,scale);
                        s.Ellipsoid({o*3.9f,0.6f,-3.6f},{1.3f,0.6f,1.7f},dark);
                    });
                    // The wing: a bone out from the shoulder, the skin hung from it in a fan;
                    // stirring, and raised as it rears.
                    const XMFLOAT3 shoulder{o*3.0f,13.0f,1.0f};
                    s.With(Shapes::About(shoulder,0,0,o*(0.12f*std::sin(k.time*1.3f)+0.35f*k.wind)),[&]{
                        const XMFLOAT3 elbow{o*8.5f,18.5f,-0.5f},tip{o*14.0f,20.0f,-3.0f};
                        const XMFLOAT3 edge[4]={{o*14.5f,12.5f,-6.0f},{o*11.0f,9.5f,-7.5f},{o*7.0f,9.8f,-7.0f},{o*3.6f,11.0f,-4.5f}};
                        s.Limb(shoulder,elbow,0.6f,0.45f,dark);
                        s.Limb(elbow,tip,0.45f,0.1f,dark);
                        s.Triangle(shoulder,elbow,edge[3],wing);
                        s.Triangle(elbow,edge[2],edge[3],wing);
                        s.Triangle(elbow,edge[1],edge[2],wing);
                        s.Triangle(elbow,tip,edge[1],wing);
                        s.Triangle(tip,edge[0],edge[1],wing);
                    });
                });
            });
            return 23.0f;
        }
    }

    float AddMonster(std::vector<MascotRenderer::GeometryVertex>& out,const Dungeon::Monster& m,float x,float z,float yaw,const MonsterPose& pose)
    {
        // The dragon is huge where its body takes 3x3 tiles (its wings and tail still reach
        // past them); one squeezed into a single tile is drawn smaller.
        const float scale=m.boss?(m.size>0?1.45f:0.8f):1.0f;
        Shapes s{out,XMMatrixScaling(scale,scale,scale)*XMMatrixRotationY(yaw)*XMMatrixTranslation(x,0,z)};
        const Motion k=Of(pose);
        float height=0;
        if(m.boss)height=Dragon(s,m,k);
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
