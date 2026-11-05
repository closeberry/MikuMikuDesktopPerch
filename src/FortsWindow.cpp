#define NOMINMAX
#include "FortsWindow.hpp"
#include "Forts.hpp"
#include "MahjongSound.hpp"
#include "Log.hpp"
#include "MascotModel.hpp"
#include "MascotRenderer.hpp"
#include "Text.hpp"
#include "BubbleText.hpp"
#include "GameLines.hpp"
#include "WesternRoom.hpp"
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <shlwapi.h>
#include <algorithm>
#include <array>
#include <cmath>
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
        using Forts::Phase;
        using Forts::You;
        using Forts::Her;
        constexpr wchar_t ClassName[]=L"MikuMikuDesktopPerchForts";
        constexpr wchar_t ViewClassName[]=L"MikuMikuDesktopPerchFortsView";
        constexpr UINT_PTR TimerId=1;
        // Her pace: before each action of hers, and after the end of a turn.
        constexpr ULONGLONG HerPlayWait=800,HerPlayWait3D=1100,TurnWait=900;
        constexpr ULONGLONG SaidShown=1300;
        constexpr ULONGLONG ReachLead=500;         // her hand starts out this long before her action
        constexpr ULONGLONG ShowCardFor=1700;      // a card she has just played, shown large
        // Sizes in logical pixels (at 96 dpi, at the size the window opens).
        constexpr float BaseWidth=1180,BaseHeight=840;
        constexpr float HandW=124,HandH=166;       // a card in your hand
        constexpr float PreviewW=216,PreviewH=302; // a card shown large

        // How often the look on her face is put on, the other way about from how she stands.
        constexpr double PutOnShare=0.2;

        enum class Act {End,Offer,Spare,Redraw};
        struct Button {Act act;std::wstring label;RectF box;bool on=true;bool strong=false;};

        // What you have chosen so far of a move.
        struct Choice
        {
            int hand=-1;               // a card of your hand
            int fort=-1;               // the fort it goes to, or its fort
            int target=-1;             // the soldier it is aimed at
            int unit=-1;               // or a soldier of yours about to act
        };
        enum class Need {Nothing,Fort,Target,Dest,UnitAct};

        // What moves over the board starts at start, which may be still to come (a number waits for
        // the shot that brings it). Past: over by now.
        bool Past(ULONGLONG now,ULONGLONG start,ULONGLONG life){return now>start&&now-start>life;}

        // Over a card or a fort: a number or a word rising, a frame of light, a flash, a cut.
        struct Fx {std::wstring text;RectF at;Color color;ULONGLONG start=0,life=1100;bool frame=false,flash=false,slash=false;};
        // A soldier fallen, sinking and fading where it stood (leanX, leanY: it fell striking).
        struct Ghost {int card=-1;RectF at;ULONGLONG start=0,leanAt=0;float leanX=0,leanY=0;};
        // A speck of light on its way: where it began, its speed and fall in pixels a second.
        struct Spark {float x=0,y=0,vx=0,vy=0,size=0,fall=0;ULONGLONG start=0,life=600;Color color;};
        // A shot or a spell crossing the board, with its trail.
        struct Bolt {PointF from,to;ULONGLONG start=0,life=240;Color color;float size=0;};
        constexpr ULONGLONG LungeFor=300;
        constexpr size_t MostSparks=300;

        struct State
        {
            HWND window=nullptr;
            ULONG_PTR gdiplus=0;
            FortsOpponent her;
            std::unique_ptr<Bitmap> face;
            std::function<void()> onEnd;
            Forts::Table table;
            bool choosing=true;        // choosing your deck
            RectF deckBoxes[Forts::DeckKinds];
            int hoverDeck=-1;
            Forts::Strength strength;  // how hard she plays, from how your games have ended
            std::vector<int> owned;    // the cards collected
            int earned=-1;             // the card this game's win brought
            bool unlockedNow=false;    // and with it the deck can be played
            bool album=false;          // the cards collected are shown
            RectF albumBox;
            bool gameOverTold=false;
            bool confirmQuit=false;
            bool help=false;
            RectF helpBox;
            RectF quitButtons[2];
            std::vector<Button> buttons;
            int hover=-1;              // the button under the mouse
            ULONGLONG nextAt=0;
            std::wstring said;         // whose turn it is, in the big bubble
            ULONGLONG saidAt=0;
            Choice choice;
            unsigned redrawMask=0;     // the cards marked to change at the start
            std::vector<std::pair<int,RectF>> handBoxes;   // your cards as drawn (place in the hand)
            std::map<int,RectF> unitBoxes;                 // the soldiers as drawn, by uid
            RectF fortBoxes[Forts::FortCount];
            RectF bannerBoxes[Forts::FortCount];
            int hoverHand=-1,hoverUnit=-1;
            std::vector<Fx> fx;
            std::vector<Ghost> ghosts;
            std::vector<Spark> sparks;
            std::vector<Bolt> bolts;
            std::vector<int> burstDue;     // soldiers just come: their light, once they have a place
            PointF scoreSpot[2];
            ULONGLONG endAt=0;
            RectF lungeBox;            // where the one striking stands
            int lungeLeftOut=-1;       // who the board was last drawn without (PaintTop draws it)
            float lungeX=0,lungeY=0;
            std::map<int,ULONGLONG> fresh;                 // soldiers just come or moved, since when
            int lungeUnit=-1;          // a soldier striking
            ULONGLONG lungeAt=0;
            int shownCard=-1;          // a card she played, shown large
            ULONGLONG shownUntil=0;
            // Her words in the bubble (as at mahjong).
            struct Words
            {
                std::wstring text,fallback;
                ULONGLONG since=0,until=0,asked=0;
                bool waiting=false;
                int weight=0;
                std::vector<std::wstring> recent;
                std::wstring revealing;
                double shown=0;
                ULONGLONG shownAt=0;
                bool voiced=false;   // the words are said by a voice file of their own
                std::wstring told;
            } words;
            // Her lines by scene (res/forts/lines.txt, said again her way by the LLM, and the
            // user's own), and the voice of a line that has one.
            GameLines lines;
            GameVoice voice;
            bool prefetching=false;
            POINT headScreen{-1,-1},besideHead{-1,-1};
            std::wstring afterword,afterwordLine;
            struct Seen
            {
                bool started=false,over=false;
                bool nearTold=false;
            } seen;
            std::unique_ptr<Bitmap> canvas;
            // Her at the table in 3D (none: 2D only).
            HWND view=nullptr;
            std::unique_ptr<MascotRenderer> renderer;
            int entrance=0;
            float herX=0,herZ=0,herAngle=0,entranceTime=0,chairZ=0;
            struct Camera {float yaw=0,pitch=0.46f,distance=3.0f,x=0,y=0,z=0;} camera;
            POINT dragFrom{};
            int dragging=0;
            float upper=10,hips=10;
            float floorY=-5;
            bool tableBuilt=false;
            double clock=0;
            std::vector<MascotRenderer::Particle> roomGlows;
            std::mt19937 sparkRandom{12345};
            std::wstring endGesture;
            double endGestureUntil=0;
            bool gestureDone=false;
            bool reachDue=false;      // her hand goes out before her next action
            int herActions=0;         // her actions so far
            int reachedFor=-1;        // and the one her hand last went out for
            bool overlayDue=true;     // the 2D layer is to be drawn again
            bool baseDue=true;        // and what stays still in it (state.base)
            std::unique_ptr<Bitmap> base;
            int basePaints=0;
            int drawnKey=-1;          // what it showed, of what changes by itself
            // Measured since the window last changed size (for the report).
            int paints=0,frames=0,reaches=0;
            double paintSeconds=0,framesSince=0;
            int glanceFort=-1;        // she looks at this fort for a moment
            double glanceUntil=0;
            Expression tell=Expression::Neutral;   // the look on her face
            int tellFor=-1;
            Expression tellFace=Expression::Neutral;
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

        // Over the 3D view the 2D layer waits for the next frame (TickFortsWindow): painting it from
        // WM_PAINT, a slow paint keeps the main loop draining messages and no frame comes at all.
        // Only what moves over it (numbers rising, her words appearing): RedrawTop, which is far
        // cheaper in a large window.
        void RedrawTop()
        {
            if(state.renderer)state.overlayDue=true;
            else if(state.window)InvalidateRect(state.window,nullptr,FALSE);
        }
        void Redraw()
        {
            state.baseDue=true;
            RedrawTop();
        }

        // The brush hand of the mahjong result screen (res/mahjong/brush.ttf), null when it
        // cannot be loaded.
        const FontFamily* Brush()
        {
            static PrivateFontCollection fonts;
            static std::unique_ptr<FontFamily> family=[]{
                std::unique_ptr<FontFamily> loaded;
                const HRSRC info=FindResourceW(nullptr,L"BRUSH",L"MAHJONGFONT");
                const HGLOBAL data=info?LoadResource(nullptr,info):nullptr;
                if(!data||fonts.AddMemoryFont(LockResource(data),static_cast<INT>(SizeofResource(nullptr,info)))!=Ok||fonts.GetFamilyCount()<1)return loaded;
                loaded=std::make_unique<FontFamily>();
                INT found=0;
                fonts.GetFamilies(1,loaded.get(),&found);
                if(found<1)loaded.reset();
                return loaded;
            }();
            return family.get();
        }

        // Whether the brush hand has every character of text (it is cut down to what the games
        // write: a name, or a word it lacks, is written in the plain hand).
        bool BrushHas(const std::wstring& text)
        {
            static HFONT font=[]{
                HFONT made=nullptr;
                const FontFamily* family=Brush();
                const HRSRC info=FindResourceW(nullptr,L"BRUSH",L"MAHJONGFONT");
                const HGLOBAL data=info?LoadResource(nullptr,info):nullptr;
                DWORD added=0;
                WCHAR name[LF_FACESIZE]{};
                if(!family||!data||family->GetFamilyName(name)!=Ok||
                   !AddFontMemResourceEx(LockResource(data),SizeofResource(nullptr,info),nullptr,&added))return made;
                made=CreateFontW(-32,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,0,0,name);
                // Another font given in its place would have the characters: none then.
                if(made)
                {
                    const HDC dc=CreateCompatibleDC(nullptr);
                    const HGDIOBJ old=SelectObject(dc,made);
                    WCHAR face[LF_FACESIZE]{};
                    GetTextFaceW(dc,LF_FACESIZE,face);
                    SelectObject(dc,old);
                    DeleteDC(dc);
                    if(_wcsicmp(face,name)!=0){DeleteObject(made);made=nullptr;}
                }
                return made;
            }();
            if(!font||text.empty())return false;
            const HDC dc=CreateCompatibleDC(nullptr);
            const HGDIOBJ old=SelectObject(dc,font);
            std::vector<WORD> glyphs(text.size());
            const bool ok=GetGlyphIndicesW(dc,text.c_str(),static_cast<int>(text.size()),glyphs.data(),GGI_MARK_NONEXISTING_GLYPHS)!=GDI_ERROR&&
                          std::none_of(glyphs.begin(),glyphs.end(),[](WORD g){return g==0xFFFF;});
            SelectObject(dc,old);
            DeleteDC(dc);
            return ok;
        }

        const FontFamily* Family()
        {
            static FontFamily gothic(L"Yu Gothic UI");
            return gothic.IsAvailable()?&gothic:FontFamily::GenericSansSerif();
        }

        // brush: in the brush hand when it has the characters.
        void Text(Graphics& g,const std::wstring& text,const RectF& box,float size,Color color,bool bold=false,
                  StringAlignment across=StringAlignmentNear,StringAlignment down=StringAlignmentNear,bool brush=false)
        {
            const FontFamily* family=brush&&Brush()&&BrushHas(text)?Brush():Family();
            if(family!=Family()){bold=false;size*=1.08f;}
            Font font(family,size,bold?FontStyleBold:FontStyleRegular,UnitPixel);
            SolidBrush paint(color);
            StringFormat format;
            format.SetAlignment(across);
            format.SetLineAlignment(down);
            format.SetFormatFlags(StringFormatFlagsNoWrap);
            g.DrawString(text.c_str(),static_cast<INT>(text.size()),&font,box,&format,&paint);
        }

        float TextWidth(Graphics& g,const std::wstring& text,float size,bool bold=false,bool brush=false)
        {
            const FontFamily* family=brush&&Brush()&&BrushHas(text)?Brush():Family();
            if(family!=Family()){bold=false;size*=1.08f;}
            Font font(family,size,bold?FontStyleBold:FontStyleRegular,UnitPixel);
            RectF box;
            StringFormat format;
            format.SetFormatFlags(StringFormatFlagsMeasureTrailingSpaces);
            g.MeasureString(text.c_str(),static_cast<INT>(text.size()),&font,PointF(0,0),&format,&box);
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

        std::wstring HerName(){return state.her.name.empty()?std::wstring(T(L"相手",L"Her")):state.her.name;}
        double CharSeconds(){return English()?0.045:0.12;}
        bool Waiting(){return state.nextAt>GetTickCount64();}
        bool Seated(){return !state.renderer||state.entrance==5;}
        bool YourTurnNow(){return !state.choosing&&state.table.Now()==Phase::Yours&&!Waiting();}
        bool RedrawNow(){return !state.choosing&&state.table.Now()==Phase::Redraw;}
        bool EndShown(){return !state.choosing&&state.table.Now()==Phase::Over&&!Waiting();}

        const wchar_t* FortName(int fort)
        {
            return fort==0?T(L"左の砦",L"West Fort"):fort==1?T(L"中央の砦",L"Central Keep"):T(L"右の砦",L"East Fort");
        }
        const wchar_t* CardName(int id){const auto& c=Forts::CardOf(id);return English()?c.nameEn:c.name;}
        const wchar_t* CardText(int id){const auto& c=Forts::CardOf(id);return English()?c.textEn:c.text;}

        // ---- The cards (stand-ins: a frame by side, the name and what it does) ----

        void Wrapped(Graphics& g,const std::wstring& text,const RectF& box,float size,Color color)
        {
            Font font(Family(),size,FontStyleBold,UnitPixel);
            SolidBrush paint(color);
            StringFormat format;
            format.SetAlignment(StringAlignmentCenter);
            format.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(text.c_str(),static_cast<INT>(text.size()),&font,box,&format,&paint);
        }

        Color SideColor(int faction)
        {
            return faction==1?Color(255,50,74,124):faction==2?Color(255,44,100,58):faction==3?Color(255,92,56,128):Color(255,104,88,70);
        }

        // The marks on a card: what it costs a cut gem, its attack a pair of crossed blades behind a
        // crest, its health a shield. Each made once for its size, colour and number.
        enum class MarkShape {Gem,Blades,Shield};

        Color Lighter(Color c,float k){const auto m=[&](BYTE v){return static_cast<BYTE>(v+(255-v)*k);};return Color(c.GetA(),m(c.GetR()),m(c.GetG()),m(c.GetB()));}
        Color Darker(Color c,float k){const auto m=[&](BYTE v){return static_cast<BYTE>(v*(1-k));};return Color(c.GetA(),m(c.GetR()),m(c.GetG()),m(c.GetB()));}

        void DrawMarkShape(Graphics& g,MarkShape shape,float d,Color fill)
        {
            const Color gold(255,246,214,130),goldDark(255,120,84,30);
            const float c=d/2;
            const auto at=[&](float x,float y){return PointF(c+x*d,c+y*d);};
            LinearGradientBrush body(RectF(0,-1,d,d+2),Lighter(fill,0.30f),Darker(fill,0.42f),LinearGradientModeVertical);
            LinearGradientBrush rim(RectF(0,-1,d,d+2),gold,goldDark,LinearGradientModeVertical);
            Pen edge(&rim,std::max(1.2f,d*0.075f));
            edge.SetLineJoin(LineJoinMiter);
            GraphicsPath path;
            if(shape==MarkShape::Gem)
            {
                // Six sides, point up, with facets of light.
                const PointF six[6]={at(0,-0.47f),at(0.41f,-0.235f),at(0.41f,0.235f),at(0,0.47f),at(-0.41f,0.235f),at(-0.41f,-0.235f)};
                path.AddPolygon(six,6);
                g.FillPath(&body,&path);
                SolidBrush top(Color(90,255,255,255)),left(Color(38,255,255,255)),low(Color(70,0,0,20));
                const PointF a[4]={six[5],six[0],six[1],at(0,-0.05f)};
                const PointF b[3]={six[5],at(0,-0.05f),six[4]};
                const PointF e[4]={six[4],at(0,-0.05f),six[2],six[3]};
                g.FillPolygon(&top,a,4);
                g.FillPolygon(&left,b,3);
                g.FillPolygon(&low,e,4);
                g.DrawPath(&edge,&path);
            }
            else if(shape==MarkShape::Blades)
            {
                // Two blades crossed behind a pointed crest.
                SolidBrush steel(Color(255,226,232,240));
                Pen dark(Color(255,50,50,60),std::max(0.8f,d*0.03f));
                for(int side=-1;side<=1;side+=2)
                {
                    const float k=static_cast<float>(side);
                    const PointF blade[4]={at(k*0.53f,-0.53f),at(k*0.41f,-0.50f),at(-k*0.20f,0.10f),at(-k*0.10f,0.20f)};
                    g.FillPolygon(&steel,blade,4);
                    g.DrawPolygon(&dark,blade,4);
                }
                const PointF crest[4]={at(0,-0.43f),at(0.45f,0),at(0,0.50f),at(-0.45f,0)};
                path.AddPolygon(crest,4);
                g.FillPath(&body,&path);
                SolidBrush light(Color(70,255,255,255));
                const PointF upper[3]={crest[3],crest[0],crest[1]};
                g.FillPolygon(&light,upper,3);
                g.DrawPath(&edge,&path);
            }
            else
            {
                // A heater shield.
                path.AddLine(at(-0.42f,-0.42f),at(0.42f,-0.42f));
                path.AddBezier(at(0.42f,-0.42f),at(0.44f,0.05f),at(0.30f,0.30f),at(0,0.49f));
                path.AddBezier(at(0,0.49f),at(-0.30f,0.30f),at(-0.44f,0.05f),at(-0.42f,-0.42f));
                path.CloseFigure();
                g.FillPath(&body,&path);
                SolidBrush light(Color(60,255,255,255));
                g.FillRectangle(&light,c-0.42f*d,c-0.42f*d,0.42f*d,0.30f*d);
                g.DrawPath(&edge,&path);
            }
        }

        void Mark(Graphics& g,float x,float y,float d,const std::wstring& number,Color fill,MarkShape shape)
        {
            struct Key
            {
                int shape,size;
                ARGB color;
                std::wstring number;
                bool operator<(const Key& o) const {return std::tie(shape,size,color,number)<std::tie(o.shape,o.size,o.color,o.number);}
            };
            static std::map<Key,std::unique_ptr<Bitmap>> made;
            if(made.size()>500)made.clear();
            const int size=std::max(4,static_cast<int>(std::lround(d)));
            auto& mark=made[Key{static_cast<int>(shape),size,fill.GetValue(),number}];
            if(!mark)
            {
                const int pad=std::max(2,size/8);
                mark=std::make_unique<Bitmap>(size+2*pad,size+2*pad,PixelFormat32bppPARGB);
                Graphics m(mark.get());
                m.SetSmoothingMode(SmoothingModeAntiAlias);
                m.SetTextRenderingHint(TextRenderingHintAntiAlias);
                m.TranslateTransform(static_cast<REAL>(pad),static_cast<REAL>(pad));
                DrawMarkShape(m,shape,static_cast<float>(size),fill);
                const float box=static_cast<float>(size),big=box*(number.size()>1?0.56f:0.66f);
                const float low=shape==MarkShape::Shield?-box*0.03f:0.0f;
                // The number, dark round its edge so that it stands out on any colour.
                for(int k=0;k<8;k++)
                {
                    const float o=std::max(0.8f,box*0.035f),a=k*0.7854f;
                    Text(m,number,RectF(-box+std::cos(a)*o,low+std::sin(a)*o,box*3,box),big,Color(235,16,10,14),true,StringAlignmentCenter,StringAlignmentCenter);
                }
                Text(m,number,RectF(-box,low,box*3,box),big,Color(255,255,252,238),true,StringAlignmentCenter,StringAlignmentCenter);
            }
            const float pad=(mark->GetWidth()-size)/2.0f,k=d/size;
            g.DrawImage(mark.get(),x-pad*k,y-pad*k,mark->GetWidth()*k,mark->GetHeight()*k);
        }

        // The costly cards are foil: rainbow over the picture and round the edge, with stars in it.
        constexpr int HoloCost=5;
        bool IsHolo(const Forts::Card& card){return card.faction>=0&&card.cost>=HoloCost;}

        // A four-pointed star of light.
        void DrawStar(Graphics& g,float x,float y,float r,float strength)
        {
            if(strength<=0||r<=0)return;
            const float t=r*0.13f;
            const PointF star[8]={PointF(x,y-r),PointF(x+t,y-t),PointF(x+r,y),PointF(x+t,y+t),PointF(x,y+r),PointF(x-t,y+t),PointF(x-r,y),PointF(x-t,y-t)};
            SolidBrush white(Color(static_cast<BYTE>(std::clamp(strength,0.0f,1.0f)*255),255,255,255));
            g.FillPolygon(&white,star,8);
        }

        void DrawFoil(Graphics& g,const RectF& art,int id)
        {
            LinearGradientBrush rainbow(RectF(art.X,art.Y,std::max(1.0f,art.Width*0.5f),std::max(1.0f,art.Height*0.7f)),Color(0,0,0,0),Color(0,0,0,0),32.0f,TRUE);
            const BYTE a=62;
            const Color colours[7]={Color(a,255,90,150),Color(a,255,190,70),Color(a,240,255,110),Color(a,90,255,170),Color(a,80,210,255),Color(a,150,120,255),Color(a,255,90,150)};
            const REAL at[7]={0,0.17f,0.33f,0.5f,0.67f,0.83f,1};
            rainbow.SetInterpolationColors(colours,at,7);
            rainbow.SetWrapMode(WrapModeTileFlipXY);
            g.FillRectangle(&rainbow,art);
            std::mt19937 dice(static_cast<std::uint32_t>(id)*2654435761u);
            std::uniform_real_distribution<float> place(0.04f,0.96f),size(0.018f,0.05f),how(0.45f,1.0f);
            for(int i=0;i<11;i++)
            {
                const float x=art.X+place(dice)*art.Width,y=art.Y+place(dice)*art.Height;
                DrawStar(g,x,y,size(dice)*art.Width,how(dice));
            }
        }

        // The margins of the card: under its name, picture and words.
        void DrawFoilBody(Graphics& g,float W,float H,float r)
        {
            LinearGradientBrush rainbow(RectF(0,0,std::max(1.0f,W*0.6f),std::max(1.0f,H*0.5f)),Color(0,0,0,0),Color(0,0,0,0),40.0f,TRUE);
            const Color colours[7]={Color(255,255,140,185),Color(255,255,215,120),Color(255,225,250,140),Color(255,135,240,190),Color(255,130,215,255),Color(255,185,160,255),Color(255,255,140,185)};
            const REAL at[7]={0,0.17f,0.33f,0.5f,0.67f,0.83f,1};
            rainbow.SetInterpolationColors(colours,at,7);
            rainbow.SetWrapMode(WrapModeTileFlipXY);
            GraphicsPath path;
            RoundRect(path,0,0,W,H,r);
            g.FillPath(&rainbow,&path);
        }

        void DrawFoilRim(Graphics& g,float W,float H,float r)
        {
            LinearGradientBrush rainbow(RectF(0,0,std::max(1.0f,W),std::max(1.0f,H)),Color(0,0,0,0),Color(0,0,0,0),45.0f,TRUE);
            const Color colours[6]={Color(255,255,120,170),Color(255,255,226,120),Color(255,130,255,190),Color(255,120,220,255),Color(255,180,150,255),Color(255,255,120,170)};
            const REAL at[6]={0,0.2f,0.4f,0.6f,0.8f,1};
            rainbow.SetInterpolationColors(colours,at,6);
            const float width=std::max(1.5f,W*0.026f);
            Pen pen(&rainbow,width);
            GraphicsPath path;
            RoundRect(path,width/2,width/2,W-width,H-width,r);
            g.DrawPath(&pen,&path);
        }

        // How much magic it gives up for: that many little gems at the top right of its picture.
        void DrawGives(Graphics& g,const RectF& art,int gives)
        {
            const float w=art.Width*0.075f,h=w*1.25f,gap=w*0.22f,m=w*0.3f;
            const float room=gives*w+(gives-1)*gap+2*m;
            const RectF back(art.GetRight()-room-m*0.5f,art.Y+m*0.5f,room,h+2*m*0.7f);
            SolidBrush dark(Color(165,10,12,30));
            g.FillRectangle(&dark,back);
            for(int i=0;i<gives;i++)
            {
                const float x=back.X+m+i*(w+gap),y=back.Y+m*0.7f;
                const PointF gem[4]={PointF(x+w/2,y),PointF(x+w,y+h/2),PointF(x+w/2,y+h),PointF(x,y+h/2)};
                LinearGradientBrush body(RectF(x,y-1,w,h+2),Color(255,170,226,255),Color(255,40,96,210),LinearGradientModeVertical);
                g.FillPolygon(&body,gem,4);
                Pen edge(Color(255,240,250,255),std::max(0.7f,w*0.09f));
                g.DrawPolygon(&edge,gem,4);
            }
        }

        // The cards' pictures (res/forts/cards.jpg): ArtW by ArtH each, card n at (n%8, n/8).
        constexpr int ArtW=480,ArtH=270,ArtColumns=8;

        Bitmap* CardPictures()
        {
            static std::unique_ptr<Bitmap> atlas=[]{
                std::unique_ptr<Bitmap> loaded;
                const HRSRC info=FindResourceW(nullptr,L"CARDS",L"FORTSCARDS");
                const HGLOBAL data=info?LoadResource(nullptr,info):nullptr;
                if(!data)return loaded;
                IStream* stream=SHCreateMemStream(static_cast<const BYTE*>(LockResource(data)),SizeofResource(nullptr,info));
                if(!stream)return loaded;
                std::unique_ptr<Bitmap> read(Bitmap::FromStream(stream));
                stream->Release();
                if(read&&read->GetLastStatus()==Ok)loaded.reset(read->Clone(0,0,read->GetWidth(),read->GetHeight(),PixelFormat32bppPARGB));
                return loaded;
            }();
            return atlas.get();
        }

        // A card's picture over box, cut from its middle to fit.
        bool DrawPicture(Graphics& g,int id,const RectF& box)
        {
            Bitmap* pictures=CardPictures();
            if(!pictures||id<0)return false;
            // (a pixel in from each side: nothing of the next picture comes in when scaling)
            float w=static_cast<float>(ArtW-2),h=static_cast<float>(ArtH-2);
            if(box.Width/box.Height<w/h)w=h*box.Width/box.Height;
            else h=w*box.Height/box.Width;
            const float x=(id%ArtColumns)*ArtW+(ArtW-w)/2,y=(id/ArtColumns)*ArtH+(ArtH-h)/2;
            const InterpolationMode before=g.GetInterpolationMode();
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            ImageAttributes edges;
            edges.SetWrapMode(WrapModeTileFlipXY);
            g.DrawImage(pictures,box,x,y,w,h,UnitPixel,&edges);
            g.SetInterpolationMode(before);
            return true;
        }

        // A card's frame, name and words at w by h pixels, made once (drawing text each frame is
        // slow). Its numbers are drawn over it, since a soldier's change.
        Bitmap* CardFace(int id,int w,int h)
        {
            static std::map<std::tuple<int,int,int,bool>,std::unique_ptr<Bitmap>> made;
            if(w<1||h<1)return nullptr;
            if(made.size()>400)made.clear();
            auto& face=made[{id,w,h,English()}];
            if(face)return face.get();
            face=std::make_unique<Bitmap>(w,h,PixelFormat32bppPARGB);
            Graphics g(face.get());
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAlias);
            const Forts::Card& card=Forts::CardOf(id);
            const float W=static_cast<float>(w),H=static_cast<float>(h),r=W*0.08f,m=W*0.045f;
            const Color side=SideColor(card.faction);
            FillRound(g,RectF(0,0,W,H),r,side,Color(255,20,14,12),std::max(1.0f,W*0.012f));
            if(IsHolo(card))DrawFoilBody(g,W,H,r);
            const bool mini=h<140*Scale();
            const float nameH=H*(mini?0.17f:0.135f),nameTop=m;
            // The name.
            FillRound(g,RectF(m,nameTop,W-2*m,nameH),r*0.5f,Color(255,236,226,200));
            const std::wstring name=CardName(id);
            float size=nameH*0.62f;
            const float room=W-2*m-(card.faction>=0?nameH*0.9f:0.0f)-W*0.03f;
            while(size>6&&TextWidth(g,name,size,true)>room)size*=0.93f;
            Text(g,name,RectF(m+(card.faction>=0?nameH*0.9f:0.0f),nameTop,room,nameH),size,Color(255,40,28,20),true,StringAlignmentCenter,StringAlignmentCenter);
            // The picture (without one, the side's colour and the name's first letter).
            const float artTop=nameTop+nameH+m*0.6f,artH=H*(mini?0.40f:0.40f);
            {
                const RectF art(m,artTop,W-2*m,artH);
                if(!DrawPicture(g,id,art))
                {
                    LinearGradientBrush shade(art,Color(255,30,26,34),Color(255,side.GetR(),side.GetG(),side.GetB()),LinearGradientModeVertical);
                    g.FillRectangle(&shade,art);
                    Text(g,name.substr(0,1),art,artH*0.62f,Color(90,255,250,230),true,StringAlignmentCenter,StringAlignmentCenter);
                }
                if(IsHolo(card))
                {
                    DrawFoil(g,art,id);
                    DrawFoilRim(g,W,H,r);
                }
                if(card.faction>=0)DrawGives(g,art,Forts::GivesMagic(card));
                if(card.spell)
                {
                    const RectF band(art.X,art.GetBottom()-artH*0.34f,art.Width,artH*0.34f);
                    LinearGradientBrush dark(RectF(band.X,band.Y-1,band.Width,band.Height+2),Color(0,10,8,16),Color(190,10,8,16),LinearGradientModeVertical);
                    g.FillRectangle(&dark,band);
                }
                if(card.spell)Text(g,T(L"呪文",L"Spell"),RectF(art.X,art.GetBottom()-artH*0.26f,art.Width,artH*0.24f),artH*0.17f,Color(255,250,236,190),true,StringAlignmentCenter,StringAlignmentCenter);
            }
            // What it does.
            const float textTop=artTop+artH+m*0.6f,textH=mini?H*0.13f:H-textTop-m-(card.spell?0.0f:H*0.13f);
            const std::wstring words=CardText(id);
            if(textH>2&&!words.empty())
            {
                const RectF box(m,textTop,W-2*m,textH);
                FillRound(g,box,r*0.4f,Color(255,236,226,200));
                if(!mini)Wrapped(g,words,RectF(box.X+m*0.4f,box.Y,box.Width-m*0.8f,box.Height),std::min(H*0.062f,W*0.085f),Color(255,40,28,20));
                // Small, only the words of one line (the rest shows large, under the mouse).
                else Wrapped(g,words.size()<=(English()?14:6)?words:std::wstring(L"…"),box,H*0.085f,Color(255,40,28,20));
            }
            return face.get();
        }

        // A card at the size given, with the numbers it has when it has none of its own.
        void DrawCard(Graphics& g,const RectF& box,int id,bool dim=false,int glow=0)
        {
            const float r=box.Width*0.08f;
            if(glow)FillRound(g,RectF(box.X-box.Width*0.04f,box.Y-box.Width*0.04f,box.Width*1.08f,box.Height+box.Width*0.08f),r*1.4f,glow==1?Color(235,255,214,90):Color(235,120,230,140));
            if(Bitmap* face=CardFace(id,static_cast<int>(std::lround(box.Width)),static_cast<int>(std::lround(box.Height))))
                g.DrawImage(face,box.X,box.Y,static_cast<REAL>(face->GetWidth()),static_cast<REAL>(face->GetHeight()));
            const Forts::Card& card=Forts::CardOf(id);
            const bool mini=box.Height<140*Scale();
            const float d=box.Height*(mini?0.22f:0.14f);
            if(card.faction>=0)Mark(g,box.X-d*0.08f,box.Y-d*0.08f,d*(mini?0.86f:1.0f),std::to_wstring(card.cost),Color(255,40,84,170),MarkShape::Gem);
            if(dim)FillRound(g,box,r,Color(130,20,20,30));
        }

        void DrawStats(Graphics& g,const RectF& box,int attack,int health,bool hurt,bool raised)
        {
            const float d=box.Height*(box.Height<140*Scale()?0.22f:0.14f);
            Mark(g,box.X+box.Width*0.02f,box.GetBottom()-d*1.02f,d,std::to_wstring(attack),raised?Color(255,214,120,20):Color(255,168,52,30),MarkShape::Blades);
            Mark(g,box.GetRight()-d*1.02f,box.GetBottom()-d*1.02f,d,std::to_wstring(health),hurt?Color(255,190,30,40):Color(255,40,120,70),MarkShape::Shield);
        }

        // now: the soldier it is, as it stands now (none: the card as printed).
        void DrawLarge(Graphics& g,const RectF& box,int id,const Forts::Unit* now=nullptr)
        {
            DrawCard(g,box,id);
            const Forts::Card& card=Forts::CardOf(id);
            if(now)
            {
                DrawStats(g,box,now->attack+now->rally,now->health,now->health<now->maxHealth,now->rally>0||now->attack>card.attack);
                if(now->frozen)FillRound(g,box,box.Width*0.08f,Color(90,120,190,255));
            }
            else if(!card.spell)DrawStats(g,box,card.attack,card.health,false,false);
        }

        // ---- What you may do next, from the moves there are ----

        std::vector<Forts::Action> Options()
        {
            std::vector<Forts::Action> out;
            const Choice& c=state.choice;
            if(c.hand<0&&c.unit<0)return out;
            for(const Forts::Action& a:state.table.Actions())
            {
                if(c.hand>=0)
                {
                    if(a.kind!=Forts::ActionKind::Play||a.hand!=c.hand)continue;
                    if(c.fort>=0&&a.fort!=c.fort)continue;
                    if(c.target>=0&&a.target!=c.target)continue;
                }
                else
                {
                    if((a.kind!=Forts::ActionKind::Move&&a.kind!=Forts::ActionKind::Attack)||a.unit!=c.unit)continue;
                }
                out.push_back(a);
            }
            return out;
        }

        Need NeedNow(const std::vector<Forts::Action>& options)
        {
            const Choice& c=state.choice;
            if(options.empty())return Need::Nothing;
            if(c.unit>=0)return Need::UnitAct;
            const bool forts=std::any_of(options.begin(),options.end(),[](const Forts::Action& a){return a.fort>=0;});
            const bool targets=std::any_of(options.begin(),options.end(),[](const Forts::Action& a){return a.target>=0;});
            if(c.fort<0&&forts)return Need::Fort;
            if(c.target<0&&targets)return Need::Target;
            return Need::Dest;
        }

        void ClearChoice(){state.choice=Choice{};}
        void Say(const wchar_t* name,int weight=2,double chance=1);

        void DoAction(const Forts::Action& a)
        {
            state.table.Do(a);
            ClearChoice();
            if(state.table.Board().who!=You)state.nextAt=GetTickCount64()+TurnWait;
            Redraw();
        }

        // Carries the move out as soon as only one is left.
        void Advance()
        {
            const std::vector<Forts::Action> options=Options();
            if(options.empty()){ClearChoice();return;}
            if(options.size()==1&&state.choice.hand>=0)DoAction(options[0]);
        }

        bool Lit(int uid,const std::vector<Forts::Action>& options,Need need)
        {
            if(need!=Need::Target&&need!=Need::UnitAct)return false;
            return std::any_of(options.begin(),options.end(),[&](const Forts::Action& a){return a.target==uid;});
        }

        bool FortLit(int fort,const std::vector<Forts::Action>& options,Need need)
        {
            if(need==Need::Fort)return std::any_of(options.begin(),options.end(),[&](const Forts::Action& a){return a.fort==fort;});
            if(need==Need::Dest||need==Need::UnitAct)
                return std::any_of(options.begin(),options.end(),[&](const Forts::Action& a){return a.dest==fort;});
            return false;
        }

        LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam);

        bool OverHud(LPARAM lParam)
        {
            const int x=GET_X_LPARAM(lParam),y=GET_Y_LPARAM(lParam);
            if(!state.canvas||x<0||y<0||x>=static_cast<int>(state.canvas->GetWidth())||y>=static_cast<int>(state.canvas->GetHeight()))return false;
            Color c;
            state.canvas->GetPixel(x,y,&c);
            return c.GetA()>0;
        }

        LRESULT CALLBACK ViewProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            // The camera as at the mahjong table: the right button turns it, the middle one (or
            // Shift and the right) moves it, the wheel comes nearer; a right double-click or F1
            // puts it back. A left click is the window's (a square, a button).
            auto& c=state.camera;
            if(message==WM_LBUTTONDOWN||message==WM_LBUTTONDBLCLK)
            {
                SetFocus(state.window);
                return Proc(state.window,WM_LBUTTONDOWN,wParam,lParam);
            }
            switch(message)
            {
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
                SetFocus(state.window);
                state.dragging=message==WM_MBUTTONDOWN||(wParam&MK_SHIFT)?2:1;
                state.dragFrom={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
                SetCapture(hwnd);
                return 0;
            case WM_MOUSEMOVE:
                if(!state.dragging)return Proc(state.window,message,wParam,lParam);
                else
                {
                    const float dx=static_cast<float>(GET_X_LPARAM(lParam)-state.dragFrom.x),dy=static_cast<float>(GET_Y_LPARAM(lParam)-state.dragFrom.y);
                    state.dragFrom={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
                    const float k=1/Scale();
                    if(state.dragging==1)
                    {
                        c.yaw-=dx*k*0.008f;
                        c.pitch=std::clamp(c.pitch+dy*k*0.006f,-0.35f,1.5f);
                    }
                    else
                    {
                        const float step=c.distance*0.0025f*k;
                        c.x-=std::cos(c.yaw)*dx*step;
                        c.z-=std::sin(c.yaw)*dx*step;
                        c.y+=dy*step;
                    }
                }
                return 0;
            case WM_RBUTTONUP:
            case WM_MBUTTONUP:
                state.dragging=0;
                ReleaseCapture();
                return 0;
            case WM_CAPTURECHANGED:
                state.dragging=0;
                return 0;
            case WM_MOUSEWHEEL:
                c.distance=std::clamp(c.distance*std::pow(0.9f,GET_WHEEL_DELTA_WPARAM(wParam)/120.0f),0.5f,4.5f);
                return 0;
            case WM_RBUTTONDBLCLK:
                c={};
                return 0;
            case WM_CONTEXTMENU:
                return 0;
            case WM_ERASEBKGND:
                return 1;
            }
            return DefWindowProcW(hwnd,message,wParam,lParam);
        }

        void PlaceView()
        {
            if(!state.view)return;
            RECT client{};
            GetClientRect(state.window,&client);
            MoveWindow(state.view,0,0,client.right,client.bottom,TRUE);
            std::wstring error;
            if(state.renderer&&!state.renderer->Resize(static_cast<unsigned>(std::max(1L,client.right)),static_cast<unsigned>(std::max(1L,client.bottom)),error))
                Log(L"forts: resize: %s",error.c_str());
            InvalidateRect(state.window,nullptr,FALSE);
        }

        void ReleaseView()
        {
            if(state.renderer){state.renderer->WaitIdle();state.renderer.reset();Log(L"forts: 3D view released");}
            state.view=nullptr;
        }
        float FloorY(){return state.floorY;}
        float FieldZ();

        bool CreateView()
        {
            if(!state.her.model)return false;
            const HINSTANCE instance=GetModuleHandleW(nullptr);
            static bool registered=false;
            if(!registered)
            {
                WNDCLASSEXW wc{sizeof(wc)};
                wc.style=CS_DBLCLKS;
                wc.lpfnWndProc=ViewProc;
                wc.hInstance=instance;
                wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
                wc.lpszClassName=ViewClassName;
                RegisterClassExW(&wc);
                registered=true;
            }
            RECT client{};
            GetClientRect(state.window,&client);
            const unsigned w=static_cast<unsigned>(std::max(1L,client.right)),h=static_cast<unsigned>(std::max(1L,client.bottom));
            state.view=CreateWindowExW(0,ViewClassName,L"",WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,0,0,static_cast<int>(w),static_cast<int>(h),state.window,nullptr,instance,nullptr);
            if(!state.view)return false;
            auto renderer=std::make_unique<MascotRenderer>();
            renderer->SetShadowMapSize(16);
            renderer->SetSelfShadow(false);
            renderer->SetBackground(30/255.0f,24/255.0f,22/255.0f,1);
            std::wstring error;
            if(!renderer->Initialize(state.view,w,h,state.her.light?1:state.her.msaa,false,error)||
               !renderer->LoadModel(*state.her.model,state.her.toonDir,state.her.light?512:state.her.maxTexture,error))
            {
                Log(L"forts: no 3D view (%s)",error.c_str());
                renderer->WaitIdle();
                DestroyWindow(state.view);
                state.view=nullptr;
                return false;
            }
            renderer->SetLight(20,45);
            std::array<float,3> lo{},hi{},legLo{},legHi{},head{};
            if(state.her.model->StandardBoneBounds(lo,hi)&&state.her.model->LegBounds(legLo,legHi)&&state.her.model->HeadPosition(head)&&head[1]>legHi[1])
            {
                state.hips=std::max(legHi[1]-lo[1],0.1f);
                state.upper=head[1]-legHi[1];
            }
            // The floor below the seat, low enough for her feet as she sits (as at mahjong).
            state.floorY=-0.5f*state.hips;
            if(state.her.model->CanSit())
            {
                MascotModel& model=*state.her.model;
                model.SetActivity(Activity::Sit,0);
                for(int i=0;i<90;++i)model.Update(1/30.0);
                if(std::array<float,3> legLo{},legHi{};model.LegBounds(legLo,legHi))
                    state.floorY=std::min(state.floorY,legLo[1]-0.03f*state.hips);
                model.SetActivity(Activity::Idle);
                for(int i=0;i<90;++i)model.Update(1/30.0);
                model.SettlePose();   // nothing begun meanwhile comes along
                model.Update(1/30.0);
                model.ResetPhysics();
            }
            Log(L"forts: 3D view %ux%u on %s (hips %.1f, upper body %.1f, floor %.1f)",w,h,renderer->AdapterName().c_str(),state.hips,state.upper,state.floorY);
            {
                std::vector<MascotRenderer::GeometryVertex> room;
                BuildWesternRoom(room,state.roomGlows,{state.upper,state.hips,state.floorY,0.12f*state.hips+0.30f*state.upper,FieldZ()});
                if(!renderer->SetRoomGeometry(room))Log(L"forts: no room");
            }
            state.renderer=std::move(renderer);
            state.entrance=0;
            state.camera={};
            state.dragging=0;
            state.tableBuilt=false;
            state.endGesture.clear();
            state.gestureDone=false;
            return true;
        }

        constexpr float Wood=0.10f;
        constexpr float Washi=0.08f;

        struct Mesh
        {
            using V=std::array<float,3>;
            std::vector<MascotRenderer::GeometryVertex> out;
            int grain=0;
            float grainScale=1;
            float clear=0;
            void Quad(const V& a,const V& b,const V& c,const V& d,const V& n,const float (&col)[4],const float* uv=nullptr)
            {
                const V* p[4]={&a,&b,&c,&d};
                for(int k:{0,1,2,0,2,3})
                {
                    const V& q=*p[k];
                    MascotRenderer::GeometryVertex v{{q[0],q[1],q[2]},{n[0],n[1],n[2]},{col[0],col[1],col[2],col[3]}};
                    if(col[3]==Wood||col[3]==Washi)
                    {
                        const float along=q[grain==0?0:grain==1?2:1],up=q[grain==2?0:1],across=q[grain==1?0:2];
                        v.wall[0]=along/grainScale;v.wall[1]=up/grainScale;v.wall[3]=across/grainScale;
                    }
                    if(uv){v.uv[0]=uv[k*2];v.uv[1]=uv[k*2+1];v.uv[2]=1;}
                    v.uv[3]=clear;
                    out.push_back(v);
                }
            }
            void Box(float x0,float y0,float z0,float x1,float y1,float z1,const float (&c)[4],bool topFace=true)
            {
                if(topFace)Quad({x0,y1,z0},{x1,y1,z0},{x1,y1,z1},{x0,y1,z1},{0,1,0},c);
                Quad({x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},{0,0,-1},c);
                Quad({x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1},{0,0,1},c);
                Quad({x0,y0,z0},{x0,y0,z1},{x0,y1,z1},{x0,y1,z0},{-1,0,0},c);
                Quad({x1,y0,z0},{x1,y0,z1},{x1,y1,z1},{x1,y1,z0},{1,0,0},c);
            }
        };

        float SeatDepth(){return 0.27f*state.hips+0.22f*state.upper;}
        float SeatFront(){return 0.27f*state.hips-SeatDepth()/2;}
        float SeatZ(){return 0.15f*state.upper+SeatDepth();}
        struct TableShape {float x0,x1,zNear,zFar,top,rw,frameTop,frameBottom,leg;};
        TableShape Table3D()
        {
            const float D=state.upper,top=0.12f*state.hips+0.30f*D;
            return {-1.3f*D,1.3f*D,-0.30f*D,-2.15f*D,top,0.11f*D,top+0.04f*D,top-0.11f*D,0.15f*D};
        }

        std::vector<MascotRenderer::GeometryVertex> BuildChair(float z)
        {
            const float D=state.upper,floor=FloorY(),cw=0.34f*D;
            z-=SeatFront();
            const float back=SeatDepth();
            const float frame[4]={0.34f,0.19f,0.10f,Wood},legs[4]={0.26f,0.14f,0.07f,Wood},cushion[4]={0.48f,0.10f,0.12f,1};
            Mesh m;
            m.grainScale=0.35f;
            m.grain=1;
            m.Box(-cw,-0.08f*D,z,cw,-0.03f*D,z+back,frame);
            m.Box(-cw+0.02f*D,-0.03f*D,z+0.02f*D,cw-0.02f*D,-0.005f*D,z+back-0.02f*D,cushion);
            m.grain=2;
            for(float lx:{-cw,cw-0.06f*D})
            {
                m.Box(lx,floor,z,lx+0.06f*D,-0.08f*D,z+0.06f*D,legs);
                m.Box(lx,floor,z+back-0.06f*D,lx+0.06f*D,0.75f*D,z+back,legs);
            }
            m.grain=0;
            for(float y:{0.30f*D,0.62f*D})m.Box(-cw,y,z+back-0.05f*D,cw,y+0.09f*D,z+back-0.01f*D,frame);
            return std::move(m.out);
        }

        // The table's wood (the chair is BuildChair's; the room round it, made once, is WesternRoom's).
        void BuildRoom(Mesh& m)
        {
            auto box=[&](float x0,float y0,float z0,float x1,float y1,float z1,const float (&c)[4]){m.Box(x0,y0,z0,x1,y1,z1,c);};
            int& grain=m.grain;
            float& grainScale=m.grainScale;
            const float D=state.upper,floor=FloorY();
            const float frame[4]={0.20f,0.09f,0.05f,Wood},skirt[4]={0.15f,0.07f,0.04f,Wood};
            const TableShape t=Table3D();
            const float x0=t.x0,x1=t.x1,zNear=t.zNear,zFar=t.zFar;
            grainScale=0.35f;
            const float rw=t.rw,frameTop=t.frameTop,frameBottom=t.frameBottom,leg=t.leg;
            grain=0;
            box(x0-rw,frameBottom,zFar-rw,x1+rw,frameTop,zFar,frame);
            box(x0-rw,frameBottom,zNear,x1+rw,frameTop,zNear+rw,frame);
            grain=1;
            box(x0-rw,frameBottom,zFar,x0,frameTop,zNear,frame);
            box(x1,frameBottom,zFar,x1+rw,frameTop,zNear,frame);
            grain=2;
            for(float lx:{x0-rw+0.02f*D,x1+rw-0.02f*D-leg})
                for(float lz:{zFar-rw+0.02f*D,zNear+rw-0.02f*D-leg})
                    box(lx,floor,lz,lx+leg,frameBottom,lz+leg,skirt);
        }
        float Top(){return 0.12f*state.hips+0.30f*state.upper;}
        float FieldZ(){return -1.10f*state.upper;}

        // The table as it stands: its wood and felt, each side's deck a plain stack.
        std::vector<MascotRenderer::GeometryVertex> BuildTable()
        {
            Mesh mesh;
            const float D=state.upper;
            const float felt[4]={0.09f,0.33f,0.22f,1},cards[4]={0.30f,0.20f,0.14f,1};
            const TableShape t=Table3D();
            BuildRoom(mesh);
            mesh.Box(t.x0,t.top-0.05f*D,t.zFar,t.x1,t.top,t.zNear,felt);
            const float hw=0.075f*D,hl=0.135f*D,h=0.05f*D,x=0.95f*D;
            mesh.Box(x-hw,t.top,-0.75f*D-hl,x+hw,t.top+h,-0.75f*D+hl,cards);
            mesh.Box(-x-hw,t.top,-1.65f*D-hl,-x+hw,t.top+h,-1.65f*D+hl,cards);
            return std::move(mesh.out);
        }

        void LoadStrength()
        {
            state.strength={};
            if(state.her.strengthFile.empty())return;
            if(FILE* in=nullptr;_wfopen_s(&in,state.her.strengthFile.c_str(),L"r")==0&&in)
            {
                Forts::Strength read;
                if(fscanf_s(in,"%lf %d",&read.level,&read.games)==2&&read.level>=0&&read.level<=Forts::Levels-1&&read.games>=0)state.strength=read;
                fclose(in);
            }
        }

        void LoadCollection()
        {
            state.owned.clear();
            if(state.her.collectionFile.empty())return;
            if(FILE* in=nullptr;_wfopen_s(&in,state.her.collectionFile.c_str(),L"r")==0&&in)
            {
                // Only what can be collected, however the file came to be.
                std::vector<int> left=Forts::CollectableCards();
                int id=0;
                while(fscanf_s(in,"%d",&id)==1)
                    if(const auto k=std::find(left.begin(),left.end(),id);k!=left.end())
                    {
                        left.erase(k);
                        state.owned.push_back(id);
                    }
                fclose(in);
            }
        }

        void SaveCollection()
        {
            if(state.her.collectionFile.empty())return;
            if(FILE* out=nullptr;_wfopen_s(&out,state.her.collectionFile.c_str(),L"w")==0&&out)
            {
                for(int id:state.owned)fprintf(out,"%d\n",id);
                fclose(out);
            }
        }

        bool DeckLocked(int deck){return deck==Forts::CollectedDeck&&static_cast<int>(state.owned.size())<Forts::UnlockAt;}

        void SaveStrength()
        {
            if(state.her.strengthFile.empty())return;
            if(FILE* out=nullptr;_wfopen_s(&out,state.her.strengthFile.c_str(),L"w")==0&&out)
            {
                fprintf(out,"%.2f %d\n",state.strength.level,state.strength.games);
                fclose(out);
            }
        }

        void Finish()
        {
            if(!state.window)return;
            // Stopping a game going badly says as much as losing it.
            if(!state.choosing&&state.seen.started&&state.table.Now()!=Phase::Over&&state.table.Board().turns>=6&&
               state.table.Board().side[Her].score>state.table.Board().side[You].score)
            {
                Forts::AfterGame(state.strength,-1,0,true);
                SaveStrength();
            }
            KillTimer(state.window,TimerId);
            state.voice.Stop();
            if(state.her.model)state.her.model->SetSolids({});
            if(state.her.model&&state.tell!=Expression::Neutral)state.her.model->SetExpression(Expression::Neutral,false);
            state.tell=Expression::Neutral;
            if(state.afterword.empty()&&state.seen.started&&state.table.Now()!=Phase::Over)
            {
                const auto& t=state.table;
                state.afterword=Fmt(T(L"いま、話し相手と三つの砦というカードゲームをしていたが、途中でやめてデスクトップに戻ってきたところ（話し相手は{0}点、あなたは{1}点だった）。そのことを話し相手にひと言（1〜2文）",
                                      L"You were playing a card game called Three Forts with them, but they stopped midway and you have come back to the desktop (they had {0} points, you {1}). Say a word about it to them (one or two sentences)"),
                                    {std::to_wstring(t.Board().side[You].score),std::to_wstring(t.Board().side[Her].score)});
                state.afterwordLine=T(L"また続き、やろうね。",L"Let's finish it another time.");
            }
            auto onEnd=std::move(state.onEnd);
            DestroyWindow(state.window);
            if(onEnd)onEnd();
        }
        void Prefetch();

        // The deck chooser comes first; a game begins with your choice.
        void NewGame()
        {
            state.gameOverTold=false;
            state.choosing=true;
            state.hover=-1;
            state.hoverDeck=-1;
            state.hoverHand=state.hoverUnit=-1;
            state.confirmQuit=false;
            state.help=false;
            state.said.clear();
            state.nextAt=GetTickCount64()+300;
            state.seen={};
            state.words.waiting=false;
            state.words.text.clear();
            state.afterword.clear();
            state.afterwordLine.clear();
            state.tellFor=-1;
            state.gestureDone=false;
            state.reachDue=false;
            ClearChoice();
            state.redrawMask=0;
            state.fx.clear();
            state.ghosts.clear();
            state.sparks.clear();
            state.bolts.clear();
            state.burstDue.clear();
            state.endAt=0;
            state.lungeAt=0;
            state.fresh.clear();
            state.unitBoxes.clear();
            state.shownCard=-1;
            Prefetch();
        }

        void StartGame(int deck)
        {
            if(DeckLocked(deck))return;
            std::random_device device;
            const std::uint32_t seed=device();
            const int herDeck=static_cast<int>(seed%Forts::DeckKinds);
            const std::vector<int> collected=Forts::CollectedDeckCards(state.owned);
            state.table.NewGame(seed,deck,herDeck,state.strength.Level(),static_cast<int>((seed>>8)%2),deck==Forts::CollectedDeck?&collected:nullptr);
            state.earned=-1;
            state.unlockedNow=false;
            state.choosing=false;
            state.nextAt=GetTickCount64()+200;
            Redraw();
        }

        void Continue()
        {
            if(EndShown())NewGame();
            Redraw();
        }

        void Do(Act act)
        {
            auto& t=state.table;
            if(act==Act::Redraw)
            {
                if(RedrawNow()){t.Redraw(state.redrawMask);state.redrawMask=0;state.nextAt=GetTickCount64()+300;Redraw();}
                return;
            }
            if(!YourTurnNow())return;
            if(act==Act::End){DoAction(Forts::Action{});return;}
            if(act==Act::Spare)
            {
                Forts::Action a;
                a.kind=Forts::ActionKind::Spare;
                for(const auto& k:t.Actions())if(k.kind==a.kind){DoAction(a);break;}
                return;
            }
            if(act==Act::Offer&&state.choice.hand>=0)
            {
                Forts::Action a;
                a.kind=Forts::ActionKind::Offer;
                a.hand=static_cast<std::int8_t>(state.choice.hand);
                for(const auto& k:t.Actions())if(k.kind==a.kind&&k.hand==a.hand){DoAction(a);break;}
            }
        }

        // ---- Her words and the watchers' (as at mahjong) ----

        constexpr ULONGLONG WordsHold=3500;
        constexpr ULONGLONG AnswerWait=9000;

        // Her words: shown in the bubble (none for a voice alone) and, with a voice file of
        // their own, played (the words revealed over it).
        void SayLine(const GameLine& line,int weight)
        {
            auto& w=state.words;
            const ULONGLONG now=GetTickCount64();
            state.voice.Stop();
            std::wstring error;
            w.voiced=!line.voice.empty()&&state.voice.Play(line,error);
            // In her voice (the voice engine) when she speaks aloud and the line has no sound of its own.
            if(state.her.speak)state.her.speak(w.voiced?std::wstring():line.text);
            if(!error.empty())w.told+=L"  voice: "+error+L"\n";
            w.text=line.text;
            w.waiting=false;
            w.weight=weight;
            w.since=now;
            w.told+=L"  she says: "+line.text+(line.voice.empty()?L"":L" ["+line.voice+L"]")+L"\n";
            const ULONGLONG reading=static_cast<ULONGLONG>(CharSeconds()*1000*line.text.size())+2600+90*static_cast<ULONGLONG>(line.text.size());
            w.until=now+(w.voiced?std::max(reading,static_cast<ULONGLONG>(state.voice.Seconds()*1000)+1800):reading);
            if(!line.text.empty()){w.recent.push_back(line.text);if(w.recent.size()>4)w.recent.erase(w.recent.begin());}
            Redraw();
        }

        void SayWords(const std::wstring& line,int weight){SayLine({line,{}},weight);}

        const std::wstring& Pick(const std::vector<std::wstring>& lines)
        {
            std::uniform_int_distribution<size_t> any(0,lines.size()-1);
            return lines[any(state.sparkRandom)];
        }

        // Something happened: she speaks of it, and those watching hear of it. facts: for her (she
        // is "you"); feel: how she is to feel about it; seen: the same for one watching; scene:
        // the scene of her lines (null: she says nothing); theirs: the watchers'.
        void Happened(int weight,const std::wstring& facts,const std::wstring& feel,const wchar_t* scene,
                      const std::wstring& seen,const std::vector<std::wstring>& theirs)
        {
            auto& t=state.table;
            state.words.told+=Fmt(L"  {0} s, weight {1}: {2}\n",{std::to_wstring(static_cast<int>(state.clock)),std::to_wstring(weight),seen});
            if(state.her.onEvent&&!theirs.empty())
                state.her.onEvent(seen+Fmt(T(L"（持ち主は{0}点、{1}は{2}点）",L" (the owner has {0} points, {1} {2})"),{std::to_wstring(t.Board().side[You].score),HerName(),std::to_wstring(t.Board().side[Her].score)}),weight,Pick(theirs));
            if(!scene)return;
            auto& w=state.words;
            const ULONGLONG now=GetTickCount64();
            if((w.waiting||now-w.since<WordsHold)&&weight<w.weight)return;
            if(!state.lines.Roll(scene,1,state.sparkRandom))return;
            GameLine line;
            if(state.lines.Pick(scene,w.recent,state.sparkRandom,line))SayLine(line,weight);
        }

        // The next scene not yet in her own words, asked of the LLM.
        void Prefetch()
        {
            if(state.prefetching||!state.her.prefetch)return;
            const std::wstring ask=state.lines.NextAsk();
            if(!ask.empty())state.prefetching=state.her.prefetch(ask);
        }

        // A line of the scene, said now and then (chance: how often, unless the user's lines
        // file says otherwise).
        void Say(const wchar_t* name,int weight,double chance)
        {
            if(!state.lines.Roll(name,chance,state.sparkRandom))return;
            GameLine line;
            if(state.lines.Pick(name,state.words.recent,state.sparkRandom,line))SayLine(line,weight);
        }
        // The look on her face: how she stands (the score and who holds what), now and then the
        // other way about, put on.
        void UpdateTell()
        {
            auto& t=state.table;
            Expression e=Expression::Neutral;
            if(state.choosing){}
            else if(t.Now()==Phase::Over)e=t.Board().winner==Her?Expression::Smile:t.Board().winner==You?Expression::Sad:Expression::Neutral;
            else
            {
                const auto& b=t.Board();
                int lead=b.side[Her].score-b.side[You].score;
                for(int f=0;f<Forts::FortCount;f++)
                {
                    if(Forts::Holds(b,Her,f))lead+=Forts::FortValue[f];
                    if(Forts::Holds(b,You,f))lead-=Forts::FortValue[f];
                }
                const int band=lead>=3?2:lead<=-3?0:1;
                const int key=b.turns/2*3+band;
                if(state.tellFor!=key)
                {
                    state.tellFor=key;
                    std::uniform_real_distribution<double> any(0,1);
                    Expression look=band==2?Expression::Smile:band==0?Expression::Troubled:Expression::Neutral;
                    if(any(state.sparkRandom)<PutOnShare)look=look==Expression::Smile?Expression::Troubled:look==Expression::Troubled?Expression::Smile:look;
                    state.tellFace=look;
                }
                e=state.tellFace;
            }
            if(e==state.tell)return;
            state.tell=e;
            if(state.her.model&&state.renderer)state.her.model->SetExpression(e,false);
        }

        void AddFx(const std::wstring& text,const RectF& at,Color color,bool frame=false,ULONGLONG wait=0,bool flash=false,bool slash=false)
        {
            Fx f;
            f.text=text;
            f.at=at;
            f.color=color;
            f.start=GetTickCount64()+wait;
            f.frame=frame;
            f.flash=flash;
            f.slash=slash;
            state.fx.push_back(std::move(f));
        }

        RectF UnitBox(int uid)
        {
            const auto k=state.unitBoxes.find(uid);
            return k==state.unitBoxes.end()?RectF():k->second;
        }

        PointF Middle(const RectF& box){return PointF(box.X+box.Width/2,box.Y+box.Height/2);}

        float Rand(float from,float to){return std::uniform_real_distribution<float>(from,to)(state.sparkRandom);}

        // Specks flying out from (x, y). fall: downward pull (less than zero, they rise).
        void Burst(float x,float y,int count,Color color,float speed,float size,ULONGLONG life,float fall=0,ULONGLONG wait=0,float spread=0)
        {
            const ULONGLONG now=GetTickCount64();
            for(int i=0;i<count&&state.sparks.size()<MostSparks;i++)
            {
                Spark p;
                const float angle=Rand(0,6.2832f),v=speed*Rand(0.25f,1.0f);
                p.x=x+Rand(-spread,spread);
                p.y=y+Rand(-spread,spread);
                p.vx=std::cos(angle)*v;
                p.vy=std::sin(angle)*v;
                p.size=size*Rand(0.6f,1.2f);
                p.fall=fall;
                p.start=now+wait;
                p.life=static_cast<ULONGLONG>(life*Rand(0.7f,1.1f));
                p.color=color;
                state.sparks.push_back(p);
            }
        }

        void AddBolt(const PointF& from,const PointF& to,Color color,float size,ULONGLONG life,ULONGLONG wait=0)
        {
            Bolt b;
            b.from=from;
            b.to=to;
            b.color=color;
            b.size=size;
            b.life=life;
            b.start=GetTickCount64()+wait;
            state.bolts.push_back(b);
        }

        // Where a spell of theirs comes from: your hand, her.
        PointF Caster(int who)
        {
            RECT client{};
            GetClientRect(state.window,&client);
            const float W=static_cast<float>(client.right),H=static_cast<float>(client.bottom),s=Scale();
            if(who==You)return PointF(std::max(W*0.3f,300*s),H-(HandH/2+10)*s);
            if(state.headScreen.x>=0)return PointF(static_cast<float>(state.headScreen.x),static_cast<float>(state.headScreen.y));
            return PointF(W/2,60*s);
        }

        // What has happened on the table: numbers and light, her words.
        void HandleEvents(const std::vector<Forts::Event>& events)
        {
            const ULONGLONG now=GetTickCount64();
            const float s=Scale();
            const Color red(255,255,110,90),gold(255,255,214,110),green(255,130,240,150),blue(255,150,210,255);
            const Color fire(255,255,150,50),ember(255,255,90,30),pale(255,225,235,255),violet(255,200,150,255);
            if(!events.empty())Redraw();
            ULONGLONG wait=0;          // how long what follows waits for what has just been shot
            // A strike that brings both down.
            const auto fell=[&](int uid){return std::any_of(events.begin(),events.end(),[&](const Forts::Event& e){return e.kind==Forts::EventKind::Fell&&e.unit==uid;});};
            int bothStriker=-1;
            float bothX=0,bothY=0;
            for(const Forts::Event& e:events)
            {
                switch(e.kind)
                {
                case Forts::EventKind::TurnStart:
                    state.said=e.who==You?T(L"あなたの番",L"Your turn"):Fmt(T(L"{0}の番",L"{0}'s turn"),{HerName()});
                    state.saidAt=now;
                    if(e.who==You&&state.table.HerAim()>=0)
                    {
                        // She lets her eyes rest on a fort, and now and then says so. Not always the truth.
                        state.glanceFort=state.table.HerAim();
                        state.glanceUntil=state.clock+2.6;
                        static const wchar_t* const scenes[]={L"狙う左",L"狙う中央",L"狙う右"};
                        Say(scenes[state.glanceFort],2,0.6);
                    }
                    break;
                case Forts::EventKind::Scored:
                {
                    AddFx(L"+"+std::to_wstring(e.amount),state.bannerBoxes[e.fort],gold,true);
                    // The points go up to the score.
                    const PointF from=Middle(state.bannerBoxes[e.fort]);
                    const PointF to=state.scoreSpot[e.who];
                    Burst(from.X,from.Y,18,gold,150*s,5*s,600,-40*s,0,state.bannerBoxes[e.fort].Width*0.2f);
                    for(int i=0;i<5;i++)AddBolt(PointF(from.X+Rand(-40*s,40*s),from.Y),to,gold,9*s,420,120+i*60);
                    Burst(to.X,to.Y,20,gold,120*s,5*s,500,0,120+4*60+420);
                    PlayMahjongSound(MahjongSound::CardTake);
                    if(e.who==Her)Say(L"砦を取った",3,0.35);
                    else Say(L"砦を取られた",3,0.3);
                    break;
                }
                case Forts::EventKind::Drew:
                    if(e.who==You&&e.amount)AddFx(T(L"手札がいっぱい",L"Hand full"),RectF(state.fortBoxes[1].X,state.fortBoxes[1].GetBottom()-20,state.fortBoxes[1].Width,20),red);
                    break;
                case Forts::EventKind::Placed:
                    state.fresh[e.unit]=now;
                    state.burstDue.push_back(e.unit);
                    PlayMahjongSound(MahjongSound::CardPlace);
                    if(e.who==Her)
                    {
                        state.shownCard=e.card;
                        state.shownUntil=now+ShowCardFor;
                        if(Forts::CardOf(e.card).cost>=5)Say(L"大きな札",4,0.7);
                    }
                    break;
                case Forts::EventKind::Spell:
                {
                    const PointF from=Caster(e.who);
                    const Forts::Effect effect=Forts::CardOf(e.card).effect;
                    if(effect==Forts::Effect::Storm&&e.fort>=0)
                    {
                        // Fire comes down over the whole fort.
                        const RectF fort=state.fortBoxes[e.fort];
                        for(int i=0;i<70&&state.sparks.size()<MostSparks;i++)
                        {
                            Spark p;
                            p.x=fort.X+Rand(0,fort.Width);
                            p.y=fort.Y+Rand(-60*s,fort.Height*0.3f);
                            p.vx=Rand(-40*s,40*s);
                            p.vy=Rand(180*s,420*s);
                            p.size=Rand(7*s,17*s);
                            p.fall=300*s;
                            p.start=now+static_cast<ULONGLONG>(Rand(0,350));
                            p.life=static_cast<ULONGLONG>(Rand(450,800));
                            p.color=i%3?fire:ember;
                            state.sparks.push_back(p);
                        }
                        wait=std::max<ULONGLONG>(wait,320);
                    }
                    else if(effect==Forts::Effect::RallyFort&&e.fort>=0)
                    {
                        const RectF fort=state.fortBoxes[e.fort];
                        for(int i=0;i<36&&state.sparks.size()<MostSparks;i++)
                        {
                            Spark p;
                            p.x=fort.X+Rand(0,fort.Width);
                            p.y=fort.GetBottom()-Rand(0,fort.Height*0.5f);
                            p.vy=Rand(-260*s,-110*s);
                            p.size=Rand(4*s,9*s);
                            p.start=now+static_cast<ULONGLONG>(Rand(0,300));
                            p.life=static_cast<ULONGLONG>(Rand(400,750));
                            p.color=gold;
                            state.sparks.push_back(p);
                        }
                    }
                    else if(effect==Forts::Effect::DrawOnPlay)Burst(from.X,from.Y,26,violet,170*s,6*s,650,-60*s);
                    else if(effect==Forts::Effect::Ambush)Burst(from.X,from.Y,22,green,190*s,6*s,600);
                    else if(effect==Forts::Effect::Push)
                    {
                        const PointF to=Middle(UnitBox(e.target));
                        AddBolt(from,to,pale,12*s,240);
                        Burst(to.X,to.Y,26,pale,300*s,5*s,450,0,220);
                    }
                    PlayMahjongSound(MahjongSound::CardPlace);
                    if(e.who==Her)
                    {
                        state.shownCard=e.card;
                        state.shownUntil=now+ShowCardFor;
                    }
                    break;
                }
                case Forts::EventKind::Moved:
                    state.fresh[e.unit]=now;
                    PlayMahjongSound(MahjongSound::CardPlace);
                    break;
                case Forts::EventKind::Hit:
                {
                    const RectF target=UnitBox(e.target);
                    const PointF to=Middle(target);
                    const Forts::Card& card=Forts::CardOf(e.card);
                    ULONGLONG hit=wait;
                    Color spark=gold;
                    if(e.unit>=0)
                    {
                        // A soldier striking leans toward the other, and back. They do not touch.
                        const PointF from=Middle(UnitBox(e.unit));
                        const float dx=to.X-from.X,dy=to.Y-from.Y,length=std::max(1.0f,std::sqrt(dx*dx+dy*dy));
                        state.lungeUnit=e.unit;
                        state.lungeAt=now;
                        state.lungeX=dx/length*24*s;
                        state.lungeY=dy/length*24*s;
                        if(card.keywords&Forts::Ranged)
                        {
                            AddBolt(from,to,pale,9*s,220,70);
                            hit=std::max<ULONGLONG>(hit,270);
                            spark=pale;
                        }
                        else hit=std::max<ULONGLONG>(hit,120);
                        AddFx(L"",target,Color(255,255,246,220),false,hit,false,!(card.keywords&Forts::Ranged));
                    }
                    else if(card.effect!=Forts::Effect::Storm)
                    {
                        // A spell's shot, or a soldier's on arrival: from whoever played it.
                        AddBolt(Caster(e.who),to,fire,15*s,280);
                        hit=std::max<ULONGLONG>(hit,260);
                        spark=fire;
                    }
                    else spark=fire;
                    AddFx(e.amount>0?L"-"+std::to_wstring(e.amount):std::wstring(L"0"),target,red,true,hit,e.amount>0);
                    if(target.Width>0)Burst(to.X,to.Y,e.amount>0?22:8,spark,330*s,6*s,430,160*s,hit);
                    if(e.unit>=0&&e.back>0)AddFx(L"-"+std::to_wstring(e.back),UnitBox(e.unit),red,false,hit+60,true);
                    if(e.unit>=0&&fell(e.unit)&&fell(e.target))
                    {
                        // Both fall: a clash of light between them, and a cut on each.
                        const RectF striker=UnitBox(e.unit);
                        const PointF from=Middle(striker),mid((from.X+to.X)/2,(from.Y+to.Y)/2);
                        bothStriker=e.unit;
                        bothX=state.lungeX;
                        bothY=state.lungeY;
                        AddFx(L"",striker,Color(255,255,246,220),false,hit,false,true);
                        Burst(mid.X,mid.Y,8,Color(255,255,240,190),90*s,34*s,420,0,hit);
                        Burst(mid.X,mid.Y,46,gold,520*s,7*s,620,240*s,hit);
                        Burst(mid.X,mid.Y,26,ember,360*s,6*s,560,200*s,hit+40);
                        const float w=220*s,h=44*s;
                        AddFx(T(L"相打ち！",L"Both fall!"),RectF(mid.X-w/2,mid.Y-h/2,w,h),gold,false,hit+40);
                        wait=std::max<ULONGLONG>(wait,LungeFor-90);
                    }
                    wait=std::max(wait,hit);
                    PlayMahjongSound(MahjongSound::CardTake);
                    break;
                }
                case Forts::EventKind::Fell:
                {
                    Ghost ghost;
                    ghost.card=e.card;
                    ghost.at=UnitBox(e.unit);
                    ghost.start=now+wait+90;
                    if(e.unit==bothStriker)
                    {
                        ghost.leanAt=now;
                        ghost.leanX=bothX;
                        ghost.leanY=bothY;
                    }
                    if(ghost.at.Width>0)
                    {
                        state.ghosts.push_back(ghost);
                        const PointF at=Middle(ghost.at);
                        // Embers going up from it.
                        Burst(at.X,at.Y,26,ember,70*s,6*s,1000,-110*s,wait+90,ghost.at.Width*0.3f);
                        Burst(at.X,at.Y,10,fire,50*s,4*s,800,-70*s,wait+150,ghost.at.Width*0.3f);
                    }
                    if(state.table.Board().who==Her&&e.who==You)Say(L"倒した",3,0.3);
                    else if(state.table.Board().who==You&&e.who==Her)Say(L"倒された",3,0.3);
                    break;
                }
                case Forts::EventKind::Healed:
                {
                    const RectF box=UnitBox(e.unit);
                    AddFx(T(L"回復",L"Healed"),box,green,true);
                    if(box.Width>0)Burst(box.X+box.Width/2,box.GetBottom(),26,green,50*s,6*s,850,-150*s,0,box.Width*0.35f);
                    break;
                }
                case Forts::EventKind::Buffed:
                {
                    const RectF box=UnitBox(e.unit);
                    AddFx(L"+",box,gold,true);
                    if(box.Width>0)Burst(box.X+box.Width/2,box.GetBottom(),16,gold,50*s,5*s,650,-170*s,0,box.Width*0.35f);
                    break;
                }
                case Forts::EventKind::Frozen:
                {
                    const RectF box=UnitBox(e.unit);
                    const PointF to=Middle(box);
                    AddBolt(Caster(e.who),to,blue,13*s,260);
                    AddFx(T(L"凍結",L"Frozen"),box,blue,true,240,true);
                    if(box.Width>0)Burst(to.X,to.Y,30,pale,240*s,5*s,600,70*s,240,box.Width*0.2f);
                    break;
                }
                case Forts::EventKind::Shield:
                {
                    const RectF fort=state.fortBoxes[e.fort];
                    AddFx(T(L"防塁",L"Rampart"),state.bannerBoxes[e.fort],blue);
                    AddFx(L"",fort,blue,true);
                    Burst(fort.X+fort.Width/2,fort.GetBottom(),30,blue,60*s,6*s,800,-170*s,0,fort.Width*0.42f);
                    break;
                }
                case Forts::EventKind::Offered:
                {
                    const PointF from=Caster(e.who),to=state.scoreSpot[e.who];
                    const PointF spot(to.X+(e.who==You?0:110*s),to.Y+(e.who==You?30*s:0));
                    AddBolt(from,spot,blue,10*s,320);
                    Burst(spot.X,spot.Y,14+6*e.amount,blue,130*s,5*s,500,0,300);
                    AddFx(Fmt(T(L"魔力+{0}",L"Magic +{0}"),{std::to_wstring(e.amount)}),RectF(spot.X-40*s,spot.Y-20*s,80*s,40*s),blue,false,300);
                    PlayMahjongSound(MahjongSound::CardPlace);
                    break;
                }
                case Forts::EventKind::Spare:
                    break;
                }
            }
            state.fx.erase(std::remove_if(state.fx.begin(),state.fx.end(),[&](const Fx& f){return Past(now,f.start,f.life);}),state.fx.end());
            state.ghosts.erase(std::remove_if(state.ghosts.begin(),state.ghosts.end(),[&](const Ghost& k){return Past(now,k.start,700);}),state.ghosts.end());
            state.sparks.erase(std::remove_if(state.sparks.begin(),state.sparks.end(),[&](const Spark& p){return Past(now,p.start,p.life);}),state.sparks.end());
            state.bolts.erase(std::remove_if(state.bolts.begin(),state.bolts.end(),[&](const Bolt& b){return Past(now,b.start,b.life);}),state.bolts.end());
        }

        // What is new in the game since the last tick, for words: its start, the game's end.
        void NoticeWords()
        {
            auto& t=state.table;
            auto& seen=state.seen;
            auto& w=state.words;
            const ULONGLONG now=GetTickCount64();
            if(w.waiting&&now-w.since>AnswerWait)SayWords(w.fallback,w.weight);
            if(!w.text.empty()&&!w.waiting&&now>w.until){w.text.clear();RedrawTop();}
            if(w.text!=w.revealing)
            {
                if(w.revealing.empty()||w.revealing==L"…"||w.text.rfind(w.revealing,0)!=0)w.shown=0;
                w.revealing=w.text;
            }
            const size_t before=static_cast<size_t>(w.shown);
            w.shown=std::min(static_cast<double>(w.text.size()),w.voiced?state.voice.Revealed():w.shown+(now-std::min(now,w.shownAt))/1000.0/CharSeconds());
            w.shownAt=now;
            if(static_cast<size_t>(w.shown)!=before)RedrawTop();
            if(state.choosing)return;
            const std::wstring name=HerName();
            if(!seen.started)
            {
                seen.started=true;
                Happened(6,T(L"これから話し相手とカードゲームの対戦が始まる。",L"A card game with them is about to begin. "),
                         T(L"わくわくする気持ち",L"how you look forward to it"),L"始まり",
                         Fmt(T(L"{0}とこのパソコンの持ち主の三つの砦が始まる。",L"{0} and the owner of this computer are starting a game of Three Forts."),{name}),
                         {T(L"がんばれー！",L"Good luck!"),T(L"どっちが勝つかな？",L"Who will win?")});
            }
            if(!seen.nearTold&&t.Now()==Phase::Yours&&t.Board().side[Her].score>=Forts::Target-2)
            {
                seen.nearTold=true;
                Say(L"もうすぐ",3,0.7);
            }
            const bool over=t.Now()==Phase::Over;
            if(over&&!seen.over)
            {
                const int you=t.Board().side[You].score,her=t.Board().side[Her].score;
                // How hard she plays next game comes from how this one ended.
                Forts::AfterGame(state.strength,t.Board().winner==You?1:t.Board().winner==Her?-1:0,std::abs(you-her));
                SaveStrength();
                // A win brings a card.
                if(t.Board().winner==You)
                {
                    const bool locked=DeckLocked(Forts::CollectedDeck);
                    std::random_device device;
                    std::mt19937 dice(device());
                    state.earned=Forts::Collect(state.owned,dice);
                    state.unlockedNow=locked&&!DeckLocked(Forts::CollectedDeck);
                    if(state.earned>=0)SaveCollection();
                }
                const std::wstring result=Fmt(T(L"話し相手は{0}点、あなたは{1}点",L"they have {0} points, you {1}"),{std::to_wstring(you),std::to_wstring(her)});
                state.afterword=Fmt(T(L"いま、話し相手と三つの砦というカードゲームをして、デスクトップに戻ってきたところ。結果は{0}。その勝負の感想を、話し相手にひと言（1〜2文）",
                                      L"You have just played a card game called Three Forts with them and come back to the desktop. In the end {0}. Tell them what you thought of the game (one or two sentences)"),{result});
                state.afterwordLine=t.Board().winner==You?T(L"強かったね！次は負けないよ。",L"You were good at that! I'll win next time."):T(L"楽しかったね！またやろう。",L"That was fun! Let's play again sometime.");
                if(t.Board().winner==You)Happened(9,Fmt(T(L"カードゲームが終わって、話し相手の勝ち（{0}）。",L"The card game is over and they won ({0})."),{result}),
                                    T(L"悔しい気持ちか、話し相手をほめる気持ち",L"how sorry you are, or praise them"),L"負けて終わる",
                                    Fmt(T(L"{0}と持ち主の三つの砦が終わり、持ち主が勝った。",L"The game of Three Forts between {0} and the owner is over: the owner won."),{name}),
                                    {T(L"おめでとう！",L"Congratulations!"),T(L"おつかれさま！",L"Well played!")});
                else Happened(9,Fmt(T(L"カードゲームが終わって、あなたの勝ち（{0}）。",L"The card game is over and you won ({0})."),{result}),
                              T(L"うれしい気持ち",L"how pleased you are"),L"勝って終わる",
                              Fmt(T(L"{0}と持ち主の三つの砦が終わり、{0}が勝った。",L"The game of Three Forts between {0} and the owner is over: {0} won."),{name}),
                              {T(L"おつかれさま！いい勝負だったね。",L"Well played, both of you!"),Fmt(T(L"{0}の勝ちだ！",L"{0} wins!"),{name})});
            }
            seen.over=over;
        }

        void Tick()
        {
            auto& t=state.table;
            const ULONGLONG now=GetTickCount64();
            if(!state.said.empty()&&now-state.saidAt>SaidShown){state.said.clear();RedrawTop();}
            if(!state.choosing&&t.Now()==Phase::Hers&&Seated())
            {
                if(now+ReachLead>=state.nextAt&&state.reachedFor!=state.herActions)
                {
                    state.reachedFor=state.herActions;
                    if(!t.HerAboutToEnd())state.reachDue=true;
                }
                if(now>=state.nextAt)
                {
                    if(!state.words.waiting&&now-state.words.since>WordsHold&&t.Board().side[Her].magic==t.Board().side[Her].maxMagic)Say(L"考える",1,0.08);
                    const Forts::Action did=t.HerStep();
                    state.herActions++;
                    std::uniform_int_distribution<int> extra(0,400);
                    state.nextAt=now+(did.kind==Forts::ActionKind::End?TurnWait:(state.renderer?HerPlayWait3D:HerPlayWait)+extra(state.sparkRandom));
                }
            }
            if(!state.choosing)HandleEvents(state.table.TakeEvents());
            // You won: gold falling for a while.
            if(EndShown())
            {
                if(!state.endAt)state.endAt=now;
                if(t.Board().winner==You&&now-state.endAt<5000&&state.sparks.size()<MostSparks)
                {
                    RECT client{};
                    GetClientRect(state.window,&client);
                    const float sc=Scale();
                    static const Color colours[]={Color(255,255,214,110),Color(255,255,246,200),Color(255,255,170,70)};
                    for(int i=0;i<3;i++)
                    {
                        Spark p;
                        p.x=Rand(0,static_cast<float>(client.right));
                        p.y=-10*sc;
                        p.vx=Rand(-25*sc,25*sc);
                        p.vy=Rand(120*sc,240*sc);
                        p.size=Rand(3*sc,7*sc);
                        p.start=now;
                        p.life=3200;
                        p.color=colours[i];
                        state.sparks.push_back(p);
                    }
                }
            }
            UpdateTell();
            // What changes by itself: what you may do as her wait runs out, a card shown large
            // going away; while numbers rise or a soldier fades, every tick.
            for(auto k=state.fresh.begin();k!=state.fresh.end();)
                k=now-k->second>=500?state.fresh.erase(k):std::next(k);
            const int key=(Waiting()?1:0)+(state.shownCard>=0&&now<state.shownUntil?2:0)+(state.clock<state.glanceUntil?4:0)+(now-state.lungeAt<LungeFor?8:0)+static_cast<int>(t.Now())*16;
            if(key!=state.drawnKey)
            {
                state.drawnKey=key;
                Redraw();
            }
            if(!state.fx.empty()||!state.ghosts.empty()||!state.fresh.empty()||!state.sparks.empty()||!state.bolts.empty()||!state.said.empty()||now-state.lungeAt<LungeFor||(!state.words.text.empty()&&state.entrance<5))RedrawTop();
            if(!state.renderer)state.voice.Tick(0.04);   // (at the 3D table the model's frame ticks it)
            NoticeWords();
            if(!state.choosing&&t.Now()==Phase::Over&&!state.gameOverTold)
            {
                state.gameOverTold=true;
                if(state.her.onGameOver)state.her.onGameOver();
            }
        }

        // ---- Drawing ----

        struct Run {std::wstring text;bool brush=true;};
        void Runs(Graphics& g,const std::vector<Run>& runs,const RectF& box,float size,Color color)
        {
            float total=0;
            for(const Run& r:runs)total+=TextWidth(g,r.text,size,false,r.brush);
            float x=box.X+(box.Width-total)/2;
            for(const Run& r:runs)
            {
                Text(g,r.text,RectF(x,box.Y,box.Width,box.Height),size,color,false,StringAlignmentNear,StringAlignmentCenter,r.brush);
                x+=TextWidth(g,r.text,size,false,r.brush);
            }
        }
        void PaintEnd(Graphics& g,float cw,float ch,float s)
        {
            auto& t=state.table;
            SolidBrush shade(Color(150,0,0,0));
            g.FillRectangle(&shade,0.0f,0.0f,cw,ch);
            const bool earned=state.earned>=0;
            const float full=std::min(cw-40*s,(earned?840:600)*s),h=std::min(ch-40*s,320*s);
            const RectF box((cw-full)/2,(ch-h)/2,full,h);
            FillRound(g,box,14*s,Color(255,40,22,22),Color(255,200,170,90),2*s);
            float y=box.Y+22*s;
            const Color gold(255,240,206,120),white(255,240,240,236),soft(255,200,190,186);
            // The card a win brought, at the right.
            const float w=earned?full-240*s:full;
            if(earned)
            {
                const float cardW=156*s,cardH=cardW*PreviewH/PreviewW,x=box.GetRight()-236*s;
                Text(g,T(L"カードを手に入れた！",L"A new card!"),RectF(x,box.Y+10*s,226*s,28*s),17*s,gold,true,StringAlignmentCenter,StringAlignmentCenter);
                DrawLarge(g,RectF(x+35*s,box.Y+42*s,cardW,cardH),state.earned);
                Text(g,Fmt(T(L"{0} {1}/{2}枚",L"{0}: {1} of {2}"),{Forts::DeckName(Forts::CollectedDeck,English()),std::to_wstring(state.owned.size()),std::to_wstring(Forts::CollectableCards().size())}),RectF(x,box.Y+42*s+cardH+4*s,226*s,24*s),14*s,soft,false,StringAlignmentCenter,StringAlignmentCenter);
            }
            Text(g,T(L"勝負あり",L"Game over"),RectF(box.X,y,w,44*s),32*s,gold,true,StringAlignmentCenter,StringAlignmentNear,true);
            y+=66*s;
            const int you=t.Board().side[You].score,her=t.Board().side[Her].score,winner=t.Board().winner;
            Text(g,Fmt(T(L"あなた {0}点　{1} {2}点",L"You {0}   {1} {2}"),{std::to_wstring(you),HerName(),std::to_wstring(her)}),RectF(box.X,y,w,40*s),26*s,white,false,StringAlignmentCenter);
            y+=82*s;
            if(winner==You)Runs(g,{{T(L"あなたの勝ち！",L"You win!")}},RectF(box.X,y,w,48*s),32*s,gold);
            else if(winner==Her)Runs(g,{{HerName()},{T(L"の勝ち",L" wins")}},RectF(box.X,y,w,48*s),32*s,gold);
            else Runs(g,{{T(L"引き分け",L"A draw")}},RectF(box.X,y,w,48*s),32*s,gold);
            if(state.unlockedNow)Text(g,Fmt(T(L"{0}のデッキが使えるようになりました！",L"You can now play the {0} deck!"),{Forts::DeckName(Forts::CollectedDeck,English())}),RectF(box.X,box.GetBottom()-76*s,w,28*s),17*s,gold,true,StringAlignmentCenter);
            Text(g,T(L"Enter：もう一度　Esc：閉じる",L"Enter: play again   Esc: close"),RectF(box.X,box.GetBottom()-44*s,w,30*s),17*s,soft,false,StringAlignmentCenter);
        }

        // How the game goes.
        void PaintHelp(Graphics& g,float W,float H,float s)
        {
            SolidBrush shade(Color(150,0,0,0));
            g.FillRectangle(&shade,0.0f,0.0f,W,H);
            const float w=std::min(W-40*s,820*s),h=std::min(H-40*s,690*s);
            const RectF box((W-w)/2,(H-h)/2,w,h);
            FillRound(g,box,14*s,Color(250,40,22,22),Color(255,200,170,90),2*s);
            const Color gold(255,240,206,120),white(255,240,240,236),soft(255,200,190,186);
            Text(g,T(L"三つの砦の遊び方",L"How to play Three Forts"),RectF(box.X,box.Y+12*s,w,36*s),24*s,gold,true,StringAlignmentCenter);
            struct Row {const wchar_t* name,*about;};
            const std::wstring goal=Fmt(T(L"先に{0}点。自分の兵だけがいる砦が、自分の番の初めに点になる",L"First to {0}. A fort with only your soldiers scores at the start of your turn"),{std::to_wstring(Forts::Target)});
            const Row rows[]={
                {T(L"勝ち",L"To win"),goal.c_str()},
                {T(L"砦の点",L"Forts"),T(L"中央2点、左右1点。各砦に自分の兵は3体まで",L"The centre 2, the sides 1. Up to 3 of your soldiers at each")},
                {T(L"魔力",L"Magic"),T(L"毎番、上限が1増えて満タンに（最大8）",L"Each turn its limit rises by 1 and it fills up (at most 8)")},
                {T(L"カード",L"Cards"),T(L"カードをクリックし、光った場所をクリック",L"Click a card, then click where it lights up")},
                {T(L"集める",L"Collecting"),T(L"勝つと魔術院のカードが1枚もらえる。10枚で使える（デッキ選びでCを押すと見られる）",L"Each win brings a Mage College card; at 10 you can play them (C when choosing a deck)")},
                {T(L"魔力に変える",L"For magic"),T(L"番に1回、手札1枚を捨てて魔力を得る（絵の右上の◆の数、1〜3）",L"Once a turn, give up a card for magic (the gems on its picture, 1 to 3)")},
                {T(L"兵",L"Soldiers"),T(L"置いた番は動けない。次から移動（隣の砦）か攻撃のどちらか1回",L"Wait a turn. Then each may move next door or attack, once")},
                {T(L"攻撃",L"Attacks"),T(L"同じ砦の敵に。お互いに攻撃力ぶん体力が減る",L"An enemy at the same fort. Each takes the other's attack")},
                {T(L"守護",L"Guard"),T(L"同じ砦の敵は、まずこの兵を攻撃する",L"Enemies at its fort must attack it first")},
                {T(L"疾駆",L"Swift"),T(L"置いた番から動ける",L"Can act on the turn it comes")},
                {T(L"飛翔",L"Flying"),T(L"守護を無視して攻撃できる",L"Can attack past Guard")},
                {T(L"遠射",L"Ranged"),T(L"攻撃したとき反撃を受けない",L"Takes nothing back when it attacks")},
                {T(L"堅固",L"Tough"),T(L"受けるダメージ-1",L"Takes 1 less damage")},
                {T(L"旗手",L"Banner"),T(L"砦を押さえているとき、その砦の点+1",L"Its fort scores 1 more while you hold it")}};
            float y=box.Y+50*s;
            // The numbers on a card, in their own colours.
            {
                Text(g,T(L"カードの数字",L"The numbers"),RectF(box.X+24*s,y,150*s,30*s),17*s,white,true);
                struct Number {const wchar_t* mark;Color color;MarkShape shape;const wchar_t* about;float width;};
                const Number numbers[]={
                    {L"2",Color(255,40,84,170),MarkShape::Gem,T(L"左上：コスト（使う魔力）",L"Top left: cost (magic)"),200},
                    {L"3",Color(255,168,52,30),MarkShape::Blades,T(L"左下：攻撃力",L"Bottom left: attack"),140},
                    {L"2",Color(255,40,120,70),MarkShape::Shield,T(L"右下：体力（0で倒れる。傷つくと赤）",L"Bottom right: health (0: it falls; red: hurt)"),280}};
                float x=box.X+172*s;
                for(const Number& n:numbers)
                {
                    Mark(g,x,y,24*s,n.mark,n.color,n.shape);
                    Text(g,n.about,RectF(x+28*s,y+1*s,n.width*s,30*s),14*s,soft);
                    x+=(n.width+18)*s;
                }
                y+=40*s;
            }
            for(const Row& r:rows)
            {
                Text(g,r.name,RectF(box.X+24*s,y,150*s,30*s),17*s,white,true);
                Text(g,r.about,RectF(box.X+172*s,y+1*s,w-186*s,30*s),15*s,soft);
                y+=40*s;
            }
            Text(g,T(L"Tab：閉じる",L"Tab: close"),RectF(box.X,box.GetBottom()-30*s,w,26*s),15*s,soft,false,StringAlignmentCenter);
        }

        // Your choice of deck, before a game.
        void PaintChooser(Graphics& g,float W,float H,float s)
        {
            const Color gold(255,240,206,120),white(255,240,240,236),soft(255,214,204,196);
            const float top=std::max(250*s,H*0.34f);
            FillRound(g,RectF(W/2-300*s,top-62*s,600*s,50*s),10*s,Color(215,40,18,16),Color(220,170,120,100),1.2f*s);
            Text(g,T(L"使うデッキを選んでください（1〜3）",L"Choose your deck (1-3)"),RectF(W/2-300*s,top-62*s,600*s,50*s),22*s,gold,true,StringAlignmentCenter,StringAlignmentCenter);
            const wchar_t* const about[]={T(L"守りが堅く、旗手で砦の点を増やす",L"Hard to shift, with banners to raise a fort's points"),
                                          T(L"速さと数で、砦を次々に押さえる",L"Speed and numbers: take fort after fort"),
                                          T(L"呪文で敵を崩し、手札で押し切る",L"Spells to break them, cards to spare")};
            const float bw=std::min(350*s,(W-60*s)/3),bh=224*s,gap=12*s;
            float x=(W-(bw*3+gap*2))/2;
            for(int d=0;d<Forts::DeckKinds;d++,x+=bw+gap)
            {
                const RectF box(x,top,bw,bh);
                state.deckBoxes[d]=box;
                const bool lit=state.hoverDeck==d;
                const Color side=SideColor(d+1);
                FillRound(g,box,12*s,Color(lit?250:225,side.GetR(),side.GetG(),side.GetB()),lit?Color(255,255,226,130):Color(255,200,170,90),(lit?3:1.5f)*s);
                Text(g,Forts::DeckName(d,English()),RectF(x,top+10*s,bw,34*s),24*s,white,true,StringAlignmentCenter);
                Wrapped(g,about[d],RectF(x+12*s,top+46*s,bw-24*s,44*s),15*s,soft);
                // A few of its cards.
                const std::vector<int> deck=Forts::DeckOf(d);
                const float cw=74*s,ch=cw*1.28f;
                const int shown[3]={deck[10],deck[18],deck[28]};
                const bool locked=DeckLocked(d);
                for(int i=0;i<3;i++)
                {
                    const RectF card(x+(bw-(cw*3+8*s*2))/2+i*(cw+8*s),top+100*s,cw,ch);
                    if(!locked)DrawLarge(g,card,shown[i]);
                    else
                    {
                        FillRound(g,card,cw*0.08f,Color(255,28,20,40),Color(255,150,120,190),1.5f*s);
                        Text(g,L"?",card,cw*0.6f,Color(255,150,120,190),true,StringAlignmentCenter,StringAlignmentCenter);
                    }
                }
                // A collected deck says how far it has come.
                if(d==Forts::CollectedDeck)
                {
                    const std::wstring have=std::to_wstring(state.owned.size());
                    const std::wstring about2=locked?Fmt(T(L"勝つと1枚もらえます　あと{0}枚で使えます（{1}/{2}）",L"A card for each win: {0} more to play it ({1}/{2})"),{std::to_wstring(Forts::UnlockAt-static_cast<int>(state.owned.size())),have,std::to_wstring(Forts::UnlockAt)}):
                                                      Fmt(T(L"持っている{0}枚で戦います（全{1}枚）",L"You play the {0} you have (of {1})"),{std::to_wstring(Forts::CollectedDeckCards(state.owned).size()),std::to_wstring(Forts::DeckSize)});
                    FillRound(g,RectF(x,top+bh+6*s,bw,30*s),8*s,Color(215,40,18,16),Color(220,170,120,100),1.2f*s);
                    Text(g,about2,RectF(x,top+bh+6*s,bw,30*s),13*s,locked?soft:gold,false,StringAlignmentCenter,StringAlignmentCenter);
                }
                if(locked)FillRound(g,box,12*s,Color(120,10,8,16));
            }
            // The cards collected.
            state.albumBox=RectF(W/2-130*s,top+bh+46*s,260*s,36*s);
            FillRound(g,state.albumBox,8*s,Color(225,40,18,16),Color(255,200,170,90),1.5f*s);
            Text(g,T(L"集めたカードを見る（C）",L"Cards collected (C)"),state.albumBox,15*s,white,true,StringAlignmentCenter,StringAlignmentCenter);
        }

        // Every card to collect: those you have, how many, and those you haven't.
        void PaintAlbum(Graphics& g,float W,float H,float s)
        {
            SolidBrush shade(Color(170,0,0,0));
            g.FillRectangle(&shade,0.0f,0.0f,W,H);
            const float w=std::min(W-30*s,780*s),h=std::min(H-30*s,620*s);
            const RectF box((W-w)/2,(H-h)/2,w,h);
            FillRound(g,box,14*s,Color(250,40,22,22),Color(255,200,170,90),2*s);
            const Color gold(255,240,206,120),soft(255,200,190,186);
            Text(g,Fmt(T(L"{0}のカード　{1}/{2}枚",L"{0}: {1} of {2}"),{Forts::DeckName(Forts::CollectedDeck,English()),std::to_wstring(state.owned.size()),std::to_wstring(Forts::CollectableCards().size())}),RectF(box.X,box.Y+12*s,w,36*s),24*s,gold,true,StringAlignmentCenter,StringAlignmentCenter);
            std::vector<int> kinds=Forts::CollectableCards();
            kinds.erase(std::unique(kinds.begin(),kinds.end()),kinds.end());
            const int columns=5,rows=(static_cast<int>(kinds.size())+columns-1)/columns;
            const float top=box.Y+56*s,bottom=box.GetBottom()-44*s;
            const float cellH=(bottom-top)/rows,cardH=cellH-8*s,cardW=std::min(cardH/1.28f,(w-30*s)/columns-10*s),rowW=columns*(cardW+10*s);
            for(size_t i=0;i<kinds.size();i++)
            {
                const float x=box.X+(w-rowW)/2+5*s+(i%columns)*(cardW+10*s),y=top+(i/columns)*cellH;
                const RectF card(x,y,cardW,std::min(cardH,cardW*1.28f));
                const auto have=std::count(state.owned.begin(),state.owned.end(),kinds[i]);
                if(have==0)
                {
                    FillRound(g,card,cardW*0.08f,Color(255,28,20,40),Color(255,110,90,140),1.5f*s);
                    Text(g,L"?",card,cardW*0.5f,Color(255,110,90,140),true,StringAlignmentCenter,StringAlignmentCenter);
                    continue;
                }
                DrawLarge(g,card,kinds[i]);
                const float d=cardW*0.3f;
                const RectF tag(card.GetRight()-d*1.15f,card.Y+card.Height*0.5f-d*0.45f,d*1.15f,d*0.9f);
                FillRound(g,tag,d*0.2f,Color(235,20,14,12),Color(255,240,206,120),1.0f*s);
                Text(g,L"×"+std::to_wstring(have),tag,d*0.5f,gold,true,StringAlignmentCenter,StringAlignmentCenter);
            }
            Text(g,T(L"どこかをクリックすると閉じます",L"Click anywhere to close"),RectF(box.X,box.GetBottom()-38*s,w,30*s),15*s,soft,false,StringAlignmentCenter,StringAlignmentCenter);
        }

        void LayButtons(Graphics& g,float W,float H,float s)
        {
            auto& t=state.table;
            std::vector<Button> b;
            if(RedrawNow())b.push_back({Act::Redraw,state.redrawMask?T(L"印のカードを引き直す（Enter）",L"Change the marked cards (Enter)"):T(L"このまま始める（Enter）",L"Keep these (Enter)"),RectF(),true,true});
            else if(!state.choosing&&t.Now()!=Phase::Over)
            {
                const bool yours=YourTurnNow();
                const auto& mine=t.Board().side[You];
                if(mine.spare)b.push_back({Act::Spare,T(L"一度だけの魔力（M）",L"The extra magic (M)"),RectF(),yours});
                if(state.choice.hand>=0&&state.choice.hand<mine.handSize&&!mine.offered)
                    b.push_back({Act::Offer,Fmt(T(L"魔力+{0}に変える（G）",L"Give up for magic +{0} (G)"),{std::to_wstring(Forts::GivesMagic(Forts::CardOf(mine.hand[state.choice.hand])))}),RectF(),yours});
                b.push_back({Act::End,T(L"番を終える（E）",L"End turn (E)"),RectF(),yours,true});
            }
            float y=H-(HandH+16+50)*s;
            float x=W-14*s;
            for(size_t i=b.size();i-->0;)
            {
                Button& k=b[i];
                const float bw=std::max(120*s,TextWidth(g,k.label,16*s,true)+26*s);
                x-=bw;
                k.box=RectF(x,y,bw,42*s);
                x-=10*s;
            }
            for(size_t i=0;i<b.size();++i)
            {
                const Button& k=b[i];
                const bool lit=k.on&&static_cast<int>(i)==state.hover;
                Color fill=!k.on?Color(170,60,50,46):lit?Color(250,250,236,200):Color(240,236,226,196);
                if(k.strong&&k.on)fill=lit?Color(250,226,90,70):Color(245,206,60,50);
                FillRound(g,k.box,10*s,fill,Color(255,90,60,30),1.5f*s);
                const Color ink=!k.on?Color(255,150,140,130):k.strong?Color(255,255,250,240):Color(255,40,30,20);
                Text(g,k.label,k.box,16*s,ink,true,StringAlignmentCenter,StringAlignmentCenter);
            }
            state.buttons=std::move(b);
        }

        // The three forts between you: her soldiers above each, yours below, its name between.
        void PaintBoard(Graphics& g,float W,float H,float s)
        {
            auto& t=state.table;
            const auto& b=t.Board();
            const ULONGLONG now=GetTickCount64();
            const float margin=12*s,gap=10*s,fortW=(W-2*margin-2*gap)/3;
            const float unitH=std::min(118*s,(H-(HandH+84+236+50)*s)/2),unitW=unitH/1.28f;
            const float herRow=H-(HandH+84)*s-unitH*2-48*s,banner=herRow+unitH+6*s,yourRow=banner+36*s;
            const std::vector<Forts::Action> options=Options();
            const Need need=NeedNow(options);
            state.unitBoxes.clear();
            state.lungeLeftOut=-1;
            for(int f=0;f<Forts::FortCount;f++)
            {
                const float x=margin+f*(fortW+gap);
                const RectF column(x,herRow-6*s,fortW,unitH*2+48*s+12*s);
                state.fortBoxes[f]=column;
                const bool youHold=Forts::Holds(b,You,f),herHold=Forts::Holds(b,Her,f);
                const bool lit=YourTurnNow()&&FortLit(f,options,need);
                FillRound(g,column,10*s,Color(lit?150:105,20,14,12),lit?Color(255,255,226,110):Color(170,170,130,100),(lit?3:1.2f)*s);
                // The name, its points, who holds it.
                const RectF name(x+6*s,banner,fortW-12*s,30*s);
                state.bannerBoxes[f]=name;
                const Color flag=youHold?Color(235,40,110,70):herHold?Color(235,150,50,60):Color(225,70,58,46);
                FillRound(g,name,7*s,flag,Color(220,230,200,140),1.2f*s);
                const std::wstring label=Fmt(T(L"{0}（{1}点）",L"{0} ({1})"),{FortName(f),std::to_wstring(Forts::FortValue[f])});
                Text(g,label,name,15*s,Color(255,250,244,226),true,StringAlignmentCenter,StringAlignmentCenter);
                if(state.glanceFort==f&&state.clock<state.glanceUntil)
                    FillRound(g,RectF(name.X,name.Y,name.Width,name.Height),7*s,Color(0,0,0,0),Color(160,255,226,150),2*s);
                // The soldiers.
                for(int who=0;who<2;who++)
                {
                    const Forts::Side& side=b.side[who];
                    const float y=who==Her?herRow:yourRow;
                    const float room=side.count[f]*unitW+(side.count[f]-1)*6*s;
                    for(int i=0;i<side.count[f];i++)
                    {
                        const Forts::Unit& u=side.units[f][i];
                        RectF box(x+(fortW-room)/2+i*(unitW+6*s),y,unitW,unitH);
                        state.unitBoxes[u.uid]=box;
                        // The one striking moves over the board (PaintTop).
                        if(state.lungeUnit==u.uid&&now-state.lungeAt<LungeFor)
                        {
                            state.lungeBox=box;
                            state.lungeLeftOut=u.uid;
                            continue;
                        }
                        int glow=0;
                        if(YourTurnNow()&&Lit(u.uid,options,need))glow=2;
                        else if(YourTurnNow()&&who==You&&state.choice.unit==u.uid)glow=1;
                        else if(YourTurnNow()&&who==You&&u.ready&&state.choice.hand<0&&state.choice.unit<0)
                        {
                            const auto acts=t.Actions();
                            if(std::any_of(acts.begin(),acts.end(),[&](const Forts::Action& a){return a.unit==u.uid;}))glow=1;
                        }
                        DrawCard(g,box,u.card,false,glow);
                        DrawStats(g,box,u.attack+u.rally,u.health,u.health<u.maxHealth,u.rally>0||u.attack>Forts::CardOf(u.card).attack);
                        if(u.frozen)FillRound(g,box,unitW*0.08f,Color(120,120,190,255));
                        if(side.shield[f])FillRound(g,box,unitW*0.08f,Color(0,0,0,0),Color(220,140,200,255),2.5f*s);
                    }
                }
            }
        }

        void PaintHand(Graphics& g,float W,float H,float s)
        {
            auto& t=state.table;
            const auto& side=t.Board().side[You];
            state.handBoxes.clear();
            const int n=side.handSize;
            if(n==0)return;
            const float left=236*s,right=14*s,cw=HandW*s,ch=HandH*s;
            const float step=std::min(cw+8*s,n>1?(W-left-right-cw)/(n-1):cw);
            const float y=H-ch-10*s;
            const auto acts=t.Actions();
            for(int i=0;i<n;i++)
            {
                const int id=side.hand[i];
                const bool playable=std::any_of(acts.begin(),acts.end(),[&](const Forts::Action& a){return a.kind==Forts::ActionKind::Play&&a.hand==i;});
                const bool chosen=state.choice.hand==i,marked=RedrawNow()&&(state.redrawMask&(1u<<i));
                const bool up=chosen||(state.hoverHand==i&&(YourTurnNow()||RedrawNow()));
                const RectF box(left+i*step,y-(up?14*s:0),cw,ch);
                const bool dim=YourTurnNow()&&!playable&&!chosen;
                DrawCard(g,box,id,dim||marked,chosen?1:(YourTurnNow()&&playable)?2:0);
                if(!Forts::CardOf(id).spell)DrawStats(g,box,Forts::CardOf(id).attack,Forts::CardOf(id).health,false,false);
                if(marked)Text(g,T(L"引き直す",L"Change"),box,18*s,Color(255,255,226,130),true,StringAlignmentCenter,StringAlignmentCenter);
                state.handBoxes.push_back({i,box});
            }
        }

        // A soft speck of light (made once for each colour and strength: drawing it is then only a copy).
        Bitmap* GlowOf(Color color,int strength)
        {
            static std::map<std::uint32_t,std::unique_ptr<Bitmap>> made;
            auto& glow=made[(static_cast<std::uint32_t>(color.GetR())<<24)|(color.GetG()<<16)|(color.GetB()<<8)|static_cast<std::uint32_t>(strength)];
            if(!glow)
            {
                constexpr int Side=32;
                glow=std::make_unique<Bitmap>(Side,Side,PixelFormat32bppARGB);
                for(int y=0;y<Side;y++)
                    for(int x=0;x<Side;x++)
                    {
                        const float dx=(x+0.5f)/Side*2-1,dy=(y+0.5f)/Side*2-1,left=std::max(0.0f,1-std::sqrt(dx*dx+dy*dy));
                        // A bright heart, whiter than its colour, in a soft edge.
                        const float heart=std::min(1.0f,left*left*left*left*7);
                        const auto mix=[&](BYTE c){return static_cast<BYTE>(c+(255-c)*heart*0.55f);};
                        glow->SetPixel(x,y,Color(static_cast<BYTE>(255*std::pow(left,1.4f)*strength/8.0f),mix(color.GetR()),mix(color.GetG()),mix(color.GetB())));
                    }
            }
            return glow.get();
        }

        void DrawGlow(Graphics& g,float x,float y,float radius,Color color,float strength)
        {
            const int level=std::clamp(static_cast<int>(strength*8+0.5f),0,8);
            if(level==0||radius<=0)return;
            g.DrawImage(GlowOf(color,level),x-radius,y-radius,radius*2,radius*2);
        }

        // A card's face faded, over box.
        void DrawFaded(Graphics& g,const RectF& box,const RectF& faceSize,int card,float alpha)
        {
            Bitmap* face=CardFace(card,static_cast<int>(std::lround(faceSize.Width)),static_cast<int>(std::lround(faceSize.Height)));
            if(!face)return;
            ColorMatrix fade={{{1,0,0,0,0},{0,1,0,0,0},{0,0,1,0,0},{0,0,0,alpha,0},{0,0,0,0,1}}};
            ImageAttributes attributes;
            attributes.SetColorMatrix(&fade);
            g.DrawImage(face,box,0,0,static_cast<REAL>(face->GetWidth()),static_cast<REAL>(face->GetHeight()),UnitPixel,&attributes);
        }

        // A frame of light, spreading from a box.
        void DrawFrame(Graphics& g,const RectF& at,float grow,Color color,float strength,float s)
        {
            const RectF box(at.X-grow,at.Y-grow,at.Width+2*grow,at.Height+2*grow);
            const auto a=[&](float k){return static_cast<BYTE>(std::clamp(255*strength*k,0.0f,255.0f));};
            const float r=std::min(at.Width,at.Height)*0.08f+grow;
            FillRound(g,box,r,Color(0,0,0,0),Color(a(0.3f),color.GetR(),color.GetG(),color.GetB()),7*s);
            FillRound(g,box,r,Color(0,0,0,0),Color(a(1),color.GetR(),color.GetG(),color.GetB()),2.2f*s);
        }

        // A cut across a card: two long thin blades of light.
        void DrawSlash(Graphics& g,const RectF& at,float u,float s)
        {
            const PointF c(at.X+at.Width/2,at.Y+at.Height/2);
            const float reach=std::max(at.Width,at.Height)*0.72f*std::min(1.0f,u*3.5f);
            const float strength=u<0.35f?1.0f:std::max(0.0f,1-(u-0.35f)/0.65f);
            const float thick=11*s*strength;
            const BYTE a=static_cast<BYTE>(255*strength);
            SolidBrush core(Color(a,255,250,232)),edge(Color(static_cast<BYTE>(a*0.75f),255,176,70));
            for(int k=0;k<2;k++)
            {
                const float angle=(k==0?-0.86f:-0.5f),dx=std::cos(angle),dy=std::sin(angle),nx=-dy,ny=dx,off=(k==0?-1.0f:1.0f)*at.Width*0.12f;
                const float x=c.X+nx*off,y=c.Y+ny*off;
                const PointF wide[4]={PointF(x-dx*reach,y-dy*reach),PointF(x+nx*thick*1.5f,y+ny*thick*1.5f),PointF(x+dx*reach,y+dy*reach),PointF(x-nx*thick*1.5f,y-ny*thick*1.5f)};
                const PointF thin[4]={PointF(x-dx*reach,y-dy*reach),PointF(x+nx*thick*0.55f,y+ny*thick*0.55f),PointF(x+dx*reach,y+dy*reach),PointF(x-nx*thick*0.55f,y-ny*thick*0.55f)};
                g.FillPolygon(&edge,wide,4);
                g.FillPolygon(&core,thin,4);
            }
            DrawGlow(g,c.X,c.Y,at.Width*0.65f*strength,Color(255,255,214,130),strength*0.8f);
        }

        // Whose turn: a band of light across the view, its ends fading.
        void PaintTurnBand(Graphics& g,float W,float H,float s)
        {
            const float t=static_cast<float>(GetTickCount64()-state.saidAt);
            const float a=std::clamp(std::min(t/170.0f,(static_cast<float>(SaidShown)-t)/260.0f),0.0f,1.0f);
            if(a<=0)return;
            const bool yours=state.table.Board().who==You;
            const Color tint=yours?Color(255,24,84,70):Color(255,110,30,44);
            const float bh=58*s,y=H*0.24f;
            const RectF band(0,y,W,bh);
            const auto ramp=[&](BYTE alpha,const RectF& box,Color colour)
            {
                LinearGradientBrush brush(RectF(box.X-1,box.Y-1,box.Width+2,box.Height+2),colour,colour,LinearGradientModeHorizontal);
                const BYTE most=static_cast<BYTE>(alpha*a);
                const Color colours[4]={Color(0,colour.GetR(),colour.GetG(),colour.GetB()),Color(most,colour.GetR(),colour.GetG(),colour.GetB()),Color(most,colour.GetR(),colour.GetG(),colour.GetB()),Color(0,colour.GetR(),colour.GetG(),colour.GetB())};
                const REAL at[4]={0,0.3f,0.7f,1};
                brush.SetInterpolationColors(colours,at,4);
                g.FillRectangle(&brush,box);
            };
            ramp(225,band,tint);
            ramp(255,RectF(0,y-1.5f*s,W,2*s),Color(255,240,206,120));
            ramp(255,RectF(0,y+bh-0.5f*s,W,2*s),Color(255,240,206,120));
            const float rise=1-std::min(1.0f,t/260.0f);
            const RectF place(rise*rise*40*s,y,W,bh);
            Text(g,state.said,RectF(place.X+1.5f*s,place.Y+1.5f*s,W,bh),30*s,Color(static_cast<BYTE>(170*a),0,0,0),true,StringAlignmentCenter,StringAlignmentCenter,true);
            Text(g,state.said,place,30*s,Color(static_cast<BYTE>(255*a),255,240,200),true,StringAlignmentCenter,StringAlignmentCenter,true);
        }

        // What moves over the rest: soldiers just come or striking, those fallen, shots, specks of
        // light, numbers rising, whose turn, her words.
        void PaintTop(Graphics& g,float W,float H,float s,bool hud)
        {
            const ULONGLONG now=GetTickCount64();
            g.SetInterpolationMode(InterpolationModeBilinear);
            // Soldiers just come: the card coming down onto its place, and light from it.
            for(int uid:state.burstDue)
                if(const auto k=state.unitBoxes.find(uid);k!=state.unitBoxes.end())
                {
                    const PointF at=Middle(k->second);
                    Burst(at.X,at.Y,22,Color(255,255,226,150),210*s,5*s,520,60*s,0,k->second.Width*0.25f);
                }
            state.burstDue.clear();
            for(const auto& [uid,since]:state.fresh)
                if(const auto k=state.unitBoxes.find(uid);k!=state.unitBoxes.end()&&now-since<500)
                {
                    const float u=(now-since)/500.0f;
                    const RectF& box=k->second;
                    if(const Forts::Unit* unit=Forts::FindUnit(state.table.Board(),uid);unit&&u<0.5f)
                    {
                        const float big=(1-u*2)*(1-u*2)*0.32f;
                        DrawFaded(g,RectF(box.X-box.Width*big/2,box.Y-box.Height*big/2,box.Width*(1+big),box.Height*(1+big)),box,unit->card,1-u*2);
                    }
                    DrawFrame(g,box,(1-(1-u)*(1-u))*18*s,Color(255,255,232,160),(1-u)*0.9f,s);
                }
            // The one striking, leaning and back.
            if(state.lungeLeftOut>=0)
                if(const Forts::Unit* u=Forts::FindUnit(state.table.Board(),state.lungeLeftOut))
                {
                    const float k=now-state.lungeAt<LungeFor?std::sin(static_cast<float>(now-state.lungeAt)/LungeFor*3.14159f):0;
                    RectF box=state.lungeBox;
                    box.X+=state.lungeX*k;
                    box.Y+=state.lungeY*k;
                    DrawCard(g,box,u->card);
                    DrawStats(g,box,u->attack+u->rally,u->health,u->health<u->maxHealth,u->rally>0||u->attack>Forts::CardOf(u->card).attack);
                }
            for(const Ghost& k:state.ghosts)
            {
                const float u=now>k.start?static_cast<float>(now-k.start)/700.0f:0;
                if(u>=1)continue;
                const float sink=u*u*20*s;
                const float lean=k.leanAt&&now-k.leanAt<LungeFor?std::sin(static_cast<float>(now-k.leanAt)/LungeFor*3.14159f):0;
                DrawFaded(g,RectF(k.at.X+k.leanX*lean,k.at.Y+sink+k.leanY*lean,k.at.Width,k.at.Height),k.at,k.card,std::clamp(1-u*1.15f,0.0f,1.0f));
            }
            for(const Bolt& b:state.bolts)
            {
                if(now<b.start)continue;
                const float u=std::min(1.0f,static_cast<float>(now-b.start)/b.life);
                for(int k=9;k>=0;k--)
                {
                    const float back=u-k*0.045f;
                    if(back<0)continue;
                    const float eased=back*back*(3-2*back);
                    const float x=b.from.X+(b.to.X-b.from.X)*eased,y=b.from.Y+(b.to.Y-b.from.Y)*eased;
                    DrawGlow(g,x,y,b.size*(1-k*0.08f)*(k==0?1.5f:1.0f),b.color,1-k*0.09f);
                }
            }
            for(const Spark& p:state.sparks)
            {
                if(now<p.start)continue;
                const float t=(now-p.start)/1000.0f,u=(now-p.start)/static_cast<float>(p.life);
                if(u>=1)continue;
                const float slow=1-0.35f*u;   // (the air holds them back)
                DrawGlow(g,p.x+p.vx*t*slow,p.y+p.vy*t*slow+0.5f*p.fall*t*t,p.size*(1-0.45f*u),p.color,std::min(1.0f,(1-u)*1.6f));
            }
            for(const Fx& f:state.fx)
            {
                if(f.at.Width<=0||now<f.start)continue;
                const float u=static_cast<float>(now-f.start)/static_cast<float>(f.life);
                if(u>=1)continue;
                if(f.slash)
                {
                    DrawSlash(g,f.at,std::min(1.0f,u*f.life/320.0f),s);
                    continue;
                }
                if(f.flash&&u<0.22f)FillRound(g,f.at,f.at.Width*0.08f,Color(static_cast<BYTE>(120*(1-u/0.22f)),f.color.GetR(),f.color.GetG(),f.color.GetB()));
                if(f.frame)
                {
                    const float w=std::min(1.0f,u*2.2f);
                    DrawFrame(g,f.at,(1-(1-w)*(1-w))*16*s,f.color,(1-w)*0.95f,s);
                }
                if(f.text.empty())continue;
                // It leaps out a little, then rises.
                const float pop=u<0.12f?1.45f-0.45f*(u/0.12f):1.0f;
                const float size=(f.text.size()>2?19:30)*s*pop,rise=(1-(1-u)*(1-u))*42*s;
                const RectF place(f.at.X-40*s,f.at.Y+f.at.Height/2-size*0.8f-rise,f.at.Width+80*s,size*1.5f);
                const BYTE a=static_cast<BYTE>(255*std::min(1.0f,(1-u)*2));
                for(int k=0;k<4;k++)
                    Text(g,f.text,RectF(place.X+(k<2?-1.5f:1.5f)*s,place.Y+(k%2?-1.5f:1.5f)*s,place.Width,place.Height),size,Color(a,0,0,0),true,StringAlignmentCenter,StringAlignmentCenter);
                Text(g,f.text,place,size,Color(a,f.color.GetR(),f.color.GetG(),f.color.GetB()),true,StringAlignmentCenter,StringAlignmentCenter);
            }
            if(state.help||state.confirmQuit||EndShown())return;
            if(!state.said.empty())PaintTurnBand(g,W,H,s);
            // Her words: a bubble just right of her head (as at mahjong).
            if(!state.words.text.empty())
            {
                const std::wstring& said=state.words.text;
                const float most=std::max(160*s,W*0.4f);
                const WordsBubble bubble(g,said,s,most);
                const float bw=bubble.Width(),bh=bubble.Height();
                const bool beside=hud&&state.besideHead.x>=0;
                const float x=beside?std::clamp(static_cast<float>(state.besideHead.x),14*s,W-14*s-bw):W-14*s-bw;
                const float y=beside?std::clamp(static_cast<float>(state.headScreen.y)-bh*0.6f,12*s,std::max(12*s,H*0.3f-bh)):120*s;
                const float ty=beside?std::clamp(static_cast<float>(state.headScreen.y),y+12*s,y+bh-12*s):y+std::min(bh*0.5f,30*s);
                const std::wstring shown=said.substr(0,std::min(said.size(),std::max<size_t>(1,static_cast<size_t>(state.words.shown))));
                bubble.Draw(g,x,y,ty,shown);
            }
        }

        void Paint(HDC dc,const RECT& dirty)
        {
            struct Timing
            {
                LARGE_INTEGER from{};
                Timing(){QueryPerformanceCounter(&from);}
                ~Timing()
                {
                    LARGE_INTEGER to{},rate{};
                    QueryPerformanceCounter(&to);
                    QueryPerformanceFrequency(&rate);
                    state.paintSeconds+=static_cast<double>(to.QuadPart-from.QuadPart)/rate.QuadPart;
                    state.paints++;
                }
            } timing;
            RECT client{};
            GetClientRect(state.window,&client);
            const int cw=std::max(1L,client.right),ch=std::max(1L,client.bottom);
            if(!state.canvas||static_cast<int>(state.canvas->GetWidth())!=cw||static_cast<int>(state.canvas->GetHeight())!=ch)
                state.canvas=std::make_unique<Bitmap>(cw,ch,PixelFormat32bppPARGB);
            if(!state.base||static_cast<int>(state.base->GetWidth())!=cw||static_cast<int>(state.base->GetHeight())!=ch)
            {
                state.base=std::make_unique<Bitmap>(cw,ch,PixelFormat32bppPARGB);
                state.baseDue=true;
            }
            Bitmap& canvas=*state.canvas;
            const bool hud=state.renderer!=nullptr;
            if(state.baseDue)
            {
            state.baseDue=false;
            state.basePaints++;
            Graphics g(state.base.get());
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(hud?TextRenderingHintAntiAlias:TextRenderingHintAntiAliasGridFit);
            g.Clear(hud?Color(0,0,0,0):Color(255,44,34,30));
            const float s=Scale(),W=static_cast<float>(cw),H=static_cast<float>(ch),mid=W/2;
            auto& t=state.table;
            const Color white(255,240,240,236),soft(255,206,196,186),gold(255,240,206,120),panel(185,40,18,16),edge(220,170,120,100);
            // Her: name, score, her magic, how many cards she holds.
            const float nameX=hud?28*s:112*s,nameY=hud?20*s:14*s;
            if(hud)FillRound(g,RectF(14*s,12*s,270*s,92*s),10*s,panel,edge,1.2f*s);
            else
            {
                const RectF faceBox(16*s,14*s,84*s,84*s);
                FillRound(g,faceBox,10*s,Color(255,14,50,36),Color(255,120,160,130),1.5f*s);
                if(state.face)g.DrawImage(state.face.get(),faceBox.X+3*s,faceBox.Y+3*s,faceBox.Width-6*s,faceBox.Height-6*s);
                else Text(g,HerName().substr(0,1),faceBox,40*s,white,true,StringAlignmentCenter,StringAlignmentCenter);
            }
            Text(g,HerName(),RectF(nameX,nameY,240*s,28*s),20*s,white,true);
            if(!state.choosing)
            {
                const auto& her=t.Board().side[Her];
                Text(g,Fmt(T(L"{0}点",L"{0} points"),{std::to_wstring(her.score)}),RectF(nameX,nameY+28*s,120*s,24*s),18*s,gold,true);
                state.scoreSpot[Her]=PointF(nameX+18*s,nameY+40*s);
                state.scoreSpot[You]=PointF(50*s,H-124*s);
                Text(g,Fmt(T(L"魔力 {0}/{1}",L"Magic {0}/{1}"),{std::to_wstring(her.magic),std::to_wstring(her.maxMagic)}),RectF(nameX+110*s,nameY+29*s,130*s,24*s),15*s,soft);
                Text(g,Fmt(T(L"手札 {0}　山札 {1}　{2}",L"Hand {0}  Deck {1}  {2}"),{std::to_wstring(her.handSize),std::to_wstring(her.deckLeft),Forts::DeckName(t.Deck(Her),English())}),RectF(nameX,nameY+54*s,250*s,24*s),14*s,soft);
            }
            // The game, and how to play (at the top right).
            {
                const RectF info(W-254*s,12*s,240*s,64*s);
                FillRound(g,info,10*s,Color(hud?215:200,40,18,16),edge,1.2f*s);
                Text(g,T(L"三つの砦",L"Three Forts"),RectF(info.X+12*s,info.Y+6*s,228*s,26*s),18*s,white,true);
                const std::wstring goal=state.choosing?Fmt(T(L"{0}点先取",L"First to {0}"),{std::to_wstring(Forts::Target)}):
                    Fmt(T(L"{0}点先取　{1}手目",L"First to {0}   turn {1}"),{std::to_wstring(Forts::Target),std::to_wstring(t.Board().turns+1)});
                Text(g,goal,RectF(info.X+12*s,info.Y+34*s,224*s,24*s),15*s,soft);
                state.helpBox=RectF(W-254*s,82*s,240*s,30*s);
                FillRound(g,state.helpBox,8*s,Color(hud?215:200,40,18,16),edge,1.2f*s);
                Text(g,T(L"遊び方（Tab）",L"How to play (Tab)"),state.helpBox,14*s,soft,false,StringAlignmentCenter,StringAlignmentCenter);
            }
            if(state.choosing)
            {
                state.handBoxes.clear();
                state.unitBoxes.clear();
                state.buttons.clear();
                PaintChooser(g,W,H,s);
            }
            else
            {
                PaintBoard(g,W,H,s);
                // You: score, magic, what is left.
                {
                    const auto& me=t.Board().side[You];
                    const float boxW=214*s,boxH=HandH*s,boxY=H-boxH-10*s;
                    FillRound(g,RectF(12*s,boxY,boxW,boxH),10*s,panel,edge,1.2f*s);
                    Text(g,T(L"あなた",L"You"),RectF(26*s,boxY+8*s,boxW,26*s),18*s,white,true);
                    Text(g,Fmt(T(L"{0}点",L"{0} points"),{std::to_wstring(me.score)}),RectF(26*s,boxY+36*s,boxW,30*s),24*s,gold,true);
                    Text(g,Fmt(T(L"魔力 {0}/{1}",L"Magic {0}/{1}"),{std::to_wstring(me.magic),std::to_wstring(me.maxMagic)}),RectF(26*s,boxY+74*s,boxW,28*s),20*s,Color(255,150,200,255),true);
                    Text(g,Fmt(T(L"山札 {0}",L"Deck {0}"),{std::to_wstring(me.deckLeft)}),RectF(26*s,boxY+106*s,boxW,24*s),15*s,soft);
                    Text(g,Forts::DeckName(t.Deck(You),English()),RectF(26*s,boxY+130*s,boxW,24*s),15*s,soft);
                }
                // What to do now.
                {
                    std::wstring prompt;
                    const std::vector<Forts::Action> options=Options();
                    const Need need=NeedNow(options);
                    if(RedrawNow())prompt=T(L"最初の手札です。引き直すカードをクリックしてください",L"Your first hand. Click any cards you want to change");
                    else if(YourTurnNow())
                    {
                        if(need==Need::Fort)prompt=Fmt(T(L"{0}：砦を選びます",L"{0}: choose a fort"),{CardName(t.Board().side[You].hand[state.choice.hand])});
                        else if(need==Need::Target)prompt=Fmt(T(L"{0}：相手を選びます",L"{0}: choose a soldier"),{CardName(t.Board().side[You].hand[state.choice.hand])});
                        else if(need==Need::Dest)prompt=T(L"行き先の砦を選びます",L"Choose the fort it goes to");
                        else if(need==Need::UnitAct)prompt=T(L"攻撃する相手か、移動する砦を選びます",L"Choose an enemy to attack, or a fort to move to");
                        else prompt=T(L"カードか、光っている兵をクリックします",L"Click a card, or a soldier that is lit");
                    }
                    if(!prompt.empty())
                    {
                        const float tw=TextWidth(g,prompt,16*s)+24*s,y=H-(HandH+16+48)*s;
                        FillRound(g,RectF(236*s,y,tw,34*s),10*s,panel,edge,1.2f*s);
                        Text(g,prompt,RectF(236*s,y,tw,34*s),16*s,gold,false,StringAlignmentCenter,StringAlignmentCenter);
                    }
                }
                PaintHand(g,W,H,s);
                LayButtons(g,W,H,s);
                // A card shown large: the one she has just played, or the one under the mouse.
                {
                    int large=-1;
                    const Forts::Unit* standing=nullptr;
                    if(state.hoverHand>=0&&state.hoverHand<t.Board().side[You].handSize)large=t.Board().side[You].hand[state.hoverHand];
                    else if(state.hoverUnit>=0)
                    {
                        standing=Forts::FindUnit(t.Board(),state.hoverUnit);
                        if(standing)large=standing->card;
                    }
                    else if(state.shownCard>=0&&GetTickCount64()<state.shownUntil)large=state.shownCard;
                    if(large>=0)
                    {
                        const float pw=PreviewW*s,ph=PreviewH*s;
                        DrawLarge(g,RectF(W-pw-14*s,std::max(118*s,(H-HandH*s)/2-ph/2-30*s),pw,ph),large,standing);
                    }
                }
            }
            if(EndShown())PaintEnd(g,W,H,s);
            if(state.album)PaintAlbum(g,W,H,s);
            if(state.help)PaintHelp(g,W,H,s);
            if(state.confirmQuit)
            {
                SolidBrush shade(Color(160,0,0,0));
                g.FillRectangle(&shade,0.0f,0.0f,W,H);
                const RectF box(mid-220*s,ch/2.0f-70*s,440*s,140*s);
                FillRound(g,box,14*s,Color(250,40,22,22),Color(255,200,170,90),2*s);
                Text(g,T(L"ジンラミーをやめますか？",L"Stop playing?"),RectF(box.X,box.Y+22*s,box.Width,36*s),24*s,white,true,StringAlignmentCenter);
                const float gap=20*s,width=(box.Width-3*gap)/2;
                const std::wstring labels[2]={T(L"やめる（Y）",L"Stop (Y)"),T(L"続ける（N）",L"Keep playing (N)")};
                for(int i=0;i<2;++i)
                {
                    const RectF button(box.X+gap+i*(width+gap),box.Y+74*s,width,44*s);
                    state.quitButtons[i]=button;
                    FillRound(g,button,10*s,i==0?Color(240,150,46,52):Color(240,46,92,150),Color(255,200,170,90),1.5f*s);
                    Text(g,labels[i],RectF(button.X,button.Y+11*s,button.Width,26*s),17*s,white,true,StringAlignmentCenter);
                }
            }
            }
            // What stays still, copied as it is, and what moves over it.
            {
                BitmapData from{},to{};
                Rect whole(0,0,cw,ch);
                if(state.base->LockBits(&whole,ImageLockModeRead,PixelFormat32bppPARGB,&from)==Ok)
                {
                    if(canvas.LockBits(&whole,ImageLockModeWrite,PixelFormat32bppPARGB,&to)==Ok)
                    {
                        for(int y=0;y<ch;++y)
                            memcpy(static_cast<BYTE*>(to.Scan0)+static_cast<size_t>(y)*to.Stride,static_cast<const BYTE*>(from.Scan0)+static_cast<size_t>(y)*from.Stride,static_cast<size_t>(cw)*4);
                        canvas.UnlockBits(&to);
                    }
                    state.base->UnlockBits(&from);
                }
                Graphics g(&canvas);
                g.SetSmoothingMode(SmoothingModeAntiAlias);
                g.SetTextRenderingHint(hud?TextRenderingHintAntiAlias:TextRenderingHintAntiAliasGridFit);
                PaintTop(g,static_cast<float>(cw),static_cast<float>(ch),Scale(),hud);
            }
            if(hud)
            {
                ImageRgba8 picture;
                picture.levels.resize(1);
                auto& level=picture.levels[0];
                level.width=static_cast<unsigned>(cw);level.height=static_cast<unsigned>(ch);
                level.pixels.resize(static_cast<size_t>(cw)*ch*4);
                BitmapData bits{};
                Rect whole(0,0,cw,ch);
                if(canvas.LockBits(&whole,ImageLockModeRead,PixelFormat32bppPARGB,&bits)==Ok)
                {
                    for(int y=0;y<ch;++y)
                    {
                        const BYTE* from=static_cast<const BYTE*>(bits.Scan0)+static_cast<size_t>(y)*bits.Stride;
                        BYTE* to=level.pixels.data()+static_cast<size_t>(y)*cw*4;
                        for(int x=0;x<cw;++x,from+=4,to+=4){to[0]=from[2];to[1]=from[1];to[2]=from[0];to[3]=from[3];}
                    }
                    canvas.UnlockBits(&bits);
                }
                state.renderer->SetOverlay(picture);
                return;
            }
            Graphics screen(dc);
            screen.DrawImage(&canvas,static_cast<INT>(dirty.left),static_cast<INT>(dirty.top),static_cast<INT>(dirty.left),static_cast<INT>(dirty.top),
                             static_cast<INT>(dirty.right-dirty.left),static_cast<INT>(dirty.bottom-dirty.top),UnitPixel);
        }

        int ButtonAt(float x,float y)
        {
            for(size_t i=0;i<state.buttons.size();++i)if(state.buttons[i].box.Contains(x,y))return static_cast<int>(i);
            return -1;
        }

        int HandAt(float x,float y)
        {
            int found=-1;
            for(const auto& [i,box]:state.handBoxes)if(box.Contains(x,y))found=i;   // (the later ones lie on top)
            return found;
        }

        int UnitAt(float x,float y)
        {
            for(const auto& [uid,box]:state.unitBoxes)if(box.Contains(x,y))return uid;
            return -1;
        }

        int FortAt(float x,float y)
        {
            for(int f=0;f<Forts::FortCount;f++)if(state.fortBoxes[f].Contains(x,y))return f;
            return -1;
        }

        // A click on the table during your turn.
        void ClickTable(float x,float y)
        {
            auto& t=state.table;
            const std::vector<Forts::Action> options=Options();
            const Need need=NeedNow(options);
            const int uid=UnitAt(x,y),place=HandAt(x,y),fort=FortAt(x,y);
            // What the choice so far is waiting for.
            if(need==Need::Target&&uid>=0&&Lit(uid,options,need))
            {
                state.choice.target=uid;
                Advance();
                return;
            }
            if(need==Need::UnitAct&&uid>=0)
                for(const auto& a:options)
                    if(a.kind==Forts::ActionKind::Attack&&a.target==uid){DoAction(a);return;}
            if(need==Need::Fort&&fort>=0&&FortLit(fort,options,need))
            {
                state.choice.fort=fort;
                Advance();
                return;
            }
            if((need==Need::Dest||need==Need::UnitAct)&&fort>=0&&FortLit(fort,options,need))
                for(const auto& a:options)
                    if(a.dest==fort&&(need==Need::Dest||a.kind==Forts::ActionKind::Move)){DoAction(a);return;}
            // A new choice.
            ClearChoice();
            if(place>=0)
            {
                state.choice.hand=place;
                const std::vector<Forts::Action> now=Options();
                if(now.size()==1&&now[0].fort<0&&now[0].target<0&&now[0].dest<0)DoAction(now[0]);
                else if(now.empty())
                {
                    // Too dear: it can still be given up for magic.
                    const auto acts=t.Actions();
                    if(std::none_of(acts.begin(),acts.end(),[&](const Forts::Action& a){return a.kind==Forts::ActionKind::Offer&&a.hand==place;}))ClearChoice();
                }
                return;
            }
            if(uid>=0)
            {
                const auto acts=t.Actions();
                if(std::any_of(acts.begin(),acts.end(),[&](const Forts::Action& a){return a.unit==uid;}))state.choice.unit=uid;
            }
        }

        void OnKey(WPARAM key)
        {
            auto& t=state.table;
            if(key==VK_F1){state.camera={};return;}
            if(state.confirmQuit)
            {
                if(key=='Y')Finish();
                else if(key=='N'||key==VK_ESCAPE){state.confirmQuit=false;Redraw();}
                return;
            }
            if(state.album){state.album=false;Redraw();return;}
            if(key==VK_TAB){state.help=!state.help;Redraw();return;}
            if(state.help){if(key==VK_ESCAPE){state.help=false;Redraw();}return;}
            if(state.choosing)
            {
                if(key=='C'){state.album=true;Redraw();return;}
                if(key>='1'&&key<'1'+Forts::DeckKinds)StartGame(static_cast<int>(key-'1'));
                else if(key==VK_ESCAPE)Finish();
                return;
            }
            if(t.Now()==Phase::Over)
            {
                if((key==VK_RETURN||key==VK_SPACE)&&EndShown())Continue();
                else if(key==VK_ESCAPE)Finish();
                return;
            }
            if(key==VK_ESCAPE)
            {
                if(state.choice.hand>=0||state.choice.unit>=0)ClearChoice();
                else state.confirmQuit=true;
                Redraw();
                return;
            }
            if(RedrawNow()){if(key==VK_RETURN||key==VK_SPACE)Do(Act::Redraw);return;}
            if(key=='E'||key==VK_RETURN)Do(Act::End);
            else if(key=='M')Do(Act::Spare);
            else if(key=='G')Do(Act::Offer);
        }

        LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            switch(message)
            {
            case WM_MOUSEWHEEL:
                if(state.view)
                {
                    POINT at{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
                    RECT r{};
                    GetWindowRect(state.view,&r);
                    if(PtInRect(&r,at))return ViewProc(state.view,message,wParam,lParam);
                }
                break;
            case WM_KEYDOWN:
                if(!(lParam&(1<<30)))OnKey(wParam);
                return 0;
            case WM_TIMER:
                Tick();
                return 0;
            case WM_MOUSEMOVE:
            {
                const float x=static_cast<float>(GET_X_LPARAM(lParam)),y=static_cast<float>(GET_Y_LPARAM(lParam));
                const int at=ButtonAt(x,y);
                if(at!=state.hover){state.hover=at;Redraw();}
                const int hand=HandAt(x,y),unit=hand>=0?-1:UnitAt(x,y);
                int deck=-1;
                if(state.choosing)
                    for(int d=0;d<Forts::DeckKinds;d++)if(state.deckBoxes[d].Contains(x,y))deck=d;
                if(hand!=state.hoverHand||unit!=state.hoverUnit||deck!=state.hoverDeck)
                {
                    state.hoverHand=hand;
                    state.hoverUnit=unit;
                    state.hoverDeck=deck;
                    Redraw();
                }
                return 0;
            }
            case WM_LBUTTONDOWN:
            {
                const float x=static_cast<float>(GET_X_LPARAM(lParam)),y=static_cast<float>(GET_Y_LPARAM(lParam));
                auto& t=state.table;
                if(state.confirmQuit)
                {
                    if(state.quitButtons[0].Contains(x,y))Finish();
                    else if(state.quitButtons[1].Contains(x,y)){state.confirmQuit=false;Redraw();}
                    return 0;
                }
                if(state.album){state.album=false;Redraw();return 0;}
                if(state.help){state.help=false;Redraw();return 0;}
                if(state.helpBox.Contains(x,y)){state.help=true;Redraw();return 0;}
                if(state.choosing)
                {
                    if(state.albumBox.Contains(x,y)){state.album=true;Redraw();return 0;}
                    for(int d=0;d<Forts::DeckKinds;d++)if(state.deckBoxes[d].Contains(x,y)){StartGame(d);break;}
                    return 0;
                }
                if(t.Now()==Phase::Over){if(EndShown())Continue();return 0;}
                if(const int b=ButtonAt(x,y);b>=0&&state.buttons[static_cast<size_t>(b)].on)
                {
                    const Button k=state.buttons[static_cast<size_t>(b)];
                    Do(k.act);
                    return 0;
                }
                if(RedrawNow())
                {
                    if(const int place=HandAt(x,y);place>=0)state.redrawMask^=1u<<place;
                    Redraw();
                    return 0;
                }
                if(YourTurnNow()){ClickTable(x,y);Redraw();}
                return 0;
            }
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC dc=BeginPaint(hwnd,&ps);
                if(state.renderer)state.overlayDue=true;
                else Paint(dc,ps.rcPaint);
                EndPaint(hwnd,&ps);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_SIZE:
                state.paints=state.basePaints=state.frames=state.reaches=0;
                state.paintSeconds=0;
                state.framesSince=state.clock;
                PlaceView();
                InvalidateRect(hwnd,nullptr,FALSE);
                return 0;
            case WM_CLOSE:
                Finish();
                return 0;
            case WM_DESTROY:
                StopMahjongSounds();
                state.window=nullptr;
                ReleaseView();
                state.face.reset();
                state.canvas.reset();
                state.base.reset();
                return 0;
            }
            return DefWindowProcW(hwnd,message,wParam,lParam);
        }
    }

    bool FortsWindowOpen(){return state.window!=nullptr;}
    HWND FortsWindowHandle(){return state.window;}
    bool FortsWindowRect(RECT& r)
    {
        return state.window&&IsWindowVisible(state.window)&&!IsIconic(state.window)&&GetWindowRect(state.window,&r);
    }

    namespace
    {
        // Diagnostics: actions played for both (yours with her own judgment).
        void PlaySimply(int moves)
        {
            auto& t=state.table;
            if(state.choosing)StartGame(0);
            if(t.Now()==Phase::Redraw)t.Redraw(0);
            for(int i=0;i<moves&&t.Now()!=Phase::Over;++i)
            {
                if(t.Now()==Phase::Hers)t.HerStep();
                else t.PlayForYou();
                t.TakeEvents();
            }
            state.nextAt=0;
            state.hover=-1;
        }
    }

    void FortsLines(const std::wstring& answer)
    {
        state.prefetching=false;
        if(!state.window)return;
        state.lines.Answered(answer);
        Prefetch();
    }

    bool FortsAfterword(std::wstring& situation,std::wstring& fallback)
    {
        if(state.afterword.empty())return false;
        situation=std::move(state.afterword);
        fallback=std::move(state.afterwordLine);
        state.afterword.clear();
        state.afterwordLine.clear();
        return true;
    }

    void FortsHeard(const std::wstring& line,bool done)
    {
        auto& w=state.words;
        if(!w.waiting||!state.window)return;
        if(done&&line.empty()){SayWords(w.fallback,w.weight);return;}
        if(!line.empty())
        {
            w.text=line;
            w.until=GetTickCount64()+static_cast<ULONGLONG>(CharSeconds()*1000*line.size())+2600+90*static_cast<ULONGLONG>(line.size());
            Redraw();
        }
        if(done)
        {
            w.waiting=false;
            if(w.recent.size()>=4)w.recent.erase(w.recent.begin());
            w.recent.push_back(line);
            w.told+=L"  she says (LLM): "+line+L"\n";
        }
    }

    void PlayFortsMoves(int moves,float yaw)
    {
        if(!state.window)return;
        PlaySimply(moves);
        state.camera.yaw+=yaw;
        Redraw();
    }
    namespace
    {
        // Her light pat from above toward a spot on the table (as at mahjong).
        bool ReachFor(MascotModel& model,const std::array<float,3>& spot,bool tap=true)
        {
            std::array<float,3> head{};
            if(!model.HeadPosition(head))return false;
            const float hx=head[0]-model.RootOffset()+state.herX,hz=head[2]+state.herZ;
            const std::array<float,3> from{spot[0]-hx,spot[1]+0.07f*state.hips-head[1],spot[2]-hz};
            return model.Reach(spot[0]>0.25f*state.upper?0:1,from,tap?1.65:3.0,tap);
        }

        void SetTableSolids(MascotModel& model,DirectX::FXMMATRIX world)
        {
            using namespace DirectX;
            const auto t=Table3D();
            const float floor=FloorY();
            std::vector<std::array<float,6>> boxes{
                {t.x0,t.top-0.05f*state.upper,t.zFar,t.x1,t.top,t.zNear},
                {t.x0-t.rw,t.frameBottom,t.zFar-t.rw,t.x1+t.rw,t.frameTop,t.zFar},
                {t.x0-t.rw,t.frameBottom,t.zNear,t.x1+t.rw,t.frameTop,t.zNear+t.rw},
                {t.x0-t.rw,t.frameBottom,t.zFar,t.x0,t.frameTop,t.zNear},
                {t.x1,t.frameBottom,t.zFar,t.x1+t.rw,t.frameTop,t.zNear}};
            for(float lx:{t.x0-t.rw+0.02f*state.upper,t.x1+t.rw-0.02f*state.upper-t.leg})
                for(float lz:{t.zFar-t.rw+0.02f*state.upper,t.zNear+t.rw-0.02f*state.upper-t.leg})
                    boxes.push_back({lx,floor,lz,lx+t.leg,t.frameBottom,lz+t.leg});
            XMVECTOR scale,turn,move;
            const XMMATRIX toModel=XMMatrixInverse(nullptr,world);
            XMMatrixDecompose(&scale,&turn,&move,toModel);
            std::vector<MascotModel::Solid> solids;
            for(const auto& b:boxes)
            {
                MascotModel::Solid s;
                const XMVECTOR middle=XMVector3Transform(XMVectorSet((b[0]+b[3])/2,(b[1]+b[4])/2,(b[2]+b[5])/2,0),toModel);
                XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(s.middle.data()),middle);
                s.half={(b[3]-b[0])/2,(b[4]-b[1])/2,(b[5]-b[2])/2};
                XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(s.turn.data()),turn);
                solids.push_back(s);
            }
            model.SetSolids(solids);
        }
    }

    void TickFortsWindow(MascotModel& model,double dt)
    {
        if(!state.window||!state.renderer)return;
        auto& r=*state.renderer;
        using namespace DirectX;
        const float D=state.upper,top=Top();
        dt=std::min(dt,0.1);
        // The entrance, as at the mahjong table: she walks in, turns to you, sits down and
        // moves in with her chair.
        const float seatZ=SeatZ(),seatHeight=-FloorY(),step=static_cast<float>(dt);
        const float standX=0,standZ=0.25f*D+SeatDepth(),pulled=standZ+0.25f*D+SeatFront(),startX=-1.8f*D,startZ=standZ;
        float lift=0,standing=1,lean=0;
        auto ease=[](float u){return u*u*(3-2*u);};
        if(state.entrance==0)
        {
            // A built-in motion begun while she stood up again must not come along (one that travels would carry her aside).
            model.SettlePose();
            model.SetFacingWalk(true);
            model.SetActivity(Activity::Walk,1);
            state.herX=startX;
            state.herZ=startZ;
            state.herAngle=std::atan2(startX-standX,startZ-standZ);
            state.chairZ=pulled;
            state.entrance=1;
        }
        if(state.entrance==1)
        {
            const float dx=standX-state.herX,dz=standZ-state.herZ,left=std::sqrt(dx*dx+dz*dz);
            const float go=std::min(left,std::max(model.GroundSpeed(),0.25f*D)*step);
            if(left>0){state.herX+=dx/left*go;state.herZ+=dz/left*go;}
            lift=-seatHeight;
            if(left-go<=0.001f*D){state.herX=standX;state.herZ=standZ;model.SetActivity(Activity::Idle);model.EndGestures();state.entrance=2;state.entranceTime=0;}
        }
        else if(state.entrance==2)
        {
            state.herAngle*=std::exp(-step/0.1f);
            lift=-seatHeight;
            if((state.entranceTime+=step)>0.45f)
            {
                model.SetFacingWalk(false);
                if(model.CanSit())model.SetActivity(Activity::Sit,0);
                state.herAngle=0;
                state.entrance=3;
                // As she sits she reaches down for the seat's sides, for moving in with it.
                model.SetChairGrip(true,0.34f*state.upper,-0.055f*state.upper,0.12f*state.upper-SeatFront());
                state.entranceTime=0;
            }
        }
        else if(state.entrance==3)
        {
            const float u=ease(std::min(1.0f,(state.entranceTime+=step)/0.7f));
            lift=-seatHeight*(1-u);
            state.herZ=standZ+(pulled-standZ)*u;
            standing=1-u;
            if(u>=1){state.entrance=4;state.entranceTime=0;}
        }
        else if(state.entrance==4)
        {
            const float tt=std::clamp(((state.entranceTime+=step)-0.45f)/0.8f,0.0f,1.0f),u=ease(tt);
            const float bump=std::sin(tt*XM_PI);
            lift=0.05f*state.hips*bump;
            lean=0.22f*bump;
            standing=0;
            state.herZ=state.chairZ=pulled+(seatZ-pulled)*u;
            if(u>=1){state.entrance=5;model.SetChairGrip(false);}
        }
        else if(state.entrance==5){standing=0;state.herX=0;state.herZ=state.chairZ=seatZ;}
        state.clock+=dt;
        state.frames++;
        if(!state.tableBuilt){state.tableBuilt=true;r.SetGeometry(BuildTable());}
        // Her hand goes out to the table before her action (tried again next frame while a gesture of hers ends).
        if(state.reachDue&&state.entrance==5)
        {
            std::uniform_real_distribution<float> along(-0.7f,0.7f);
            if(ReachFor(model,{along(state.sparkRandom)*D,Top(),FieldZ()+0.25f*D})){state.reachDue=false;state.reaches++;}
        }
        {
            auto& t=state.table;
            // The game over: a clap (or her cheeks) when she has won, "oops" when you have.
            if(!state.choosing&&t.Now()==Phase::Over&&!state.gestureDone)
            {
                state.gestureDone=true;
                std::uniform_real_distribution<double> any(0,1);
                if(t.Board().winner==You)state.endGesture=L"oops";
                else if(t.Board().winner==Her)state.endGesture=any(state.sparkRandom)<0.5?L"cheeks":L"clap";
                if(!state.endGesture.empty())state.endGestureUntil=state.clock+1.5;
            }
            if(!state.endGesture.empty()&&state.entrance==5&&(model.EmoteSeated(state.endGesture,3.2)||state.clock>state.endGestureUntil))
                state.endGesture.clear();
        }
        // What moves: her chair.
        {
            Mesh moving;
            moving.out=BuildChair(state.chairZ);
            r.SetMovingGeometry(std::move(moving.out));
        }
        {
            std::vector<MascotRenderer::Particle> shown;
            ShowRoomGlows(shown,state.roomGlows,state.clock);
            r.SetParticles(shown);
        }
        const float hip=0.12f*state.hips;
        const XMMATRIX world=XMMatrixTranslation(-model.RootOffset(),-hip,0)*XMMatrixRotationX(-lean)*XMMatrixTranslation(0,hip,0)*XMMatrixRotationY(state.herAngle)*XMMatrixTranslation(state.herX,lift,state.entrance==0?seatZ:state.herZ);
        r.SetModelTransform(world);
        SetTableSolids(model,world);
        // From your side of the table, above her, looking down at the table between you.
        const auto& c=state.camera;
        const XMVECTOR target=XMVectorSet(c.x*D,top+(0.40f+c.y+0.7f*standing)*D,(c.z-1.22f)*D,0);
        const XMVECTOR toEye=XMVectorSet(std::sin(c.yaw)*std::cos(c.pitch),std::sin(c.pitch),-std::cos(c.yaw)*std::cos(c.pitch),0);
        const XMVECTOR eye=XMVectorAdd(target,XMVectorScale(toEye,c.distance*(1+0.22f*standing)*D));
        r.SetCamera(eye,target,XMConvertToRadians(42.0f));
        if(std::array<float,3> head{};model.HeadPosition(head))
        {
            const XMVECTOR at=XMVector3Transform(XMVectorSet(head[0],head[1],head[2],1),world);
            const XMVECTOR right=XMVector3Normalize(XMVector3Cross(XMVectorSet(0,1,0,0),XMVectorSubtract(target,eye)));
            const XMVECTOR beside=XMVectorAdd(at,XMVectorScale(right,0.26f*D));
            state.headScreen=r.Project({XMVectorGetX(at),XMVectorGetY(at),XMVectorGetZ(at)});
            state.besideHead=r.Project({XMVectorGetX(beside),XMVectorGetY(beside),XMVectorGetZ(beside)});
        }
        {
            XMVECTOR det;
            const XMVECTOR you=XMVector3Transform(eye,XMMatrixInverse(&det,world));
            std::array<float,3> head{};
            const bool talking=!state.words.text.empty()&&model.HeadPosition(head);
            // For a moment her eyes rest on the fort she has in mind.
            if(state.clock<state.glanceUntil&&state.glanceFort>=0&&model.HeadPosition(head))
            {
                const XMVECTOR spot=XMVector3Transform(XMVectorSet((state.glanceFort-1)*0.8f*D,Top(),FieldZ(),1),XMMatrixInverse(&det,world));
                model.LookToward({XMVectorGetX(spot),XMVectorGetY(spot),XMVectorGetZ(spot)},false);
            }
            else model.LookToward({XMVectorGetX(you),head[1],XMVectorGetZ(you)},talking);
        }
        {
            MascotRenderer::PointLight lamp;
            lamp.position[0]=0;lamp.position[1]=top+2.3f*D;lamp.position[2]=FieldZ();
            lamp.radius=4.2f*D;
            lamp.color[0]=0.42f;lamp.color[1]=0.30f;lamp.color[2]=0.16f;
            lamp.onModel=false;
            r.SetLights({lamp});
        }
        {
            // Her mouth moves while her words are being shown (a vowel or two, opening and
            // closing about five times a second), and closes after; with a voice file, with it.
            const auto& w=state.words;
            const bool saying=!w.text.empty()&&w.text!=L"…"&&w.shown<static_cast<double>(w.text.size());
            state.voice.Tick(dt);
            if(state.voice.Playing())model.SetMouth(state.voice.Mouth(),true);
            else if(saying)
            {
                const float u=static_cast<float>(state.clock*10.5);
                const float open=0.25f+0.75f*std::abs(std::sin(u));
                const int vowel=static_cast<int>(state.clock*5.3)%3;
                model.SetMouth({vowel==0?open:0.0f,vowel==1?open*0.6f:0.0f,0,0,vowel==2?open*0.8f:0.0f,0},true);
            }
            else model.SetMouth({},false);
        }
        if(state.overlayDue)
        {
            state.overlayDue=false;
            const RECT none{};
            Paint(nullptr,none);
        }
        r.Render(model,state.her.edgeScale);
    }

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

    std::wstring DescribeForts()
    {
        if(!state.window)return L"";
        auto& t=state.table;
        const auto& b=t.Board();
        std::wstring out=Fmt(L"forts: entrance {0}, choosing {1}, phase {2}, turns {3}, score you {4} her {5}, magic you {6} her {7}, hand you {8} her {9}, face {10}, aim {11}\n",
                             {std::to_wstring(state.entrance),std::to_wstring(state.choosing?1:0),std::to_wstring(static_cast<int>(t.Now())),std::to_wstring(b.turns),
                              std::to_wstring(b.side[You].score),std::to_wstring(b.side[Her].score),std::to_wstring(b.side[You].magic),std::to_wstring(b.side[Her].magic),
                              std::to_wstring(b.side[You].handSize),std::to_wstring(b.side[Her].handSize),std::to_wstring(static_cast<int>(state.tell)),std::to_wstring(t.HerAim())});
        out+=L"forts level: "+std::to_wstring(state.strength.level)+L" after "+std::to_wstring(state.strength.games)+L" games\n";
        out+=L"forts collected: "+std::to_wstring(state.owned.size())+L", this game brought "+std::to_wstring(state.earned)+L"\n";
        wchar_t speed[160];
        RECT client{};
        GetClientRect(state.window,&client);
        const double seconds=std::max(0.001,state.clock-state.framesSince);
        swprintf_s(speed,L"forts speed: %ldx%ld, %.1f frames a second, %d paints (%d whole) at %.1f ms, %d reaches\n",client.right,client.bottom,state.frames/seconds,state.paints,state.basePaints,state.paints?1000*state.paintSeconds/state.paints:0.0,state.reaches);
        out+=speed;
        return out+(state.words.told.empty()?std::wstring():L"forts words:\n"+state.words.told);
    }

    bool SaveFortsView(const std::wstring& path)
    {
        return state.renderer&&state.renderer->SaveLastFrame(path);
    }

    bool SaveFortsShot(const std::wstring& path,int moves,std::uint32_t seed,const std::wstring& mode)
    {
        GdiplusStartupInput input;
        if(!state.gdiplus)GdiplusStartup(&state.gdiplus,&input,nullptr);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc=DefWindowProcW;
        wc.hInstance=GetModuleHandleW(nullptr);
        wc.lpszClassName=L"MikuMikuDesktopPerchFortsShot";
        RegisterClassExW(&wc);
        const int dpi=static_cast<int>(GetDpiForSystem());
        state.window=CreateWindowExW(0,wc.lpszClassName,L"",WS_OVERLAPPEDWINDOW,0,0,MulDiv(static_cast<int>(BaseWidth)+16,dpi,96),MulDiv(static_cast<int>(BaseHeight)+39,dpi,96),
                                     nullptr,nullptr,wc.hInstance,nullptr);
        state.her=FortsOpponent{L"ミク"};
        std::mt19937 dice(seed);
        state.choosing=mode==L"choose"||mode==L"unlocked"||mode==L"album";
        if(mode==L"unlocked"||mode==L"album")
        {
            std::mt19937 some(seed);
            for(int i=0;i<(mode==L"album"?17:12);i++)Forts::Collect(state.owned,some);
            state.album=mode==L"album";
        }
        if(!state.choosing)
        {
            state.table.NewGame(seed,seed%Forts::DeckKinds,(seed/7)%Forts::DeckKinds,Forts::Levels-1,seed%2);
            if(mode!=L"redraw")PlaySimply(moves);
            state.help=mode==L"help";
            if(mode==L"earned")
            {
                auto& over=state.table.Edit();
                over.over=true;
                over.winner=You;
                over.side[You].score=8;
                for(int i=0;i<Forts::UnlockAt-1;i++)state.owned.push_back(Forts::CollectableCards()[static_cast<size_t>(i)]);
                state.owned.push_back(35);
                state.earned=35;
                state.unlockedNow=true;
            }
            // Your move, so that what you may do shows.
            while(mode!=L"redraw"&&state.table.Now()==Phase::Hers)state.table.HerStep();
            state.table.TakeEvents();
            if(mode==L"play"||mode==L"unit")
                for(const auto& a:state.table.Actions())
                {
                    if(mode==L"play"&&a.kind==Forts::ActionKind::Play){state.choice.hand=a.hand;break;}
                    if(mode==L"unit"&&a.kind==Forts::ActionKind::Attack){state.choice.unit=a.unit;break;}
                }
        }
        if(!SampleWords().empty()){state.words.text=SampleWords();state.words.shown=1e9;}
        RECT client{};
        GetClientRect(state.window,&client);
        // fx:<kind>: frames of what moves, saved as <path>.NN.png, every 90 ms.
        if(mode.starts_with(L"fx:"))
        {
            const std::wstring kind=mode.substr(3);
            Bitmap frame(client.right,client.bottom,PixelFormat32bppARGB);
            const auto draw=[&]{
                Graphics g(&frame);
                const HDC dc=g.GetHDC();
                state.baseDue=true;
                Paint(dc,client);
                g.ReleaseHDC(dc);
            };
            draw();
            draw();
            const auto& b=state.table.Board();
            int mine=-1,hers=-1,fort=0;
            for(int f=0;f<Forts::FortCount;f++)
            {
                if(mine<0&&b.side[You].count[f])mine=b.side[You].units[f][0].uid;
                if(hers<0&&b.side[Her].count[f]){hers=b.side[Her].units[f][0].uid;fort=f;}
            }
            std::vector<Forts::Event> events;
            const auto add=[&](Forts::EventKind what,int who,int card,int unit,int target,int amount,int back=0,int at=-1){
                Forts::Event e;
                e.kind=what;
                e.who=static_cast<std::int8_t>(who);
                e.card=static_cast<std::int8_t>(card);
                e.unit=static_cast<std::int16_t>(unit);
                e.target=static_cast<std::int16_t>(target);
                e.amount=static_cast<std::int8_t>(amount);
                e.back=static_cast<std::int8_t>(back);
                e.fort=static_cast<std::int8_t>(at);
                events.push_back(e);
            };
            const int card=mine>=0?Forts::FindUnit(b,mine)->card:0,theirs=hers>=0?Forts::FindUnit(b,hers)->card:0;
            using K=Forts::EventKind;
            if(kind==L"holo")
            {
                auto& edit=state.table.Edit();
                edit.who=You;
                auto& mine2=edit.side[You];
                mine2.handSize=4;
                mine2.hand[0]=35;
                mine2.hand[1]=34;
                mine2.hand[2]=15;
                mine2.hand[3]=0;
                mine2.count[1]=1;
                Forts::Unit u;
                u.uid=950;
                u.card=25;
                u.attack=6;
                u.health=2;
                u.maxHealth=5;
                u.rally=1;
                mine2.units[1][0]=u;
                state.hoverHand=-1;
                state.hoverUnit=950;
                draw();
                draw();
            }
            else if(kind==L"both")
            {
                auto& edit=state.table.Edit();
                edit.who=You;
                for(int who=0;who<2;who++)
                {
                    edit.side[who].count[0]=1;
                    Forts::Unit u;
                    u.uid=static_cast<std::int16_t>(900+who);
                    u.card=2;
                    u.attack=3;
                    u.health=u.maxHealth=2;
                    u.ready=true;
                    edit.side[who].units[0][0]=u;
                }
                draw();
                draw();
                Forts::Action a;
                a.kind=Forts::ActionKind::Attack;
                a.unit=900;
                a.target=901;
                state.table.Do(a);
                events=state.table.TakeEvents();
            }
            else if(kind==L"magic")add(K::Offered,You,5,-1,-1,3);
            else if(kind==L"hit"){add(K::Hit,You,card,mine,hers,2,1);add(K::Fell,Her,theirs,hers,-1,0);}
            else if(kind==L"arrow")add(K::Hit,You,9,mine,hers,2);
            else if(kind==L"fire"){add(K::Spell,You,27,-1,hers,0,0,fort);add(K::Hit,You,27,-1,hers,2,0,fort);}
            else if(kind==L"storm"){add(K::Spell,You,34,-1,-1,0,0,fort);add(K::Hit,You,34,-1,hers,3,0,fort);add(K::Fell,Her,theirs,hers,-1,0);}
            else if(kind==L"frost"){add(K::Spell,You,29,-1,hers,0,0,fort);add(K::Frozen,You,29,hers,-1,0);}
            else if(kind==L"place")add(K::Placed,You,card,mine,-1,0);
            else if(kind==L"score")add(K::Scored,You,-1,-1,-1,2,0,fort);
            else if(kind==L"heal"){add(K::Healed,You,13,mine,-1,0);add(K::Shield,You,7,-1,-1,0,0,fort);}
            else if(kind==L"rally")add(K::Spell,You,10,-1,-1,0,0,fort);
            else if(kind==L"turn"){state.said=T(L"あなたの番",L"Your turn");state.saidAt=GetTickCount64();}
            HandleEvents(events);
            for(int i=0;i<12;i++)
            {
                Sleep(90);
                HandleEvents({});
                draw();
                wchar_t name[16];
                swprintf_s(name,L".%02d.png",i);
                SavePng(frame,path+name);
            }
            DestroyWindow(state.window);
            state.window=nullptr;
            return true;
        }
        Bitmap shot(client.right,client.bottom,PixelFormat32bppARGB);
        {
            Graphics g(&shot);
            const HDC dc=g.GetHDC();
            Paint(dc,client);   // (the first lays the board out, for what is lit on the second)
            state.baseDue=true;
            Paint(dc,client);
            g.ReleaseHDC(dc);
        }
        DestroyWindow(state.window);
        state.window=nullptr;
        return SavePng(shot,path);
    }

    void ShowFortsWindow(HWND owner,const FortsOpponent& her,std::function<void()> onEnd)
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
            Bitmap view(her.faceSize,her.faceSize,her.faceSize*4,PixelFormat32bppPARGB,const_cast<BYTE*>(state.her.face.data()));
            state.face.reset(view.Clone(0,0,her.faceSize,her.faceSize,PixelFormat32bppPARGB));
        }
        state.tableBuilt=false;
        LoadStrength();
        LoadCollection();
        state.album=false;
        state.lines.Load(L"FORTSLINES",L"forts",her.ownLinesFile,her.linesFile,her.styleKey,English(),
                         {L"三つの砦というカードゲーム",L"a card game called Three Forts",L"",L""});
        state.voice.Stop();
        state.prefetching=false;
        state.tell=Expression::Neutral;
        state.words={};
        state.words.told=state.lines.Report();
        if(!her.linesFile.empty())ShowLinesProblem(owner,state.lines);   // (none kept: a diagnostic run, which must not stop)
        NewGame();

        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&info);
        const RECT work=info.rcWork;
        const int dpi=static_cast<int>(GetDpiForSystem());
        const int w=std::min(static_cast<int>(work.right-work.left)*9/10,MulDiv(static_cast<int>(BaseWidth)+16,dpi,96));
        const int h=std::min(static_cast<int>(work.bottom-work.top)*9/10,MulDiv(static_cast<int>(BaseHeight)+39,dpi,96));
        const std::wstring title=std::wstring(T(L"三つの砦",L"Three Forts"))+L" - "+HerName();
        state.window=CreateWindowExW(0,ClassName,title.c_str(),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                                     work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,
                                     nullptr,nullptr,instance,nullptr);
        if(CreateView())PlaceView();
        SetTimer(state.window,TimerId,40,nullptr);
        ShowWindow(state.window,SW_SHOWNORMAL);
        SetForegroundWindow(state.window);
    }
}
