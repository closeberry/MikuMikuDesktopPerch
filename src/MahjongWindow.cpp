#define NOMINMAX
#include "MahjongWindow.hpp"
#include "Mahjong.hpp"
#include "Text.hpp"
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <shlwapi.h>
#include <algorithm>
#include <map>
#include <memory>
#include <tuple>
#include <random>
#pragma comment(lib,"gdiplus.lib")
#pragma comment(lib,"shlwapi.lib")

namespace MikuMikuDesktopPerch
{
    namespace
    {
        using namespace Gdiplus;
        using Mahjong::Piece;
        using Mahjong::Phase;
        constexpr wchar_t ClassName[]=L"MikuMikuDesktopPerchMahjong";
        constexpr UINT_PTR TimerId=1;
        // How carefully she plays (Mahjong::Table::NewGame): a little less as you grow closer, and
        // after games you lost in a row.
        constexpr double Care=0.85,LessPerBond=0.03,LessPerLoss=0.08,LeastCare=0.4;
        // Her pace: before her draw, before her discard, after a call of hers, before a hand's
        // end is shown, and your discards after riichi.
        constexpr ULONGLONG HerDrawWait=350,HerDiscardWait=450,CallWait=900,EndWait=900,RiichiDiscardWait=350;
        constexpr ULONGLONG SaidShown=1600;
        constexpr ULONGLONG RiichiShowDrawn=450;   // after riichi, the drawn tile is seen this long before it goes
        // Sizes in logical pixels (at 96 dpi, at the size the window opens).
        constexpr float BaseWidth=1180,BaseHeight=760;
        constexpr float HandW=46,HandH=62,RiverW=30,RiverH=40,SmallW=26,SmallH=35,BackW=26,BackH=36;
        constexpr int RiverRow=10;

        enum class Act {Tsumo,Riichi,Kan,Ron,Pon,OpenKan,Pass,LetGo,CancelRiichi};
        struct Button {Act act;int kind=-1;std::wstring label;RectF box;};

        struct State
        {
            HWND window=nullptr;
            ULONG_PTR gdiplus=0;
            MahjongOpponent her;
            std::unique_ptr<Bitmap> face;
            std::function<void()> onEnd;
            Mahjong::Table table;
            int lost=0;   // games you lost in a row
            int selected=-1,hover=-1;
            bool riichiMode=false,confirmQuit=false;
            ULONGLONG nextAt=0;
            int callsSeen=0;
            std::wstring said;
            ULONGLONG saidAt=0;
            std::vector<RectF> handBoxes;   // by hand index (the drawn tile last)
            std::vector<Button> buttons;
            ULONGLONG autoAt=0;              // after riichi: when the drawn tile goes by itself
            std::unique_ptr<Bitmap> canvas;  // drawn into, then the changed part shown
        } state;

        float Scale()
        {
            if(!state.window)return 1;
            const float dpi=static_cast<float>(GetDpiForWindow(state.window))/96.0f;
            RECT client{};
            GetClientRect(state.window,&client);
            const float grow=std::min(static_cast<float>(client.right)/(BaseWidth*dpi),static_cast<float>(client.bottom)/(BaseHeight*dpi));
            return dpi*std::clamp(grow,0.6f,2.2f);
        }

        // Parts of the window to draw again: her side at the top, the rivers and the round in the
        // middle, your hand and the buttons at the bottom.
        enum Part {Top=1,Middle=2,Bottom=4,Everything=7};
        void Redraw(int parts)
        {
            if(!state.window||!parts)return;
            if(parts==Everything){InvalidateRect(state.window,nullptr,FALSE);return;}
            RECT client{};
            GetClientRect(state.window,&client);
            const float s=Scale();
            const LONG top=static_cast<LONG>(112*s),bottom=static_cast<LONG>(468*s);
            const RECT bands[]={{0,0,client.right,top},{0,top,client.right,bottom},{0,bottom,client.right,client.bottom}};
            for(int i=0;i<3;++i)if(parts&(1<<i))InvalidateRect(state.window,&bands[i],FALSE);
        }

        const FontFamily* Family()
        {
            static FontFamily gothic(L"Yu Gothic UI");
            return gothic.IsAvailable()?&gothic:FontFamily::GenericSansSerif();
        }

        void Text(Graphics& g,const std::wstring& text,const RectF& box,float size,Color color,bool bold=false,
                  StringAlignment across=StringAlignmentNear,StringAlignment down=StringAlignmentNear)
        {
            Font font(Family(),size,bold?FontStyleBold:FontStyleRegular,UnitPixel);
            SolidBrush brush(color);
            StringFormat format;
            format.SetAlignment(across);
            format.SetLineAlignment(down);
            format.SetFormatFlags(StringFormatFlagsNoWrap);
            g.DrawString(text.c_str(),static_cast<INT>(text.size()),&font,box,&format,&brush);
        }

        float TextWidth(Graphics& g,const std::wstring& text,float size,bool bold=false)
        {
            Font font(Family(),size,bold?FontStyleBold:FontStyleRegular,UnitPixel);
            RectF box;
            g.MeasureString(text.c_str(),static_cast<INT>(text.size()),&font,PointF(0,0),&box);
            return box.Width;
        }

        void RoundRect(GraphicsPath& path,float x,float y,float w,float h,float r)
        {
            const float d=r*2;
            path.AddArc(x,y,d,d,180,90);
            path.AddArc(x+w-d,y,d,d,270,90);
            path.AddArc(x+w-d,y+h-d,d,d,0,90);
            path.AddArc(x,y+h-d,d,d,90,90);
            path.CloseFigure();
        }

