#define NOMINMAX
#include "DungeonWindow.hpp"
#include "Dungeon.hpp"
#include "Text.hpp"
#include "Log.hpp"
#include "MascotRenderer.hpp"
#include "MascotModel.hpp"
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <memory>
#pragma comment(lib,"gdiplus.lib")

namespace MikuMikuDesktopPerch
{
    namespace
    {
        using namespace Gdiplus;
        constexpr wchar_t ClassName[]=L"MikuMikuDesktopPerchDungeon";
        constexpr wchar_t ViewClassName[]=L"MikuMikuDesktopPerchDungeonView";
        // The 3D view: a tile's side in MMD units (a character is about 20 tall), and the walls'
        // height (low, so a camera from above sees over them).
        constexpr float TileSize=10,WallHeight=4.5f;
        // Patterns the geometry shader draws, by the colour's alpha (see GeometryPS.hlsl).
        constexpr float Lit=1,Stones=0.75f,Bricks=0.5f,Glow=0.25f;

        struct
        {
            HWND window=nullptr;
            ULONG_PTR gdiplus=0;
            Dungeon::Game game;
            DungeonHero hero;
            std::unique_ptr<Bitmap> face;
            std::function<void(const DungeonResult&)> onEnd;
            bool confirmQuit=false,ended=false;
            // The 3D view (a child window over the map's place) and its renderer, only while
            // open. sceneKey: the turn and floor the geometry was last built for.
            HWND view=nullptr;
            std::unique_ptr<MascotRenderer> renderer;
            bool show3D=true;
            long long sceneKey=-1;
            float drawnX=0,drawnZ=0,drawnAngle=0;   // where she is drawn, walking toward her tile
            bool placed=false;
            Activity gait=Activity::Idle;           // walking or running there, or standing
            int floor=0;                            // the floor she was placed on (a new one: placed again)
            bool hurry=false;                       // a move key held down: she runs
            // The move key held down (0: none) and since when (GetTickCount64): while it is down,
            // the view keeps her going; after RunAfter, running.
            WPARAM heldKey=0;
            ULONGLONG heldSince=0;
            WPARAM queuedKey=0;   // a move pressed while she was still on her way: her next step, if still held
            // The camera: from above (as in Shiren) or behind her, easing between.
            bool behind=false;
            DirectX::XMFLOAT3 eye{},target{};
            float cameraAngle=0;
        } state;

        int Px(int logical){return MulDiv(logical,state.window?static_cast<int>(GetDpiForWindow(state.window)):96,96);}

        Color Argb(std::uint32_t c){return Color(static_cast<BYTE>(c>>24),static_cast<BYTE>(c>>16),static_cast<BYTE>(c>>8),static_cast<BYTE>(c));}

        // Where the map goes in the client area: left of the panel, above the log.
        RECT MapArea()
        {
            RECT client{};
            GetClientRect(state.window,&client);
            const int panel=Px(250),logHeight=Px(128),pad=Px(10);
            return {pad,pad,std::max<LONG>(pad+1,client.right-panel-pad),std::max<LONG>(pad+1,client.bottom-logHeight-pad)};
        }

        bool Showing3D(){return state.renderer&&state.show3D;}

        void PlaceView()
        {
            if(!state.view)return;
            const RECT r=MapArea();
            MoveWindow(state.view,r.left,r.top,r.right-r.left,r.bottom-r.top,TRUE);
            ShowWindow(state.view,Showing3D()?SW_SHOWNA:SW_HIDE);
            if(state.renderer)
            {
                std::wstring error;
                if(!state.renderer->Resize(static_cast<unsigned>(r.right-r.left),static_cast<unsigned>(r.bottom-r.top),error))Log(L"dungeon: resize: %s",error.c_str());
            }
        }

        void ReleaseView()
        {
            // The renderer's memory (its buffers, the model's textures) goes now.
            if(state.renderer){state.renderer->WaitIdle();state.renderer.reset();Log(L"dungeon: 3D view released");}
            state.view=nullptr;
        }

