#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch
{
    // A comic-style speech bubble over the mascot's head: a layered window clicks pass
    // through, drawn with GDI+ (soft shadow, gradient body and outline, a curved tail, a name
    // tag). It pops in, the reply streams in with Append and is revealed a few characters at
    // a time (Talking() meanwhile, for the mouth), and it fades out a while after Finish.
    class SpeechBubble
    {
    public:
        SpeechBubble()=default;
        ~SpeechBubble();
        SpeechBubble(const SpeechBubble&)=delete;
        SpeechBubble& operator=(const SpeechBubble&)=delete;

        bool Create(HWND owner);
        void Begin(const std::wstring& speaker);   // waiting for speaker's reply: bouncing dots
        void Append(const std::wstring& text);     // more of the reply
        void Finish();                             // the reply is complete
        void ShowNotice(const std::wstring& text); // a message of the app's own (errors), shown whole
        void Hide();                               // fade out
        bool Visible() const {return visible;}
        const std::wstring& Text() const {return text;}   // the reply (or notice) as it stands
        bool Talking() const {return visible&&!notice&&!fading&&revealed<static_cast<double>(text.size());}
        // Show the reply as far as this many characters (it follows the speech); without it
        // the text appears at a reading pace of its own.
        void SetReveal(double characters){externalReveal=characters;}
        // Screen rectangles to keep clear of (other characters, their bubbles): above the head,
        // the bubble slides sideways, as far as its tail still starts over the head.
        void SetAvoid(std::vector<RECT> rects){avoid=std::move(rects);}
        // Where it was last drawn (body and name tag, screen pixels); false while hidden.
        bool Placed(RECT& rect) const {rect=placed;return visible&&!fading&&placed.right>placed.left;}
        double Age() const {return clock;}   // seconds since it popped up
        HWND Handle() const {return window;}

        // Per frame: advance the animations, the reveal and the hide timer, and place the
        // bubble so its tail points at anchor (the top of the head, screen pixels), inside
        // work. headHalfWidth is used to go beside the head when there is no room above it.
        // hidden: keep it off screen for now (full-screen app).
        void Tick(double dt,POINT anchor,int headHalfWidth,const RECT& work,bool hidden);

    private:
        void Render(POINT anchor,int headHalfWidth,const RECT& work);
        void Show();

        HWND window=nullptr;
        ULONG_PTR gdiplus=0;
        std::wstring text,speaker;
        double revealed=0;     // characters shown so far
        double clock=0;        // since shown: pop-in and dots
        double fade=0;         // fade-out progress, 0..1
        double holdLeft=0;     // seconds before fading once all is shown
        double externalReveal=-1;
        bool visible=false,waiting=false,finished=false,notice=false,fading=false;
        std::vector<RECT> avoid;
        RECT placed{};
    };

    // A question's two answers (はい／いいえ) in a small box under the mascot who asks. Clicking
    // one closes it and calls onChoose (true: the first). Clicking elsewhere leaves it open.
    class ChoiceBox
    {
    public:
        ~ChoiceBox();
        void Open(HWND owner,POINT bottomCentre,const std::wstring& yes,const std::wstring& no,std::function<void(bool)> onChoose);
        void Close();
        bool IsOpen() const {return window!=nullptr;}
        HWND Handle() const {return window;}

    private:
        static LRESULT CALLBACK Proc(HWND,UINT,WPARAM,LPARAM);
        HWND window=nullptr;
        HFONT font=nullptr;
        std::function<void(bool)> onChoose;
    };

    // A one-line box for talking to the mascot. Enter sends (onSend gets the text) and clears
    // it, Esc or clicking elsewhere closes it.
    class ChatInput
    {
    public:
        ~ChatInput();
        void Open(HWND owner,POINT bottomCentre,std::function<void(const std::wstring&)> onSend);
        void Close();
        bool IsOpen() const {return window!=nullptr;}
        HWND Handle() const {return window;}

    private:
        static LRESULT CALLBACK Proc(HWND,UINT,WPARAM,LPARAM);
        static LRESULT CALLBACK EditProc(HWND,UINT,WPARAM,LPARAM);
        HWND window=nullptr,edit=nullptr;
        HFONT font=nullptr;
        WNDPROC editDefault=nullptr;
        std::function<void(const std::wstring&)> onSend;
    };
}