        void FillRound(Graphics& g,const RectF& box,float r,Color fill,Color edge=Color(0,0,0,0),float edgeWidth=1)
        {
            GraphicsPath path;
            RoundRect(path,box.X,box.Y,box.Width,box.Height,r);
            SolidBrush brush(fill);
            g.FillPath(&brush,&path);
            if(edge.GetA()){Pen pen(edge,edgeWidth);g.DrawPath(&pen,&path);}
        }

        // The tiles' faces (res/mahjong/tiles.png): TileSide by
        // TileTop each, ten across, the 34 kinds in order and then the red fives of each suit.
        constexpr int TileSide=150,TileTop=200,AtlasColumns=10;

        Bitmap* Faces()
        {
            static std::unique_ptr<Bitmap> atlas=[]{
                std::unique_ptr<Bitmap> loaded;
                const HRSRC info=FindResourceW(nullptr,L"TILES",L"MAHJONGTILES");
                const HGLOBAL data=info?LoadResource(nullptr,info):nullptr;
                if(!data)return loaded;
                IStream* stream=SHCreateMemStream(static_cast<const BYTE*>(LockResource(data)),SizeofResource(nullptr,info));
                if(!stream)return loaded;
                std::unique_ptr<Bitmap> read(Bitmap::FromStream(stream));
                stream->Release();
                // A copy of its own (GDI+ keeps reading the stream it was made from).
                if(read&&read->GetLastStatus()==Ok)loaded.reset(read->Clone(0,0,read->GetWidth(),read->GetHeight(),PixelFormat32bppPARGB));
                return loaded;
            }();
            return atlas.get();
        }

        // A tile's face at w by h pixels, made once from the atlas (scaling it each time it is
        // drawn is slow).
        Bitmap* Face(int at,int w,int h)
        {
            static std::map<std::tuple<int,int,int>,std::unique_ptr<Bitmap>> made;
            Bitmap* faces=Faces();
            if(!faces||w<1||h<1)return nullptr;
            if(made.size()>600)made.clear();   // sizes the window had before
            auto& face=made[{at,w,h}];
            if(!face)
            {
                face=std::make_unique<Bitmap>(w,h,PixelFormat32bppPARGB);
                Graphics g(face.get());
                g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                g.SetPixelOffsetMode(PixelOffsetModeHalf);
                g.DrawImage(faces,Rect(0,0,w,h),at%AtlasColumns*TileSide,at/AtlasColumns*TileTop,TileSide,TileTop,UnitPixel);
            }
            return face.get();
        }

        // A tile standing up at (x, y), w by h: its face (or its back).
        void DrawTile(Graphics& g,float x,float y,float w,float h,const Piece& p,bool faceUp=true,bool dim=false)
        {
            const float r=w*0.14f;
            FillRound(g,RectF(x+w*0.05f,y+h*0.06f,w,h),r,Color(70,0,0,0));
            if(!faceUp)
            {
                FillRound(g,RectF(x,y,w,h),r,Color(255,214,120,52),Color(255,150,80,30));
                FillRound(g,RectF(x+w*0.12f,y+h*0.1f,w*0.76f,h*0.8f),r*0.6f,Color(255,232,146,70));
                return;
            }
            const int at=p.red&&p.kind<Mahjong::East?Mahjong::Kinds+p.kind/9:p.kind;
            if(Bitmap* face=Face(at,static_cast<int>(std::lround(w)),static_cast<int>(std::lround(h))))
                g.DrawImage(face,RectF(x,y,static_cast<REAL>(face->GetWidth()),static_cast<REAL>(face->GetHeight())));
            else
            {
                FillRound(g,RectF(x,y,w,h),r,Color(255,251,248,240),Color(255,170,160,140));
                Text(g,Mahjong::TileText(p.kind),RectF(x,y,w,h),h*0.3f,Color(255,30,30,36),true,StringAlignmentCenter,StringAlignmentCenter);
            }
            if(dim)FillRound(g,RectF(x,y,w,h),r,Color(130,30,30,40));
        }

        // Lying on its side (a riichi discard): the box is h wide, w tall.
        void DrawSideways(Graphics& g,float x,float y,float w,float h,const Piece& p,bool dim=false)
        {
            const GraphicsState saved=g.Save();
            g.TranslateTransform(x+h/2,y+w/2);
            g.RotateTransform(-90);
            DrawTile(g,-w/2,-h/2,w,h,p,true,dim);
            g.Restore(saved);
        }

        // A river, rows of RiverRow from (x, y); taken tiles are faded.
        void DrawRiver(Graphics& g,const std::vector<Mahjong::RiverTile>& river,float x,float y,float s)
        {
            float cx=x,cy=y;
            int inRow=0;
            for(const auto& r:river)
            {
                if(inRow==RiverRow){cx=x;cy+=(RiverH+3)*s;inRow=0;}
                if(r.sideways){DrawSideways(g,cx,cy+(RiverH-RiverW)*s,RiverW*s,RiverH*s,r.piece,r.taken);cx+=(RiverH+1)*s;}
                else {DrawTile(g,cx,cy,RiverW*s,RiverH*s,r.piece,true,r.taken);cx+=(RiverW+1)*s;}
                ++inRow;
            }
        }

