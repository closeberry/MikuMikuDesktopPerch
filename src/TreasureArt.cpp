#define NOMINMAX
#include "TreasureArt.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace MikuMikuDesktopPerch
{
    namespace
    {
        using namespace Gdiplus;

        // The picture's own units: (0, 0) top left to (1, 1) bottom right of the box.
        struct Canvas
        {
            Graphics& g;
            RectF b;
            PointF P(float u,float v) const {return PointF(b.X+u*b.Width,b.Y+v*b.Height);}
            float S(float s) const {return s*b.Width;}
            RectF R(float u,float v,float w,float h) const {return RectF(b.X+u*b.Width,b.Y+v*b.Height,w*b.Width,h*b.Height);}
        };

        // Filled light at (u, v), darker toward the edges: a rounded, lit look.
        void Shade(Canvas& c,GraphicsPath& path,Color light,Color dark,float u,float v)
        {
            PathGradientBrush brush(&path);
            brush.SetCenterPoint(c.P(u,v));
            brush.SetCenterColor(light);
            INT n=1;
            brush.SetSurroundColors(&dark,&n);
            c.g.FillPath(&brush,&path);
        }

        void Outline(Canvas& c,GraphicsPath& path,Color color,float width)
        {
            Pen pen(color,c.S(width));
            pen.SetLineJoin(LineJoinRound);
            c.g.DrawPath(&pen,&path);
        }

        void Ellipse(Canvas& c,const RectF& r,Color light,Color dark,float u,float v,Color edge,float width=0.012f)
        {
            GraphicsPath path;
            path.AddEllipse(r);
            Shade(c,path,light,dark,u,v);
            if(width>0)Outline(c,path,edge,width);
        }

        void Polygon(Canvas& c,std::initializer_list<PointF> points,Color light,Color dark,float u,float v,Color edge,float width=0.012f)
        {
            GraphicsPath path;
            path.AddPolygon(points.begin(),static_cast<INT>(points.size()));
            Shade(c,path,light,dark,u,v);
            if(width>0)Outline(c,path,edge,width);
        }

        // A soft halo behind it.
        void Glow(Canvas& c,float u,float v,float r,Color color)
        {
            GraphicsPath path;
            path.AddEllipse(c.R(u-r,v-r,2*r,2*r));
            PathGradientBrush brush(&path);
            brush.SetCenterColor(color);
            Color clear(0,color.GetR(),color.GetG(),color.GetB());
            INT n=1;
            brush.SetSurroundColors(&clear,&n);
            c.g.FillPath(&brush,&path);
        }

        // A four-pointed twinkle.
        void Sparkle(Canvas& c,float u,float v,float s,BYTE alpha=235)
        {
            const PointF p[8]={c.P(u,v-s),c.P(u+s*0.22f,v-s*0.22f),c.P(u+s,v),c.P(u+s*0.22f,v+s*0.22f),
                               c.P(u,v+s),c.P(u-s*0.22f,v+s*0.22f),c.P(u-s,v),c.P(u-s*0.22f,v-s*0.22f)};
            SolidBrush white(Color(alpha,255,255,255));
            c.g.FillPolygon(&white,p,8);
        }

        void Shine(Canvas& c,float u,float v,float w,float h,BYTE alpha=150)
        {
            SolidBrush white(Color(alpha,255,255,255));
            c.g.FillEllipse(&white,c.R(u,v,w,h));
        }

        const Color GoldLight(255,255,244,180),GoldDark(255,184,124,24),GoldEdge(255,130,82,10);

        void Coin(Canvas& c)
        {
            Ellipse(c,c.R(0.14f,0.14f,0.72f,0.72f),GoldLight,GoldDark,0.38f,0.34f,GoldEdge,0.02f);
            Pen ring(Color(255,160,104,20),c.S(0.025f));
            c.g.DrawEllipse(&ring,c.R(0.24f,0.24f,0.52f,0.52f));
            // A worn crown stamped in the middle.
            Polygon(c,{c.P(0.36f,0.6f),c.P(0.36f,0.42f),c.P(0.43f,0.52f),c.P(0.5f,0.38f),c.P(0.57f,0.52f),c.P(0.64f,0.42f),c.P(0.64f,0.6f)},
                    Color(255,230,180,70),Color(255,170,110,20),0.5f,0.45f,Color(255,140,90,15));
            Shine(c,0.26f,0.24f,0.14f,0.08f,120);
        }

        void Star(Canvas& c)
        {
            Glow(c,0.5f,0.52f,0.46f,Color(140,255,240,160));
            PointF p[10];
            for(int i=0;i<10;++i)
            {
                const float a=-1.5708f+i*0.6283f,r=i%2?0.15f:0.36f;
                p[i]=c.P(0.5f+std::cos(a)*r,0.54f+std::sin(a)*r);
            }
            GraphicsPath path;
            path.AddPolygon(p,10);
            Shade(c,path,Color(255,255,255,240),Color(255,255,200,70),0.47f,0.45f);
            Outline(c,path,Color(255,215,150,30),0.015f);
        }

        void Vial(Canvas& c)
        {
            Glow(c,0.5f,0.62f,0.4f,Color(90,255,170,220));
            Ellipse(c,c.R(0.26f,0.36f,0.48f,0.5f),Color(170,240,250,255),Color(200,150,200,235),0.42f,0.5f,Color(255,110,150,190));
            Ellipse(c,c.R(0.31f,0.55f,0.38f,0.27f),Color(230,255,200,235),Color(230,230,90,170),0.45f,0.62f,Color(0,0,0,0),0);
            GraphicsPath neck;
            neck.AddRectangle(c.R(0.43f,0.2f,0.14f,0.18f));
            Shade(c,neck,Color(180,240,250,255),Color(200,170,210,235),0.47f,0.25f);
            Outline(c,neck,Color(255,110,150,190),0.012f);
            GraphicsPath cork;
            cork.AddRectangle(c.R(0.41f,0.12f,0.18f,0.09f));
            Shade(c,cork,Color(255,215,170,120),Color(255,150,100,60),0.45f,0.14f);
            Outline(c,cork,Color(255,110,70,40),0.012f);
            Shine(c,0.33f,0.42f,0.08f,0.16f,170);
            Sparkle(c,0.56f,0.66f,0.05f);
            Sparkle(c,0.44f,0.72f,0.035f);
        }

        void Ring(Canvas& c)
        {
            LinearGradientBrush gold(c.P(0.2f,0.4f),c.P(0.8f,0.85f),GoldLight,GoldDark);
            Pen band(&gold,c.S(0.08f));
            c.g.DrawEllipse(&band,c.R(0.24f,0.38f,0.52f,0.44f));
            Polygon(c,{c.P(0.42f,0.38f),c.P(0.58f,0.38f),c.P(0.55f,0.44f),c.P(0.45f,0.44f)},GoldLight,GoldDark,0.5f,0.39f,GoldEdge);
            Ellipse(c,c.R(0.37f,0.15f,0.26f,0.26f),Color(255,255,160,170),Color(255,170,10,40),0.45f,0.22f,Color(255,110,0,20),0.015f);
            Shine(c,0.42f,0.19f,0.07f,0.05f,190);
        }

        void Mirror(Canvas& c)
        {
            GraphicsPath handle;
            handle.AddRectangle(c.R(0.45f,0.62f,0.1f,0.3f));
            Shade(c,handle,Color(255,250,250,255),Color(255,130,130,150),0.48f,0.7f);
            Outline(c,handle,Color(255,100,100,120),0.012f);
            Ellipse(c,c.R(0.2f,0.08f,0.6f,0.6f),Color(255,252,252,255),Color(255,140,140,160),0.36f,0.2f,Color(255,100,100,120),0.015f);
            Ellipse(c,c.R(0.27f,0.15f,0.46f,0.46f),Color(255,235,248,255),Color(255,130,180,230),0.4f,0.28f,Color(255,120,140,170),0.01f);
            SolidBrush shine(Color(150,255,255,255));
            const PointF p[4]={c.P(0.33f,0.3f),c.P(0.45f,0.2f),c.P(0.5f,0.24f),c.P(0.37f,0.36f)};
            c.g.FillPolygon(&shine,p,4);
        }

        void Drop(Canvas& c)
        {
            Glow(c,0.5f,0.6f,0.44f,Color(110,140,190,255));
            GraphicsPath path;
            path.AddBezier(c.P(0.5f,0.1f),c.P(0.62f,0.32f),c.P(0.8f,0.48f),c.P(0.8f,0.64f));
            path.AddBezier(c.P(0.8f,0.64f),c.P(0.8f,0.82f),c.P(0.66f,0.9f),c.P(0.5f,0.9f));
            path.AddBezier(c.P(0.5f,0.9f),c.P(0.34f,0.9f),c.P(0.2f,0.82f),c.P(0.2f,0.64f));
            path.AddBezier(c.P(0.2f,0.64f),c.P(0.2f,0.48f),c.P(0.38f,0.32f),c.P(0.5f,0.1f));
            path.CloseFigure();
            Shade(c,path,Color(255,225,240,255),Color(255,40,80,200),0.42f,0.55f);
            Outline(c,path,Color(255,30,60,150),0.012f);
            // A crescent glinting inside.
            SolidBrush moon(Color(220,255,250,210));
            GraphicsPath crescent;
            crescent.AddEllipse(c.R(0.4f,0.48f,0.22f,0.22f));
            Region inside(&crescent);
            GraphicsPath bite;
            bite.AddEllipse(c.R(0.46f,0.45f,0.2f,0.2f));
            inside.Exclude(&bite);
            c.g.FillRegion(&moon,&inside);
            Shine(c,0.3f,0.5f,0.07f,0.16f,170);
        }

        void Amber(Canvas& c)
        {
            GraphicsPath path;
            const PointF p[6]={c.P(0.24f,0.32f),c.P(0.5f,0.17f),c.P(0.8f,0.32f),c.P(0.82f,0.62f),c.P(0.55f,0.84f),c.P(0.24f,0.72f)};
            path.AddClosedCurve(p,6);
            Shade(c,path,Color(245,255,215,120),Color(245,190,90,15),0.45f,0.4f);
            Outline(c,path,Color(255,140,70,10),0.015f);
            // A tiny curled shell held inside.
            Pen line(Color(200,110,55,10),c.S(0.018f));
            float r=0.16f;
            for(int i=0;i<4;++i,r*=0.66f)c.g.DrawArc(&line,c.R(0.52f-r,0.52f-r,2*r,2*r),i*90.0f,200.0f);
            Shine(c,0.33f,0.3f,0.12f,0.06f,140);
        }

        void Hourglass(Canvas& c)
        {
            SolidBrush glass(Color(90,200,230,255));
            const PointF g[6]={c.P(0.33f,0.19f),c.P(0.67f,0.19f),c.P(0.53f,0.5f),c.P(0.67f,0.81f),c.P(0.33f,0.81f),c.P(0.47f,0.5f)};
            c.g.FillPolygon(&glass,g,6);
            Pen edge(Color(200,150,190,220),c.S(0.012f));
            c.g.DrawPolygon(&edge,g,6);
            Polygon(c,{c.P(0.38f,0.3f),c.P(0.62f,0.3f),c.P(0.5f,0.46f)},Color(255,255,230,140),Color(255,220,160,50),0.5f,0.33f,Color(0,0,0,0),0);
            Polygon(c,{c.P(0.35f,0.8f),c.P(0.65f,0.8f),c.P(0.5f,0.66f)},Color(255,255,230,140),Color(255,220,160,50),0.5f,0.76f,Color(0,0,0,0),0);
            Pen sand(Color(255,240,190,80),c.S(0.012f));
            c.g.DrawLine(&sand,c.P(0.5f,0.46f),c.P(0.5f,0.68f));
            for(const float v:{0.11f,0.81f})
            {
                GraphicsPath bar;
                bar.AddRectangle(c.R(0.24f,v,0.52f,0.08f));
                Shade(c,bar,Color(255,200,140,80),Color(255,110,60,25),0.4f,v+0.02f);
                Outline(c,bar,Color(255,80,40,15),0.012f);
            }
            SolidBrush post(Color(255,120,70,30));
            c.g.FillRectangle(&post,c.R(0.27f,0.19f,0.035f,0.62f));
            c.g.FillRectangle(&post,c.R(0.695f,0.19f,0.035f,0.62f));
            Sparkle(c,0.6f,0.25f,0.04f);
        }

        void Shell(Canvas& c)
        {
            GraphicsPath fan;
            fan.AddPie(c.R(0.12f,0.22f,0.76f,0.76f),200,140);
            LinearGradientBrush rainbow(c.P(0.12f,0.3f),c.P(0.88f,0.5f),Color::Red,Color::Violet);
            const Color colors[6]={Color(255,255,150,160),Color(255,255,210,140),Color(255,250,255,160),Color(255,150,235,200),Color(255,150,190,255),Color(255,210,160,255)};
            const REAL at[6]={0,0.2f,0.4f,0.6f,0.8f,1};
            rainbow.SetInterpolationColors(colors,at,6);
            c.g.FillPath(&rainbow,&fan);
            Pen rib(Color(140,255,255,255),c.S(0.012f));
            for(int i=1;i<7;++i)
            {
                const float a=(200+i*20)*3.14159f/180;
                c.g.DrawLine(&rib,c.P(0.5f,0.6f),c.P(0.5f+std::cos(a)*0.37f,0.6f+std::sin(a)*0.37f));
            }
            Outline(c,fan,Color(255,190,120,160),0.015f);
            Polygon(c,{c.P(0.42f,0.6f),c.P(0.58f,0.6f),c.P(0.56f,0.72f),c.P(0.44f,0.72f)},Color(255,255,225,220),Color(255,210,160,170),0.5f,0.64f,Color(255,190,120,160));
            Sparkle(c,0.32f,0.42f,0.045f);
        }

        void MusicBox(Canvas& c)
        {
            Polygon(c,{c.P(0.2f,0.46f),c.P(0.8f,0.46f),c.P(0.85f,0.2f),c.P(0.25f,0.16f)},Color(255,190,120,80),Color(255,110,55,25),0.5f,0.3f,Color(255,80,35,15));
            GraphicsPath body;
            body.AddRectangle(c.R(0.18f,0.46f,0.64f,0.36f));
            Shade(c,body,Color(255,205,135,90),Color(255,115,60,25),0.4f,0.55f);
            Outline(c,body,Color(255,80,35,15),0.015f);
            Pen trim(Color(255,240,200,90),c.S(0.022f));
            c.g.DrawLine(&trim,c.P(0.18f,0.52f),c.P(0.82f,0.52f));
            c.g.DrawLine(&trim,c.P(0.18f,0.76f),c.P(0.82f,0.76f));
            Ellipse(c,c.R(0.46f,0.59f,0.08f,0.09f),GoldLight,GoldDark,0.49f,0.61f,GoldEdge,0.008f);
            // Notes floating up from it.
            SolidBrush note(Color(240,255,255,255));
            Pen stem(Color(240,255,255,255),c.S(0.016f));
            for(const auto& [u,v]:{std::pair{0.66f,0.12f},std::pair{0.78f,0.04f}})
            {
                c.g.FillEllipse(&note,c.R(u,v+0.07f,0.07f,0.05f));
                c.g.DrawLine(&stem,c.P(u+0.065f,v+0.09f),c.P(u+0.065f,v));
            }
        }

        void Feather(Canvas& c)
        {
            Glow(c,0.52f,0.48f,0.42f,Color(110,255,250,220));
            GraphicsPath vane;
            vane.AddBezier(c.P(0.7f,0.08f),c.P(0.95f,0.26f),c.P(0.72f,0.66f),c.P(0.34f,0.8f));
            vane.AddBezier(c.P(0.34f,0.8f),c.P(0.26f,0.5f),c.P(0.44f,0.2f),c.P(0.7f,0.08f));
            vane.CloseFigure();
            Shade(c,vane,Color(255,255,255,255),Color(255,225,215,170),0.55f,0.36f);
            Outline(c,vane,Color(255,190,170,110),0.012f);
            Pen barb(Color(110,200,180,120),c.S(0.008f));
            for(int i=1;i<6;++i)
            {
                const float t=i/6.0f,u=0.7f-0.4f*t,v=0.1f+0.72f*t;
                c.g.DrawLine(&barb,c.P(u,v),c.P(u+0.14f-0.05f*t,v-0.02f));
                c.g.DrawLine(&barb,c.P(u,v),c.P(u-0.1f+0.03f*t,v-0.06f));
            }
            Pen shaft(Color(255,215,175,90),c.S(0.022f));
            shaft.SetStartCap(LineCapRound);
            c.g.DrawBezier(&shaft,c.P(0.26f,0.92f),c.P(0.38f,0.6f),c.P(0.5f,0.32f),c.P(0.7f,0.09f));
            Sparkle(c,0.76f,0.3f,0.05f);
        }

        void Fang(Canvas& c)
        {
            GraphicsPath path;
            path.AddBezier(c.P(0.24f,0.16f),c.P(0.78f,0.16f),c.P(0.86f,0.6f),c.P(0.56f,0.9f));
            path.AddBezier(c.P(0.56f,0.9f),c.P(0.64f,0.56f),c.P(0.54f,0.34f),c.P(0.24f,0.34f));
            path.CloseFigure();
            Shade(c,path,Color(255,255,252,235),Color(255,190,170,120),0.5f,0.3f);
            Outline(c,path,Color(255,130,110,70),0.015f);
            GraphicsPath band;
            band.AddRectangle(c.R(0.18f,0.15f,0.1f,0.2f));
            Shade(c,band,Color(255,150,95,60),Color(255,80,45,25),0.21f,0.2f);
            Outline(c,band,Color(255,60,30,15),0.012f);
            Shine(c,0.46f,0.22f,0.16f,0.05f,170);
        }

        void Crown(Canvas& c)
        {
            Glow(c,0.5f,0.5f,0.46f,Color(110,255,230,140));
            Polygon(c,{c.P(0.14f,0.76f),c.P(0.14f,0.34f),c.P(0.3f,0.56f),c.P(0.4f,0.2f),c.P(0.5f,0.48f),c.P(0.6f,0.2f),c.P(0.7f,0.56f),c.P(0.86f,0.34f),c.P(0.86f,0.76f)},
                    GoldLight,GoldDark,0.42f,0.45f,GoldEdge,0.018f);
            GraphicsPath band;
            band.AddRectangle(c.R(0.14f,0.66f,0.72f,0.12f));
            Shade(c,band,Color(255,250,220,120),Color(255,170,110,20),0.4f,0.7f);
            Outline(c,band,GoldEdge,0.015f);
            for(const auto& [u,v]:{std::pair{0.14f,0.34f},std::pair{0.4f,0.2f},std::pair{0.6f,0.2f},std::pair{0.86f,0.34f}})
                Ellipse(c,c.R(u-0.035f,v-0.05f,0.07f,0.07f),Color(255,255,255,255),Color(255,210,210,230),u-0.01f,v-0.03f,Color(255,150,150,170),0.008f);
            Ellipse(c,c.R(0.44f,0.665f,0.12f,0.11f),Color(255,255,160,170),Color(255,170,10,40),0.48f,0.69f,Color(255,110,0,20),0.01f);
            Ellipse(c,c.R(0.24f,0.68f,0.09f,0.08f),Color(255,170,210,255),Color(255,20,60,170),0.27f,0.7f,Color(255,10,40,120),0.008f);
            Ellipse(c,c.R(0.67f,0.68f,0.09f,0.08f),Color(255,170,255,190),Color(255,10,130,60),0.7f,0.7f,Color(255,0,90,40),0.008f);
            Sparkle(c,0.62f,0.3f,0.05f);
        }

        void Scale(Canvas& c)
        {
            GraphicsPath path;
            path.AddBezier(c.P(0.5f,0.12f),c.P(0.76f,0.12f),c.P(0.86f,0.36f),c.P(0.82f,0.54f));
            path.AddBezier(c.P(0.82f,0.54f),c.P(0.76f,0.76f),c.P(0.58f,0.86f),c.P(0.5f,0.9f));
            path.AddBezier(c.P(0.5f,0.9f),c.P(0.42f,0.86f),c.P(0.24f,0.76f),c.P(0.18f,0.54f));
            path.AddBezier(c.P(0.18f,0.54f),c.P(0.14f,0.36f),c.P(0.24f,0.12f),c.P(0.5f,0.12f));
            path.CloseFigure();
            Shade(c,path,Color(255,255,150,110),Color(255,120,10,15),0.42f,0.34f);
            Outline(c,path,Color(255,80,0,0),0.018f);
            Pen ridge(Color(150,90,0,0),c.S(0.016f));
            c.g.DrawBezier(&ridge,c.P(0.5f,0.18f),c.P(0.48f,0.4f),c.P(0.5f,0.62f),c.P(0.5f,0.84f));
            c.g.DrawArc(&ridge,c.R(0.28f,0.36f,0.44f,0.3f),200,140);
            Shine(c,0.32f,0.24f,0.14f,0.07f,150);
        }

        void Chalice(Canvas& c)
        {
            Glow(c,0.5f,0.4f,0.42f,Color(100,255,230,150));
            GraphicsPath bowl;
            bowl.AddBezier(c.P(0.24f,0.16f),c.P(0.24f,0.42f),c.P(0.36f,0.56f),c.P(0.5f,0.56f));
            bowl.AddBezier(c.P(0.5f,0.56f),c.P(0.64f,0.56f),c.P(0.76f,0.42f),c.P(0.76f,0.16f));
            bowl.CloseFigure();
            Shade(c,bowl,GoldLight,GoldDark,0.4f,0.3f);
            Outline(c,bowl,GoldEdge,0.015f);
            Ellipse(c,c.R(0.24f,0.11f,0.52f,0.1f),Color(255,200,140,40),Color(255,120,70,10),0.5f,0.16f,GoldEdge,0.012f);
            Polygon(c,{c.P(0.46f,0.55f),c.P(0.54f,0.55f),c.P(0.53f,0.78f),c.P(0.47f,0.78f)},GoldLight,GoldDark,0.49f,0.6f,GoldEdge);
            Ellipse(c,c.R(0.43f,0.6f,0.14f,0.07f),GoldLight,GoldDark,0.48f,0.62f,GoldEdge,0.01f);
            Ellipse(c,c.R(0.28f,0.76f,0.44f,0.12f),GoldLight,GoldDark,0.44f,0.79f,GoldEdge,0.015f);
            Ellipse(c,c.R(0.45f,0.3f,0.1f,0.1f),Color(255,255,160,170),Color(255,170,10,40),0.48f,0.32f,Color(255,110,0,20),0.008f);
            Shine(c,0.3f,0.22f,0.06f,0.18f,140);
            Sparkle(c,0.7f,0.28f,0.045f);
        }

        void Gem(Canvas& c)
        {
            Polygon(c,{c.P(0.5f,0.15f),c.P(0.8f,0.42f),c.P(0.5f,0.86f),c.P(0.2f,0.42f)},Color(255,220,255,255),Color(255,40,170,200),0.42f,0.36f,Color(255,20,110,140));
        }
    }

    void PaintTreasure(Graphics& g,const std::wstring& id,const RectF& box)
    {
        const SmoothingMode smoothing=g.GetSmoothingMode();
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        Canvas c{g,box};
        if(id==L"Ancient coin")Coin(c);
        else if(id==L"Star fragment")Star(c);
        else if(id==L"Fairy's vial")Vial(c);
        else if(id==L"Old ring")Ring(c);
        else if(id==L"Silver hand mirror")Mirror(c);
        else if(id==L"Moon drop")Drop(c);
        else if(id==L"Amber fossil")Amber(c);
        else if(id==L"Magic hourglass")Hourglass(c);
        else if(id==L"Rainbow shell")Shell(c);
        else if(id==L"Spirit music box")MusicBox(c);
        else if(id==L"Goddess feather")Feather(c);
        else if(id==L"Ancient dragon fang")Fang(c);
        else if(id==L"Legendary crown")Crown(c);
        else if(id==L"Dragon scale")Scale(c);
        else if(id==L"Golden chalice")Chalice(c);
        else Gem(c);
        g.SetSmoothingMode(smoothing);
    }

    using namespace Gdiplus;

    TreasurePicture::~TreasurePicture(){Hide();}

    void TreasurePicture::Hide()
    {
        if(window){DestroyWindow(window);window=nullptr;}
        if(memory){SelectObject(memory,previous);DeleteDC(memory);memory=nullptr;}
        if(bitmap){DeleteObject(bitmap);bitmap=nullptr;}
        bits=nullptr;
        id.clear();
        size=0;
    }

    // The picture and its shadow into the bitmap (premultiplied BGRA, as layered windows take).
    void TreasurePicture::Draw()
    {
        if(memory){SelectObject(memory,previous);DeleteDC(memory);memory=nullptr;}
        if(bitmap){DeleteObject(bitmap);bitmap=nullptr;}
        BITMAPINFO info{};
        info.bmiHeader.biSize=sizeof(info.bmiHeader);
        info.bmiHeader.biWidth=size;
        info.bmiHeader.biHeight=-size;
        info.bmiHeader.biPlanes=1;
        info.bmiHeader.biBitCount=32;
        bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        if(!bitmap)return;
        memory=CreateCompatibleDC(nullptr);
        previous=SelectObject(memory,bitmap);
        std::memset(bits,0,static_cast<size_t>(size)*size*4);
        Bitmap canvas(size,size,size*4,PixelFormat32bppPARGB,static_cast<BYTE*>(bits));
        Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        const REAL s=static_cast<REAL>(size);
        // Where it rests: a soft shadow on the palm.
        GraphicsPath shade;
        shade.AddEllipse(RectF(s*0.22f,s*0.86f,s*0.56f,s*0.12f));
        PathGradientBrush brush(&shade);
        brush.SetCenterColor(Color(90,0,0,0));
        Color clear(0,0,0,0);
        INT n=1;
        brush.SetSurroundColors(&clear,&n);
        g.FillPath(&brush,&shade);
        PaintTreasure(g,id,RectF(s*0.04f,0,s*0.92f,s*0.92f));
    }

    void TreasurePicture::Place(const std::wstring& which,POINT bottom,int pixels,BYTE alpha)
    {
        const HINSTANCE instance=GetModuleHandleW(nullptr);
        if(!window)
        {
            static bool registered=false;
            if(!registered)
            {
                WNDCLASSEXW wc{sizeof(wc)};
                wc.lpfnWndProc=DefWindowProcW;
                wc.hInstance=instance;
                wc.lpszClassName=L"MikuMikuDesktopPerchTreasure";
                RegisterClassExW(&wc);
                registered=true;
            }
            window=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST,L"MikuMikuDesktopPerchTreasure",L"",
                                   WS_POPUP,0,0,1,1,nullptr,nullptr,instance,nullptr);
            if(!window)return;
        }
        pixels=std::max(16,pixels);
        if(which!=id||pixels!=size||!bitmap){id=which;size=pixels;Draw();}
        if(!memory)return;
        POINT position{bottom.x-size/2,bottom.y-size};
        SIZE extent{size,size};
        POINT origin{0,0};
        BLENDFUNCTION blend{AC_SRC_OVER,0,alpha,AC_SRC_ALPHA};
        HDC screen=GetDC(nullptr);
        UpdateLayeredWindow(window,screen,&position,&extent,memory,&origin,0,&blend,ULW_ALPHA);
        ReleaseDC(nullptr,screen);
        // Kept above the characters' windows (they are raised now and then).
        SetWindowPos(window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER|SWP_SHOWWINDOW);
    }
}