        // The floor as boxes and plates, from what she has seen: lit where in sight, dim where
        // only remembered. Map y runs toward -z, so up on the map is away from the camera.
        std::vector<MascotRenderer::GeometryVertex> BuildScene()
        {
            std::vector<MascotRenderer::GeometryVertex> out;
            const auto& game=state.game;
            auto quad=[&](const float (&a)[3],const float (&b)[3],const float (&c)[3],const float (&d)[3],const float (&n)[3],const float (&col)[4]){
                for(const auto* p:{&a,&b,&c,&a,&c,&d})
                    out.push_back({{(*p)[0],(*p)[1],(*p)[2]},{n[0],n[1],n[2]},{col[0],col[1],col[2],col[3]}});
            };
            auto box=[&](float x0,float y0,float z0,float x1,float y1,float z1,const float (&c)[4]){
                quad({x0,y1,z0},{x1,y1,z0},{x1,y1,z1},{x0,y1,z1},{0,1,0},c);    // top
                quad({x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},{0,0,-1},c);   // front (toward the camera)
                quad({x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1},{0,0,1},c);
                quad({x0,y0,z0},{x0,y0,z1},{x0,y1,z1},{x0,y1,z0},{-1,0,0},c);
                quad({x1,y0,z0},{x1,y0,z1},{x1,y1,z1},{x1,y1,z0},{1,0,0},c);
            };
            auto shade=[](const float (&c)[3],float k,float pattern=Lit)->std::array<float,4>{return {c[0]*k,c[1]*k,c[2]*k,pattern};};
            auto with=[](const std::array<float,4>& a,auto f){float c[4]={a[0],a[1],a[2],a[3]};return f(c);};
            constexpr float T=TileSize,gap=0;
            constexpr float floorColor[3]={0.50f,0.46f,0.58f},wallColor[3]={0.56f,0.24f,0.24f};
            for(int y=0;y<Dungeon::Game::Height;++y)
                for(int x=0;x<Dungeon::Game::Width;++x)
                {
                    if(game.At(x,y)==Dungeon::Tile::Wall||!game.Seen(x,y))continue;
                    const float k=game.Visible(x,y)?1.0f:0.45f;
                    const float x0=x*T,x1=(x+1)*T,z1=-y*T,z0=-(y+1)*T;
                    with(shade(floorColor,k,Stones),[&](const float (&c)[4]){quad({x0+gap,0,z0+gap},{x1-gap,0,z0+gap},{x1-gap,0,z1-gap},{x0+gap,0,z1-gap},{0,1,0},c);});
                    // A low wall along each side that meets rock.
                    const float th=0.18f*T;
                    with(shade(wallColor,k,Bricks),[&](const float (&c)[4]){
                        if(game.At(x,y-1)==Dungeon::Tile::Wall)box(x0,0,z1,x1,WallHeight,z1+th,c);
                        if(game.At(x,y+1)==Dungeon::Tile::Wall)box(x0,0,z0-th,x1,WallHeight,z0,c);
                        if(game.At(x-1,y)==Dungeon::Tile::Wall)box(x0-th,0,z0-th,x0,WallHeight,z1+th,c);
                        if(game.At(x+1,y)==Dungeon::Tile::Wall)box(x1,0,z0-th,x1+th,WallHeight,z1+th,c);
                    });
                    if(game.At(x,y)==Dungeon::Tile::Stairs)
                    {
                        // Steps going down into a dark well.
                        with(shade(floorColor,0.15f),[&](const float (&c)[4]){quad({x0+0.12f*T,0.05f,z0+0.12f*T},{x1-0.12f*T,0.05f,z0+0.12f*T},{x1-0.12f*T,0.05f,z1-0.12f*T},{x0+0.12f*T,0.05f,z1-0.12f*T},{0,1,0},c);});
                        const float gold[4]={0.90f*k,0.76f*k,0.35f*k,1};
                        for(int i=0;i<3;++i)box(x0+0.15f*T,0,z1-(0.2f+0.22f*i)*T-0.16f*T,x1-0.15f*T,(0.3f-0.1f*i)*T*0.5f,z1-(0.2f+0.22f*i)*T,gold);
                    }
                }
            for(const auto& c:game.Chests())
                if(game.Seen(c.x,c.y))
                {
                    const float cx=(c.x+0.5f)*T,cz=-(c.y+0.5f)*T,k=game.Visible(c.x,c.y)?1.0f:0.5f;
                    const float body[4]={0.84f*k,0.60f*k,0.22f*k,1},lid[4]={0.55f*k,0.33f*k,0.12f*k,1};
                    box(cx-0.32f*T,0,cz-0.22f*T,cx+0.32f*T,0.36f*T,cz+0.22f*T,body);
                    box(cx-0.34f*T,0.36f*T,cz-0.24f*T,cx+0.34f*T,0.46f*T,cz+0.24f*T,lid);
                }
            for(const auto& m:game.Monsters())
                if(game.Visible(m.x,m.y))
                {
                    // A body and a head in its colour, bigger for a boss.
                    const float cx=(m.x+0.5f)*T,cz=-(m.y+0.5f)*T,s=m.boss?1.6f:1.0f;
                    const float r=((m.color>>16)&255)/255.0f,g=((m.color>>8)&255)/255.0f,b=(m.color&255)/255.0f;
                    const float body[4]={r,g,b,1},head[4]={r*0.85f,g*0.85f,b*0.85f,1};
                    box(cx-0.3f*T*s,0,cz-0.3f*T*s,cx+0.3f*T*s,0.6f*T*s,cz+0.3f*T*s,body);
                    box(cx-0.2f*T*s,0.6f*T*s,cz-0.2f*T*s,cx+0.2f*T*s,0.95f*T*s,cz+0.2f*T*s,head);
                    // Its health above it when hurt.
                    if(m.hp<m.maxHp)
                    {
                        const float w=0.8f*T,left=cx-w/2,top=1.15f*T*s,f=static_cast<float>(std::max(0,m.hp))/m.maxHp;
                        const float back[4]={0.15f,0.12f,0.18f,1},red[4]={0.9f,0.3f,0.3f,1};
                        box(left,top,cz-0.05f*T,left+w,top+0.08f*T,cz+0.05f*T,back);
                        box(left,top+0.01f*T,cz-0.07f*T,left+w*f,top+0.09f*T,cz+0.07f*T,red);
                    }
                }
            for(const auto& [x,y]:game.Bolt())
            {
                const float cx=(x+0.5f)*T,cz=-(y+0.5f)*T,glow[4]={1.0f,0.92f,0.55f,Glow};
                box(cx-0.12f*T,0.45f*T,cz-0.12f*T,cx+0.12f*T,0.69f*T,cz+0.12f*T,glow);
            }
            return out;
        }