        // Melds from the right edge leftward; returns their left edge.
        float DrawMelds(Graphics& g,const std::vector<Mahjong::Meld>& melds,float right,float y,float s,float w,float h)
        {
            float x=right;
            for(auto m=melds.rbegin();m!=melds.rend();++m)
            {
                const int n=m->kind==Mahjong::MeldKind::Pon?3:4;
                x-=n*(w+s)+8*s;
                for(int i=0;i<n;++i)
                {
                    const bool back=m->kind==Mahjong::MeldKind::ClosedKan&&(i==0||i==3);
                    DrawTile(g,x+i*(w+s),y,w,h,{m->tile,false},!back);
                }
            }
            return x;
        }

        std::wstring HerName(){return state.her.name.empty()?std::wstring(T(L"相手",L"Her")):state.her.name;}

        std::wstring Points(int n){return std::to_wstring(n)+T(L"点",L"");}

        std::wstring Signed(int n){return (n>0?L"+":L"")+std::to_wstring(n);}

        bool Waiting(){return state.nextAt>GetTickCount64();}

        void UpdateButtons()
        {
            auto& t=state.table;
            const auto& you=t.Of(Mahjong::You);
            std::vector<Button> b;
            if(!Waiting()&&t.Now()==Phase::YourTurn)
            {
                if(state.riichiMode)b.push_back({Act::CancelRiichi,-1,T(L"やめる",L"Cancel")});
                else
                {
                    if(t.CanTsumo())b.push_back({Act::Tsumo,-1,T(L"ツモ",L"Tsumo")});
                    if(!t.RiichiDiscards().empty())b.push_back({Act::Riichi,-1,T(L"リーチ",L"Riichi")});
                    for(int k:t.KanKinds())b.push_back({Act::Kan,k,std::wstring(T(L"カン ",L"Kan "))+Mahjong::TileText(k)});
                    if(you.riichi&&t.CanTsumo())b.push_back({Act::LetGo,-1,T(L"ツモ切り",L"Discard it")});
                }
            }
            else if(!Waiting()&&t.Now()==Phase::YourCall)
            {
                if(t.CanRon())b.push_back({Act::Ron,-1,T(L"ロン",L"Ron")});
                if(t.CanPon())b.push_back({Act::Pon,-1,T(L"ポン",L"Pon")});
                if(t.CanOpenKan())b.push_back({Act::OpenKan,-1,T(L"カン",L"Kan")});
                b.push_back({Act::Pass,-1,T(L"見送り",L"Pass")});
            }
            state.buttons=std::move(b);
        }

        void Finish()
        {
            if(!state.window)return;
            KillTimer(state.window,TimerId);
            auto onEnd=std::move(state.onEnd);
            DestroyWindow(state.window);
            if(onEnd)onEnd();
        }

        void NewGame()
        {
            std::random_device device;
            state.table.NewGame(device(),std::max(LeastCare,Care-LessPerBond*state.her.bond-LessPerLoss*state.lost));
            state.callsSeen=state.table.HerCalls();
            state.selected=-1;
            state.riichiMode=state.confirmQuit=false;
            state.said.clear();
            state.nextAt=GetTickCount64()+HerDrawWait;
        }

        // After a move of yours: her turn waits a moment.
        void Moved(ULONGLONG wait=HerDrawWait)
        {
            state.riichiMode=false;
            state.selected=-1;
            state.nextAt=GetTickCount64()+wait;
            if(state.table.Now()==Phase::RoundOver)state.nextAt=GetTickCount64()+EndWait/2;
            InvalidateRect(state.window,nullptr,FALSE);
        }

        void Do(const Button& b)
        {
            auto& t=state.table;
            switch(b.act)
            {
            case Act::Tsumo:t.Tsumo();break;
            case Act::Riichi:
                state.riichiMode=true;
                state.selected=t.RiichiDiscards().back();
                InvalidateRect(state.window,nullptr,FALSE);
                return;
            case Act::CancelRiichi:state.riichiMode=false;InvalidateRect(state.window,nullptr,FALSE);return;
            case Act::Kan:t.Kan(b.kind);Moved(0);return;
            case Act::Ron:t.Ron();break;
            case Act::Pon:t.Pon();Moved(0);return;
            case Act::OpenKan:t.OpenKan();Moved(0);return;
            case Act::Pass:t.Pass();Moved(0);return;
            case Act::LetGo:t.Discard(static_cast<int>(t.Of(Mahjong::You).hand.size()));break;
            }
            Moved();
        }

        bool CanDiscard(int index)
        {
            auto& t=state.table;
            const auto& you=t.Of(Mahjong::You);
            const int n=static_cast<int>(you.hand.size());
            if(Waiting()||t.Now()!=Phase::YourTurn||index<0||index>n||(index==n&&!you.hasDrawn))return false;
            if(state.riichiMode)
            {
                const auto can=t.RiichiDiscards();
                return std::find(can.begin(),can.end(),index)!=can.end();
            }
            return !you.riichi||index==n;
        }

        void DiscardAt(int index)
        {
            if(!CanDiscard(index))return;
            state.table.Discard(index,state.riichiMode);
            Moved();
        }

        void Continue()
        {
            auto& t=state.table;
            if(t.Now()==Phase::RoundOver&&!Waiting()){t.NextRound();state.nextAt=GetTickCount64()+HerDrawWait;state.selected=-1;}
            else if(t.Now()==Phase::GameOver)
            {
                state.lost=t.Of(Mahjong::You).points<t.Of(Mahjong::Her).points?state.lost+1:0;
                NewGame();
            }
            InvalidateRect(state.window,nullptr,FALSE);
        }

