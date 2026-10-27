#define NOMINMAX
#include "DungeonWindow.hpp"
#include "Album.hpp"
#include "Dungeon.hpp"
#include "Text.hpp"
#include "Log.hpp"
#include "MascotRenderer.hpp"
#include "MascotModel.hpp"
#include "MonsterArt.hpp"
#include "PropArt.hpp"
#include "TreasureArt.hpp"
#include <objidl.h>
#include <gdiplus.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
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
        // height: rock a whole tile thick, about twice her height (with a ceiling on top seen
        // from behind her); the walls between her and the camera are low (they would hide her).
        // Doors are a little taller than she is, with bricks above.
        constexpr float TileSize=10,WallHeight=40.0f,NearWallHeight=3.0f,DoorHeight=14.0f;
        // Patterns the geometry shader draws, by the colour's alpha (see GeometryPS.hlsl).
        constexpr float Lit=1,Stones=0.75f,Bricks=0.5f,Glow=0.25f;
        // Her words: how long a lighter one waits behind one that matters more, how often the
        // LLM is asked about small things, and how long its line is waited for.
        constexpr ULONGLONG WordsHold=3000,AskEvery=12000,AskWait=9000;
        constexpr ULONGLONG DefeatShown=1500;   // the defeat stays at least this long before a key takes her home

        enum class Strike {None,Sword,Magic};   // her attacks, shown in 3D
        // A monster as drawn: where it is on its way to its tile, which way it faces, its stride,
        // and how far through an attack or a flinch it is (-1: none).
        struct MonsterLook {float x=0,z=0,yaw=0,walk=0,moving=0,strike=-1,flinch=-1,breath=-1;};
        // The dragon breathing fire: how long it takes (rearing, then the fire from about a
        // third of the way, its head sweeping), and when the fire leaves its mouth and stops.
        constexpr double BreathTime=1.8;
        constexpr float FireFrom=0.27f,FireTo=0.84f;

        // A spark flying from a hit (world units, per second), fading over its life.
        struct Spark {DirectX::XMFLOAT3 at,speed;float size;float color[3];double age,life;};
        // Her jump (Shift with a way): 1 forward, -1 back, 2 a step to the side (0: none), from
        // where she was drawn to her new tile, and how long into it.
        struct Leap {int kind=0;float fromX=0,fromZ=0,toX=0,toZ=0;double time=0;};
        // Something she picked up (a chest opened, gold, a key, goods bought): drawn where it
        // lay until she gets there (age below 0), then it glows and fades away.
        enum class Picked {Chest,Gold,Key,Goods};
        struct Pickup {Picked kind;int x,y;int amount=0;Dungeon::Item item{};double age=-1,waited=0;};
        constexpr double PickupLife=1.1;

        // A talk by a campfire (C beside one, no monster about): the user writes in a box under
        // the view and she answers in her bubble; what was said is kept while the box is open.
        struct CampTalk
        {
            HWND edit=nullptr;
            HFONT font=nullptr;
            std::vector<std::pair<bool,std::wstring>> said;   // true: the user's
            bool asking=false;                                // her answer is coming
        };

        struct
        {
            HWND window=nullptr;
            ULONG_PTR gdiplus=0;
            Dungeon::Game game;
            Dungeon::Record record;
            int bestBefore=0;
            DungeonHero hero;
            std::unique_ptr<Bitmap> face;
            std::function<void(const DungeonResult&)> onEnd;
            bool confirmQuit=false,ended=false,showBook=false;
            ULONGLONG deadAt=0;   // when she was beaten (GetTickCount64)
            // The 3D view (a child window over the map's place) and its renderer, only while
            // open. sceneKey: the turn and floor the geometry was last built for.
            HWND view=nullptr;
            std::unique_ptr<MascotRenderer> renderer;
            bool show3D=true;
            long long sceneKey=-1;
            float drawnX=0,drawnZ=0,drawnAngle=0;   // where she is drawn, walking toward her tile
            Leap leap;
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
            float cameraBack=TileSize*3.2f;   // behind her: how far back the camera is (in short of a wall)
            bool firstPerson=false;           // behind her with a wall close behind: seen through her eyes
            float firstBlend=0;               // easing into it (0: behind her, 1: her eyes)
            float eyeHeight=15;               // her eyes' height, eased (her walk bobs it)
            // Her attacks in 3D: one asked for (she cuts or casts as soon as she stands), and
            // the one playing, with how long it has, the sword tip's recent places (its trail) and
            // how far the spell flies (tiles).
            Strike strikeAsked=Strike::None,strike=Strike::None;
            ULONGLONG strikeAskedAt=0;
            double strikeTime=0;
            std::vector<DirectX::XMFLOAT3> trail;
            float boltTiles=0;
            // Where the attack lands (the tile cut at, the bolt's last), and when (seconds into
            // it): the 3D scene waits for that before it shows what the attack did (a monster
            // gone), and the hit flashes there.
            std::pair<int,int> strikeTile{-1,-1};
            double strikeLands=0;
            std::vector<Spark> burst;   // flying from a hit
            // The dragon's fire: where its mouth was last drawn and how far through breathing
            // it is (-1: not), and the flames flying from it.
            MonsterMouth mouth;
            float breathing=-1;
            std::vector<Spark> fire;
            double fireOwed=0;
            // What she picked up, and the things lying about last frame (to see what went).
            std::vector<Pickup> pickups;
            std::vector<Dungeon::Chest> lastChests;
            std::vector<Dungeon::Gold> lastGolds;
            std::vector<std::pair<int,int>> lastKeys;
            std::vector<Dungeon::Ware> lastWares;
            int lastFloor=-1;
            DirectX::XMFLOAT3 flashAt{};   // and a flash of light there, for a moment
            double flashAge=-1;
            // The monsters as last shown (the game's, taken once her blow has landed), and how
            // each is drawn (by its uid).
            std::vector<Dungeon::Monster> shown;
            long long shownKey=-1;
            int shownFloor=-1;
            std::map<int,MonsterLook> looks;
            double monsterClock=0;
            // The dragon's fire in 3D: the tiles it swept (from the dragon out), for a moment.
            std::vector<std::pair<int,int>> flame;
            int flameTurn=-1;
            double flameTime=0;
            // What she says (in the bubble under the view): her own line, or the LLM's as it
            // comes (waiting: asked, "…" until it does). weight: how much it matters.
            struct
            {
                std::wstring text,fallback;
                bool waiting=false;
                int weight=0;
                ULONGLONG since=0,asked=0;
                std::deque<std::wstring> recent;   // her last lines (the LLM is asked not to repeat them)
            } speech;
            // A picture for the album, taken from the 3D view at `at` (0: none).
            struct {std::wstring label;bool automatic=false;ULONGLONG at=0;} photo;
            std::wstring toast;   // a short note under the view ("saved to the album")
            ULONGLONG toastUntil=0;
            CampTalk camp;
        } state;

        bool ByFire();

        // The crystals along the edges of the crystal rooms seen: their tile, where they stand
        // (world units, pushed toward the room's wall), how tall, and the seed of their shape.
        struct CrystalSpot {int tx,ty;float x,z,height;int seed;};
        std::vector<CrystalSpot> CrystalSpots(const Dungeon::Game& game)
        {
            std::vector<CrystalSpot> spots;
            for(const auto& r:game.Rooms())
            {
                if(r.kind!=Dungeon::RoomKind::Crystal)continue;
                for(int y=r.y;y<r.y+r.h;++y)
                    for(int x=r.x;x<r.x+r.w;++x)
                    {
                        const bool edge=x==r.x||y==r.y||x==r.x+r.w-1||y==r.y+r.h-1;
                        if(!edge||(x*7+y*3)%3!=0||!game.Seen(x,y)||game.At(x,y)!=Dungeon::Tile::Floor)continue;
                        const float dx=x==r.x?-0.3f:x==r.x+r.w-1?0.3f:0,dz=y==r.y?-0.3f:y==r.y+r.h-1?0.3f:0;
                        spots.push_back({x,y,(x+0.5f+dx)*TileSize,-(y+0.5f+dz)*TileSize,(0.5f+0.12f*((x+y)%4))*TileSize,x*7+y*3});
                    }
            }
            return spots;
        }

        // Logical pixels (at 96 dpi) to the window's: by its dpi, and larger as the window grows
        // past the size it opens at (1180x760 at most), so the panel, the log, their text and the
        // gauges keep their share of it (up to a little over twice as large).
        int Px(int logical)
        {
            if(!state.window)return logical;
            const float dpi=static_cast<float>(GetDpiForWindow(state.window))/96.0f;
            RECT client{};
            GetClientRect(state.window,&client);
            const float grow=std::min(static_cast<float>(client.right)/(1164.0f*dpi),static_cast<float>(client.bottom)/(721.0f*dpi));
            return static_cast<int>(std::lround(logical*dpi*std::clamp(grow,1.0f,2.2f)));
        }

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
            ShowWindow(state.view,Showing3D()&&!state.showBook?SW_SHOWNA:SW_HIDE);
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

        void Toast(const std::wstring& text)
        {
            state.toast=text;
            state.toastUntil=GetTickCount64()+3000;
            if(state.window)InvalidateRect(state.window,nullptr,FALSE);
        }

        void SaveRecord()
        {
            if(!state.hero.recordPath.empty()&&!state.record.Save(state.hero.recordPath))Log(L"dungeon: records not saved (%s)",state.hero.recordPath.c_str());
        }

        // A picture for the album from the 3D view: by hand at once, or a moment after something
        // worth keeping (once she has turned to it and said her piece).
        void QueuePhoto(const std::wstring& label,bool automatic)
        {
            if(!state.renderer||state.hero.recordPath.empty())
            {
                if(!automatic)Toast(T(L"写真は3Dの表示で撮れます",L"Pictures are taken from the 3D view"));
                return;
            }
            if(!automatic&&state.hero.shutter)PlayShutter();
            state.photo={label,automatic,GetTickCount64()+(automatic?2500:150)};
        }

        void TakePhoto()
        {
            auto& p=state.photo;
            p.at=0;
            AlbumShot shot;
            shot.automatic=p.automatic;
            shot.label=p.label;
            AlbumLayer layer;
            UINT width=0,height=0;
            if(!state.renderer||!state.renderer->ReadLastFrame(layer.pixels,width,height))return;
            // Opaque: the dungeon fills it.
            for(size_t i=3;i<layer.pixels.size();i+=4)layer.pixels[i]=255;
            layer.rect={0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
            shot.characters.push_back(std::move(layer));
            if(!state.speech.waiting&&!state.speech.text.empty())shot.words.push_back({state.hero.name,state.speech.text});
            std::wstring error;
            const std::wstring file=SaveAlbumShot(shot,error);
            Log(L"dungeon: picture %s",file.empty()?error.c_str():file.c_str());
            Toast(file.empty()?T(L"写真を保存できませんでした",L"The picture could not be saved"):T(L"アルバムに写真を残しました",L"Saved a picture to the album"));
        }

        void Say(const std::wstring& line,int weight)
        {
            auto& s=state.speech;
            s.text=line;
            s.waiting=false;
            s.weight=weight;
            s.since=GetTickCount64();
            if(!line.empty()){s.recent.push_back(line);while(s.recent.size()>4)s.recent.pop_front();}
        }

        // What she has words for since the last action: the weightiest is said (in her own
        // words through the LLM when it is there and the moment is worth it), and the moments
        // worth keeping become pictures.
        void Listen()
        {
            auto talks=state.game.TakeTalks();
            if(talks.empty())return;
            using Dungeon::Event;
            const std::wstring f=std::to_wstring(state.game.Floor());
            for(const auto& t:talks)
            {
                if(t.event==Event::Dead)state.deadAt=GetTickCount64();
                const wchar_t* what=nullptr;
                if(t.event==Event::BossDown)what=T(L"ボス撃破",L"boss");
                else if(t.event==Event::Camp&&t.weight>=8)what=T(L"焚き火",L"campfire");
                else if(t.event==Event::Crystal)what=T(L"水晶の部屋",L"crystals");
                else if(t.event==Event::Arrive&&t.weight==5)what=T(L"新記録",L"record");
                else if(t.event==Event::Treasure)what=T(L"宝物",L"treasure");
                if(what)QueuePhoto(Fmt(T(L"ダンジョン{0}階 {1}",L"Dungeon floor {0} {1}"),{f,what}),true);
            }
            // Kept as it goes (a small file), so nothing is lost if the app is closed mid-way.
            SaveRecord();
            const Dungeon::Talk* pick=&talks.front();
            for(const auto& t:talks)if(t.weight>=pick->weight)pick=&t;
            auto& s=state.speech;
            const ULONGLONG now=GetTickCount64();
            // A lighter word does not cut in on one that matters more while it is fresh (or coming).
            if((s.waiting||now-s.since<WordsHold)&&pick->weight<s.weight)return;
            if(state.hero.talk&&pick->event!=Event::Refused&&pick->weight>=2&&(pick->weight>=6||now-s.asked>AskEvery))
            {
                const auto& h=state.game.Hero();
                std::wstring situation=pick->facts+Fmt(T(L"。（いまは{0}階、HP{1}/{2}、レベル{3}）",L". (Now on floor {0}, HP {1}/{2}, level {3})"),
                                                       {f,std::to_wstring(h.hp),std::to_wstring(h.MaxHp()),std::to_wstring(h.level)});
                if(!s.recent.empty())
                {
                    situation+=T(L"さっきあなたが言ったこと：",L" What you said just before: ");
                    for(const auto& r:s.recent)situation+=English()?L"\""+r+L"\" ":L"「"+r+L"」";
                    situation+=T(L"。同じ言い回しは避けて。",L"- do not say the same again. ");
                }
                if(pick->weight<8)situation+=T(L"そのことを、話し相手に一言（1〜2文）",L"Say a word about it to them (one or two sentences)");
                if(state.hero.talk(situation))
                {
                    s.waiting=true;
                    s.fallback=pick->line;
                    s.text=L"…";
                    s.weight=pick->weight;
                    s.since=s.asked=now;
                    return;
                }
            }
            Say(pick->line,pick->weight);
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
            // A box of side 2*r around the middle of tile (x, y), from y0 to y1.
            auto block=[&](int x,int y,float dx,float dz,float r,float y0,float y1,const float (&c)[4]){
                const float cx=(x+0.5f+dx)*TileSize,cz=-(y+0.5f+dz)*TileSize;
                box(cx-r*TileSize,y0,cz-r*TileSize,cx+r*TileSize,y1,cz+r*TileSize,c);
            };
            auto plate=[&](int x,int y,float r,float height,const float (&c)[4]){
                const float cx=(x+0.5f)*TileSize,cz=-(y+0.5f)*TileSize,s=r*TileSize;
                quad({cx-s,height,cz-s},{cx+s,height,cz-s},{cx+s,height,cz+s},{cx-s,height,cz+s},{0,1,0},c);
            };
            auto shade=[](const float (&c)[3],float k,float pattern=Lit)->std::array<float,4>{return {c[0]*k,c[1]*k,c[2]*k,pattern};};
            auto with=[](const std::array<float,4>& a,auto f){float c[4]={a[0],a[1],a[2],a[3]};return f(c);};
            auto rgba=[](float r,float g,float b,float k,float pattern,float (&c)[4]){c[0]=r*k;c[1]=g*k;c[2]=b*k;c[3]=pattern;};
            auto light=[&](int x,int y){return game.Visible(x,y)?1.0f:0.5f;};
            constexpr float T=TileSize,gap=0;
            // Each stretch of five floors (to its boss) has its own look (GeometryPS): old bricks,
            // mossy stone ruins, an ice cave, a lava cave, an amethyst abyss, then round again.
            // The look goes to the shader with the pattern, in the colour's alpha.
            struct Look {float floor[3],wall[3],ceiling[3];};   // the ceiling bright: lit only by the torch from below
            static constexpr Look Looks[]={
                {{0.50f,0.46f,0.58f},{0.54f,0.38f,0.33f},{0.85f,0.78f,0.80f}},
                {{0.50f,0.50f,0.44f},{0.52f,0.52f,0.46f},{0.72f,0.74f,0.66f}},
                {{0.70f,0.82f,0.94f},{0.52f,0.66f,0.82f},{0.82f,0.90f,1.00f}},
                {{0.34f,0.28f,0.26f},{0.32f,0.26f,0.24f},{0.42f,0.32f,0.30f}},
                {{0.44f,0.38f,0.56f},{0.34f,0.28f,0.46f},{0.52f,0.44f,0.62f}}};
            const int look=((game.Floor()-1)/5)%static_cast<int>(std::size(Looks));
            const auto& floorColor=Looks[look].floor;
            const auto& wallColor=Looks[look].wall;
            const auto& ceilingColor=Looks[look].ceiling;
            const float floorPattern=Stones+0.02f*look,wallPattern=Bricks+0.02f*look;
            // The vertices added since from, above bottom, may sink (a wall's top, where it would
            // hide her: GeometryVS works out how far as she walks and the camera moves), down to
            // lowest, for the wall of tile (x, y); floorBeyond: floor straight above it on the map.
            auto sinking=[&](size_t from,int x,int y,float bottom,float lowest,bool floorBeyond){
                for(size_t i=from;i<out.size();++i)
                    if(out[i].position[1]>bottom+0.01f)
                    {
                        auto& w=out[i].wall;
                        w[0]=(x+0.5f)*TileSize;w[1]=-(y+0.5f)*TileSize;w[2]=out[i].position[1]-lowest;w[3]=floorBeyond?1.0f:0.0f;
                    }
            };
            for(int y=0;y<Dungeon::Game::Height;++y)
                for(int x=0;x<Dungeon::Game::Width;++x)
                {
                    if(game.At(x,y)==Dungeon::Tile::Wall||!game.Seen(x,y))continue;
                    const float k=game.Visible(x,y)?1.0f:0.45f;
                    const float x0=x*T,x1=(x+1)*T,z1=-y*T,z0=-(y+1)*T;
                    // The stairs' tile has a well in it instead (AddStairs).
                    if(game.At(x,y)!=Dungeon::Tile::Stairs)
                        with(shade(floorColor,k,floorPattern),[&](const float (&c)[4]){quad({x0+gap,0,z0+gap},{x1-gap,0,z0+gap},{x1-gap,0,z1-gap},{x0+gap,0,z1-gap},{0,1,0},c);});
                    // Seen from behind her, a ceiling of slabs at the top of the walls (facing down).
                    if(state.behind)
                        with(shade(ceilingColor,k,floorPattern),[&](const float (&c)[4]){quad({x0,WallHeight,z1},{x1,WallHeight,z1},{x1,WallHeight,z0},{x0,WallHeight,z0},{0,-1,0},c);});
                    if(game.At(x,y)==Dungeon::Tile::Door)
                    {
                        // A heavy wooden door, bricks above it up to the ceiling.
                        const bool alongZ=game.At(x,y-1)!=Dungeon::Tile::Wall||game.At(x,y+1)!=Dungeon::Tile::Wall;
                        AddDoor(out,(x+0.5f)*T,-(y+0.5f)*T,DoorHeight,alongZ,k);
                        const size_t from=out.size();
                        with(shade(wallColor,k,wallPattern),[&](const float (&c)[4]){box(x0,DoorHeight,z0,x1,WallHeight,z1,c);});
                        sinking(from,x,y,DoorHeight,DoorHeight,game.At(x,y-1)!=Dungeon::Tile::Wall);
                    }
                    if(game.At(x,y)==Dungeon::Tile::Stairs)
                    {
                        // Steps going down into a dark well beyond a landing.
                        AddStairs(out,(x+0.5f)*T,-(y+0.5f)*T,k);
                    }
                }
            // The rock around the floor seen: a block a whole tile wide for each wall tile next to
            // it (corners too), bricks on its sides, dark on top. One that would hide her sinks
            // (see sinking).
            constexpr float rockTop[3]={0.20f,0.16f,0.20f};
            for(int y=0;y<Dungeon::Game::Height;++y)
                for(int x=0;x<Dungeon::Game::Width;++x)
                {
                    if(game.At(x,y)!=Dungeon::Tile::Wall)continue;
                    bool beside=false,lit=false,floorAbove=false;
                    for(int dy=-1;dy<=1;++dy)
                        for(int dx=-1;dx<=1;++dx)
                            if((dx||dy)&&game.At(x+dx,y+dy)!=Dungeon::Tile::Wall&&game.Seen(x+dx,y+dy))
                            {
                                beside=true;
                                lit|=game.Visible(x+dx,y+dy);
                                // Floor straight above it on the map: it stands in front of that
                                // floor as the camera above sees it (a corridor's side walls do not).
                                floorAbove|=dy<0&&dx==0;
                            }
                    if(!beside)continue;
                    const float k=lit?1.0f:0.45f,h=WallHeight;
                    const float x0=x*T,x1=(x+1)*T,z1=-y*T,z0=-(y+1)*T;
                    const size_t from=out.size();
                    with(shade(wallColor,k,wallPattern),[&](const float (&c)[4]){box(x0,0,z0,x1,h,z1,c);});
                    with(shade(rockTop,k),[&](const float (&c)[4]){quad({x0,h+0.02f,z0},{x1,h+0.02f,z0},{x1,h+0.02f,z1},{x0,h+0.02f,z1},{0,1,0},c);});
                    sinking(from,x,y,0,NearWallHeight,floorAbove);
                }
            // Crystals glowing along the edges of a crystal room.
            for(const auto& c:CrystalSpots(game))AddCrystals(out,c.x,c.z,c.height,c.seed,light(c.tx,c.ty));
            for(const auto& t:game.Traps())
                if(t.found&&game.Seen(t.x,t.y))AddTrap(out,(t.x+0.5f)*T,-(t.y+0.5f)*T,t.kind,light(t.x,t.y));
            for(const auto& g:game.Golds())
                if(game.Seen(g.x,g.y))AddGold(out,(g.x+0.5f)*T,-(g.y+0.5f)*T,g.amount,light(g.x,g.y));
            for(const auto& [x,y]:game.Keys())
                if(game.Seen(x,y))AddKey(out,(x+0.5f)*T,-(y+0.5f)*T,light(x,y));
            // A shop's goods on their mats (she stands on one to buy).
            for(const auto& w:game.Wares())
                if(game.Seen(w.x,w.y))AddGoods(out,(w.x+0.5f)*T,-(w.y+0.5f)*T,w.item,light(w.x,w.y));
            for(const auto& f:game.Fires())
                if(game.Seen(f.x,f.y))AddCampfire(out,(f.x+0.5f)*T,-(f.y+0.5f)*T,!f.used,light(f.x,f.y));
            for(const auto& c:game.Chests())
                if(game.Seen(c.x,c.y))AddChest(out,(c.x+0.5f)*T,-(c.y+0.5f)*T,light(c.x,c.y));
            // The monsters move: drawn every frame (MovingMonsters).
            // The spell's path is not marked here: its light flies along it (TickDungeonWindow).
            return out;
        }

        void Finish()
        {
            if(state.ended)return;
            state.ended=true;
            ReleaseView();
            auto& g=state.game;
            if(g.Turns()>0)g.End();
            SaveRecord();
            DungeonResult result;
            result.floor=g.Floor();result.kills=g.Kills();result.turns=g.Turns();result.rested=g.Rested();
            result.died=g.GetState()==Dungeon::Game::State::Dead;
            result.deepest=state.record.best>state.bestBefore;
            result.daily=g.Daily();
            for(const auto& id:g.NewTitles())
                for(const auto& t:Dungeon::Titles())
                    if(id==t.id)result.titles.push_back(English()?t.en:t.ja);
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

        // Wrapped within the box (cut off at its bottom).
        void TextBox(Graphics& g,const std::wstring& text,const RectF& box,float size,Color color)
        {
            FontFamily family(L"Yu Gothic UI");
            Font font(family.IsAvailable()?&family:FontFamily::GenericSansSerif(),size,FontStyleRegular,UnitPixel);
            SolidBrush brush(color);
            StringFormat format;
            format.SetTrimming(StringTrimmingEllipsisCharacter);
            g.DrawString(text.c_str(),static_cast<INT>(text.size()),&font,box,&format,&brush);
        }

        void Bar(Graphics& g,float x,float y,float w,float h,int value,int max,Color fill)
        {
            SolidBrush back(Color(255,40,36,52)),front(fill);
            g.FillRectangle(&back,x,y,w,h);
            if(max>0)g.FillRectangle(&front,x,y,w*std::clamp(value,0,max)/max,h);
        }

        // Her face in a ring (or her initial on pink).
        void Face(Graphics& g,float fx,float fy,float size)
        {
            if(state.face)
            {
                GraphicsPath circle;
                circle.AddEllipse(fx,fy,size,size);
                Region kept;
                g.GetClip(&kept);
                g.SetClip(&circle,CombineModeIntersect);
                SolidBrush back(Color(255,250,240,245));
                g.FillEllipse(&back,fx,fy,size,size);
                g.DrawImage(state.face.get(),fx,fy,size,size);
                g.SetClip(&kept);
            }
            else
            {
                SolidBrush body(Color(255,255,140,180));
                g.FillEllipse(&body,fx,fy,size,size);
                Text(g,state.hero.name.substr(0,1),fx+size*0.2f,fy+size*0.1f,size*0.55f,Color(255,255,255,255),true);
            }
            Pen ring(Color(255,255,255,255),std::max(1.5f,size/12));
            g.DrawEllipse(&ring,fx,fy,size,size);
        }

        // Her words in a bubble beside her face.
        void SpeechBubble(Graphics& g,const RectF& area)
        {
            const auto& s=state.speech;
            if(s.text.empty())return;
            const float face=std::min(area.Height*0.62f,static_cast<float>(Px(64)));
            Face(g,area.X,area.Y+(area.Height-face)/2,face);
            const float bx=area.X+face+static_cast<float>(Px(14)),bw=area.Width-face-static_cast<float>(Px(14)),r=static_cast<float>(Px(12));
            GraphicsPath bubble;
            bubble.AddArc(bx,area.Y,2*r,2*r,180,90);
            bubble.AddArc(bx+bw-2*r,area.Y,2*r,2*r,270,90);
            bubble.AddArc(bx+bw-2*r,area.Y+area.Height-2*r,2*r,2*r,0,90);
            bubble.AddArc(bx,area.Y+area.Height-2*r,2*r,2*r,90,90);
            bubble.CloseFigure();
            SolidBrush fill(Color(255,250,246,252));
            g.FillPath(&fill,&bubble);
            // The tail toward her face.
            const float ty=area.Y+area.Height/2;
            PointF tail[3]={PointF(bx-static_cast<float>(Px(10)),ty),PointF(bx+1,ty-static_cast<float>(Px(8))),PointF(bx+1,ty+static_cast<float>(Px(8)))};
            g.FillPolygon(&fill,tail,3);
            const float pad=static_cast<float>(Px(10));
            Text(g,state.hero.name,bx+pad,area.Y+pad*0.6f,static_cast<float>(Px(12)),Color(255,200,90,140),true);
            TextBox(g,s.text,RectF(bx+pad,area.Y+pad*0.6f+static_cast<float>(Px(18)),bw-2*pad,area.Height-pad*1.2f-static_cast<float>(Px(18))),
                    static_cast<float>(Px(15)),Color(255,40,34,52));
        }

        // The records (Z): monsters and treasures, things found, and the titles.
        void PaintBook(Graphics& g,const RectF& area)
        {
            const auto& r=state.record;
            const Color white(255,236,232,245),grey(255,120,114,140),gold(255,240,200,110),dim(255,160,154,180);
            SolidBrush back(Color(255,28,25,40));
            g.FillRectangle(&back,area);
            const float pad=static_cast<float>(Px(14)),line=static_cast<float>(Px(18)),size=static_cast<float>(Px(13));
            float y=area.Y+pad;
            Text(g,T(L"冒険の記録",L"Adventure records"),area.X+pad,y,static_cast<float>(Px(18)),gold,true);
            SYSTEMTIME t{};
            GetLocalTime(&t);
            wchar_t today[16];
            swprintf_s(today,L"%04d-%02d-%02d",t.wYear,t.wMonth,t.wDay);
            const std::wstring daily=r.dailyDate==today&&r.dailyBest>0?Fmt(T(L"{0}階",L"floor {0}"),{std::to_wstring(r.dailyBest)}):T(L"まだ",L"not yet");
            Text(g,Fmt(T(L"いちばん深い階：{0}階　冒険した回数：{1}　今日のダンジョン：{2}　（Z・Escで戻る）",L"Deepest: floor {0}   Adventures: {1}   Today's dungeon: {2}   (Z/Esc: back)"),
                       {std::to_wstring(r.best),std::to_wstring(r.runs),daily}),area.X+pad,y+line*1.5f,size,dim);
            y+=line*3.2f;
            const float c1=area.X+pad,c2=area.X+area.Width*0.27f,c3=area.X+area.Width*0.55f;
            float y1=y,y2=y,y3=y;
            const auto monsters=Dungeon::MonsterBook();
            int defeated=0;
            for(const auto& [id,name]:monsters)defeated+=r.kills.count(id)?1:0;
            Text(g,Fmt(T(L"モンスター（{0}/{1}）",L"Monsters ({0}/{1})"),{std::to_wstring(defeated),std::to_wstring(monsters.size())}),c1,y1,size,gold,true);y1+=line*1.2f;
            for(const auto& [id,name]:monsters)
            {
                const auto k=r.kills.find(id);
                Text(g,k==r.kills.end()?std::wstring(L"？？？"):name+L"  ×"+std::to_wstring(k->second),c1,y1,size,k==r.kills.end()?grey:white);
                y1+=line;
            }
            y1+=line*0.6f;
            // Treasures as their pictures, five to a row (the ones not found yet, a "?").
            const auto treasures=Dungeon::TreasureBook();
            Text(g,Fmt(T(L"宝物（{0}/{1}）　デスクトップで眺められます",L"Treasures ({0}/{1})  to look at on the desktop"),
                       {std::to_wstring(r.treasures.size()),std::to_wstring(treasures.size())}),c1,y1,size,gold,true);y1+=line*1.2f;
            const float cell=std::min((c2-c1-pad)/5,line*2.6f);
            SolidBrush slot(Color(255,40,36,56));
            for(size_t i=0;i<treasures.size();++i)
            {
                const RectF box(c1+(i%5)*cell,y1+(i/5)*cell,cell-4,cell-4);
                g.FillRectangle(&slot,box);
                if(r.treasures.count(treasures[i].first))PaintTreasure(g,treasures[i].first,box);
                else Text(g,L"?",box.X+box.Width*0.36f,box.Y+box.Height*0.18f,box.Height*0.5f,grey);
            }
            const auto items=Dungeon::ItemBook();
            int found=0;
            for(const auto& [id,name]:items)found+=r.items.count(id)?1:0;
            Text(g,Fmt(T(L"持ち物（{0}/{1}）",L"Things ({0}/{1})"),{std::to_wstring(found),std::to_wstring(items.size())}),c2,y2,size,gold,true);y2+=line*1.2f;
            for(const auto& [id,name]:items){const bool got=r.items.count(id)>0;Text(g,got?name:L"？？？",c2,y2,size,got?white:grey);y2+=line;}
            const auto& titles=Dungeon::Titles();
            Text(g,Fmt(T(L"称号（{0}/{1}）",L"Titles ({0}/{1})"),{std::to_wstring(r.titles.size()),std::to_wstring(titles.size())}),c3,y3,size,gold,true);y3+=line*1.2f;
            for(const auto& t:titles)
            {
                const bool got=r.titles.count(std::wstring(t.id,t.id+std::strlen(t.id)))>0;
                Text(g,got?std::wstring(English()?t.en:t.ja):std::wstring(L"？？？"),c3,y3,size,got?gold:grey,got);
                Text(g,English()?t.hintEn:t.hintJa,c3+static_cast<float>(Px(120)),y3+1,static_cast<float>(Px(11)),got?dim:grey);
                y3+=line;
            }
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
            if(state.showBook)PaintBook(g,RectF(pad,pad,areaW,areaH));
            else if(!Showing3D())
            {

            // Floors seen (dim) and in sight (lit), and the walls of rooms seen, as in a map.
            // Tiles edge to edge without smoothing, so no seams show between them.
            SolidBrush lit(Color(255,78,72,98)),dim(Color(255,40,37,54)),door(Color(255,128,80,40));
            g.SetSmoothingMode(SmoothingModeNone);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            for(int y=0;y<H;++y)
                for(int x=0;x<W;++x)
                    if(game.At(x,y)!=Dungeon::Tile::Wall&&game.Seen(x,y))
                    {
                        const PointF p=at(x,y);
                        g.FillRectangle(game.At(x,y)==Dungeon::Tile::Door?&door:game.Visible(x,y)?&lit:&dim,p.X,p.Y,tile,tile);
                    }
            g.SetPixelOffsetMode(PixelOffsetModeDefault);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            Pen wall(Color(255,138,28,43),std::max(1.5f,tile/7));
            for(const auto& r:game.Rooms())
            {
                bool any=false;
                for(int y=r.y;y<r.y+r.h&&!any;++y)for(int x=r.x;x<r.x+r.w&&!any;++x)any=game.Seen(x,y);
                if(!any)continue;
                const PointF p=at(r.x,r.y);
                g.DrawRectangle(&wall,p.X,p.Y,r.w*tile,r.h*tile);
                if(r.kind==Dungeon::RoomKind::Crystal)
                {
                    SolidBrush crystal(Color(200,120,220,255));
                    for(int y=r.y;y<r.y+r.h;++y)
                        for(int x=r.x;x<r.x+r.w;++x)
                            if((x==r.x||y==r.y||x==r.x+r.w-1||y==r.y+r.h-1)&&(x*7+y*3)%3==0&&game.Seen(x,y))
                            {const PointF c=at(x,y);g.FillRectangle(&crystal,c.X+tile*0.4f,c.Y+tile*0.2f,tile*0.2f,tile*0.6f);}
                }
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
            for(const auto& t:game.Traps())
                if(t.found&&game.Seen(t.x,t.y))
                {
                    using K=Dungeon::TrapKind;
                    const PointF p=at(t.x,t.y);
                    SolidBrush mark(t.kind==K::Spikes?Color(255,190,190,200):t.kind==K::Warp?Color(255,180,110,255):t.kind==K::Pit?Color(255,8,6,10):
                                    t.kind==K::Alarm?Color(255,240,200,60):Color(255,255,160,210));
                    PointF d[4]={PointF(p.X+tile/2,p.Y+tile*0.15f),PointF(p.X+tile*0.85f,p.Y+tile/2),PointF(p.X+tile/2,p.Y+tile*0.85f),PointF(p.X+tile*0.15f,p.Y+tile/2)};
                    g.FillPolygon(&mark,d,4);
                }
            for(const auto& gd:game.Golds())
                if(game.Seen(gd.x,gd.y)){const PointF p=at(gd.x,gd.y);SolidBrush b(Color(255,250,205,70));g.FillEllipse(&b,p.X+tile*0.3f,p.Y+tile*0.3f,tile*0.4f,tile*0.4f);}
            for(const auto& [x,y]:game.Keys())
                if(game.Seen(x,y))
                {
                    const PointF p=at(x,y);
                    SolidBrush key(Color(255,255,215,80));
                    g.FillEllipse(&key,p.X+tile*0.55f,p.Y+tile*0.3f,tile*0.3f,tile*0.3f);
                    g.FillRectangle(&key,p.X+tile*0.15f,p.Y+tile*0.4f,tile*0.45f,tile*0.1f);
                    g.FillRectangle(&key,p.X+tile*0.15f,p.Y+tile*0.4f,tile*0.08f,tile*0.22f);
                }
            for(const auto& w:game.Wares())
                if(game.Seen(w.x,w.y))
                {
                    const PointF p=at(w.x,w.y);
                    SolidBrush stand(Color(255,140,105,75)),goods(Color(255,240,240,250));
                    g.FillRectangle(&stand,p.X+tile*0.15f,p.Y+tile*0.55f,tile*0.7f,tile*0.3f);
                    g.FillRectangle(&goods,p.X+tile*0.35f,p.Y+tile*0.2f,tile*0.3f,tile*0.38f);
                }
            for(const auto& f:game.Fires())
                if(game.Seen(f.x,f.y))
                {
                    const PointF p=at(f.x,f.y);
                    SolidBrush wood(Color(255,110,70,40)),flame(Color(255,255,150,50));
                    g.FillRectangle(&wood,p.X+tile*0.15f,p.Y+tile*0.65f,tile*0.7f,tile*0.15f);
                    if(!f.used)g.FillEllipse(&flame,p.X+tile*0.3f,p.Y+tile*0.2f,tile*0.4f,tile*0.5f);
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
            SolidBrush fire(Color(170,255,110,50));
            for(const auto& [x,y]:game.Flame()){const PointF p=at(x,y);g.FillEllipse(&fire,p.X+tile*0.1f,p.Y+tile*0.1f,tile*0.8f,tile*0.8f);}
            for(const auto& m:game.Monsters())
                if(game.Visible(m.x,m.y))
                {
                    const PointF p=at(m.x,m.y);
                    // A big one over all the tiles it takes.
                    const float grow=tile*m.size+(m.boss?tile*0.1f:0);
                    SolidBrush body(Argb(m.color));
                    g.FillEllipse(&body,p.X+tile*0.08f-grow,p.Y+tile*0.08f-grow,tile*0.84f+2*grow,tile*0.84f+2*grow);
                    const float letter=tile*0.6f*(1+m.size);
                    Text(g,m.name.substr(0,1),p.X+tile/2-letter*0.55f,p.Y+tile/2-letter*0.7f,letter,Color(255,20,18,28),true);
                    if(m.hp<m.maxHp)Bar(g,p.X,p.Y-tile*0.2f-grow,tile,tile*0.12f,m.hp,m.maxHp,Color(255,230,80,80));
                }

            // The hero: her face in a ring, and a notch for the way she faces.
            {
                const PointF p=at(hero.x,hero.y);
                const float size=tile*1.1f,fx=p.X+tile/2-size/2,fy=p.Y+tile/2-size/2;
                Face(g,fx,fy,size);
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
            Text(g,Fmt(game.Daily()?T(L"今日のダンジョン {0}階　レベル{1}",L"Today's: floor {0}  Level {1}"):T(L"{0}階　レベル{1}",L"Floor {0}  Level {1}"),
                       {std::to_wstring(game.Floor()),std::to_wstring(hero.level)}),px,y,static_cast<float>(Px(14)),gold);y+=line*1.1f;
            Text(g,Fmt(L"HP {0}/{1}",{std::to_wstring(hero.hp),std::to_wstring(hero.MaxHp())}),px,y,static_cast<float>(Px(13)),white);y+=line*0.9f;
            Bar(g,px,y,panel-pad*2,static_cast<float>(Px(7)),hero.hp,hero.MaxHp(),Color(255,110,210,120));y+=line*0.6f;
            Text(g,Fmt(L"MP {0}/{1}",{std::to_wstring(hero.mp),std::to_wstring(hero.MaxMp())}),px,y,static_cast<float>(Px(13)),white);y+=line*0.9f;
            Bar(g,px,y,panel-pad*2,static_cast<float>(Px(7)),hero.mp,hero.MaxMp(),Color(255,110,160,240));y+=line*0.7f;
            Text(g,Fmt(T(L"攻撃 {0}　防御 {1}　魔力 {2}",L"ATK {0}  DEF {1}  MAG {2}"),{std::to_wstring(hero.Atk()),std::to_wstring(hero.Def()),std::to_wstring(hero.Mag())}),
                 px,y,static_cast<float>(Px(13)),white);y+=line;
            std::wstring purse=Fmt(T(L"{0}ゴールド　おなか {1}%",L"{0} gold  Full {1}%"),{std::to_wstring(hero.gold),std::to_wstring(hero.food)});
            if(hero.keys>0)purse+=Fmt(T(L"　鍵 {0}",L"  Keys {0}"),{std::to_wstring(hero.keys)});
            Text(g,purse,px,y,static_cast<float>(Px(13)),hero.food<=20?Color(255,255,150,120):white);y+=line*1.4f;

            // The whole floor, small: where she has been, the stairs once seen, foes in sight.
            {
                const float scale=std::floor((panel-pad*2)/W*4)/4,mw=scale*W,mh=scale*H;
                SolidBrush frame(Color(255,30,27,42)),seenTile(Color(255,96,90,120)),litTile(Color(255,150,142,180)),doorTile(Color(255,150,95,50));
                g.FillRectangle(&frame,px-2,y-2,mw+4,mh+4);
                g.SetSmoothingMode(SmoothingModeNone);
                for(int ty=0;ty<H;++ty)
                    for(int tx=0;tx<W;++tx)
                        if(game.At(tx,ty)!=Dungeon::Tile::Wall&&game.Seen(tx,ty))
                            g.FillRectangle(game.At(tx,ty)==Dungeon::Tile::Door?&doorTile:game.Visible(tx,ty)?&litTile:&seenTile,px+tx*scale,y+ty*scale,scale,scale);
                g.SetSmoothingMode(SmoothingModeAntiAlias);
                auto dot=[&](int tx,int ty,Color c,float r){SolidBrush b(c);g.FillEllipse(&b,px+(tx+0.5f)*scale-r,y+(ty+0.5f)*scale-r,2*r,2*r);};
                const float r=std::max(2.0f,scale*1.2f);
                for(int ty=0;ty<H;++ty)for(int tx=0;tx<W;++tx)if(game.At(tx,ty)==Dungeon::Tile::Stairs&&game.Seen(tx,ty))dot(tx,ty,Color(255,240,200,110),r);
                for(const auto& f:game.Fires())if(game.Seen(f.x,f.y)&&!f.used)dot(f.x,f.y,Color(255,255,150,50),r);
                for(const auto& m:game.Monsters())if(game.Visible(m.x,m.y))dot(m.x,m.y,Color(255,235,90,90),r);
                dot(hero.x,hero.y,Color(255,255,140,180),r*1.2f);
                y+=mh+line*0.8f;
            }

            Text(g,game.InShop()?T(L"持ち物　数字：使う　Shift＋数字：売る",L"Bag  number: use  Shift+number: sell")
                                :T(L"持ち物　数字：使う　Shift＋数字：捨てる",L"Bag  number: use  Shift+number: drop"),px,y,static_cast<float>(Px(12)),grey);y+=line;
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
            const bool fresh=game.Floor()==1&&game.Turns()==0&&!game.Daily();
            const float help=ch-pad-line*(fresh?7.2f:6.2f);
            // How to fight is said in plain words (newcomers did not guess that walking into a
            // monster attacks it).
            std::wstring keys=state.behind&&Showing3D()?T(L"↑：進む　←→：向きを変える　↓：振り向く\n敵に向かって進むと、剣で攻撃\nShift＋↑：跳ぶ　↓：後ろへ跳ぶ　←→：横へ\n",
                                                           L"Up: go  Left/Right: turn  Down: turn round\nWalk into a monster to strike it with the sword\nShift+Up: jump  Down: step back  Left/Right: sidestep\n")
                                                     :T(L"矢印・WASD：移動\n敵に向かって進むと、剣で攻撃\nShift＋方向：前へジャンプ・後ろへ跳ぶ・横へ\n",
                                                        L"Arrows/WASD: move\nWalk into a monster to strike it with the sword\nShift+way: jump ahead, step back, sidestep\n");
            keys+=T(L"F：魔法（MP3）　スペース：待つ・罠を探す\nEnter：買う　P：写真　Z：図鑑\nV：カメラ　M：2D/3D　Esc：帰る",
                    L"F: magic (3 MP)  Space: wait, find traps\nEnter: buy  P: picture  Z: records\nV: camera  M: 2D/3D  Esc: go home");
            if(fresh)keys+=T(L"\nT：今日のダンジョンで遊ぶ",L"\nT: play today's dungeon instead");
            const bool fire=ByFire()&&!state.camp.edit;
            if(fire)keys+=T(L"\nC：焚き火で話す",L"\nC: talk by the fire");
            Text(g,keys,px,std::max(y,help-(fire?line:0.0f)),static_cast<float>(Px(12)),grey);

            // A question over everything when there is one.
            std::wstring ask;
            if(state.confirmQuit)ask=T(L"冒険をやめて、デスクトップに帰る？\n\nY：帰る　　N・Esc：続ける",L"Give up and go back to the desktop?\n\nY: go back    N/Esc: keep going");
            else if(game.GetState()==Dungeon::Game::State::AtStairs)
                ask=T(L"階段だ！\n\nEnter：次の階へ　　B：デスクトップに帰る",L"The stairs!\n\nEnter: the next floor    B: back to the desktop");
            else if(game.GetState()==Dungeon::Game::State::Dead)
                ask=Fmt(T(L"やられてしまった…（{0}階）\n\nEnter・Esc：デスクトップに帰る",L"Defeated... (floor {0})\n\nEnter/Esc: back to the desktop"),{std::to_wstring(game.Floor())});
            else if(const auto* w=game.WareHere())
                ask=Fmt(static_cast<int>(hero.bag.size())>=Dungeon::Game::BagSize
                            ?T(L"{0}：{1}ゴールド（持っているのは{2}ゴールド）\n持ち物がいっぱい：Shift＋数字キーで売ってから\nEnter：買う",
                               L"{0}: {1} gold (you have {2})\nThe bag is full: sell something first (Shift + number)\nEnter: buy")
                            :T(L"{0}：{1}ゴールド（持っているのは{2}ゴールド）\n\nEnter：買う",L"{0}: {1} gold (you have {2})\n\nEnter: buy"),
                        {w->item.name,std::to_wstring(w->price),std::to_wstring(hero.gold)});
            // Below the view: her words on the left, what happened on the right (a question in
            // 3D takes its place: the view hides what is drawn under it).
            const float top=ch-logHeight+pad/2,speechW=std::floor(areaW*0.56f);
            SpeechBubble(g,RectF(pad,top,speechW,logHeight-pad*1.5f));
            const float rx=pad*2+speechW,rw=areaW-speechW-pad;
            float ly=top;
            g.SetClip(RectF(rx,top,rw,logHeight-pad));
            if(!state.toast.empty()&&GetTickCount64()<state.toastUntil){Text(g,state.toast,rx,ly,static_cast<float>(Px(13)),gold,true);ly+=line;}
            if(state.camp.edit)
            {
                // The talk: what it is above its box (the log waits meanwhile).
                Text(g,T(L"焚き火で話す　Enter：話しかける　Esc：やめる",L"Talking by the fire   Enter: say it   Esc: stop"),rx,ly,static_cast<float>(Px(13)),gold,true);
            }
            else if((Showing3D()||state.showBook)&&!ask.empty())
            {
                SolidBrush box(Color(255,40,34,58));
                g.FillRectangle(&box,rx,ly,rw,top+logHeight-pad-ly);
                Text(g,ask,rx+pad,ly+pad/2,static_cast<float>(Px(14)),gold);
                ask.clear();
            }
            else
            {
                const auto& log=game.Log();
                const int fit=std::max(1,static_cast<int>((top+logHeight-pad-ly)/(line*0.95f)));
                for(size_t i=log.size()>static_cast<size_t>(fit)?log.size()-fit:0;i<log.size();++i)
                {Text(g,log[i],rx,ly,static_cast<float>(Px(13)),white);ly+=line*0.95f;}
            }
            g.ResetClip();
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
            // An attack still swinging counts too: the next waits for it.
            if(state.strikeAsked!=Strike::None||(state.strike!=Strike::None&&state.strikeTime<0.6))return true;
            if(state.leap.kind)return true;
            const auto& h=state.game.Hero();
            const float dx=(h.x+0.5f)*TileSize-state.drawnX,dz=-(h.y+0.5f)*TileSize-state.drawnZ;
            return dx*dx+dz*dz>TileSize*TileSize*0.11f;
        }

        // With Shift: a jump the way she faces (two tiles) or back (one), or a step to the side
        // keeping the way she faces. Behind her, up and down are her front and back, left and
        // right her sides; from above, the way pressed on the map.
        void Leap(int dx,int dy)
        {
            auto& game=state.game;
            const auto& h=game.Hero();
            int kx=dx,ky=dy;
            if(Showing3D()&&state.behind)
            {
                if(dy<0){kx=h.dirX;ky=h.dirY;}
                else if(dy>0){kx=-h.dirX;ky=-h.dirY;}
                else if(dx<0){kx=h.dirY;ky=-h.dirX;}
                else{kx=-h.dirY;ky=h.dirX;}
            }
            const int floor=game.Floor();
            game.StepOffStairs();
            const int kind=kx==h.dirX&&ky==h.dirY?1:kx==-h.dirX&&ky==-h.dirY?-1:2;
            const bool moved=kind==2?game.Sidestep(kx,ky):game.Jump(kind<0);
            if(!moved||!Showing3D()||!state.placed||game.Floor()!=floor)return;
            state.leap.kind=kind;
            state.leap.fromX=state.drawnX;state.leap.fromZ=state.drawnZ;
            state.leap.toX=(h.x+0.5f)*TileSize;state.leap.toZ=-(h.y+0.5f)*TileSize;
            state.leap.time=0;
        }

        // A step, or a cut at the monster there (in 3D, her sword swings).
        void Step(int dx,int dy)
        {
            auto& game=state.game;
            const auto& h=game.Hero();
            bool foe=false;
            for(const auto& m:game.Monsters())if(Dungeon::Occupies(m,h.x+dx,h.y+dy))foe=true;
            game.StepOffStairs();
            const int tx=h.x+dx,ty=h.y+dy;
            if(game.Move(dx,dy)&&foe){state.strikeAsked=Strike::Sword;state.strikeAskedAt=GetTickCount64();state.strikeTile={tx,ty};}
        }

        // A spell, cast the way she faces (in 3D, it flies from her hand along the bolt's path).
        void Cast()
        {
            auto& game=state.game;
            if(!game.Cast())return;
            const auto& h=game.Hero();
            state.boltTiles=static_cast<float>(Dungeon::Game::MagicRange);
            state.strikeTile={-1,-1};
            if(!game.Bolt().empty())
            {
                const auto& [x,y]=game.Bolt().back();
                state.boltTiles=static_cast<float>(std::max(std::abs(x-h.x),std::abs(y-h.y)));
                state.strikeTile={x,y};
            }
            state.strikeAsked=Strike::Magic;state.strikeAskedAt=GetTickCount64();
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

        // A new adventure: an ordinary one (a new floor each time), or today's (the same floors
        // all day, its best kept apart).
        void StartAdventure(bool daily)
        {
            SYSTEMTIME t{};
            GetLocalTime(&t);
            std::uint32_t seed=GetTickCount()^static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&state));
            wchar_t today[16];
            swprintf_s(today,L"%04d-%02d-%02d",t.wYear,t.wMonth,t.wDay);
            state.record.today=today;   // the day treasures found now are remembered by
            state.shownKey=-1;state.shownFloor=-1;state.looks.clear();state.shown.clear();
            if(daily)
            {
                if(state.record.dailyDate!=today){state.record.dailyDate=today;state.record.dailyBest=0;}
                seed=static_cast<std::uint32_t>(t.wYear*10000+t.wMonth*100+t.wDay)*2654435761u;
            }
            state.bestBefore=state.record.best;
            state.game.Start(seed,state.hero.name,state.hero.bond,daily,state.record);
            state.sceneKey=-1;
            state.placed=false;
            state.heldKey=state.queuedKey=0;
            state.speech.text.clear();
            state.speech.waiting=false;
            state.speech.weight=0;
            Listen();
        }

        // Beside a campfire (or on its tile's edge), playing, with no monster awake in sight, and
        // the LLM there to talk with: C opens the talk.
        bool ByFire()
        {
            const auto& game=state.game;
            if(!state.hero.chat||game.GetState()!=Dungeon::Game::State::Playing)return false;
            const auto& h=game.Hero();
            if(std::any_of(game.Monsters().begin(),game.Monsters().end(),[&](const Dungeon::Monster& m){return m.awake&&game.Visible(m.x,m.y);}))return false;
            return std::any_of(game.Fires().begin(),game.Fires().end(),[&](const Dungeon::Fire& f){return std::abs(f.x-h.x)+std::abs(f.y-h.y)<=1;});
        }

        // The talk's box: under the view, right of her bubble (where the log is), below a line
        // saying what it is.
        RECT TalkBox()
        {
            RECT client{};
            GetClientRect(state.window,&client);
            const int panel=Px(250),logHeight=Px(128),pad=Px(10);
            const int areaW=client.right-panel-2*pad,speechW=static_cast<int>(areaW*0.56f);
            const int x=pad*2+speechW,w=std::max(Px(80),areaW-speechW-pad),top=client.bottom-logHeight+pad/2+Px(24);
            return {x,top,x+w,top+Px(30)};
        }

        void CloseTalk()
        {
            auto& c=state.camp;
            if(c.edit){DestroyWindow(c.edit);c.edit=nullptr;}
            if(c.font){DeleteObject(c.font);c.font=nullptr;}
            c.said.clear();
            c.asking=false;
            if(state.window){SetFocus(state.window);InvalidateRect(state.window,nullptr,FALSE);}
        }

        // The adventure so far, for her answers: where they are, how she is, what has happened.
        std::wstring AdventureFacts()
        {
            const auto& game=state.game;
            const auto& h=game.Hero();
            std::wstring facts=Fmt(T(L"いまは{0}階、レベル{1}、HP{2}/{3}、{4}ゴールド、ここまでに{5}匹倒した。",L"Now on floor {0}, level {1}, HP {2}/{3}, {4} gold, {5} monsters defeated so far."),
                                   {std::to_wstring(game.Floor()),std::to_wstring(h.level),std::to_wstring(h.hp),std::to_wstring(h.MaxHp()),std::to_wstring(h.gold),std::to_wstring(game.Kills())});
            if(state.record.best>0)facts+=Fmt(T(L"これまでの最高は{0}階。",L" The deepest so far is floor {0}."),{std::to_wstring(state.record.best)});
            const auto& log=game.Log();
            if(!log.empty())
            {
                facts+=T(L"最近のできごと：",L" Lately: ");
                const size_t from=log.size()>8?log.size()-8:0;
                for(size_t i=from;i<log.size();++i)facts+=log[i]+(English()?L" ":L"");
            }
            return facts;
        }

        void SendTalk()
        {
            auto& c=state.camp;
            auto& s=state.speech;
            if(!c.edit||s.waiting)return;
            wchar_t buffer[512]{};
            GetWindowTextW(c.edit,buffer,512);
            std::wstring said=buffer;
            const size_t a=said.find_first_not_of(L" 　\t"),b=said.find_last_not_of(L" 　\t");
            said=a==std::wstring::npos?std::wstring():said.substr(a,b-a+1);
            if(said.empty())return;
            SetWindowTextW(c.edit,L"");
            const ULONGLONG now=GetTickCount64();
            if(state.hero.chat&&state.hero.chat(said,AdventureFacts(),c.said))
            {
                c.said.push_back({true,said});
                c.asking=true;
                s.waiting=true;
                s.fallback=T(L"うん…そうだね。",L"Mm... yeah.");
                s.text=L"…";
                s.weight=9;
                s.since=s.asked=now;
            }
            else Say(T(L"ごめんね、いまはうまく話せないみたい…。",L"Sorry, I can't really talk right now..."),9);
            InvalidateRect(state.window,nullptr,FALSE);
        }

        // The box's own keys: Enter sends, Esc closes (neither beeps).
        LRESULT CALLBACK TalkKeys(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR)
        {
            if(message==WM_KEYDOWN&&wParam==VK_RETURN){SendTalk();return 0;}
            if(message==WM_KEYDOWN&&wParam==VK_ESCAPE){CloseTalk();return 0;}
            if(message==WM_CHAR&&(wParam==L'\r'||wParam==27))return 0;
            if(message==WM_GETDLGCODE)return DLGC_WANTALLKEYS;
            return DefSubclassProc(hwnd,message,wParam,lParam);
        }

        void OpenTalk()
        {
            auto& c=state.camp;
            if(c.edit){SetFocus(c.edit);return;}
            const RECT r=TalkBox();
            c.edit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL,r.left,r.top,r.right-r.left,r.bottom-r.top,
                                   state.window,nullptr,GetModuleHandleW(nullptr),nullptr);
            if(!c.edit)return;
            c.font=CreateFontW(-Px(15),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Yu Gothic UI");
            SendMessageW(c.edit,WM_SETFONT,reinterpret_cast<WPARAM>(c.font),TRUE);
            SendMessageW(c.edit,EM_SETLIMITTEXT,200,0);
            SetWindowSubclass(c.edit,TalkKeys,1,0);
            SetFocus(c.edit);
            InvalidateRect(state.window,nullptr,FALSE);
        }

        // repeat: the key is held down (the keyboard's auto-repeat).
        void OnKey(WPARAM key,bool repeat)
        {
            auto& game=state.game;
            using State=Dungeon::Game::State;
            if(state.showBook)
            {
                if(key=='Z'||key==VK_ESCAPE||key==VK_RETURN){state.showBook=false;PlaceView();}
                InvalidateRect(state.window,nullptr,FALSE);
                return;
            }
            // Going home takes a key pressed for it: Y to give up (not Enter, which also buys and
            // takes the stairs), and after a defeat Enter or Esc once it has been shown a moment
            // (a key still held or hammered for the fight would otherwise skip it unseen).
            if(state.confirmQuit)
            {
                if(key=='Y'&&!repeat){Finish();return;}
                if(key=='N'||key==VK_ESCAPE)state.confirmQuit=false;
            }
            else if(game.GetState()==State::Dead)
            {
                if((key==VK_RETURN||key==VK_ESCAPE)&&!repeat&&GetTickCount64()-state.deadAt>=DefeatShown){Finish();return;}
            }
            else if(key==VK_ESCAPE)state.confirmQuit=true;
            else if(key=='M'&&state.renderer){state.show3D=!state.show3D;PlaceView();}
            else if(key=='V'&&Showing3D())state.behind=!state.behind;
            else if(key=='Z'&&!repeat){state.showBook=true;PlaceView();}
            else if(key=='C'&&!repeat&&ByFire())OpenTalk();
            else if(key=='P'&&!repeat)QueuePhoto({},false);
            else if(key=='T'&&!repeat&&game.Floor()==1&&game.Turns()==0&&!game.Daily())StartAdventure(true);
            else if(game.GetState()==State::AtStairs&&key==VK_RETURN&&!repeat)game.Descend();
            else if(game.GetState()==State::AtStairs&&key=='B'&&!repeat){Finish();return;}
            else if(key==VK_RETURN&&game.WareHere()&&!repeat)game.Buy();
            else
            {
                int dx=0,dy=0;
                if(key==VK_LEFT||key=='A')dx=-1;
                else if(key==VK_RIGHT||key=='D')dx=1;
                else if(key==VK_UP||key=='W')dy=-1;
                else if(key==VK_DOWN||key=='S')dy=1;
                // With Shift, a jump or a step to the side: one a press, once she stands.
                if((dx||dy)&&(GetKeyState(VK_SHIFT)&0x8000))
                {
                    if(!repeat&&!Underway()&&game.GetState()!=State::Dead)Leap(dx,dy);
                    return;
                }
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
                    if(dy<0)Step(h.dirX,h.dirY);
                    else if(dx<0)game.Face(h.dirY,-h.dirX);
                    else if(dx>0)game.Face(-h.dirY,h.dirX);
                    else game.Face(-h.dirX,-h.dirY);
                }
                else if(dx||dy)Step(dx,dy);
                else if(game.GetState()!=State::Playing)return;
                else if(key=='F')Cast();
                // One turn a press: held down, it does not keep waiting (nor looking for traps).
                else if((key==VK_SPACE||key==VK_OEM_PERIOD)&&!repeat)game.Rest();
                else
                {
                    // A number: that thing used or put on; with Shift, sold (in a shop) or left behind.
                    int index=-1;
                    if(key>='1'&&key<='9')index=static_cast<int>(key-'1');
                    else if(key=='0')index=9;
                    else if(key>=VK_NUMPAD1&&key<=VK_NUMPAD9)index=static_cast<int>(key-VK_NUMPAD1);
                    else return;
                    if(repeat)return;
                    if(GetKeyState(VK_SHIFT)&0x8000)game.Drop(index);
                    else game.Use(index);
                }
            }
            Listen();
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
                if(auto& c=state.camp;c.edit)
                {
                    // The talk box and its text grow with the window too.
                    const RECT r=TalkBox();
                    MoveWindow(c.edit,r.left,r.top,r.right-r.left,r.bottom-r.top,TRUE);
                    const HFONT old=c.font;
                    c.font=CreateFontW(-Px(15),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Yu Gothic UI");
                    SendMessageW(c.edit,WM_SETFONT,reinterpret_cast<WPARAM>(c.font),TRUE);
                    if(old)DeleteObject(old);
                }
                PlaceView();
                InvalidateRect(hwnd,nullptr,FALSE);
                return 0;
            case WM_CLOSE:
                if(!state.ended)Finish();
                return 0;
            case WM_DESTROY:
                CloseTalk();
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

    void DungeonHeard(const std::wstring& line,bool done)
    {
        auto& s=state.speech;
        if(!state.window||!s.waiting)return;
        if(!line.empty())s.text=line;
        if(done)
        {
            s.waiting=false;
            if(line.empty())s.text=s.fallback;
            s.recent.push_back(s.text);
            while(s.recent.size()>4)s.recent.pop_front();
            if(state.camp.asking)
            {
                state.camp.asking=false;
                auto& said=state.camp.said;
                said.push_back({false,s.text});
                if(said.size()>16)said.erase(said.begin(),said.begin()+2);
            }
        }
        InvalidateRect(state.window,nullptr,FALSE);
    }

    void TickDungeonWindow(MascotModel& model,double dt)
    {
        if(!state.window)return;
        // Her words: the LLM's line waited for no longer than AskWait (then her own), a note
        // under the view cleared when its time is up.
        {
            auto& s=state.speech;
            const ULONGLONG now=GetTickCount64();
            if(s.waiting&&s.text==L"…"&&now-s.since>AskWait){s.waiting=false;s.text=s.fallback;InvalidateRect(state.window,nullptr,FALSE);}
            if(!state.toast.empty()&&now>=state.toastUntil){state.toast.clear();InvalidateRect(state.window,nullptr,FALSE);}
            // Away from the fire (a monster about, or she has walked off): the talk ends.
            if(state.camp.edit&&!state.camp.asking&&!ByFire())CloseTalk();
        }
        if(!Showing3D()||state.showBook)return;
        auto& r=*state.renderer;
        const auto& game=state.game;
        // Rebuilt after each turn, and when the camera changes (the ceiling is there from behind
        // her only). The walls that would hide her sink by themselves (GeometryVS).
        const long long key=(static_cast<long long>(game.Floor())*100000+game.Turns())*8+static_cast<int>(game.GetState())*2+(state.behind?1:0);
        // While her attack is on its way, the scene stays as it was (the monster still there)
        // until the blow lands.
        const bool waitingForBlow=state.strikeAsked!=Strike::None||(state.strike!=Strike::None&&state.strikeTime<state.strikeLands);
        if(key!=state.sceneKey&&!waitingForBlow)
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
                Step(sx,sy);
                Listen();
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
                    Step(sx,sy);
                    Listen();
                    InvalidateRect(state.window,nullptr,FALSE);
                }
            }
        }
        const auto& hero=game.Hero();
        const float tx=(hero.x+0.5f)*TileSize,tz=-(hero.y+0.5f)*TileSize;
        if(!state.placed||state.floor!=game.Floor())
        {
            state.drawnX=tx;state.drawnZ=tz;
            state.leap.kind=0;
            state.drawnAngle=std::atan2(-static_cast<float>(hero.dirX),static_cast<float>(hero.dirY));
            state.cameraAngle=std::atan2(static_cast<float>(hero.dirX),-static_cast<float>(hero.dirY));
            state.floor=game.Floor();
            model.SetFacingWalk(true,1.4f);   // brisker than on the desktop: a dungeon is walked a lot
        }

        // She walks to her tile: her gait's own pace, so the planted foot stays put; she runs
        // when she has fallen more than a tile behind (keys pressed faster than she walks), and
        // a new floor (or a warp) puts her there at once.
        const float dx=tx-state.drawnX,dz=tz-state.drawnZ,distance=std::sqrt(dx*dx+dz*dz);
        // A jump: pushing off, then flying in an arc to her tile, landing with a dip. She keeps
        // facing the way she faced.
        float lift=0;
        if(state.leap.kind)
        {
            auto& j=state.leap;
            const bool side=j.kind==2;
            // Ahead: a long hop off one foot (a triple jump's first step), at once; back: a
            // crouch, then a small jump; aside: a quick low hop.
            const float crouch=static_cast<float>(j.kind==1?BoundTakeoffSeconds:side?SideTakeoffSeconds:JumpCrouchSeconds);
            const float air=static_cast<float>(j.kind==1?BoundAirSeconds:side?SideAirSeconds:0.38),height=j.kind==1?4.5f:j.kind<0?3.5f:3.0f;
            // The hops land in their own poses (a knee giving, the other leg coming in).
            const float settle=static_cast<float>(j.kind==1?BoundLandSeconds:side?SideLandSeconds:0.0);
            if(j.time==0)
            {
                // To the side: which way in her own frame (+x or -x of the model), from where
                // she goes on the map (world x, and z is -y) and the way she faces.
                const float a=std::atan2(-static_cast<float>(hero.dirX),static_cast<float>(hero.dirY));
                const float wx=j.toX-j.fromX,wz=j.toZ-j.fromZ,localX=wx*std::cos(a)-wz*std::sin(a);
                const Activity act=j.kind==1?Activity::Bound:side?Activity::SideHop:Activity::Jump;
                model.SetActivity(act,side?(localX>0?1:-1):0);
                state.gait=act;
            }
            j.time+=dt;
            const float u=std::clamp((static_cast<float>(j.time)-crouch)/air,0.0f,1.0f);
            state.drawnX=j.fromX+(j.toX-j.fromX)*u;
            state.drawnZ=j.fromZ+(j.toZ-j.fromZ)*u;
            lift=4*height*u*(1-u);
            const float rise=u>0&&u<1?4*height*(1-2*u)/air:0.0f;
            // Falling with her, the hair and the tail float (no more than weightless: the arc
            // is steeper than the physics' own gravity, which would throw them up).
            model.SetCarryMotion(0,rise,0,u>0&&u<1?-0.8f*std::min(8*height/(air*air),98.0f):0.0f);
            if(u>=1&&j.time>=crouch+air+settle)
            {
                model.SetCarryMotion(0,0,0,0);
                if(j.kind<0)model.Land(4*height/air);
                if(state.gait!=Activity::Idle){model.SetActivity(Activity::Idle);state.gait=Activity::Idle;}
                j.kind=0;
            }
            const float angle=std::atan2(-static_cast<float>(hero.dirX),static_cast<float>(hero.dirY));
            state.drawnAngle+=std::remainder(angle-state.drawnAngle,XM_2PI)*static_cast<float>(1-std::exp(-dt/0.06));
        }
        else if(distance>TileSize*4){state.drawnX=tx;state.drawnZ=tz;}
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
        const XMMATRIX world=XMMatrixTranslation(-model.RootOffset(),0,0)*XMMatrixRotationY(state.drawnAngle)*XMMatrixTranslation(state.drawnX,lift,state.drawnZ);
        r.SetModelTransform(world);

        // Her attack: started once she stands (asked for while she still walked or turned, it
        // waits a moment, then is let go). The sword is light in her fist, a blade of sparks
        // with a trail where its tip went through the cut; the spell gathers in her palm and
        // flies along the bolt's path.
        if(state.strikeAsked!=Strike::None)
        {
            if(model.Strike(state.strikeAsked==Strike::Magic))
            {
                state.strike=state.strikeAsked;
                state.strikeTime=0;
                state.strikeLands=MascotModel::StrikeHit+(state.strike==Strike::Magic?state.boltTiles/12.0f:0.0);
                state.trail.clear();
                state.strikeAsked=Strike::None;
            }
            else if(GetTickCount64()-state.strikeAskedAt>600)state.strikeAsked=Strike::None;
        }
        std::vector<MascotRenderer::Particle> sparks;
        // Fire and magic light what is near them (most wanted first: the attack's own light,
        // then the campfires nearest her).
        std::vector<MascotRenderer::PointLight> lights;
        auto shine=[&](FXMVECTOR p,float radius,float r,float g,float b){
            MascotRenderer::PointLight l;
            l.position[0]=XMVectorGetX(p);l.position[1]=XMVectorGetY(p);l.position[2]=XMVectorGetZ(p);
            l.radius=radius;l.color[0]=r;l.color[1]=g;l.color[2]=b;
            lights.push_back(l);
        };
        // A soft halo of light around fire and magic.
        auto halo=[&](FXMVECTOR p,float size,float r,float g,float b,float a){
            MascotRenderer::Particle q;
            q.position={XMVectorGetX(p),XMVectorGetY(p),XMVectorGetZ(p)};
            q.size=size;q.color={r,g,b,std::clamp(a,0.0f,1.0f)};q.glow=true;
            sparks.push_back(q);
        };
        // How bright a flame is at t, about 1: waves that never line up.
        auto flicker=[](float t,float seed){return 1.0f+0.08f*std::sin(t*9.1f+seed)+0.05f*std::sin(t*17.3f+seed*2.1f)+0.04f*std::sin(t*29.9f+seed*3.7f);};
        const float clock=static_cast<float>(state.monsterClock);
        if(state.strike!=Strike::None)
        {
            const double before=state.strikeTime;
            state.strikeTime+=dt;
            // The blow lands: sparks fly from the monster there, and a puff where one fell.
            if(before<state.strikeLands&&state.strikeTime>=state.strikeLands&&state.strikeTile.first>=0)
            {
                const auto [x,y]=state.strikeTile;
                const bool fell=std::none_of(game.Monsters().begin(),game.Monsters().end(),[&](const Dungeon::Monster& m){return Dungeon::Occupies(m,x,y);});
                for(const auto& m:state.shown)
                    if(Dungeon::Occupies(m,x,y))if(const auto it=state.looks.find(m.uid);it!=state.looks.end())it->second.flinch=0;
                const XMFLOAT3 at{(x+0.5f)*TileSize,6.0f,-(y+0.5f)*TileSize};
                state.flashAt=at;
                state.flashAge=0;
                std::uint32_t seed=static_cast<std::uint32_t>(GetTickCount64());
                auto random=[&]{seed=seed*1664525u+1013904223u;return (seed>>8)/16777216.0f;};
                for(int i=0;i<(fell?30:16);++i)
                {
                    const float a=random()*XM_2PI,up=random()*2.0f-0.5f,v=(fell?10.0f:18.0f)+random()*16.0f;
                    Spark s{at,{std::cos(a)*v,up*v,std::sin(a)*v},(fell?1.6f:0.9f)+random()*0.8f,{}, 0,0.3+random()*(fell?0.5:0.25)};
                    const bool purple=fell&&i%2==0;
                    s.color[0]=purple?0.8f:1.0f;s.color[1]=purple?0.6f:0.95f;s.color[2]=purple?1.0f:0.7f;
                    state.burst.push_back(s);
                }
            }
            const float t=static_cast<float>(state.strikeTime),hit=static_cast<float>(MascotModel::StrikeHit);
            auto place=[&](FXMVECTOR p,float size,float r,float g,float b,float a,float angle=0){
                MascotRenderer::Particle q;
                q.position={XMVectorGetX(p),XMVectorGetY(p),XMVectorGetZ(p)};
                q.size=size;q.color={r,g,b,std::clamp(a,0.0f,1.0f)};q.angle=angle;
                sparks.push_back(q);
            };
            auto toWorld=[&](const std::array<float,3>& p){return XMVector3TransformCoord(XMVectorSet(p[0],p[1],p[2],0),world);};
            if(state.strike==Strike::Sword)
            {
                std::array<float,3> middle,along;
                if(t>1.15f)state.strike=Strike::None;
                else if(model.Grip(1,middle,along))
                {
                    const float shown=std::clamp(t/0.1f,0.0f,1.0f)*std::clamp((1.15f-t)/0.3f,0.0f,1.0f);
                    const XMVECTOR base=toWorld(middle);
                    const XMVECTOR way=XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(along[0],along[1],along[2],0),world));
                    constexpr float Blade=7.5f;
                    for(int i=0;i<16;++i)
                    {
                        const float f=0.6f+Blade*i/15.0f;
                        place(XMVectorAdd(base,XMVectorScale(way,f)),0.45f-0.15f*i/15.0f,0.65f,0.85f,1.0f,shown*0.9f);
                    }
                    halo(XMVectorAdd(base,XMVectorScale(way,Blade*0.55f)),3.2f,0.55f,0.75f,1.0f,shown*0.35f);
                    shine(XMVectorAdd(base,XMVectorScale(way,Blade*0.55f)),TileSize*1.0f,0.25f*shown,0.35f*shown,0.5f*shown);
                    // The tip's trail, through the cut only.
                    if(t>hit-0.11f&&t<hit+0.05f)
                    {
                        XMFLOAT3 tip;
                        XMStoreFloat3(&tip,XMVectorAdd(base,XMVectorScale(way,Blade)));
                        state.trail.push_back(tip);
                    }
                    const size_t n=state.trail.size();
                    for(size_t i=0;i<n;++i)
                    {
                        const float age=static_cast<float>(n-i)/std::max<size_t>(n,1);
                        const XMVECTOR tip=XMLoadFloat3(&state.trail[i]);
                        // Each tip and halfway down its blade, fading as the cut goes on.
                        const float fade=std::clamp((hit+0.35f-t)/0.3f,0.0f,1.0f)*(1.0f-0.6f*age);
                        place(tip,0.9f,0.75f,0.9f,1.0f,fade*0.8f);
                        place(XMVectorLerp(tip,base,0.35f),0.7f,0.75f,0.9f,1.0f,fade*0.5f);
                        if(i%3==0)halo(XMVectorLerp(tip,base,0.2f),2.4f,0.6f,0.8f,1.0f,fade*0.3f);
                    }
                }
            }
            else
            {
                std::array<float,3> palm,other;
                const auto& h=game.Hero();
                const XMVECTOR forward=XMVector3Normalize(XMVectorSet(static_cast<float>(h.dirX),0,-static_cast<float>(h.dirY),0));
                const float speed=TileSize*12.0f,reach=state.boltTiles*TileSize;
                if(t>std::max(hit+reach/speed+0.35f,1.25f))state.strike=Strike::None;
                else if(model.PalmPosition(1,palm))
                {
                    // Between her two palms (both hands cast it).
                    if(model.PalmPosition(0,other))for(int i=0;i<3;++i)palm[i]=(palm[i]+other[i])*0.5f;
                    const XMVECTOR hand=XMVectorAdd(toWorld(palm),XMVectorScale(forward,0.8f));
                    if(t<hit)
                    {
                        // Gathering: sparks circling in to the palm, brighter as they come.
                        const float k=t/hit;
                        for(int i=0;i<6;++i)
                        {
                            const float a=XM_2PI*i/6.0f+t*9.0f,radius=3.0f*(1.0f-k);
                            place(XMVectorAdd(hand,XMVectorSet(radius*std::cos(a),radius*std::sin(a),radius*0.5f*std::sin(a*0.7f),0)),0.5f,1.0f,0.75f,0.4f,k);
                        }
                        place(hand,1.5f+2.0f*k,1.0f,0.8f,0.45f,k);
                        const float f=flicker(t*1.5f,3.0f);
                        halo(hand,(2.5f+4.5f*k)*f,1.0f,0.7f,0.35f,0.7f*k);
                        shine(hand,TileSize*(0.6f+0.8f*k)*f,0.9f*k,0.6f*k,0.25f*k);
                    }
                    else
                    {
                        // Flying the way she faces in an arc (over her head's height, so it shows from
                        // behind her too) down to a monster's middle at the end of the path, where
                        // it bursts and fades.
                        const float flown=std::min((t-hit)*speed,reach);
                        const float end=std::clamp((t-hit-reach/speed)/0.35f,0.0f,1.0f);
                        const float from=XMVectorGetY(hand),arc=std::min(10.0f,reach*0.3f);
                        auto path=[&](float d){
                            const float f=reach>0?d/reach:1.0f;
                            const XMVECTOR p=XMVectorAdd(hand,XMVectorScale(forward,d));
                            return XMVectorSetY(p,from+(7.0f-from)*f+arc*std::sin(XM_PI*f));
                        };
                        place(path(flown),4.0f+6.0f*end,1.0f,0.8f,0.45f,1.0f-end,t*4.0f);
                        const float f=flicker(t*1.5f,3.0f);
                        halo(path(flown),(8.0f+8.0f*end)*f,1.0f,0.65f,0.3f,0.7f*(1.0f-end));
                        halo(path(std::max(0.0f,flown-5.0f)),5.0f*f,1.0f,0.6f,0.25f,0.35f*(1.0f-end));
                        shine(path(flown),TileSize*1.8f*f,1.0f*(1.0f-end),0.7f*(1.0f-end),0.3f*(1.0f-end));
                        for(int i=1;i<=8;++i)
                            place(path(std::max(0.0f,flown-i*1.5f)),2.6f-0.25f*i,1.0f,0.7f,0.35f,(1.0f-end)*(1.0f-i/9.0f),t*3.0f+i);
                    }
                }
            }
        }
        // The dragon's fire: flames running out along its line from the dragon, flickering up,
        // then dying down.
        if(!game.Flame().empty()&&state.flameTurn!=game.Turns())
        {
            state.flameTurn=game.Turns();
            state.flame=game.Flame();
            state.flameTime=0;
        }
        if(!state.flame.empty())
        {
            state.flameTime+=dt;
            const float t=static_cast<float>(state.flameTime);
            const float wait=static_cast<float>(BreathTime)*FireFrom;   // the fire reaching them
            if(t>wait+1.0f)state.flame.clear();
            for(size_t k=0;k<state.flame.size();++k)
            {
                const float age=t-wait-0.06f*static_cast<float>(k);
                if(age<0)continue;
                const float fade=std::clamp(1.0f-(age-0.35f)/0.4f,0.0f,1.0f)*std::clamp(age/0.08f,0.0f,1.0f);
                const auto [x,y]=state.flame[k];
                const XMVECTOR middle=XMVectorSet((x+0.5f)*TileSize,4.0f,-(y+0.5f)*TileSize,0);
                const float f=flicker(t*1.3f,static_cast<float>(k));
                halo(middle,11.0f*f,1.0f,0.4f,0.1f,0.6f*fade);
                if(k%2==0)shine(middle,TileSize*1.6f*f,0.9f*fade,0.4f*fade,0.1f*fade);
                for(int j=0;j<10;++j)
                {
                    // Each flame rises and sways on its own (j spreads them over the tile).
                    const float u=static_cast<float>(j)/10.0f,rise=std::fmod(age*14.0f+u*7.0f,7.0f);
                    MascotRenderer::Particle q;
                    q.position={(x+0.25f+0.5f*std::fmod(u*3.7f+0.3f*k,1.0f))*TileSize+std::sin(age*11.0f+j)*0.8f,
                                2.0f+rise*1.6f,
                                -(y+0.25f+0.5f*std::fmod(u*2.3f+0.6f,1.0f))*TileSize};
                    q.size=6.0f-0.5f*rise;
                    q.color={1.0f,0.3f+0.35f*(1.0f-rise/7.0f),0.08f,fade*(1.0f-rise/9.0f)};
                    q.angle=age*3.0f+j;
                    sparks.push_back(q);
                }
            }
        }
        for(auto& s:state.burst)
        {
            s.age+=dt;
            s.at.x+=s.speed.x*static_cast<float>(dt);s.at.y+=s.speed.y*static_cast<float>(dt);s.at.z+=s.speed.z*static_cast<float>(dt);
            s.speed.y-=30.0f*static_cast<float>(dt);
            const float k=static_cast<float>(std::exp(-dt/0.25));
            s.speed.x*=k;s.speed.z*=k;
            const float fade=1.0f-static_cast<float>(s.age/s.life);
            if(fade<=0)continue;
            MascotRenderer::Particle q;
            q.position={s.at.x,std::max(0.5f,s.at.y),s.at.z};
            q.size=s.size*(0.6f+0.4f*fade);
            q.color={s.color[0],s.color[1],s.color[2],fade};
            q.angle=static_cast<float>(s.age*6.0);
            sparks.push_back(q);
        }
        state.burst.erase(std::remove_if(state.burst.begin(),state.burst.end(),[](const auto& s){return s.age>=s.life;}),state.burst.end());
        // The dragon's fire: flames pouring from its mouth toward her, carried across as its
        // head sweeps, spreading over the floor where they reach it, white-hot as they leave,
        // red as they die, and lighting all along the way.
        {
            const float b=state.breathing;
            if(b>=FireFrom&&b<=FireTo)
            {
                const XMVECTOR from=XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(state.mouth.at));
                const XMVECTOR aim=XMVector3Normalize(XMVectorSubtract(XMVectorSet(state.drawnX,2.0f,state.drawnZ,0),from));
                const XMVECTOR way=XMVector3TransformNormal(aim,XMMatrixRotationY(state.mouth.sweep*1.25f));
                const XMVECTOR side=XMVector3Normalize(XMVector3Cross(way,XMVectorSet(0,1,0,0))),up=XMVector3Cross(side,way);
                std::uint32_t seed=static_cast<std::uint32_t>(GetTickCount64()*2654435761u);
                auto random=[&]{seed=seed*1664525u+1013904223u;return (seed>>8)/16777216.0f;};
                for(state.fireOwed+=dt*150.0;state.fireOwed>=1;state.fireOwed-=1)
                {
                    const float a=random()*XM_2PI,r=0.13f*random();
                    const XMVECTOR v=XMVectorScale(XMVector3Normalize(XMVectorAdd(way,XMVectorAdd(XMVectorScale(side,r*std::cos(a)),XMVectorScale(up,r*std::sin(a))))),60.0f+20.0f*random());
                    Spark f{};
                    XMStoreFloat3(&f.at,XMVectorAdd(from,XMVectorScale(v,random()*static_cast<float>(dt))));
                    XMStoreFloat3(&f.speed,v);
                    f.size=random();
                    f.life=0.45+0.2*random();
                    state.fire.push_back(f);
                }
            }
            else state.fireOwed=0;
            XMVECTOR sum=XMVectorZero();
            int n=0;
            for(auto& f:state.fire)
            {
                f.age+=dt;
                const float drag=static_cast<float>(std::exp(-dt/0.5));
                f.at.x+=f.speed.x*static_cast<float>(dt);f.at.y+=f.speed.y*static_cast<float>(dt);f.at.z+=f.speed.z*static_cast<float>(dt);
                f.speed.x*=drag;f.speed.z*=drag;f.speed.y=f.speed.y*drag+6.0f*static_cast<float>(dt);
                if(f.at.y<1.0f){f.at.y=1.0f;f.speed.y=std::max(0.0f,f.speed.y);}   // spreading over the floor
                const float u=static_cast<float>(f.age/f.life);
                if(u>=1)continue;
                const XMVECTOR at=XMLoadFloat3(&f.at);
                // White-hot to orange to dark red, growing as it goes.
                const float cg=u<0.25f?0.95f-1.6f*u:std::max(0.15f,0.55f-0.55f*(u-0.25f)/0.75f),cb=std::max(0.05f,0.6f-2.2f*u);
                const float size=1.2f+7.0f*u+1.5f*f.size,fade=std::pow(1.0f-u,0.7f);
                if(static_cast<int>(f.size*100)%2==0)halo(at,size*1.3f,1.0f,cg,cb,0.4f*fade);
                else
                {
                    MascotRenderer::Particle q;
                    q.position={f.at.x,f.at.y,f.at.z};
                    q.size=size*0.55f;q.color={1.0f,cg,cb,fade};q.angle=static_cast<float>(f.age*5.0)+f.size*6.0f;
                    sparks.push_back(q);
                }
                sum=XMVectorAdd(sum,at);++n;
            }
            state.fire.erase(std::remove_if(state.fire.begin(),state.fire.end(),[](const Spark& f){return f.age>=f.life;}),state.fire.end());
            if(n>0)
            {
                const float k=std::min(1.0f,n/40.0f);
                shine(XMVectorScale(sum,1.0f/n),TileSize*3.0f,1.4f*k,0.65f*k,0.2f*k);
                if(b>=FireFrom&&b<=FireTo)shine(XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(state.mouth.at)),TileSize*1.6f,1.0f,0.6f,0.25f);
            }
        }
        // The flash where a blow landed.
        if(state.flashAge>=0)
        {
            state.flashAge+=dt;
            const float a=static_cast<float>(state.flashAge),f=1.0f-a/0.3f;
            if(f<=0)state.flashAge=-1;
            else
            {
                const XMVECTOR at=XMLoadFloat3(&state.flashAt);
                halo(at,8.0f+14.0f*a,1.0f,0.9f,0.7f,0.8f*f);
                shine(at,TileSize*1.6f,1.2f*f,1.05f*f,0.8f*f);
            }
        }
        // The campfires in sight: a warm glow over the flames and on all around, flickering,
        // and sparks rising from them.
        {
            std::vector<const Dungeon::Fire*> burning;
            for(const auto& f:game.Fires())
                if(!f.used&&game.Visible(f.x,f.y))burning.push_back(&f);
            const auto& h=game.Hero();
            std::sort(burning.begin(),burning.end(),[&](const Dungeon::Fire* a,const Dungeon::Fire* b){
                return std::abs(a->x-h.x)+std::abs(a->y-h.y)<std::abs(b->x-h.x)+std::abs(b->y-h.y);});
            for(const auto* f:burning)
            {
                const float x=(f->x+0.5f)*TileSize,z=-(f->y+0.5f)*TileSize,seed=static_cast<float>(f->x*7+f->y*13);
                const float b=flicker(clock,seed);
                halo(XMVectorSet(x,4.0f,z,0),10.0f*b,1.0f,0.45f,0.12f,0.32f*b);
                halo(XMVectorSet(x,2.5f,z,0),5.0f*b,1.0f,0.75f,0.35f,0.3f*b);
                if(lights.size()<MascotRenderer::MaxLights)
                    shine(XMVectorSet(x,4.0f,z,0),TileSize*3.3f*(0.7f+0.3f*b),1.5f*b,0.7f*b,0.22f*b);
                for(int j=0;j<7;++j)
                {
                    // Each spark rises and drifts, fading out as it goes, then starts again.
                    const float p=std::fmod(clock*0.45f+j/7.0f+seed*0.37f,1.0f),a=seed+j*2.4f;
                    MascotRenderer::Particle q;
                    q.position={x+std::cos(a)*(0.6f+2.2f*p)+0.8f*std::sin(clock*2.3f+j),3.0f+p*15.0f,z+std::sin(a)*(0.6f+2.2f*p)};
                    q.size=0.12f+0.3f*(1.0f-p);
                    q.color={1.0f,0.55f+0.3f*(1.0f-p),0.2f,(1.0f-p)*std::clamp(p*8.0f,0.0f,1.0f)};
                    q.angle=clock*3.0f+j;
                    sparks.push_back(q);
                }
            }
        }
        // What she picked up: the things gone from the floor since last frame (where she has
        // seen them), each glowing up once she is there, then fading.
        {
            if(state.lastFloor!=game.Floor())state.pickups.clear();
            else
            {
                auto gone=[&](int x,int y,auto& now){return game.Seen(x,y)&&std::none_of(now.begin(),now.end(),[&](const auto& t){return t.x==x&&t.y==y;});};
                for(const auto& c:state.lastChests)if(gone(c.x,c.y,game.Chests()))state.pickups.push_back({Picked::Chest,c.x,c.y,0,c.item});
                for(const auto& g:state.lastGolds)if(gone(g.x,g.y,game.Golds()))state.pickups.push_back({Picked::Gold,g.x,g.y,g.amount});
                for(const auto& w:state.lastWares)if(gone(w.x,w.y,game.Wares()))state.pickups.push_back({Picked::Goods,w.x,w.y,0,w.item});
                for(const auto& [x,y]:state.lastKeys)
                    if(game.Seen(x,y)&&std::find(game.Keys().begin(),game.Keys().end(),std::pair{x,y})==game.Keys().end())state.pickups.push_back({Picked::Key,x,y});
            }
            state.lastFloor=game.Floor();
            state.lastChests=game.Chests();state.lastGolds=game.Golds();state.lastKeys=game.Keys();state.lastWares=game.Wares();
            for(auto& p:state.pickups)
            {
                const float x=(p.x+0.5f)*TileSize,z=-(p.y+0.5f)*TileSize;
                if(p.age<0)
                {
                    // Until she reaches it (as she is drawn), or a moment has passed.
                    p.waited+=dt;
                    const float dx=state.drawnX-x,dz=state.drawnZ-z;
                    if(dx*dx+dz*dz<0.16f*TileSize*TileSize||p.waited>0.6)p.age=0;
                    continue;
                }
                p.age+=dt;
                const float f=static_cast<float>(p.age/PickupLife);
                if(f>=1)continue;
                // Brightening quickly, then fading slowly; gold for gold, a chest and a key.
                const float a=f<0.3f?f/0.3f:1.0f-(f-0.3f)/0.7f;
                const bool golden=p.kind!=Picked::Goods;
                const float cr=1.0f,cg=golden?0.85f:0.95f,cb=golden?0.45f:0.8f;
                halo(XMVectorSet(x,3.0f+2.0f*f,z,0),6.0f+4.0f*f,cr,cg,cb,0.5f*a);
                shine(XMVectorSet(x,4.0f,z,0),TileSize*1.6f,0.9f*cr*a,0.9f*cg*a,0.9f*cb*a);
                for(int j=0;j<7;++j)
                {
                    // Sparkles rising around it.
                    const float an=XM_2PI*j/7+f*2.0f+p.x,rise=f*(10.0f+2.0f*(j%3));
                    MascotRenderer::Particle q;
                    q.position={x+std::cos(an)*(1.5f+1.5f*f),2.0f+rise,z+std::sin(an)*(1.5f+1.5f*f)};
                    q.size=0.55f*(1.0f-f)+0.15f;
                    q.color={cr,cg,cb,a};
                    q.angle=f*6.0f+j;
                    sparks.push_back(q);
                }
            }
            state.pickups.erase(std::remove_if(state.pickups.begin(),state.pickups.end(),[](const Pickup& p){return p.age>=PickupLife;}),state.pickups.end());
        }
        // Crystals and magic circles (warp traps) in sight: a glow that swells and fades
        // slowly, lighting what is near (the nearest few, after the fires).
        {
            struct Gleam {XMVECTOR at;float size,r,g,b,pulse;int distance;};
            std::vector<Gleam> gleams;
            const auto& h=game.Hero();
            for(const auto& c:CrystalSpots(game))
            {
                if(!game.Visible(c.tx,c.ty))continue;
                const float pulse=0.8f+0.2f*std::sin(clock*1.6f+c.seed*0.9f);
                // The colour of its tallest crystal (AddCrystals).
                static constexpr float colours[3][3]={{0.45f,0.85f,1.0f},{0.80f,0.60f,1.0f},{0.55f,0.95f,0.95f}};
                const float* k=colours[c.seed%3];
                gleams.push_back({XMVectorSet(c.x,c.height*0.5f,c.z,0),c.height*1.2f,k[0],k[1],k[2],pulse,std::abs(c.tx-h.x)+std::abs(c.ty-h.y)});
            }
            for(const auto& t:game.Traps())
            {
                if(!t.found||t.kind!=Dungeon::TrapKind::Warp||!game.Visible(t.x,t.y))continue;
                const float x=(t.x+0.5f)*TileSize,z=-(t.y+0.5f)*TileSize,seed=static_cast<float>(t.x*5+t.y*11);
                const float pulse=0.75f+0.25f*std::sin(clock*2.2f+seed);
                gleams.push_back({XMVectorSet(x,1.0f,z,0),7.0f,0.75f,0.45f,1.0f,pulse,std::abs(t.x-h.x)+std::abs(t.y-h.y)});
                // Motes of light circling up from it.
                for(int j=0;j<5;++j)
                {
                    const float p=std::fmod(clock*0.35f+j/5.0f,1.0f),a=clock*1.2f+XM_2PI*j/5;
                    MascotRenderer::Particle q;
                    q.position={x+std::cos(a)*2.8f*(1.0f-0.4f*p),0.5f+p*9.0f,z+std::sin(a)*2.8f*(1.0f-0.4f*p)};
                    q.size=0.35f*(1.0f-p)+0.1f;
                    q.color={0.85f,0.65f,1.0f,(1.0f-p)*std::clamp(p*6.0f,0.0f,1.0f)};
                    q.angle=a;
                    sparks.push_back(q);
                }
            }
            std::sort(gleams.begin(),gleams.end(),[](const Gleam& a,const Gleam& b){return a.distance<b.distance;});
            for(const auto& g:gleams)
            {
                halo(g.at,g.size*(0.9f+0.1f*g.pulse),g.r,g.g,g.b,0.6f*g.pulse);
                halo(g.at,g.size*0.45f,0.5f+0.5f*g.r,0.5f+0.5f*g.g,0.5f+0.5f*g.b,0.45f*g.pulse);
                if(lights.size()<MascotRenderer::MaxLights)
                    shine(XMVectorSetY(g.at,4.0f),TileSize*2.2f,0.9f*g.r*g.pulse,0.9f*g.g*g.pulse,0.9f*g.b*g.pulse);
            }
        }
        r.SetParticles(sparks);
        r.SetLights(lights);

        // The monsters: taken from the game once her blow has landed (a fallen one goes then,
        // the others move on and strike back after it), then each glides to its tile, walking,
        // turned to her, striking when it went for her.
        {
            const bool waiting=state.strikeAsked!=Strike::None||(state.strike!=Strike::None&&state.strikeTime<state.strikeLands);
            const long long key=static_cast<long long>(game.Floor())*100000+game.Turns();
            if(key!=state.shownKey&&!waiting)
            {
                if(state.shownFloor!=game.Floor()){state.looks.clear();state.shownFloor=game.Floor();}
                state.shown=game.Monsters();
                state.shownKey=key;
                for(auto it=state.looks.begin();it!=state.looks.end();)
                    it=std::any_of(state.shown.begin(),state.shown.end(),[&](const Dungeon::Monster& m){return m.uid==it->first;})?std::next(it):state.looks.erase(it);
                for(const auto& m:state.shown)
                    if(m.struck)if(const auto it=state.looks.find(m.uid);it!=state.looks.end())
                    {
                        // The dragon's fire (the game's flame this turn) instead of a bite.
                        if(m.boss&&!game.Flame().empty())it->second.breath=0;
                        else it->second.strike=0;
                    }
            }
            state.monsterClock+=dt;
            std::vector<MascotRenderer::GeometryVertex> moving;
            state.breathing=-1;
            for(const auto& m:state.shown)
            {
                const float tx=(m.x+0.5f)*TileSize,tz=-(m.y+0.5f)*TileSize;
                auto [it,fresh]=state.looks.try_emplace(m.uid);
                auto& l=it->second;
                if(fresh)
                {
                    l.x=tx;l.z=tz;l.yaw=std::atan2(state.drawnX-tx,state.drawnZ-tz);
                    if(m.struck)(m.boss&&!game.Flame().empty()?l.breath:l.strike)=0;
                }
                const float dx=tx-l.x,dz=tz-l.z,distance=std::sqrt(dx*dx+dz*dz);
                if(distance>TileSize*3){l.x=tx;l.z=tz;}
                else if(distance>0.05f)
                {
                    // Brisker when it has fallen behind (two steps in a turn).
                    const float speed=TileSize/(distance>TileSize*1.2f?0.16f:0.3f);
                    const float step=std::min(distance,speed*static_cast<float>(dt));
                    l.x+=dx/distance*step;l.z+=dz/distance*step;
                    l.moving=std::min(1.0f,l.moving+static_cast<float>(dt/0.12));
                }
                else{l.x=tx;l.z=tz;l.moving=std::max(0.0f,l.moving-static_cast<float>(dt/0.2));}
                if(l.moving>0)l.walk+=static_cast<float>(dt)*XM_2PI*3.0f;
                const float face=std::atan2(state.drawnX-l.x,state.drawnZ-l.z);
                l.yaw+=std::remainder(face-l.yaw,XM_2PI)*static_cast<float>(1-std::exp(-dt/0.12));
                if(l.strike>=0&&(l.strike+=static_cast<float>(dt/0.6))>1)l.strike=-1;
                if(l.flinch>=0&&(l.flinch+=static_cast<float>(dt/0.35))>1)l.flinch=-1;
                if(l.breath>=0&&(l.breath+=static_cast<float>(dt/BreathTime))>1)l.breath=-1;
                if(!game.Visible(m.x,m.y))continue;
                MonsterPose pose;
                pose.walk=l.walk;pose.moving=l.moving;pose.strike=l.strike;pose.flinch=l.flinch;
                // Each one a little apart (kept small: a float this large would step).
                pose.time=static_cast<float>(state.monsterClock+(m.uid%61)*1.7);
                pose.charging=m.charging;
                pose.breath=l.breath;
                MonsterMouth mouth;
                const float tall=AddMonster(moving,m,l.x,l.z,l.yaw,pose,&mouth);
                if(l.breath>=0){state.mouth=mouth;state.breathing=l.breath;}
                // Its health above it when hurt.
                if(m.hp<m.maxHp)
                {
                    const float w=(0.8f+1.6f*m.size)*TileSize,left=l.x-w/2,top=tall+0.15f*TileSize,f=static_cast<float>(std::max(0,m.hp))/m.maxHp;
                    auto bar=[&](float x0,float y0,float x1,float y1,float cr,float cg,float cb){
                        const float z0=l.z-0.06f*TileSize,z1=l.z+0.06f*TileSize;
                        const float quads[3][4][3]={{{x0,y1,z0},{x1,y1,z0},{x1,y1,z1},{x0,y1,z1}},{{x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0}},{{x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1}}};
                        const float normals[3][3]={{0,1,0},{0,0,-1},{0,0,1}};
                        for(int q=0;q<3;++q)
                            for(int k:{0,1,2,0,2,3})
                                moving.push_back({{quads[q][k][0],quads[q][k][1],quads[q][k][2]},{normals[q][0],normals[q][1],normals[q][2]},{cr,cg,cb,1}});
                    };
                    bar(left,top,left+w,top+0.08f*TileSize,0.15f,0.12f,0.18f);
                    bar(left,top+0.01f*TileSize,left+w*f,top+0.09f*TileSize,0.9f,0.3f,0.3f);
                }
            }
            // What she picked up: as it lay until she gets there, then brightening, rising and
            // shrinking away.
            for(const auto& p:state.pickups)
            {
                const float x=(p.x+0.5f)*TileSize,z=-(p.y+0.5f)*TileSize,k=game.Visible(p.x,p.y)?1.0f:0.5f;
                std::vector<MascotRenderer::GeometryVertex> thing;
                switch(p.kind)
                {
                case Picked::Chest:AddChest(thing,x,z,k);break;
                case Picked::Gold:AddGold(thing,x,z,p.amount,k);break;
                case Picked::Key:AddKey(thing,x,z,k);break;
                case Picked::Goods:AddGoods(thing,x,z,p.item,k);break;
                }
                const float f=p.age<0?0.0f:static_cast<float>(p.age/PickupLife);
                const float bright=1.0f+0.8f*std::min(1.0f,f/0.3f),size=f<0.3f?1.0f:std::max(0.0f,1.0f-(f-0.3f)/0.7f),lift=4.0f*f;
                for(auto& v:thing)
                {
                    v.position[0]=x+(v.position[0]-x)*size;
                    v.position[1]=v.position[1]*size+lift;
                    v.position[2]=z+(v.position[2]-z)*size;
                    for(int i=0;i<3;++i)v.color[i]*=bright;
                }
                moving.insert(moving.end(),thing.begin(),thing.end());
            }
            // The campfires' flames, flickering.
            for(const auto& f:game.Fires())
                if(!f.used&&game.Seen(f.x,f.y))
                    AddFlames(moving,(f.x+0.5f)*TileSize,-(f.y+0.5f)*TileSize,static_cast<float>(state.monsterClock)+f.x*1.3f+f.y*0.7f,game.Visible(f.x,f.y)?1.0f:0.5f);
            r.SetMovingGeometry(std::move(moving));
        }
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
            // Looking a little up the passage, so the ceiling over it shows. The walls stand
            // tall here, so the camera comes in short of the first wall behind her (and a little
            // lower, closer in).
            float back=TileSize*3.2f;
            for(float s=0.3f;s<=3.2f;s+=0.1f)
            {
                const float px=state.drawnX-std::sin(state.cameraAngle)*s*TileSize,pz=state.drawnZ-std::cos(state.cameraAngle)*s*TileSize;
                if(game.At(static_cast<int>(std::floor(px/TileSize)),static_cast<int>(std::floor(-pz/TileSize)))==Dungeon::Tile::Wall)
                {
                    // Half a tile in from the wall: a wall 2 tiles behind her puts the camera
                    // on the middle of the tile behind her.
                    back=std::max(TileSize*0.6f,(s-0.5f)*TileSize);
                    break;
                }
            }
            // In at once (as she turns in a passage the camera would swing through its side
            // wall), out easing.
            if(back<state.cameraBack)state.cameraBack=back;
            else state.cameraBack+=(back-state.cameraBack)*static_cast<float>(1-std::exp(-dt/0.12));
            const float rise=14+8*state.cameraBack/(TileSize*3.2f);
            target=XMVectorAdd(XMVectorAdd(at,XMVectorScale(forward,TileSize*1.6f)),XMVectorSet(0,15,0,0));
            eye=XMVectorAdd(XMVectorSubtract(at,XMVectorScale(forward,state.cameraBack)),XMVectorSet(0,rise,0,0));

            // A wall right behind her leaves the camera so close that her back fills the view:
            // then it goes on into her eyes (first person), and back out once there is room
            // (apart enough that it does not flicker between the two at the edge).
            if(back<TileSize*0.8f)state.firstPerson=true;        // the tile behind her is a wall
            else if(back>TileSize*0.9f)state.firstPerson=false;
            std::array<float,3> eyes{};
            if(model.EyePosition(eyes)&&eyes[1]>1)
                state.eyeHeight+=(eyes[1]-state.eyeHeight)*static_cast<float>(1-std::exp(-dt/0.3));
            state.firstBlend+=((state.firstPerson?1.0f:0.0f)-state.firstBlend)*static_cast<float>(1-std::exp(-dt/0.15));
            if(state.firstBlend>0.001f)
            {
                const XMVECTOR firstEye=XMVectorAdd(XMVectorAdd(at,XMVectorScale(forward,TileSize*0.2f)),XMVectorSet(0,state.eyeHeight,0,0));
                const XMVECTOR firstTarget=XMVectorAdd(XMVectorAdd(at,XMVectorScale(forward,TileSize*1.6f)),XMVectorSet(0,state.eyeHeight-2,0,0));
                eye=XMVectorLerp(eye,firstEye,state.firstBlend);
                target=XMVectorLerp(target,firstTarget,state.firstBlend);
            }
        }
        else
        {
            target=XMVectorSet(state.drawnX,8,state.drawnZ,0);
            eye=XMVectorAdd(target,XMVectorSet(0,62,-48,0));
            state.firstPerson=false;
            state.firstBlend=0;
        }
        if(!state.placed){XMStoreFloat3(&state.eye,eye);XMStoreFloat3(&state.target,target);}
        const float c=static_cast<float>(1-std::exp(-dt/(state.behind?0.07:0.15)));
        XMStoreFloat3(&state.eye,XMVectorLerp(XMLoadFloat3(&state.eye),eye,c));
        XMStoreFloat3(&state.target,XMVectorLerp(XMLoadFloat3(&state.target),target,c));
        state.placed=true;
        // She is left out once the camera comes nearer than the third-person view ever does
        // (it stops 0.6 tiles back): from there on it would pass through her hair and body.
        const float ex=state.eye.x-state.drawnX,ez=state.eye.z-state.drawnZ;
        r.SetModelHidden(state.behind&&ex*ex+ez*ez<TileSize*0.55f*TileSize*0.55f);
        r.SetCamera(XMLoadFloat3(&state.eye),XMLoadFloat3(&state.target),XMConvertToRadians(state.behind?50.0f:32.0f));
        r.Render(model,state.hero.edgeScale);
        if(state.photo.at&&GetTickCount64()>=state.photo.at)TakePhoto();
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
        state.showBook=false;
        state.photo.at=0;
        state.toast.clear();
        state.speech.recent.clear();
        state.face.reset();
        if(hero.faceSize>0&&hero.face.size()>=static_cast<size_t>(hero.faceSize)*hero.faceSize*4)
        {
            // A copy the bitmap owns (GDI+ keeps pointing at the pixels it is given).
            Bitmap view(hero.faceSize,hero.faceSize,hero.faceSize*4,PixelFormat32bppPARGB,const_cast<BYTE*>(state.hero.face.data()));
            state.face.reset(view.Clone(0,0,hero.faceSize,hero.faceSize,PixelFormat32bppPARGB));
        }
        if(hero.recordPath.empty())state.record=Dungeon::Record{};
        else state.record.Load(hero.recordPath);

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
        StartAdventure(false);
        state.show3D=CreateView();
        PlaceView();
        ShowWindow(state.window,SW_SHOWNORMAL);
        SetForegroundWindow(state.window);
    }
}