        void Finish()
        {
            if(state.ended)return;
            state.ended=true;
            ReleaseView();
            const auto& g=state.game;
            DungeonResult result{g.Floor(),g.Kills(),g.Turns(),g.GetState()==Dungeon::Game::State::Dead};
            auto onEnd=std::move(state.onEnd);
            DestroyWindow(state.window);
            if(onEnd)onEnd(result);
        }

        void Text(Graphics& g,const std::wstring& text,float x,float y,float size,Color color,bool bold=false)
        {
            FontFamily family(L"Yu Gothic UI");
            Font font(family.IsAvailable()?&family:FontFamily::GenericSansSerif(),size,bold?FontStyleBold:FontStyleRegular,UnitPixel);
            SolidBrush brush(color);
            g.DrawString(text.c_str(),static_cast<INT>(text.size()),&font,PointF(x,y),&brush);
        }

        void Bar(Graphics& g,float x,float y,float w,float h,int value,int max,Color fill)
        {
            SolidBrush back(Color(255,40,36,52)),front(fill);
            g.FillRectangle(&back,x,y,w,h);
            if(max>0)g.FillRectangle(&front,x,y,w*std::clamp(value,0,max)/max,h);
        }

        void Paint(HDC dc)
        {
            RECT client{};
            GetClientRect(state.window,&client);
            const int cw=std::max(1L,client.right),ch=std::max(1L,client.bottom);
            Bitmap canvas(cw,ch,PixelFormat32bppPARGB);
            Graphics g(&canvas);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            g.Clear(Color(255,20,18,28));

            const auto& game=state.game;
            const auto& hero=game.Hero();
            const float panel=static_cast<float>(Px(250)),logHeight=static_cast<float>(Px(128)),pad=static_cast<float>(Px(10));
            const float areaW=cw-panel-2*pad,areaH=ch-logHeight-2*pad;
            // Close up, as in Shiren: about 15 by 9.5 tiles in view, so one room fills most of
            // it, the view following her. The whole floor is in the small map beside it.
            constexpr int W=Dungeon::Game::Width,H=Dungeon::Game::Height;
            const float tile=std::max(8.0f,std::floor(std::min(areaW/15,areaH/9.5f)));
            // She stays in the middle, even near the edge of the floor (only darkness lies beyond).
            const float ox=pad+std::floor(areaW/2-(hero.x+0.5f)*tile),oy=pad+std::floor(areaH/2-(hero.y+0.5f)*tile);
            auto at=[&](int x,int y){return PointF(ox+x*tile,oy+y*tile);};
            g.SetClip(RectF(pad,pad,areaW,areaH));
            if(!Showing3D())
            {

            // Floors seen (dim) and in sight (lit), and the walls of rooms seen, as in a map.
            // Tiles edge to edge without smoothing, so no seams show between them.
            SolidBrush lit(Color(255,78,72,98)),dim(Color(255,40,37,54));
            g.SetSmoothingMode(SmoothingModeNone);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            for(int y=0;y<H;++y)
                for(int x=0;x<W;++x)
                    if(game.At(x,y)!=Dungeon::Tile::Wall&&game.Seen(x,y))
                    {
                        const PointF p=at(x,y);
                        g.FillRectangle(game.Visible(x,y)?&lit:&dim,p.X,p.Y,tile,tile);
                    }
            g.SetPixelOffsetMode(PixelOffsetModeDefault);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            Pen wall(Color(255,138,28,43),std::max(1.5f,tile/7));
            for(const auto& r:game.Rooms())
            {
                bool any=false;
                for(int y=r.y;y<r.y+r.h&&!any;++y)for(int x=r.x;x<r.x+r.w&&!any;++x)any=game.Seen(x,y);
                if(any){const PointF p=at(r.x,r.y);g.DrawRectangle(&wall,p.X,p.Y,r.w*tile,r.h*tile);}
            }
            for(int y=0;y<Dungeon::Game::Height;++y)
                for(int x=0;x<Dungeon::Game::Width;++x)
                    if(game.At(x,y)==Dungeon::Tile::Stairs&&game.Seen(x,y))
                    {
                        // Steps going down.
                        const PointF p=at(x,y);
                        SolidBrush step(Color(255,230,196,90));
                        for(int i=0;i<3;++i)g.FillRectangle(&step,p.X+tile*(0.15f+0.12f*i),p.Y+tile*(0.2f+0.22f*i),tile*(0.7f-0.24f*i),tile*0.14f);
                    }
            for(const auto& c:game.Chests())
                if(game.Seen(c.x,c.y))
                {
                    const PointF p=at(c.x,c.y);
                    SolidBrush box(Color(255,214,160,60)),lid(Color(255,150,96,30));
                    g.FillRectangle(&box,p.X+tile*0.15f,p.Y+tile*0.3f,tile*0.7f,tile*0.55f);
                    g.FillRectangle(&lid,p.X+tile*0.15f,p.Y+tile*0.3f,tile*0.7f,tile*0.14f);
                }
            SolidBrush flash(Color(150,255,236,140));
            for(const auto& [x,y]:game.Bolt()){const PointF p=at(x,y);g.FillEllipse(&flash,p.X+tile*0.2f,p.Y+tile*0.2f,tile*0.6f,tile*0.6f);}
            for(const auto& m:game.Monsters())
                if(game.Visible(m.x,m.y))
                {
                    const PointF p=at(m.x,m.y);
                    const float grow=m.boss?tile*0.25f:0;
                    SolidBrush body(Argb(m.color));
                    g.FillEllipse(&body,p.X+tile*0.08f-grow,p.Y+tile*0.08f-grow,tile*0.84f+2*grow,tile*0.84f+2*grow);
                    Text(g,m.name.substr(0,1),p.X+tile*0.18f,p.Y+tile*0.06f,tile*0.6f,Color(255,20,18,28),true);
                    if(m.hp<m.maxHp)Bar(g,p.X,p.Y-tile*0.2f-grow,tile,tile*0.12f,m.hp,m.maxHp,Color(255,230,80,80));
                }

            // The hero: her face in a ring, and a notch for the way she faces.
            {
                const PointF p=at(hero.x,hero.y);
                const float size=tile*1.1f,fx=p.X+tile/2-size/2,fy=p.Y+tile/2-size/2;
                if(state.face)
                {
                    GraphicsPath circle;
                    circle.AddEllipse(fx,fy,size,size);
                    Region map;
                    g.GetClip(&map);
                    g.SetClip(&circle,CombineModeIntersect);
                    SolidBrush back(Color(255,250,240,245));
                    g.FillEllipse(&back,fx,fy,size,size);
                    g.DrawImage(state.face.get(),fx,fy,size,size);
                    g.SetClip(&map);
                }
                else
                {
                    SolidBrush body(Color(255,255,140,180));
                    g.FillEllipse(&body,fx,fy,size,size);
                    Text(g,state.hero.name.substr(0,1),fx+size*0.2f,fy+size*0.1f,size*0.55f,Color(255,255,255,255),true);
                }
                Pen ring(Color(255,255,255,255),std::max(1.5f,tile/8));
                g.DrawEllipse(&ring,fx,fy,size,size);
                SolidBrush notch(Color(255,255,230,120));
                const float cx=p.X+tile/2+hero.dirX*size*0.62f,cy=p.Y+tile/2+hero.dirY*size*0.62f;
                g.FillEllipse(&notch,cx-tile*0.15f,cy-tile*0.15f,tile*0.3f,tile*0.3f);
            }
            }

            g.ResetClip();

            // Beside the map: her state, what she wears and carries, and the keys.
            const float px=cw-panel,line=static_cast<float>(Px(20));
            float y=pad;
            const Color white(255,236,232,245),grey(255,160,154,180),gold(255,240,200,110);
            Text(g,state.hero.name,px,y,static_cast<float>(Px(17)),white,true);y+=line*1.3f;
            Text(g,Fmt(T(L"{0}階　レベル{1}",L"Floor {0}  Level {1}"),{std::to_wstring(game.Floor()),std::to_wstring(hero.level)}),px,y,static_cast<float>(Px(14)),gold);y+=line*1.1f;
            Text(g,Fmt(L"HP {0}/{1}",{std::to_wstring(hero.hp),std::to_wstring(hero.MaxHp())}),px,y,static_cast<float>(Px(13)),white);y+=line*0.9f;
            Bar(g,px,y,panel-pad*2,static_cast<float>(Px(7)),hero.hp,hero.MaxHp(),Color(255,110,210,120));y+=line*0.6f;
            Text(g,Fmt(L"MP {0}/{1}",{std::to_wstring(hero.mp),std::to_wstring(hero.MaxMp())}),px,y,static_cast<float>(Px(13)),white);y+=line*0.9f;
            Bar(g,px,y,panel-pad*2,static_cast<float>(Px(7)),hero.mp,hero.MaxMp(),Color(255,110,160,240));y+=line*0.7f;
            Text(g,Fmt(T(L"攻撃 {0}　防御 {1}　魔力 {2}",L"ATK {0}  DEF {1}  MAG {2}"),{std::to_wstring(hero.Atk()),std::to_wstring(hero.Def()),std::to_wstring(hero.Mag())}),
                 px,y,static_cast<float>(Px(13)),white);y+=line*1.4f;

            // The whole floor, small: where she has been, the stairs once seen, foes in sight.
            {
                const float scale=std::floor((panel-pad*2)/W*4)/4,mw=scale*W,mh=scale*H;
                SolidBrush frame(Color(255,30,27,42)),seenTile(Color(255,96,90,120)),litTile(Color(255,150,142,180));
                g.FillRectangle(&frame,px-2,y-2,mw+4,mh+4);
                g.SetSmoothingMode(SmoothingModeNone);
                for(int ty=0;ty<H;++ty)
                    for(int tx=0;tx<W;++tx)
                        if(game.At(tx,ty)!=Dungeon::Tile::Wall&&game.Seen(tx,ty))
                            g.FillRectangle(game.Visible(tx,ty)?&litTile:&seenTile,px+tx*scale,y+ty*scale,scale,scale);
                g.SetSmoothingMode(SmoothingModeAntiAlias);
                auto dot=[&](int tx,int ty,Color c,float r){SolidBrush b(c);g.FillEllipse(&b,px+(tx+0.5f)*scale-r,y+(ty+0.5f)*scale-r,2*r,2*r);};
                const float r=std::max(2.0f,scale*1.2f);
                for(int ty=0;ty<H;++ty)for(int tx=0;tx<W;++tx)if(game.At(tx,ty)==Dungeon::Tile::Stairs&&game.Seen(tx,ty))dot(tx,ty,Color(255,240,200,110),r);
                for(const auto& m:game.Monsters())if(game.Visible(m.x,m.y))dot(m.x,m.y,Color(255,235,90,90),r);
                dot(hero.x,hero.y,Color(255,255,140,180),r*1.2f);
                y+=mh+line*0.8f;
            }

            Text(g,T(L"持ち物（数字キーで使う・装備）",L"Bag (number keys: use, equip)"),px,y,static_cast<float>(Px(12)),grey);y+=line;
            for(size_t i=0;i<hero.bag.size();++i)
            {
                const auto& item=hero.bag[i];
                const bool on=hero.Equipped(static_cast<int>(i));
                std::wstring label=std::to_wstring((i+1)%10)+L"  "+item.name;
                if(item.atk)label+=L" "+std::wstring(T(L"攻",L"A"))+L"+"+std::to_wstring(item.atk);
                if(item.def)label+=L" "+std::wstring(T(L"防",L"D"))+L"+"+std::to_wstring(item.def);
                if(item.mag)label+=L" "+std::wstring(T(L"魔",L"M"))+L"+"+std::to_wstring(item.mag);
                if(on)label+=T(L"［装備中］",L" [on]");
                Text(g,label,px,y,static_cast<float>(Px(13)),on?gold:white);y+=line*0.95f;
            }
            if(hero.bag.empty()){Text(g,T(L"（なし）",L"(empty)"),px,y,static_cast<float>(Px(13)),grey);y+=line;}
            const float help=ch-pad-line*3.2f;
            Text(g,state.behind&&Showing3D()?T(L"↑：進む　←→：向きを変える　↓：振り向く\nF：魔法（MP3）　スペース：待つ\nV：カメラ　M：2D・3D　Esc：帰る",L"Up: go  Left/Right: turn  Down: turn round\nF: magic (3 MP)  Space: wait\nV: camera  M: 2D/3D  Esc: go home")
                                             :T(L"矢印・WASD：移動（敵へ：剣）\nF：魔法（MP3）　スペース：待つ\nV：カメラ　M：2D・3D　Esc：帰る",L"Arrows/WASD: move (into a foe: sword)\nF: magic (3 MP)  Space: wait\nV: camera  M: 2D/3D  Esc: go home"),
                 px,std::max(y,help),static_cast<float>(Px(12)),grey);

            // A question over everything when there is one.
            std::wstring ask;
            if(state.confirmQuit)ask=T(L"冒険をやめて、デスクトップに帰る？\n\nY：帰る　　N：続ける",L"Give up and go back to the desktop?\n\nY: go back    N: keep going");
            else if(game.GetState()==Dungeon::Game::State::AtStairs)
                ask=T(L"階段だ！\n\nEnter：次の階へ　　B：デスクトップに帰る",L"The stairs!\n\nEnter: the next floor    B: back to the desktop");
            else if(game.GetState()==Dungeon::Game::State::Dead)
                ask=Fmt(T(L"やられてしまった…（{0}階）\n\nEnter：デスクトップに帰る",L"Defeated... (floor {0})\n\nEnter: back to the desktop"),{std::to_wstring(game.Floor())});
            // What happened, newest last (in 3D, the question takes its place: the view hides
            // what is drawn under it).
            float ly=ch-logHeight+pad/2;
            if(Showing3D()&&!ask.empty())
            {
                SolidBrush box(Color(255,40,34,58));
                g.FillRectangle(&box,pad,ly-pad/4,areaW,logHeight-pad);
                Text(g,ask,pad*2,ly,static_cast<float>(Px(15)),gold);
                ask.clear();
            }
            else for(const auto& entry:game.Log()){Text(g,entry,pad,ly,static_cast<float>(Px(14)),white);ly+=line;}
            if(!ask.empty())
            {
                const float bw=static_cast<float>(Px(420)),bh=static_cast<float>(Px(120));
                const float bx=(cw-panel-bw)/2,by=(ch-logHeight-bh)/2;
                SolidBrush box(Color(235,30,26,44));
                Pen edge(Color(255,240,200,110),2);
                g.FillRectangle(&box,bx,by,bw,bh);
                g.DrawRectangle(&edge,bx,by,bw,bh);
                Text(g,ask,bx+pad*2,by+pad*2,static_cast<float>(Px(16)),white);
            }

            Graphics screen(dc);
            screen.DrawImage(&canvas,0,0);
        }