        void Tick()
        {
            auto& t=state.table;
            const ULONGLONG now=GetTickCount64();
            int parts=0;
            if(t.HerCalls()!=state.callsSeen)
            {
                state.callsSeen=t.HerCalls();
                state.said=std::wstring(English()?t.HerCallEn():t.HerCallJa())+T(L"！",L"!");
                state.saidAt=now;
                state.nextAt=std::max(state.nextAt,now+CallWait);
                parts|=Top;
            }
            if(!state.said.empty()&&now-state.saidAt>SaidShown){state.said.clear();parts|=Top;}
            if(now>=state.nextAt)
            {
                const auto& you=t.Of(Mahjong::You);
                if(t.Now()==Phase::HerTurn)
                {
                    t.Step();
                    state.nextAt=now+(t.Now()==Phase::HerTurn?HerDiscardWait:0);
                    // Her draw changes only her side; her discard the middle and your side too.
                    parts|=t.Now()==Phase::HerTurn?Top:t.Now()==Phase::RoundOver?0:Everything;
                    if(t.Now()==Phase::RoundOver)state.nextAt=now+EndWait;
                }
                else if(t.Now()==Phase::YourTurn&&you.riichi&&you.hasDrawn&&!t.CanTsumo()&&t.KanKinds().empty())
                {
                    if(!state.autoAt)state.autoAt=now+RiichiShowDrawn;
                    else if(now>=state.autoAt)
                    {
                        state.autoAt=0;
                        t.Discard(static_cast<int>(you.hand.size()));
                        state.nextAt=now+RiichiDiscardWait;
                        parts|=Middle|Bottom;
                    }
                }
                else if(state.nextAt&&now-state.nextAt<100)
                    parts|=t.Now()==Phase::RoundOver?Everything:Bottom;   // what waited (the buttons, the end) shows now
            }
            Redraw(parts);
        }

        std::wstring ScoreLine(const Mahjong::Score& s)
        {
            if(*s.limitJa)return std::wstring(English()?s.limitEn:s.limitJa)+L"  "+Points(s.points);
            return English()?std::to_wstring(s.fu)+L" fu "+std::to_wstring(s.han)+L" han  "+std::to_wstring(s.points)
                            :std::to_wstring(s.fu)+L"符 "+std::to_wstring(s.han)+L"翻  "+Points(s.points);
        }

        void PaintEnd(Graphics& g,float cw,float ch,float s)
        {
            auto& t=state.table;
            SolidBrush shade(Color(150,0,0,0));
            g.FillRectangle(&shade,0.0f,0.0f,cw,ch);
            const float w=std::min(cw-40*s,860*s),h=std::min(ch-40*s,520*s);
            const RectF box((cw-w)/2,(ch-h)/2,w,h);
            FillRound(g,box,14*s,Color(255,24,40,36),Color(255,200,170,90),2*s);
            const float left=box.X+28*s;
            float y=box.Y+20*s;
            const Color gold(255,240,206,120),white(255,240,240,236),soft(255,190,196,190);
            if(t.Now()==Phase::GameOver)
            {
                const int you=t.Of(Mahjong::You).points,her=t.Of(Mahjong::Her).points;
                Text(g,T(L"対局終了",L"Game over"),RectF(box.X,y,w,40*s),30*s,gold,true,StringAlignmentCenter);
                y+=70*s;
                Text(g,std::wstring(T(L"あなた  ",L"You  "))+Points(you),RectF(box.X,y,w,36*s),26*s,white,false,StringAlignmentCenter);
                y+=44*s;
                Text(g,HerName()+L"  "+Points(her),RectF(box.X,y,w,36*s),26*s,white,false,StringAlignmentCenter);
                y+=70*s;
                const std::wstring verdict=you>her?T(L"あなたの勝ち！",L"You win!"):you<her?Fmt(T(L"{0}の勝ち",L"{0} wins"),{HerName()}):std::wstring(T(L"引き分け",L"A tie"));
                Text(g,verdict,RectF(box.X,y,w,44*s),32*s,gold,true,StringAlignmentCenter);
                Text(g,T(L"Enter：もう一度　Esc：閉じる",L"Enter: play again   Esc: close"),RectF(box.X,box.GetBottom()-44*s,w,30*s),17*s,soft,false,StringAlignmentCenter);
                return;
            }
            const auto& e=t.Ended();
            if(e.draw)
            {
                Text(g,T(L"流局",L"Drawn hand"),RectF(box.X,y,w,40*s),30*s,gold,true,StringAlignmentCenter);
                y+=60*s;
                Text(g,std::wstring(T(L"あなた：",L"You: "))+(e.tenpai[Mahjong::You]?T(L"テンパイ",L"waiting"):T(L"ノーテン",L"not waiting")),RectF(left,y,w,30*s),20*s,white);
                y+=36*s;
                Text(g,HerName()+T(L"：",L": ")+(e.tenpai[Mahjong::Her]?T(L"テンパイ",L"waiting"):T(L"ノーテン",L"not waiting")),RectF(left,y,w,30*s),20*s,white);
                y+=40*s;
                for(size_t i=0;i<e.herHand.size();++i)DrawTile(g,left+i*(HandW*0.8f+1)*s,y,HandW*0.8f*s,HandH*0.8f*s,e.herHand[i]);
                y+=HandH*s+16*s;
            }
            else
            {
                const std::wstring who=e.winner==Mahjong::You?std::wstring(T(L"あなた",L"You")):HerName();
                Text(g,who+L"  "+(e.tsumo?T(L"ツモ",L"Tsumo"):T(L"ロン",L"Ron")),RectF(box.X,y,w,40*s),30*s,gold,true,StringAlignmentCenter);
                y+=56*s;
                // The hand: its concealed tiles, the winning one apart, then the melds.
                const float tw=HandW*0.8f*s,th=HandH*0.8f*s;
                float x=left;
                for(size_t i=0;i<e.hand.size();++i)
                {
                    if(i+1==e.hand.size())x+=10*s;
                    DrawTile(g,x,y,tw,th,e.hand[i]);
                    x+=tw+s;
                }
                DrawMelds(g,e.melds,box.GetRight()-28*s,y,s,tw,th);
                y+=th+18*s;
                // The yaku in two columns.
                const float colW=(w-56*s)/2;
                for(size_t i=0;i<e.score.yaku.size();++i)
                {
                    const auto& yk=e.score.yaku[i];
                    const float cx=left+(i%2)*colW,cy=y+(i/2)*28*s;
                    Text(g,English()?yk.en:yk.ja,RectF(cx,cy,colW,28*s),19*s,white);
                    const std::wstring han=e.score.yakuman?std::wstring(T(L"役満",L"yakuman")):std::to_wstring(yk.han)+T(L"翻",L" han");
                    Text(g,han,RectF(cx,cy,colW-24*s,28*s),19*s,soft,false,StringAlignmentFar);
                }
                y+=((e.score.yaku.size()+1)/2)*28*s+12*s;
                Text(g,ScoreLine(e.score),RectF(box.X,y,w,40*s),28*s,gold,true,StringAlignmentCenter);
                y+=48*s;
                if(!e.ura.empty())
                {
                    Text(g,T(L"裏ドラ表示",L"Ura indicators"),RectF(left,y+6*s,140*s,30*s),17*s,soft);
                    for(size_t i=0;i<e.ura.size();++i)DrawTile(g,left+140*s+i*(SmallW+2)*s,y,SmallW*s,SmallH*s,{e.ura[i],false});
                    y+=SmallH*s+10*s;
                }
            }
            const std::wstring changes=std::wstring(T(L"あなた ",L"You "))+Signed(e.change[Mahjong::You])+L"　　"+HerName()+L" "+Signed(e.change[Mahjong::Her]);
            Text(g,changes,RectF(box.X,box.GetBottom()-84*s,w,30*s),20*s,white,false,StringAlignmentCenter);
            Text(g,T(L"Enter：次へ",L"Enter: next"),RectF(box.X,box.GetBottom()-44*s,w,30*s),17*s,soft,false,StringAlignmentCenter);
        }

        // dirty: the part of the window to draw (the rest of the picture is left as it was).
        void Paint(HDC dc,const RECT& dirty)
        {
            RECT client{};
            GetClientRect(state.window,&client);
            const int cw=std::max(1L,client.right),ch=std::max(1L,client.bottom);
            if(!state.canvas||static_cast<int>(state.canvas->GetWidth())!=cw||static_cast<int>(state.canvas->GetHeight())!=ch)
                state.canvas=std::make_unique<Bitmap>(cw,ch,PixelFormat32bppPARGB);
            Bitmap& canvas=*state.canvas;
            Graphics g(&canvas);
            g.SetClip(Rect(dirty.left,dirty.top,dirty.right-dirty.left,dirty.bottom-dirty.top));
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            g.Clear(Color(255,22,74,52));
            const float s=Scale(),W=static_cast<float>(cw),mid=W/2;
            auto& t=state.table;
            const auto& you=t.Of(Mahjong::You);
            const auto& her=t.Of(Mahjong::Her);
            const Color white(255,240,240,236),soft(255,186,206,190),gold(255,240,206,120);
            UpdateButtons();

            // Her: face, name, points, riichi, her words; her tiles face down and her melds.
            const RectF faceBox(16*s,14*s,84*s,84*s);
            FillRound(g,faceBox,10*s,Color(255,14,50,36),Color(255,120,160,130),1.5f*s);
            if(state.face)g.DrawImage(state.face.get(),faceBox.X+3*s,faceBox.Y+3*s,faceBox.Width-6*s,faceBox.Height-6*s);
            else Text(g,HerName().substr(0,1),faceBox,40*s,white,true,StringAlignmentCenter,StringAlignmentCenter);
            Text(g,HerName(),RectF(112*s,14*s,240*s,28*s),20*s,white,true);
            Text(g,Points(her.points)+(t.Dealer()==Mahjong::Her?T(L"　親",L"  dealer"):L""),RectF(112*s,44*s,240*s,24*s),17*s,soft);
            if(her.riichi){FillRound(g,RectF(112*s,74*s,90*s,8*s),3*s,Color(255,245,245,240));SolidBrush dot(Color(255,210,30,40));g.FillEllipse(&dot,153*s,74*s,8*s,8*s);}
            if(!state.said.empty())
            {
                const float tw=TextWidth(g,state.said,30*s,true)+30*s;
                const RectF bubble(262*s,18*s,tw,52*s);
                FillRound(g,bubble,14*s,Color(255,255,252,240),Color(255,230,120,60),2*s);
                Text(g,state.said,bubble,30*s,Color(255,200,40,40),true,StringAlignmentCenter,StringAlignmentCenter);
            }
            {
                const int backs=static_cast<int>(her.hand.size())+(her.hasDrawn?1:0);
                const float right=W-20*s;
                const float meldLeft=DrawMelds(g,her.melds,right,22*s,s,BackW*s,BackH*s);
                float x=meldLeft-(her.melds.empty()?0:12*s)-backs*(BackW+1)*s;
                for(int i=0;i<backs;++i){DrawTile(g,x,22*s,BackW*s,BackH*s,{},false);x+=(BackW+1)*s;}
            }

            // The rivers, hers above the middle and yours below.
            const float riverLeft=mid-RiverRow*(RiverW+1)*s/2;
            DrawRiver(g,her.river,riverLeft,118*s,s);
            const RectF info(mid-170*s,252*s,340*s,64*s);
            FillRound(g,info,10*s,Color(200,12,40,28),Color(255,90,130,100),1.2f*s);
            {
                const bool south=t.RoundWind()==Mahjong::South;
                const std::wstring round=Fmt(T(L"{0}{1}局 {2}本場",L"{0} {1}, {2} honba"),
                                             {south?T(L"南",L"South"):T(L"東",L"East"),std::to_wstring((t.RoundNumber()-1)%2+1),std::to_wstring(t.Honba())});
                Text(g,round,RectF(info.X+12*s,info.Y+6*s,200*s,26*s),18*s,white,true);
                Text(g,Fmt(T(L"残り {0}　供託 {1}",L"Wall {0}  Sticks {1}"),{std::to_wstring(t.WallLeft()),std::to_wstring(t.Sticks())}),
                     RectF(info.X+12*s,info.Y+34*s,200*s,24*s),15*s,soft);
                Text(g,T(L"ドラ",L"Dora"),RectF(info.X+204*s,info.Y+20*s,40*s,24*s),15*s,soft);
                for(size_t i=0;i<t.Dora().size();++i)DrawTile(g,info.X+240*s+i*(SmallW*0.85f+2)*s,info.Y+12*s,SmallW*0.85f*s,SmallH*0.85f*s,{t.Dora()[i],false});
            }
            DrawRiver(g,you.river,riverLeft,326*s,s);

            // Your points, riichi, the buttons and your hand.
            const float handY=548*s;
            Text(g,T(L"あなた",L"You"),RectF(16*s,handY-6*s,110*s,26*s),18*s,white,true);
            Text(g,Points(you.points),RectF(16*s,handY+20*s,110*s,24*s),16*s,soft);
            if(t.Dealer()==Mahjong::You)Text(g,T(L"親",L"dealer"),RectF(16*s,handY+44*s,110*s,24*s),15*s,gold);
            if(you.riichi){FillRound(g,RectF(16*s,handY+72*s,90*s,8*s),3*s,Color(255,245,245,240));SolidBrush dot(Color(255,210,30,40));g.FillEllipse(&dot,57*s,handY+72*s,8*s,8*s);}
            {
                float x=W-20*s;
                for(size_t i=state.buttons.size();i-->0;)
                {
                    Button& b=state.buttons[i];
                    const std::wstring label=std::to_wstring(i+1)+L"  "+b.label;
                    const float bw=std::max(96*s,TextWidth(g,label,18*s,true)+28*s);
                    x-=bw;
                    b.box=RectF(x,handY-62*s,bw,40*s);
                    const bool main=b.act==Act::Tsumo||b.act==Act::Ron;
                    FillRound(g,b.box,10*s,main?Color(255,206,60,50):Color(255,236,226,196),Color(255,90,60,30),1.5f*s);
                    Text(g,label,b.box,18*s,main?white:Color(255,40,30,20),true,StringAlignmentCenter,StringAlignmentCenter);
                    x-=8*s;
                }
            }
            {
                const int n=static_cast<int>(you.hand.size());
                const float handLeft=130*s;
                state.handBoxes.assign(static_cast<size_t>(n)+1,RectF());
                std::vector<int> riichiOk;
                if(state.riichiMode)riichiOk=t.RiichiDiscards();
                const bool yourTurn=t.Now()==Phase::YourTurn&&!Waiting();
                if(yourTurn&&state.selected<0)state.selected=you.hasDrawn?n:n-1;
                for(int i=0;i<=n;++i)
                {
                    if(i==n&&!you.hasDrawn)break;
                    const Piece& p=i<n?you.hand[i]:you.drawn;
                    const float x=handLeft+i*(HandW+1)*s+(i==n?12*s:0);
                    // After riichi the drawn tile is not lifted (it goes by itself), only when pointed at.
                    const bool raised=yourTurn&&CanDiscard(i)&&(i==state.hover||(i==state.selected&&!you.riichi));
                    const float y=handY-(raised?10*s:0);
                    const bool dim=yourTurn&&!CanDiscard(i);
                    DrawTile(g,x,y,HandW*s,HandH*s,p,true,dim&&state.riichiMode);
                    state.handBoxes[i]=RectF(x,handY-10*s,HandW*s,HandH*s+10*s);
                }
                DrawMelds(g,you.melds,W-20*s,handY+(HandH-SmallH)*s,s,SmallW*s,SmallH*s);
            }

            // What you wait on (and whether you are furiten).
            {
                const std::vector<int> waits=t.Now()==Phase::YourTurn?std::vector<int>{}:t.Waits();
                const float y=handY+HandH*s+14*s;
                if(!waits.empty())
                {
                    float x=130*s;
                    Text(g,T(L"待ち",L"Waits"),RectF(x,y+4*s,60*s,24*s),16*s,soft);
                    x+=50*s;
                    for(int w:waits){DrawTile(g,x,y,SmallW*0.8f*s,SmallH*0.8f*s,{w,false});x+=(SmallW*0.8f+2)*s;}
                    if(t.Furiten())Text(g,T(L"フリテン",L"Furiten"),RectF(x+10*s,y+4*s,120*s,24*s),16*s,Color(255,255,120,110),true);
                }
                else if(state.riichiMode)Text(g,T(L"リーチ：切る牌を選んでください（Escでやめる）",L"Riichi: choose the tile to discard (Esc to cancel)"),RectF(130*s,y+4*s,600*s,24*s),16*s,gold);
            }

            if((t.Now()==Phase::RoundOver&&!Waiting())||t.Now()==Phase::GameOver)PaintEnd(g,W,static_cast<float>(ch),s);
            if(state.confirmQuit)
            {
                SolidBrush shade(Color(160,0,0,0));
                g.FillRectangle(&shade,0.0f,0.0f,W,static_cast<float>(ch));
                const RectF box(mid-220*s,ch/2.0f-70*s,440*s,140*s);
                FillRound(g,box,14*s,Color(250,24,40,36),Color(255,200,170,90),2*s);
                Text(g,T(L"麻雀をやめますか？",L"Stop playing?"),RectF(box.X,box.Y+22*s,box.Width,36*s),24*s,white,true,StringAlignmentCenter);
                Text(g,T(L"Y：やめる　N：続ける",L"Y: stop   N: keep playing"),RectF(box.X,box.Y+80*s,box.Width,30*s),18*s,soft,false,StringAlignmentCenter);
            }

            Graphics screen(dc);
            screen.DrawImage(&canvas,static_cast<INT>(dirty.left),static_cast<INT>(dirty.top),static_cast<INT>(dirty.left),static_cast<INT>(dirty.top),
                             static_cast<INT>(dirty.right-dirty.left),static_cast<INT>(dirty.bottom-dirty.top),UnitPixel);
        }