        // In 3D, still on her way to her tile (more than a third of one to go).
        bool Underway()
        {
            if(!Showing3D()||!state.placed)return false;
            const auto& h=state.game.Hero();
            const float dx=(h.x+0.5f)*TileSize-state.drawnX,dz=-(h.y+0.5f)*TileSize-state.drawnZ;
            return dx*dx+dz*dz>TileSize*TileSize*0.11f;
        }

        constexpr ULONGLONG RunAfter=250;   // milliseconds a move key is held before she runs

        // The step a move key asks for (in 3D from behind her, up goes the way she faces); false
        // for keys that only turn her or are not moves.
        bool StepFor(WPARAM key,int& dx,int& dy)
        {
            dx=dy=0;
            if(key==VK_LEFT||key=='A')dx=-1;
            else if(key==VK_RIGHT||key=='D')dx=1;
            else if(key==VK_UP||key=='W')dy=-1;
            else if(key==VK_DOWN||key=='S')dy=1;
            if(!dx&&!dy)return false;
            if(Showing3D()&&state.behind)
            {
                if(dy>=0)return false;
                const auto& h=state.game.Hero();
                dx=h.dirX;dy=h.dirY;
            }
            return true;
        }

        // repeat: the key is held down (the keyboard's auto-repeat).
        void OnKey(WPARAM key,bool repeat)
        {
            auto& game=state.game;
            using State=Dungeon::Game::State;
            if(state.confirmQuit)
            {
                if(key=='Y'||key==VK_RETURN){Finish();return;}
                if(key=='N'||key==VK_ESCAPE)state.confirmQuit=false;
            }
            else if(game.GetState()==State::Dead)
            {
                if(key==VK_RETURN||key==VK_ESCAPE||key==VK_SPACE){Finish();return;}
            }
            else if(key==VK_ESCAPE)state.confirmQuit=true;
            else if(key=='M'&&state.renderer){state.show3D=!state.show3D;PlaceView();}
            else if(key=='V'&&Showing3D())state.behind=!state.behind;
            else if(game.GetState()==State::AtStairs&&key==VK_RETURN)game.Descend();
            else if(game.GetState()==State::AtStairs&&key=='B'){Finish();return;}
            else
            {
                int dx=0,dy=0;
                if(key==VK_LEFT||key=='A')dx=-1;
                else if(key==VK_RIGHT||key=='D')dx=1;
                else if(key==VK_UP||key=='W')dy=-1;
                else if(key==VK_DOWN||key=='S')dy=1;
                // A step at a time as she walks it. In 3D a key held down is followed by the view
                // (TickDungeonWindow: she keeps going, and runs after RunAfter), not by the
                // keyboard's repeat, and a key pressed while she is still on her way waits.
                int sx,sy;
                const bool step=StepFor(key,sx,sy);
                if((dx||dy)&&Showing3D()&&repeat)return;
                if(step&&Showing3D())
                {
                    state.heldKey=key;state.heldSince=GetTickCount64();state.hurry=false;
                    // Still on her way: the step waits until she gets there, and is taken only if the
                    // key is still down then (a quick 右 then 下 is not two steps).
                    if(Underway()){state.queuedKey=key;return;}
                }
                if((dx||dy)&&Showing3D()&&state.behind)
                {
                    // Behind her: up goes the way she faces, left and right turn her, down turns
                    // her round. Turning takes no turn.
                    const auto& h=game.Hero();
                    if(dy<0){game.StepOffStairs();game.Move(h.dirX,h.dirY);}
                    else if(dx<0)game.Face(h.dirY,-h.dirX);
                    else if(dx>0)game.Face(-h.dirY,h.dirX);
                    else game.Face(-h.dirX,-h.dirY);
                }
                else if(dx||dy){game.StepOffStairs();game.Move(dx,dy);}
                else if(game.GetState()!=State::Playing)return;
                else if(key=='F')game.Cast();
                else if(key==VK_SPACE||key==VK_OEM_PERIOD)game.Rest();
                else if(key>='1'&&key<='9')game.Use(static_cast<int>(key-'1'));
                else if(key=='0')game.Use(9);
                else if(key>=VK_NUMPAD1&&key<=VK_NUMPAD9)game.Use(static_cast<int>(key-VK_NUMPAD1));
                else return;
            }
            InvalidateRect(state.window,nullptr,FALSE);
        }

        LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            switch(message)
            {
            case WM_KEYDOWN:
                OnKey(wParam,(lParam&(1<<30))!=0);
                return 0;
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC dc=BeginPaint(hwnd,&ps);
                Paint(dc);
                EndPaint(hwnd,&ps);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_SIZE:
                PlaceView();
                InvalidateRect(hwnd,nullptr,FALSE);
                return 0;
            case WM_CLOSE:
                if(!state.ended)Finish();
                return 0;
            case WM_DESTROY:
                ReleaseView();
                state.window=nullptr;
                state.face.reset();
                return 0;
            }
            return DefWindowProcW(hwnd,message,wParam,lParam);
        }

        // The 3D view takes no input of its own: keys go to the window, clicks give it the focus.
        LRESULT CALLBACK ViewProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            switch(message)
            {
            case WM_KEYDOWN:
                OnKey(wParam,(lParam&(1<<30))!=0);
                return 0;
            case WM_LBUTTONDOWN:
                SetFocus(state.window);
                return 0;
            case WM_ERASEBKGND:
                return 1;
            }
            return DefWindowProcW(hwnd,message,wParam,lParam);
        }

        // The 3D view in the map's place, with a renderer of its own: no MSAA or blur, small
        // textures, no self-shadow (and so the smallest shadow map), opaque. False: 2D only.
        bool CreateView()
        {
            const HINSTANCE instance=GetModuleHandleW(nullptr);
            static bool registered=false;
            if(!registered)
            {
                WNDCLASSEXW wc{sizeof(wc)};
                wc.lpfnWndProc=ViewProc;
                wc.hInstance=instance;
                wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
                wc.lpszClassName=ViewClassName;
                RegisterClassExW(&wc);
                registered=true;
            }
            const RECT r=MapArea();
            state.view=CreateWindowExW(0,ViewClassName,L"",WS_CHILD|WS_CLIPSIBLINGS,r.left,r.top,r.right-r.left,r.bottom-r.top,
                                       state.window,nullptr,instance,nullptr);
            if(!state.view||!state.hero.model)return false;
            auto renderer=std::make_unique<MascotRenderer>();
            renderer->SetShadowMapSize(16);
            renderer->SetSelfShadow(false);
            renderer->SetBackground(20/255.0f,18/255.0f,28/255.0f,1);
            std::wstring error;
            // Light: no MSAA, small textures, three quarters of the pixels drawn.
            const bool light=state.hero.light;
            if(light)renderer->SetResolutionScale(0.75f);
            if(!renderer->Initialize(state.view,static_cast<unsigned>(r.right-r.left),static_cast<unsigned>(r.bottom-r.top),light?1:state.hero.msaa,false,error)||
               !renderer->LoadModel(*state.hero.model,state.hero.toonDir,light?512:state.hero.maxTexture,error))
            {
                Log(L"dungeon: no 3D view (%s)",error.c_str());
                renderer->WaitIdle();
                DestroyWindow(state.view);
                state.view=nullptr;
                return false;
            }
            Log(L"dungeon: 3D view %ldx%ld on %s (%s, MSAA x%u)",r.right-r.left,r.bottom-r.top,renderer->AdapterName().c_str(),
                light?L"light":L"full",renderer->SampleCount());
            state.renderer=std::move(renderer);
            state.sceneKey=-1;
            state.placed=false;
            state.gait=Activity::Idle;
            return true;
        }
    }

    bool DungeonWindowOpen(){return state.window!=nullptr;}

    void TickDungeonWindow(MascotModel& model,double dt)
    {
        if(!state.window||!Showing3D())return;
        auto& r=*state.renderer;
        const auto& game=state.game;
        const long long key=static_cast<long long>(game.Floor())*1000000+game.Turns()*4+static_cast<int>(game.GetState());
        if(key!=state.sceneKey)
        {
            state.sceneKey=key;
            r.SetGeometry(BuildScene());
        }
        using namespace DirectX;
        // A move pressed on her way: taken as soon as she is nearly there, if its key is still
        // down; let go by then, it is dropped.
        if(state.queuedKey&&!Underway())
        {
            int sx,sy;
            if((GetAsyncKeyState(static_cast<int>(state.queuedKey))&0x8000)&&GetForegroundWindow()==state.window&&
               state.game.GetState()!=Dungeon::Game::State::Dead&&!state.confirmQuit&&StepFor(state.queuedKey,sx,sy))
            {
                state.game.StepOffStairs();
                state.game.Move(sx,sy);
                InvalidateRect(state.window,nullptr,FALSE);
            }
            state.queuedKey=0;
        }
        // A move key still held: running after RunAfter, and the next step as soon as she is
        // nearly there (the same as pressing it again).
        if(state.heldKey)
        {
            int sx,sy;
            if(!(GetAsyncKeyState(static_cast<int>(state.heldKey))&0x8000)||GetForegroundWindow()!=state.window||
               game.GetState()!=Dungeon::Game::State::Playing||state.confirmQuit||!StepFor(state.heldKey,sx,sy))
                state.heldKey=0;
            else
            {
                if(GetTickCount64()-state.heldSince>=RunAfter)state.hurry=true;
                if(state.hurry&&!Underway())
                {
                    state.game.StepOffStairs();
                    state.game.Move(sx,sy);
                    InvalidateRect(state.window,nullptr,FALSE);
                }
            }
        }
        const auto& hero=game.Hero();
        const float tx=(hero.x+0.5f)*TileSize,tz=-(hero.y+0.5f)*TileSize;
        const float k=static_cast<float>(1-std::exp(-dt/0.12));
        if(!state.placed||state.floor!=game.Floor())
        {
            state.drawnX=tx;state.drawnZ=tz;
            state.drawnAngle=std::atan2(-static_cast<float>(hero.dirX),static_cast<float>(hero.dirY));
            state.cameraAngle=std::atan2(static_cast<float>(hero.dirX),-static_cast<float>(hero.dirY));
            state.floor=game.Floor();
            model.SetFacingWalk(true,1.4f);   // brisker than on the desktop: a dungeon is walked a lot
        }

        // She walks to her tile: her gait's own pace, so the planted foot stays put; she runs
        // when she has fallen more than a tile behind (keys pressed faster than she walks), and
        // a new floor puts her there at once.
        const float dx=tx-state.drawnX,dz=tz-state.drawnZ,distance=std::sqrt(dx*dx+dz*dz);
        if(distance>TileSize*4){state.drawnX=tx;state.drawnZ=tz;}
        else if(distance>0.05f)
        {
            const Activity want=state.hurry||distance>TileSize*1.6f?Activity::Run:Activity::Walk;
            if(state.gait!=want){model.SetActivity(want,1);state.gait=want;}
            // At least a slow pace while the stride eases in, so she never stalls.
            const float step=std::min(distance,std::max(model.GroundSpeed(),TileSize*0.6f)*static_cast<float>(dt));
            state.drawnX+=dx/distance*step;
            state.drawnZ+=dz/distance*step;
            // Facing the way she walks.
            const float way=std::atan2(-dx,-dz);
            state.drawnAngle+=std::remainder(way-state.drawnAngle,XM_2PI)*static_cast<float>(1-std::exp(-dt/0.06));
        }
        else
        {
            state.drawnX=tx;state.drawnZ=tz;
            if(state.gait!=Activity::Idle){model.SetActivity(Activity::Idle);state.gait=Activity::Idle;}
            if(!state.heldKey)state.hurry=false;
            // Standing: turned the way she faces (the model's front is -z; map (dx, dy) is world (dx, -dy)).
            const float angle=std::atan2(-static_cast<float>(hero.dirX),static_cast<float>(hero.dirY));
            state.drawnAngle+=std::remainder(angle-state.drawnAngle,XM_2PI)*static_cast<float>(1-std::exp(-dt/0.08));
        }
        // Her walks on the desktop leave センター shifted sideways in the model (the window
        // follows it there): taken back out, or she would stand off her tile, and the shift would
        // swing round with her as she turns.
        r.SetModelTransform(XMMatrixTranslation(-model.RootOffset(),0,0)*XMMatrixRotationY(state.drawnAngle)*XMMatrixTranslation(state.drawnX,0,state.drawnZ));
        r.SetTorch(state.drawnX,state.drawnZ,TileSize*(state.behind?9.0f:7.0f));

        // The camera: from above and behind the map's bottom edge (as in Shiren), or behind her
        // looking the way she faces (turning with her). It eases from one to the other.
        const float faceAngle=std::atan2(static_cast<float>(hero.dirX),-static_cast<float>(hero.dirY));
        state.cameraAngle+=std::remainder(faceAngle-state.cameraAngle,XM_2PI)*static_cast<float>(1-std::exp(-dt/0.09));
        XMVECTOR eye,target;
        if(state.behind)
        {
            const XMVECTOR forward=XMVectorSet(std::sin(state.cameraAngle),0,std::cos(state.cameraAngle),0);
            const XMVECTOR at=XMVectorSet(state.drawnX,0,state.drawnZ,0);
            target=XMVectorAdd(XMVectorAdd(at,XMVectorScale(forward,TileSize*1.6f)),XMVectorSet(0,11,0,0));
            eye=XMVectorAdd(XMVectorSubtract(at,XMVectorScale(forward,TileSize*3.2f)),XMVectorSet(0,24,0,0));
        }
        else
        {
            target=XMVectorSet(state.drawnX,8,state.drawnZ,0);
            eye=XMVectorAdd(target,XMVectorSet(0,62,-48,0));
        }
        if(!state.placed){XMStoreFloat3(&state.eye,eye);XMStoreFloat3(&state.target,target);}
        const float c=static_cast<float>(1-std::exp(-dt/(state.behind?0.07:0.15)));
        XMStoreFloat3(&state.eye,XMVectorLerp(XMLoadFloat3(&state.eye),eye,c));
        XMStoreFloat3(&state.target,XMVectorLerp(XMLoadFloat3(&state.target),target,c));
        state.placed=true;
        r.SetCamera(XMLoadFloat3(&state.eye),XMLoadFloat3(&state.target),XMConvertToRadians(state.behind?50.0f:32.0f));
        r.Render(model,state.hero.edgeScale);
    }

    void ShowDungeonWindow(HWND owner,const DungeonHero& hero,std::function<void(const DungeonResult&)> onEnd)
    {
        if(state.window)
        {
            ShowWindow(state.window,SW_SHOWNORMAL);
            SetForegroundWindow(state.window);
            return;
        }
        if(!state.gdiplus){GdiplusStartupInput input;GdiplusStartup(&state.gdiplus,&input,nullptr);}
        static bool registered=false;
        const HINSTANCE instance=GetModuleHandleW(nullptr);
        if(!registered)
        {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc=Proc;
            wc.hInstance=instance;
            wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
            wc.lpszClassName=ClassName;
            wc.hIcon=owner?reinterpret_cast<HICON>(GetClassLongPtrW(owner,GCLP_HICON)):nullptr;
            RegisterClassExW(&wc);
            registered=true;
        }
        state.hero=hero;
        state.onEnd=std::move(onEnd);
        state.confirmQuit=false;
        state.ended=false;
        state.face.reset();
        if(hero.faceSize>0&&hero.face.size()>=static_cast<size_t>(hero.faceSize)*hero.faceSize*4)
        {
            // A copy the bitmap owns (GDI+ keeps pointing at the pixels it is given).
            Bitmap view(hero.faceSize,hero.faceSize,hero.faceSize*4,PixelFormat32bppPARGB,const_cast<BYTE*>(state.hero.face.data()));
            state.face.reset(view.Clone(0,0,hero.faceSize,hero.faceSize,PixelFormat32bppPARGB));
        }
        state.game.Start(GetTickCount()^static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&state)),hero.name);

        // On the owner's monitor, about two thirds of its work area.
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&info);
        const RECT work=info.rcWork;
        const int w=std::min(static_cast<int>(work.right-work.left)*9/10,MulDiv(1180,static_cast<int>(GetDpiForSystem()),96));
        const int h=std::min(static_cast<int>(work.bottom-work.top)*9/10,MulDiv(760,static_cast<int>(GetDpiForSystem()),96));
        const std::wstring title=std::wstring(T(L"ダンジョン",L"Dungeon"))+L" - "+hero.name;
        state.window=CreateWindowExW(0,ClassName,title.c_str(),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                                     work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,
                                     nullptr,nullptr,instance,nullptr);
        state.show3D=CreateView();
        PlaceView();
        ShowWindow(state.window,SW_SHOWNORMAL);
        SetForegroundWindow(state.window);
    }
}