        int HandAt(float x,float y)
        {
            for(size_t i=0;i<state.handBoxes.size();++i)
                if(state.handBoxes[i].Width>0&&state.handBoxes[i].Contains(x,y))return static_cast<int>(i);
            return -1;
        }

        void OnKey(WPARAM key)
        {
            auto& t=state.table;
            if(state.confirmQuit)
            {
                if(key=='Y')Finish();
                else if(key=='N'||key==VK_ESCAPE){state.confirmQuit=false;InvalidateRect(state.window,nullptr,FALSE);}
                return;
            }
            if(t.Now()==Phase::GameOver)
            {
                if(key==VK_RETURN||key==VK_SPACE)Continue();
                else if(key==VK_ESCAPE)Finish();
                return;
            }
            if(key==VK_ESCAPE)
            {
                if(state.riichiMode)state.riichiMode=false;
                else state.confirmQuit=true;
                InvalidateRect(state.window,nullptr,FALSE);
                return;
            }
            if(t.Now()==Phase::RoundOver){if(key==VK_RETURN||key==VK_SPACE)Continue();return;}
            if(key>='1'&&key<='9')
            {
                const size_t i=key-'1';
                if(i<state.buttons.size())Do(state.buttons[i]);
                return;
            }
            if(key==VK_SPACE&&t.Now()==Phase::YourCall&&!Waiting()){t.Pass();Moved(0);return;}
            if(t.Now()!=Phase::YourTurn||Waiting())return;
            const int n=static_cast<int>(t.Of(Mahjong::You).hand.size())+(t.Of(Mahjong::You).hasDrawn?1:0);
            if(key==VK_LEFT||key==VK_RIGHT)
            {
                // To the next tile that can be discarded.
                int i=state.selected;
                for(int step=0;step<n;++step)
                {
                    i=(i+(key==VK_LEFT?n-1:1))%n;
                    if(CanDiscard(i))break;
                }
                state.selected=i;
                InvalidateRect(state.window,nullptr,FALSE);
            }
            else if(key==VK_RETURN)DiscardAt(state.selected);
        }

        LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            switch(message)
            {
            case WM_KEYDOWN:
                if(!(lParam&(1<<30)))OnKey(wParam);
                return 0;
            case WM_TIMER:
                Tick();
                return 0;
            case WM_MOUSEMOVE:
            {
                const int at=HandAt(static_cast<float>(GET_X_LPARAM(lParam)),static_cast<float>(GET_Y_LPARAM(lParam)));
                if(at!=state.hover){state.hover=at;Redraw(Bottom);}
                return 0;
            }
            case WM_LBUTTONDOWN:
            {
                const float x=static_cast<float>(GET_X_LPARAM(lParam)),y=static_cast<float>(GET_Y_LPARAM(lParam));
                auto& t=state.table;
                if(state.confirmQuit)return 0;
                if(t.Now()==Phase::GameOver||(t.Now()==Phase::RoundOver&&!Waiting())){Continue();return 0;}
                for(const Button& b:state.buttons)if(b.box.Contains(x,y)){Do(b);return 0;}
                const int at=HandAt(x,y);
                if(at>=0)DiscardAt(at);
                return 0;
            }
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC dc=BeginPaint(hwnd,&ps);
                Paint(dc,ps.rcPaint);
                EndPaint(hwnd,&ps);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_SIZE:
                InvalidateRect(hwnd,nullptr,FALSE);
                return 0;
            case WM_CLOSE:
                Finish();
                return 0;
            case WM_DESTROY:
                state.window=nullptr;
                state.face.reset();
                state.canvas.reset();
                return 0;
            }
            return DefWindowProcW(hwnd,message,wParam,lParam);
        }
    }

    bool MahjongWindowOpen(){return state.window!=nullptr;}

    namespace
    {
        bool SavePng(Bitmap& image,const std::wstring& path)
        {
            UINT count=0,size=0;
            GetImageEncodersSize(&count,&size);
            std::vector<BYTE> buffer(size);
            auto* encoders=reinterpret_cast<ImageCodecInfo*>(buffer.data());
            GetImageEncoders(count,size,encoders);
            for(UINT i=0;i<count;++i)
                if(std::wstring(encoders[i].MimeType)==L"image/png")return image.Save(path.c_str(),&encoders[i].Clsid,nullptr)==Ok;
            return false;
        }
    }

    bool SaveMahjongShot(const std::wstring& path,int moves,std::uint32_t seed)
    {
        GdiplusStartupInput input;
        if(!state.gdiplus)GdiplusStartup(&state.gdiplus,&input,nullptr);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc=DefWindowProcW;
        wc.hInstance=GetModuleHandleW(nullptr);
        wc.lpszClassName=L"MikuMikuDesktopPerchMahjongShot";
        RegisterClassExW(&wc);
        const int dpi=static_cast<int>(GetDpiForSystem());
        state.window=CreateWindowExW(0,wc.lpszClassName,L"",WS_OVERLAPPEDWINDOW,0,0,MulDiv(static_cast<int>(BaseWidth)+16,dpi,96),MulDiv(static_cast<int>(BaseHeight)+39,dpi,96),
                                     nullptr,nullptr,wc.hInstance,nullptr);
        state.her=MahjongOpponent{L"ミク"};
        auto& t=state.table;
        t.NewGame(seed,Care);
        // A simple player of yours: discards what it draws, wins when it can, calls a pon now and then.
        for(int i=0;i<moves&&t.Now()!=Phase::RoundOver&&t.Now()!=Phase::GameOver;++i)
        {
            const auto& you=t.Of(Mahjong::You);
            if(t.Now()==Phase::HerTurn)t.Step();
            else if(t.Now()==Phase::YourCall){if(t.CanRon())t.Ron();else if(t.CanPon())t.Pon();else t.Pass();}
            else if(t.CanTsumo())t.Tsumo();
            else if(!t.RiichiDiscards().empty()&&!you.riichi)t.Discard(t.RiichiDiscards()[0],true);
            else t.Discard(you.hasDrawn?static_cast<int>(you.hand.size()):0);
        }
        state.callsSeen=t.HerCalls();
        state.said=t.HerCalls()?std::wstring(t.HerCallJa())+L"！":L"";
        state.nextAt=0;
        state.selected=-1;
        RECT client{};
        GetClientRect(state.window,&client);
        Bitmap shot(client.right,client.bottom,PixelFormat32bppARGB);
        {
            Graphics g(&shot);
            const HDC dc=g.GetHDC();
            Paint(dc,client);
            g.ReleaseHDC(dc);
        }
        DestroyWindow(state.window);
        state.window=nullptr;
        return SavePng(shot,path);
    }

    bool SaveMahjongTiles(const std::wstring& path)
    {
        GdiplusStartupInput input;
        if(!state.gdiplus)GdiplusStartup(&state.gdiplus,&input,nullptr);
        constexpr float S=2;   // as on a 192 dpi screen
        Bitmap sheet(static_cast<INT>(40+10*(HandW+4)*S),static_cast<INT>(40+4*(HandH+8)*S+3*(RiverH+8)*S),PixelFormat32bppARGB);
        Graphics g(&sheet);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Color(255,22,74,52));
        // Each suit in a row (with its red five last), the honors in the fourth; below, the same at the river's size.
        auto row=[&](int r,float w,float h,float top)
        {
            for(int i=0;i<10;++i)
            {
                Piece p{};
                if(r<3){if(i==9)p={r*9+4,true};else p={r*9+i,false};}
                else if(i<7)p={Mahjong::East+i,false};
                else continue;
                DrawTile(g,20+i*(w+4*S),top,w,h,p);
            }
        };
        for(int r=0;r<4;++r)row(r,HandW*S,HandH*S,20+r*(HandH+8)*S);
        for(int r=0;r<3;++r)row(r,RiverW*S,RiverH*S,20+4*(HandH+8)*S+r*(RiverH+8)*S);
        return SavePng(sheet,path);
    }

    void ShowMahjongWindow(HWND owner,const MahjongOpponent& her,std::function<void()> onEnd)
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
        state.her=her;
        state.onEnd=std::move(onEnd);
        state.face.reset();
        if(her.faceSize>0&&her.face.size()>=static_cast<size_t>(her.faceSize)*her.faceSize*4)
        {
            // A copy the bitmap owns (GDI+ keeps pointing at the pixels it is given).
            Bitmap view(her.faceSize,her.faceSize,her.faceSize*4,PixelFormat32bppPARGB,const_cast<BYTE*>(state.her.face.data()));
            state.face.reset(view.Clone(0,0,her.faceSize,her.faceSize,PixelFormat32bppPARGB));
        }
        NewGame();

        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&info);
        const RECT work=info.rcWork;
        const int dpi=static_cast<int>(GetDpiForSystem());
        const int w=std::min(static_cast<int>(work.right-work.left)*9/10,MulDiv(static_cast<int>(BaseWidth)+16,dpi,96));
        const int h=std::min(static_cast<int>(work.bottom-work.top)*9/10,MulDiv(static_cast<int>(BaseHeight)+39,dpi,96));
        const std::wstring title=std::wstring(T(L"麻雀",L"Mahjong"))+L" - "+HerName();
        state.window=CreateWindowExW(0,ClassName,title.c_str(),WS_OVERLAPPEDWINDOW,
                                     work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,
                                     nullptr,nullptr,instance,nullptr);
        SetTimer(state.window,TimerId,40,nullptr);
        ShowWindow(state.window,SW_SHOWNORMAL);
        SetForegroundWindow(state.window);
    }
}
