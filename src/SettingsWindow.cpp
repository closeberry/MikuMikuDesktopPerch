#include "SettingsWindow.hpp"
#include "Album.hpp"
#include <shellapi.h>
#include "Text.hpp"
#include "LocalChat.hpp"
#include "Voice.hpp"
#include <commctrl.h>
#include <mmsystem.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// Themed (version 6) common controls for the settings window, the message boxes and the file
// dialogs.
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib,"comctl32.lib")
#pragma comment(lib,"winmm.lib")

namespace MikuMikuDesktopPerch
{
    namespace
    {
        constexpr wchar_t ClassName[]=L"MikuMikuDesktopPerchSettings";

        enum ControlId:int
        {
            IdAuto=1001,IdWindowMode,IdPhysics,IdBlink,IdBreath,IdLookAt,
            IdScale,IdEdge,IdTopmost,IdStartup,
            IdFps,IdMotionBlur,IdMsaa,IdTexture,IdPhysicsFps,IdPhysicsIterations,IdPhysicsFilter,IdToonDir,IdToonBrowse,IdGpu,
            IdChatEndpoint,IdChatModel,IdChatList,IdPersona,IdPersonaName,IdUserName,
            IdPersonaText,IdPersonaReset,IdPersonaCount,IdChatMemory,IdMemory,IdSelfShadow,IdChatInitiative,IdVoice,IdVoiceList,IdVoiceFetch,IdVoiceSample,IdVoiceMode,IdVoiceFolder,IdVoiceBrowse,IdVoiceStrength,IdSunAzimuth,IdSunElevation,
            IdMemoryList,IdMemoryNew,IdMemoryAdd,IdMemoryDelete,IdMemoryClear,IdMemoryCount,
            IdChatterMinutes,IdBanterMinutes,IdUpdateCheck,IdLanguage,IdMorningSummary,IdRestMinutes,IdFocusMinutes,IdFocusBreakMinutes,IdCalendarAddress,IdCalendarLead,IdUpdateNow,IdGestures,IdGestureList,IdGestureAll,IdGestureNone,IdApi,IdApiPort,IdBond,IdAlbumHotkey,IdShutterSound,IdOpenAlbum,IdAlbumFolder,IdAlbumBrowse,IdTabs
        };
        // The longest persona (characters, a line break counting as one).
        constexpr size_t MaxPersona=4000;
        // Where the voice comes from (the voice source choices' values).
        enum VoiceMode:int{VoiceVoicevox=0,VoiceFiles=1,VoiceAivisSpeech=2};
        constexpr wchar_t PersonaClass[]=L"MikuMikuDesktopPerchPersona";
        constexpr wchar_t MemoryClass[]=L"MikuMikuDesktopPerchMemory";
        constexpr wchar_t GesturesClass[]=L"MikuMikuDesktopPerchGestures";

        struct State
        {
            HWND window=nullptr;
            HWND persona=nullptr;   // the persona dialog, while open
            HWND memory=nullptr;    // the memory dialog, while open
            HWND gestures=nullptr;  // the gesture choice, while open
            HWND tabs=nullptr;
            // The controls on each tab, and the tab Add puts controls on (-1: on every tab).
            std::vector<std::vector<HWND>> pages;
            int page=-1;
            HFONT font=nullptr;
            SettingsChoice choice;
            std::function<void(const SettingsChoice&)> onApply;
            std::function<void(int,int)> onLight;
            std::function<void()> onCheckUpdate;
            std::string sample;     // the sample voice playing (PlaySound reads it from here)
            int dpi=96;
        } state;

        int Px(int logical){return MulDiv(logical,state.dpi,96);}

        HWND AddTo(HWND parent,const wchar_t* cls,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id=0)
        {
            HWND c=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,Px(x),Px(y),Px(w),Px(h),parent,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(state.font),TRUE);
            return c;
        }
        HWND Add(const wchar_t* cls,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id=0)
        {
            HWND c=AddTo(state.window,cls,text,style,x,y,w,h,id);
            if(state.page>=0)
            {
                state.pages[static_cast<size_t>(state.page)].push_back(c);
                ShowWindow(c,SW_HIDE);
            }
            return c;
        }

        // Puts the following controls on a new tab (of the tab control along the top).
        void Page(const wchar_t* title)
        {
            TCITEMW item{TCIF_TEXT};
            item.pszText=const_cast<wchar_t*>(title);
            TabCtrl_InsertItem(state.tabs,static_cast<int>(state.pages.size()),&item);
            state.page=static_cast<int>(state.pages.size());
            state.pages.emplace_back();
        }

        void ShowPage(int page)
        {
            for(size_t p=0;p<state.pages.size();++p)
                for(HWND c:state.pages[p])ShowWindow(c,static_cast<int>(p)==page?SW_SHOWNA:SW_HIDE);
        }

        void Check(int id,bool on){CheckDlgButton(state.window,id,on?BST_CHECKED:BST_UNCHECKED);}
        bool Checked(int id){return IsDlgButtonChecked(state.window,id)==BST_CHECKED;}

        // A drop-down list of (label, value) choices with value selected. A value not in the
        // list (edited into the ini by hand) is added so it is kept as it is.
        void FillCombo(int id,std::vector<std::pair<std::wstring,int>> items,int value,const std::wstring& otherLabel)
        {
            HWND combo=GetDlgItem(state.window,id);
            if(std::none_of(items.begin(),items.end(),[&](const auto& i){return i.second==value;}))items.push_back({otherLabel,value});
            for(const auto& [label,v]:items)
            {
                const auto index=SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
                SendMessageW(combo,CB_SETITEMDATA,index,v);
                if(v==value)SendMessageW(combo,CB_SETCURSEL,index,0);
            }
        }

        int ComboValue(int id,int fallback)
        {
            HWND combo=GetDlgItem(state.window,id);
            const auto index=SendMessageW(combo,CB_GETCURSEL,0,0);
            return index==CB_ERR?fallback:static_cast<int>(SendMessageW(combo,CB_GETITEMDATA,index,0));
        }

        void Group(const wchar_t* title,int y,int h){Add(L"BUTTON",title,BS_GROUPBOX,12,y,436,h);}
        void Label(const wchar_t* text,int x,int y,int w){Add(L"STATIC",text,SS_LEFT,x,y+3,w,20);}
        void CheckBox(int id,const wchar_t* text,int x,int y,int w,bool on){Add(L"BUTTON",text,BS_AUTOCHECKBOX|WS_TABSTOP,x,y,w,22,id);Check(id,on);}
        // A horizontal slider (from..to) showing its value in a tooltip while dragged.
        void Slider(int id,int x,int y,int w,int from,int to,int value)
        {
            HWND s=Add(TRACKBAR_CLASSW,L"",TBS_HORZ|TBS_NOTICKS|TBS_TOOLTIPS|WS_TABSTOP,x,y,w,26,id);
            SendMessageW(s,TBM_SETRANGEMIN,FALSE,from);
            SendMessageW(s,TBM_SETRANGEMAX,FALSE,to);
            SendMessageW(s,TBM_SETPAGESIZE,0,15);
            SendMessageW(s,TBM_SETPOS,TRUE,value);
        }
        int SliderValue(int id){return static_cast<int>(SendDlgItemMessageW(state.window,id,TBM_GETPOS,0,0));}

        void Combo(int id,int x,int y,int w){Add(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,x,y,w,240,id);}

        void UpdateVoiceMode();

        // "Auto" (the GPU Windows picks for this app), then each GPU by name, the one drawing now
        // marked. The item data is the place in state.choice.gpus plus one (0: auto); a chosen
        // GPU that is not there now (taken out) is kept as it is.
        void FillGpus()
        {
            const auto& gpus=state.choice.gpus;
            const std::wstring& chosen=state.choice.settings.gpu;
            std::vector<std::pair<std::wstring,int>> items{{T(L"自動（Windowsのグラフィックの設定に従う）",L"Auto (as Windows' graphics settings say)"),0}};
            int value=0;
            for(size_t i=0;i<gpus.size();++i)
            {
                const auto& [id,name]=gpus[i];
                items.push_back({name==state.choice.gpuInUse?name+T(L"（使用中）",L" (in use)"):name,static_cast<int>(i+1)});
                if(id==chosen)value=static_cast<int>(i+1);
            }
            if(!chosen.empty()&&value==0)value=-1;
            FillCombo(IdGpu,items,value,Fmt(T(L"見つからないGPU（{0}）",L"GPU not found ({0})"),{chosen}));
        }

        std::wstring GpuValue()
        {
            const int value=ComboValue(IdGpu,-1);
            const auto& gpus=state.choice.gpus;
            if(value<0)return state.choice.settings.gpu;
            return value==0||static_cast<size_t>(value)>gpus.size()?std::wstring():gpus[value-1].first;
        }

        // How often only matters while speaking first is on.
        void UpdateInitiative()
        {
            const bool on=Checked(IdChatInitiative);
            EnableWindow(GetDlgItem(state.window,IdChatterMinutes),on);
            EnableWindow(GetDlgItem(state.window,IdBanterMinutes),on);
        }

        void Build()
        {
            const Settings& s=state.choice.settings;
            const CharacterSettings& own=state.choice.mascot;
            // The window is kept low by putting its sections on tabs: only the tab strip of the
            // tab control is shown, and the controls sit on the window below it.
            state.tabs=Add(WC_TABCONTROLW,L"",WS_TABSTOP|WS_CLIPSIBLINGS,10,8,440,30,IdTabs);
            constexpr int top=48;
            int y=top;
            Page(T(L"動きと表示",L"Motion and display"));
            Group(T(L"動き",L"Motion"),y,128);
            CheckBox(IdAuto,T(L"おまかせ行動（ひとりで歩き回る）",L"Auto behavior (walks around on her own)"),28,y+22,270,s.autoBehavior);
            Add(L"BUTTON",T(L"しぐさを選ぶ...",L"Gestures..."),BS_PUSHBUTTON|WS_TABSTOP,316,y+18,120,26,IdGestures);
            CheckBox(IdWindowMode,T(L"アクティブウィンドウに乗る",L"Ride on the active window"),28,y+46,400,s.windowMode);
            CheckBox(IdPhysics,T(L"物理演算（髪やスカートの揺れ）",L"Physics (hair and skirt sway)"),28,y+70,228,s.physics);
            Label(T(L"震え止め",L"Smoothing"),262,y+70,60);
            Combo(IdPhysicsFilter,326,y+68,110);
            CheckBox(IdBlink,T(L"まばたき",L"Blink"),28,y+94,120,s.blink);
            CheckBox(IdBreath,T(L"呼吸",L"Breathe"),156,y+94,120,s.breath);
            CheckBox(IdLookAt,T(L"マウスの方を見る",L"Look at the mouse"),284,y+94,150,s.lookAtMouse);
            y+=138;
            Group(T(L"表示",L"Display"),y,156);
            Label(T(L"大きさ",L"Size"),28,y+22,130);
            Combo(IdScale,170,y+22,120);
            Label(T(L"言語",L"Language"),300,y+22,56);
            Combo(IdLanguage,356,y+22,80);
            Label(T(L"輪郭線の太さ",L"Outline width"),28,y+50,130);
            Combo(IdEdge,170,y+50,120);
            CheckBox(IdTopmost,T(L"最前面に表示",L"Always on top"),28,y+78,200,s.topmost);
            CheckBox(IdSelfShadow,T(L"セルフシャドウ（体に落ちる影）",L"Self-shadow"),250,y+78,190,s.selfShadow);
            CheckBox(IdStartup,T(L"Windowsの起動時に起動",L"Start with Windows"),28,y+100,200,state.choice.startWithWindows);
            CheckBox(IdUpdateCheck,T(L"更新を自動で確認",L"Check for updates"),250,y+100,112,s.updateCheck);
            Add(L"BUTTON",T(L"今すぐ確認",L"Check now"),BS_PUSHBUTTON|WS_TABSTOP,366,y+97,70,24,IdUpdateNow);
            Label(T(L"太陽の位置（左右・高さ）",L"Sun (side, height)"),28,y+128,140);
            Slider(IdSunAzimuth,170,y+126,130,-180,180,s.lightAzimuth);
            Slider(IdSunElevation,306,y+126,130,5,90,s.lightElevation);
            y=top;
            Page(T(L"会話と声",L"Talking and voice"));
            Group(T(L"会話（このコンピューターのLLM：Ollama、LM Studio など）",L"Talking (an LLM on this computer: Ollama, LM Studio, ...)"),y,280);
            Label(T(L"接続先",L"Server"),28,y+22,130);
            Add(L"EDIT",s.chatEndpoint.c_str(),ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,170,y+22,266,24,IdChatEndpoint);
            Label(T(L"モデル",L"Model"),28,y+52,130);
            Add(L"COMBOBOX",L"",CBS_DROPDOWN|CBS_AUTOHSCROLL|WS_VSCROLL|WS_TABSTOP,170,y+52,190,240,IdChatModel);
            SetDlgItemTextW(state.window,IdChatModel,s.chatModel.c_str());
            Add(L"BUTTON",T(L"一覧を取得",L"Get list"),BS_PUSHBUTTON|WS_TABSTOP,366,y+51,70,26,IdChatList);
            Label(T(L"あなたの呼び名",L"Your name"),28,y+82,130);
            Add(L"EDIT",s.userName.c_str(),ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,170,y+82,150,24,IdUserName);
            SendDlgItemMessageW(state.window,IdUserName,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(T(L"例: たろう",L"e.g. Alex")));
            CheckBox(IdBond,T(L"なかよし度",L"Closeness"),336,y+83,100,s.bond);
            CheckBox(IdChatMemory,T(L"会話の内容を覚える",L"Remember chats"),28,y+112,140,s.chatMemory);
            Add(L"BUTTON",T(L"人物設定...",L"Persona..."),BS_PUSHBUTTON|WS_TABSTOP,170,y+110,110,26,IdPersona);
            Add(L"BUTTON",T(L"記憶...",L"Memory..."),BS_PUSHBUTTON|WS_TABSTOP,286,y+110,110,26,IdMemory);
            if(!state.choice.memory)EnableWindow(GetDlgItem(state.window,IdMemory),FALSE);
            CheckBox(IdChatInitiative,T(L"ときどき自分から話しかける",L"Speak first now and then"),28,y+138,200,s.chatInitiative);
            Label(T(L"ひと言",L"Remarks"),262,y+138,60);
            Combo(IdChatterMinutes,326,y+136,110);
            Label(T(L"キャラクター同士のおしゃべり（2人以上のとき）",L"Characters chatting together (two or more)"),46,y+166,270);
            Combo(IdBanterMinutes,326,y+164,110);
            y+=28;
            CheckBox(IdVoice,T(L"声で話す",L"Voice"),28,y+166,140,s.voice);
            Combo(IdVoiceList,170,y+164,128);
            {
                HWND list=GetDlgItem(state.window,IdVoiceList);
                const std::wstring label=own.voiceName.empty()?T(L"声 ",L"Voice ")+std::to_wstring(own.voiceSpeaker):own.voiceName;
                const auto index=SendMessageW(list,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
                SendMessageW(list,CB_SETITEMDATA,index,own.voiceSpeaker);
                SendMessageW(list,CB_SETCURSEL,index,0);
            }
            Add(L"BUTTON",T(L"試聴",L"Listen"),BS_PUSHBUTTON|WS_TABSTOP,304,y+163,56,26,IdVoiceSample);
            Add(L"BUTTON",T(L"一覧を取得",L"Get list"),BS_PUSHBUTTON|WS_TABSTOP,366,y+163,70,26,IdVoiceFetch);
            Combo(IdVoiceMode,46,y+192,120);
            FillCombo(IdVoiceMode,{{L"VOICEVOX",VoiceVoicevox},{L"AivisSpeech",VoiceAivisSpeech},{T(L"VOICEVOXのファイル",L"VOICEVOX files"),VoiceFiles}},
                      s.voiceDirect?VoiceFiles:IsAivisSpeech(s.voiceEndpoint)?VoiceAivisSpeech:VoiceVoicevox,L"");
            Add(L"EDIT",s.voiceFolder.c_str(),ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,170,y+192,190,24,IdVoiceFolder);
            SendDlgItemMessageW(state.window,IdVoiceFolder,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(T(L"VOICEVOXのフォルダ",L"VOICEVOX folder")));
            Add(L"BUTTON",T(L"参照...",L"Browse..."),BS_PUSHBUTTON|WS_TABSTOP,366,y+191,70,26,IdVoiceBrowse);
            Label(T(L"スタイルの強さ",L"Style strength"),46,y+222,120);
            Combo(IdVoiceStrength,170,y+220,190);
            FillCombo(IdVoiceStrength,{{T(L"0.5（弱め）",L"0.5 (weak)"),50},{T(L"1.0（標準）",L"1.0 (standard)"),100},{T(L"1.5（強め）",L"1.5 (strong)"),150},{T(L"2.0（とても強い）",L"2.0 (very strong)"),200}},
                      s.voiceStyleStrength,std::to_wstring(s.voiceStyleStrength/100)+L"."+std::to_wstring(s.voiceStyleStrength/10%10)+std::to_wstring(s.voiceStyleStrength%10));
            UpdateVoiceMode();
            y=top;
            Page(T(L"秘書",L"Assistant"));
            Group(T(L"秘書（タイマー・リマインダー・やることは、メニューと会話から）",L"Assistant (timers, reminders and to-dos: from the menu or by talking)"),y,164);
            CheckBox(IdMorningSummary,T(L"朝に、今日のリマインダーとやることを話す",L"Tell today's reminders and to-dos in the morning"),28,y+22,400,s.morningSummary);
            Label(T(L"使い続けたら休憩をすすめる",L"Suggest a break after using the PC for"),28,y+50,290);
            Combo(IdRestMinutes,326,y+48,110);
            Label(T(L"集中モードの作業と休憩",L"Focus mode: work and break"),28,y+78,200);
            Combo(IdFocusMinutes,220,y+76,104);
            Combo(IdFocusBreakMinutes,332,y+76,104);
            // The calendar's address is a secret: shown as dots.
            Label(T(L"Googleカレンダー",L"Google Calendar"),28,y+106,140);
            Add(L"EDIT",s.calendarAddress.c_str(),ES_AUTOHSCROLL|ES_PASSWORD|WS_BORDER|WS_TABSTOP,170,y+104,266,24,IdCalendarAddress);
            SendDlgItemMessageW(state.window,IdCalendarAddress,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(T(L"iCal形式の非公開アドレス",L"Secret address in iCal format")));
            Label(T(L"カレンダーの予定の前に知らせる",L"Tell me before calendar events"),28,y+134,290);
            Combo(IdCalendarLead,326,y+132,110);
            y+=172;
            Group(T(L"アルバム（後ろのウィンドウは写りません）",L"Album (windows behind are never in it)"),y,112);
            Label(T(L"この瞬間を撮るキー",L"Key to take a picture"),28,y+24,140);
            Add(HOTKEY_CLASSW,L"",WS_BORDER|WS_TABSTOP,170,y+22,150,24,IdAlbumHotkey);
            SendDlgItemMessageW(state.window,IdAlbumHotkey,HKM_SETHOTKEY,static_cast<WPARAM>(s.albumHotkey),0);
            Add(L"BUTTON",T(L"アルバムを開く",L"Open album"),BS_PUSHBUTTON|WS_TABSTOP,326,y+21,110,26,IdOpenAlbum);
            Label(T(L"保存先",L"Folder"),28,y+52,140);
            Add(L"EDIT",s.albumFolder.c_str(),ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,170,y+50,190,24,IdAlbumFolder);
            SendDlgItemMessageW(state.window,IdAlbumFolder,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(T(L"既定（LOCALAPPDATAの中）",L"Default (in LOCALAPPDATA)")));
            Add(L"BUTTON",T(L"参照...",L"Browse..."),BS_PUSHBUTTON|WS_TABSTOP,366,y+49,70,26,IdAlbumBrowse);
            CheckBox(IdShutterSound,T(L"撮るときにシャッター音を鳴らす",L"Play a shutter sound"),28,y+80,300,s.shutterSound);
            y=top;
            Page(T(L"詳細",L"Advanced"));
            Group(T(L"詳細（変更すると再起動します）",L"Advanced (changes restart the app)"),y,194);
            Label(T(L"描画のfps",L"Drawing fps"),28,y+22,130);
            Combo(IdFps,170,y+22,120);
            CheckBox(IdMotionBlur,T(L"モーションブラー",L"Motion blur"),304,y+22,132,s.motionBlur);
            Label(T(L"アンチエイリアス",L"Antialiasing"),28,y+50,130);
            Combo(IdMsaa,170,y+50,120);
            Label(T(L"テクスチャの最大サイズ",L"Max texture size"),28,y+78,140);
            Combo(IdTexture,170,y+78,120);
            // Other programs on this computer may drive the characters (127.0.0.1 only), on this port.
            CheckBox(IdApi,T(L"外部から操作",L"Remote API"),300,y+78,92,s.api);
            Add(L"EDIT",std::to_wstring(s.apiPort).c_str(),ES_NUMBER|WS_BORDER|WS_TABSTOP,392,y+78,44,22,IdApiPort);
            Label(T(L"物理演算の更新",L"Physics rate"),28,y+106,130);
            Combo(IdPhysicsFps,170,y+106,120);
            Label(T(L"反復回数",L"Iterations"),300,y+106,56);
            Combo(IdPhysicsIterations,360,y+106,76);
            Label(T(L"共有トゥーンのフォルダ",L"Shared toon folder"),28,y+134,140);
            Add(L"EDIT",s.toonDir.c_str(),ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,170,y+134,190,24,IdToonDir);
            Add(L"BUTTON",T(L"参照...",L"Browse..."),BS_PUSHBUTTON|WS_TABSTOP,366,y+133,70,26,IdToonBrowse);
            Label(T(L"描画に使うGPU",L"GPU for drawing"),28,y+162,140);
            Combo(IdGpu,170,y+162,266);
            FillGpus();
            state.page=-1;
            y=top+138+166;
            Add(L"BUTTON",L"OK",BS_DEFPUSHBUTTON|WS_TABSTOP,276,y,82,28,IDOK);
            Add(L"BUTTON",T(L"キャンセル",L"Cancel"),BS_PUSHBUTTON|WS_TABSTOP,366,y,82,28,IDCANCEL);

            std::vector<std::pair<std::wstring,int>> scales;
            for(int p=Settings::MinScale;p<=Settings::MaxScale;p+=10)scales.push_back({std::to_wstring(p)+L"%",p});
            FillCombo(IdScale,scales,s.scale,std::to_wstring(s.scale)+L"%");
            const int edge=static_cast<int>(std::lround(s.edgeScale*100));
            FillCombo(IdEdge,{{T(L"なし",L"None"),0},{T(L"細い（0.5倍）",L"Thin (0.5x)"),50},{T(L"やや細い（0.75倍）",L"Thinner (0.75x)"),75},{T(L"標準",L"Standard"),100},{T(L"やや太い（1.25倍）",L"Thicker (1.25x)"),125},{T(L"太い（1.5倍）",L"Thick (1.5x)"),150},{T(L"とても太い（2倍）",L"Very thick (2x)"),200}},
                      edge,std::to_wstring(edge/100.0).substr(0,4)+T(L"倍",L"x"));
            FillCombo(IdFps,{{L"15",15},{L"20",20},{T(L"30（標準）",L"30 (standard)"),30},{L"45",45},{L"60",60}},s.fps,std::to_wstring(s.fps));
            FillCombo(IdMsaa,{{T(L"なし",L"None"),1},{L"2x",2},{T(L"4x（標準）",L"4x (standard)"),4},{L"8x",8}},s.msaa,std::to_wstring(s.msaa)+L"x");
            FillCombo(IdTexture,{{L"512",512},{T(L"1024（標準）",L"1024 (standard)"),1024},{L"2048",2048},{L"4096",4096},{T(L"縮小しない",L"Do not shrink"),0}},s.maxTextureSize,std::to_wstring(s.maxTextureSize));
            FillCombo(IdPhysicsFps,{{T(L"30回/秒",L"30/s"),30},{T(L"60回/秒（標準）",L"60/s (standard)"),60},{T(L"90回/秒",L"90/s"),90},{T(L"120回/秒",L"120/s"),120}},s.physicsFps,std::to_wstring(s.physicsFps)+T(L"回/秒",L"/s"));
            FillCombo(IdPhysicsFilter,{{T(L"なし",L"None"),0},{T(L"弱",L"Weak"),1},{T(L"中",L"Medium"),2},{T(L"強（標準）",L"Strong"),3}},s.physicsFilter,L"");
            FillCombo(IdPhysicsIterations,{{T(L"10（標準）",L"10"),10},{L"20",20},{L"30",30},{L"50",50}},s.physicsIterations,std::to_wstring(s.physicsIterations));
            FillCombo(IdCalendarLead,{{T(L"しない",L"Never"),0},{T(L"5分前",L"5 min before"),5},{T(L"10分前（標準）",L"10 min before"),10},{T(L"15分前",L"15 min before"),15},{T(L"30分前",L"30 min before"),30}},
                      s.calendarLeadMinutes,Fmt(T(L"{0}分前",L"{0} min before"),{std::to_wstring(s.calendarLeadMinutes)}));
            FillCombo(IdRestMinutes,{{T(L"しない",L"Never"),0},{T(L"30分",L"30 min"),30},{T(L"45分",L"45 min"),45},{T(L"60分（標準）",L"60 min"),60},{T(L"90分",L"90 min"),90},{T(L"2時間",L"2 hours"),120}},
                      s.restMinutes,Fmt(T(L"{0}分",L"{0} min"),{std::to_wstring(s.restMinutes)}));
            FillCombo(IdFocusMinutes,{{T(L"作業15分",L"Work 15 min"),15},{T(L"作業20分",L"Work 20 min"),20},{T(L"作業25分（標準）",L"Work 25 min"),25},{T(L"作業30分",L"Work 30 min"),30},{T(L"作業45分",L"Work 45 min"),45},{T(L"作業50分",L"Work 50 min"),50},{T(L"作業60分",L"Work 60 min"),60}},
                      s.focusMinutes,Fmt(T(L"作業{0}分",L"Work {0} min"),{std::to_wstring(s.focusMinutes)}));
            FillCombo(IdFocusBreakMinutes,{{T(L"休憩3分",L"Break 3 min"),3},{T(L"休憩5分（標準）",L"Break 5 min"),5},{T(L"休憩10分",L"Break 10 min"),10},{T(L"休憩15分",L"Break 15 min"),15}},
                      s.focusBreakMinutes,Fmt(T(L"休憩{0}分",L"Break {0} min"),{std::to_wstring(s.focusBreakMinutes)}));
            FillCombo(IdChatterMinutes,{{T(L"10分ごと",L"Every 10 min"),10},{T(L"20分ごと",L"Every 20 min"),20},{T(L"30分ごと",L"Every 30 min"),30},{T(L"45分ごと（標準）",L"Every 45 min"),45},{T(L"1時間ごと",L"Every hour"),60},{T(L"1時間半ごと",L"Every 1.5 hours"),90},{T(L"2時間ごと",L"Every 2 hours"),120}},
                      s.chatterMinutes,Fmt(T(L"{0}分ごと",L"Every {0} min"),{std::to_wstring(s.chatterMinutes)}));
            FillCombo(IdBanterMinutes,{{T(L"しない",L"Never"),0},{T(L"1分ごと",L"Every minute"),1},{T(L"10分ごと",L"Every 10 min"),10},{T(L"20分ごと",L"Every 20 min"),20},{T(L"30分ごと（標準）",L"Every 30 min"),30},{T(L"45分ごと",L"Every 45 min"),45},{T(L"1時間ごと",L"Every hour"),60},{T(L"1時間半ごと",L"Every 1.5 hours"),90}},
                      s.banterMinutes,Fmt(T(L"{0}分ごと",L"Every {0} min"),{std::to_wstring(s.banterMinutes)}));
            FillCombo(IdLanguage,{{T(L"自動",L"Auto"),0},{L"日本語",1},{L"English",2}},s.language==L"ja"?1:s.language==L"en"?2:0,L"");
            UpdateInitiative();
            // Down to the tab row, leaving the pane's top line under it.
            RECT row{};
            TabCtrl_GetItemRect(state.tabs,0,&row);
            SetWindowPos(state.tabs,nullptr,0,0,Px(440),row.bottom+2,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            ShowPage(0);
        }

        void BrowseToonDir()
        {
            Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
            if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog))))return;
            DWORD options=0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM);
            dialog->SetTitle(T(L"共有トゥーン（toon01.bmp〜toon10.bmp）のあるフォルダ",L"The folder with the shared toons (toon01.bmp to toon10.bmp)"));
            if(FAILED(dialog->Show(state.window)))return;
            Microsoft::WRL::ComPtr<IShellItem> item;
            PWSTR path=nullptr;
            if(SUCCEEDED(dialog->GetResult(&item))&&SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path)))
            {
                SetDlgItemTextW(state.window,IdToonDir,path);
                CoTaskMemFree(path);
            }
        }

        // Where the voice comes from, as the window shows it now (an engine or VOICEVOX's files).
        // AivisSpeech is at its own address; VOICEVOX at the one in the settings file (another
        // compatible engine's, edited in by hand, is kept).
        VoiceSource CurrentVoiceSource()
        {
            wchar_t folder[MAX_PATH]{};
            GetDlgItemTextW(state.window,IdVoiceFolder,folder,MAX_PATH);
            const int mode=ComboValue(IdVoiceMode,VoiceVoicevox);
            const std::wstring& endpoint=state.choice.settings.voiceEndpoint;
            return {mode==VoiceFiles,mode==VoiceAivisSpeech?AivisSpeechEndpoint:IsAivisSpeech(endpoint)?VoicevoxEndpoint:endpoint,folder,
                    ComboValue(IdVoiceStrength,state.choice.settings.voiceStyleStrength)/100.0};
        }

        // What to start when the engine does not answer.
        const wchar_t* StartEngineHint()
        {
            return ComboValue(IdVoiceMode,VoiceVoicevox)==VoiceAivisSpeech
                ?T(L"\n\nAivisSpeech を起動してから、もう一度押してください。",L"\n\nStart AivisSpeech, then press it again.")
                :T(L"\n\nVOICEVOX（または互換の音声エンジン）を起動してから、もう一度押してください。",L"\n\nStart VOICEVOX (or a compatible voice engine), then press it again.");
        }

        // The folder only matters when VOICEVOX's files are used directly, the style strength
        // with AivisSpeech.
        void UpdateVoiceMode()
        {
            const int mode=ComboValue(IdVoiceMode,VoiceVoicevox);
            EnableWindow(GetDlgItem(state.window,IdVoiceFolder),mode==VoiceFiles);
            EnableWindow(GetDlgItem(state.window,IdVoiceBrowse),mode==VoiceFiles);
            EnableWindow(GetDlgItem(state.window,IdVoiceStrength),mode==VoiceAivisSpeech);
        }

        // A folder picked into the box id.
        void BrowseFolder(int id,const wchar_t* title)
        {
            Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
            if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog))))return;
            DWORD options=0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM);
            dialog->SetTitle(title);
            if(FAILED(dialog->Show(state.window)))return;
            Microsoft::WRL::ComPtr<IShellItem> item;
            PWSTR path=nullptr;
            if(SUCCEEDED(dialog->GetResult(&item))&&SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path)))
            {
                SetDlgItemTextW(state.window,id,path);
                CoTaskMemFree(path);
            }
        }

        // The chosen voice says a short line.
        void PlayVoiceSample()
        {
            const int speaker=ComboValue(IdVoiceList,state.choice.mascot.voiceSpeaker);
            SetCursor(LoadCursorW(nullptr,IDC_WAIT));
            PlaySoundW(nullptr,nullptr,0);
            std::string wav;
            const std::wstring error=SynthesizeVoice(CurrentVoiceSource(),speaker,L"こんにちは。今日もいっしょに、がんばろうね。",wav);
            SetCursor(LoadCursorW(nullptr,IDC_ARROW));
            if(!error.empty())
            {
                MessageBoxW(state.window,(error+StartEngineHint()).c_str(),L"MikuMikuDesktopPerch",MB_ICONWARNING);
                return;
            }
            state.sample=std::move(wav);
            PlaySoundW(reinterpret_cast<LPCWSTR>(state.sample.data()),nullptr,SND_MEMORY|SND_ASYNC|SND_NODEFAULT);
        }

        // The voices the engine (at the endpoint in the settings file) offers.
        void ListVoiceChoices()
        {
            SetCursor(LoadCursorW(nullptr,IDC_WAIT));
            std::vector<std::pair<std::wstring,int>> voices;
            const std::wstring error=ListVoices(CurrentVoiceSource(),voices);
            SetCursor(LoadCursorW(nullptr,IDC_ARROW));
            if(!error.empty())
            {
                const wchar_t* hint=CurrentVoiceSource().direct?T(L"\n\nVOICEVOXをインストールしたフォルダ（VOICEVOX.exe のあるフォルダ）を指定してください。",L"\n\nChoose the folder VOICEVOX is installed in (where VOICEVOX.exe is).")
                                                              :StartEngineHint();
                MessageBoxW(state.window,(error+hint).c_str(),L"MikuMikuDesktopPerch",MB_ICONWARNING);
                return;
            }
            HWND list=GetDlgItem(state.window,IdVoiceList);
            const int current=ComboValue(IdVoiceList,state.choice.mascot.voiceSpeaker);
            SendMessageW(list,CB_RESETCONTENT,0,0);
            for(const auto& [name,id]:voices)
            {
                const auto index=SendMessageW(list,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));
                SendMessageW(list,CB_SETITEMDATA,index,id);
                if(id==current)SendMessageW(list,CB_SETCURSEL,index,0);
            }
            if(SendMessageW(list,CB_GETCURSEL,0,0)==CB_ERR)SendMessageW(list,CB_SETCURSEL,0,0);
            SendMessageW(list,CB_SHOWDROPDOWN,TRUE,0);
        }

        void ListChatModels()
        {
            wchar_t endpoint[512]{},model[256]{};
            GetDlgItemTextW(state.window,IdChatEndpoint,endpoint,512);
            GetDlgItemTextW(state.window,IdChatModel,model,256);
            SetCursor(LoadCursorW(nullptr,IDC_WAIT));
            std::vector<std::wstring> models;
            const std::wstring error=ListModels(endpoint,models);
            SetCursor(LoadCursorW(nullptr,IDC_ARROW));
            if(!error.empty()){MessageBoxW(state.window,error.c_str(),L"MikuMikuDesktopPerch",MB_ICONWARNING);return;}
            HWND combo=GetDlgItem(state.window,IdChatModel);
            SendMessageW(combo,CB_RESETCONTENT,0,0);
            for(const auto& m:models)SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(m.c_str()));
            SetWindowTextW(combo,model[0]?model:models.front().c_str());
            SendMessageW(combo,CB_SHOWDROPDOWN,TRUE,0);
        }

        // The persona dialog: a small window over the settings window, which is disabled
        // meanwhile. OK keeps the edits in state.choice.character (saved with the settings).
        void ClosePersona()
        {
            if(!state.persona)return;
            EnableWindow(state.window,TRUE);
            DestroyWindow(state.persona);
            state.persona=nullptr;
            SetForegroundWindow(state.window);
        }

        // The persona as written (line breaks as \n).
        std::wstring PersonaText(HWND hwnd)
        {
            HWND editor=GetDlgItem(hwnd,IdPersonaText);
            std::wstring text(static_cast<size_t>(GetWindowTextLengthW(editor))+1,L'\0');
            text.resize(static_cast<size_t>(GetWindowTextW(editor,text.data(),static_cast<int>(text.size()))));
            std::erase(text,L'\r');
            return text;
        }

        // How long the persona is, against the limit.
        void ShowPersonaCount(HWND hwnd)
        {
            const size_t count=PersonaText(hwnd).size();
            const std::wstring label=Fmt(count>MaxPersona?T(L"{0} / {1} 文字（超過）",L"{0} / {1} (too long)"):T(L"{0} / {1} 文字",L"{0} / {1} characters"),
                                         {std::to_wstring(count),std::to_wstring(MaxPersona)});
            SetDlgItemTextW(hwnd,IdPersonaCount,label.c_str());
        }

        LRESULT CALLBACK PersonaProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            switch(message)
            {
            case WM_COMMAND:
                switch(LOWORD(wParam))
                {
                case IdPersonaText:
                    if(HIWORD(wParam)==EN_CHANGE)ShowPersonaCount(hwnd);
                    break;
                case IDOK:
                {
                    std::wstring name(256,L'\0');
                    name.resize(static_cast<size_t>(GetDlgItemTextW(hwnd,IdPersonaName,name.data(),256)));
                    const std::wstring text=PersonaText(hwnd);
                    if(text.size()>MaxPersona)
                    {
                        MessageBoxW(hwnd,Fmt(T(L"人物設定は{0}文字までです（いまは{1}文字）。短くしてから、もう一度OKを押してください。\n\n"
                                               L"人物設定は、話しかけるたびにLLMへ送ります。LLMのサーバーが扱える長さ（コンテキスト長）を超えると、人物設定から先に読まれなくなるので、数百〜1,000文字くらいに要点をまとめるのがおすすめです。",
                                               L"A persona can be up to {0} characters (it is {1} now). Shorten it, then press OK again.\n\n"
                                               L"The persona is sent to the LLM every time she is talked to. Beyond the length the LLM server handles (its context length), the persona is the first thing lost, so a few hundred to a thousand characters of the essentials work best."),
                                        {std::to_wstring(MaxPersona),std::to_wstring(text.size())}).c_str(),
                                    L"MikuMikuDesktopPerch",MB_ICONWARNING);
                        SetFocus(GetDlgItem(hwnd,IdPersonaText));
                        return 0;
                    }
                    state.choice.character.name=name;
                    state.choice.character.persona=text.empty()?CharacterProfile::DefaultPersona():text;
                    ClosePersona();
                    return 0;
                }
                case IDCANCEL:
                    ClosePersona();
                    return 0;
                case IdPersonaReset:
                {
                    std::wstring text=CharacterProfile::DefaultPersona();
                    for(size_t n=0;(n=text.find(L'\n',n))!=std::wstring::npos;n+=2)text.insert(n,1,L'\r');
                    SetDlgItemTextW(hwnd,IdPersonaText,text.c_str());
                    return 0;
                }
                }
                break;
            case WM_CLOSE:
                ClosePersona();
                return 0;
            }
            return DefWindowProcW(hwnd,message,wParam,lParam);
        }

        void OpenPersona()
        {
            if(state.persona){SetForegroundWindow(state.persona);return;}
            static bool registered=false;
            const HINSTANCE instance=GetModuleHandleW(nullptr);
            if(!registered)
            {
                WNDCLASSEXW wc{sizeof(wc)};
                wc.lpfnWndProc=PersonaProc;
                wc.hInstance=instance;
                wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
                wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);
                wc.lpszClassName=PersonaClass;
                if(!RegisterClassExW(&wc))return;
                registered=true;
            }
            const DWORD style=WS_CAPTION|WS_SYSMENU;
            const DWORD exStyle=WS_EX_CONTROLPARENT|WS_EX_DLGMODALFRAME;
            RECT frame{0,0,Px(460),Px(330)};
            AdjustWindowRectExForDpi(&frame,style,FALSE,exStyle,state.dpi);
            RECT owner{};
            GetWindowRect(state.window,&owner);
            state.persona=CreateWindowExW(exStyle,PersonaClass,T(L"人物設定（このモデル）",L"Persona (this model)"),style,owner.left+Px(20),owner.top+Px(60),
                                          frame.right-frame.left,frame.bottom-frame.top,state.window,nullptr,instance,nullptr);
            if(!state.persona)return;
            const HWND p=state.persona;
            AddTo(p,L"STATIC",T(L"名前",L"Name"),SS_LEFT,16,19,80,20);
            AddTo(p,L"EDIT",state.choice.character.name.c_str(),ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,100,16,344,24,IdPersonaName);
            AddTo(p,L"STATIC",T(L"性格や話し方を自由に書いてください。返事が吹き出しに収まる短い話し言葉になるよう、アプリが指示を足します。",
                                    L"Write her personality and way of talking freely. The app adds instructions so replies stay short enough for the bubble."),
                  SS_LEFT,16,50,428,40);
            std::wstring text=state.choice.character.persona;
            for(size_t n=0;(n=text.find(L'\n',n))!=std::wstring::npos;n+=2)text.insert(n,1,L'\r');
            HWND editor=AddTo(p,L"EDIT",text.c_str(),ES_MULTILINE|ES_WANTRETURN|ES_AUTOVSCROLL|WS_VSCROLL|WS_BORDER|WS_TABSTOP,16,92,428,180,IdPersonaText);
            SendMessageW(editor,EM_SETLIMITTEXT,MaxPersona*4,0);   // more is let in, so OK can say what is too long
            AddTo(p,L"BUTTON",T(L"既定に戻す",L"Default"),BS_PUSHBUTTON|WS_TABSTOP,16,286,100,28,IdPersonaReset);
            AddTo(p,L"STATIC",L"",SS_LEFT,124,292,144,20,IdPersonaCount);
            ShowPersonaCount(p);
            AddTo(p,L"BUTTON",L"OK",BS_DEFPUSHBUTTON|WS_TABSTOP,272,286,82,28,IDOK);
            AddTo(p,L"BUTTON",T(L"キャンセル",L"Cancel"),BS_PUSHBUTTON|WS_TABSTOP,362,286,82,28,IDCANCEL);
            EnableWindow(state.window,FALSE);
            ShowWindow(p,SW_SHOWNORMAL);
            SetForegroundWindow(p);
            SetFocus(GetDlgItem(p,IdPersonaName));
        }

        // The gesture choice: a checked list of the gestures and built-in idle motions picked
        // while standing. OK keeps it in state.choice.settings.disabledGestures (saved with the
        // settings); keys not in the list (a motion no longer built in) stay as they were.
        void CloseGestures()
        {
            if(!state.gestures)return;
            EnableWindow(state.window,TRUE);
            DestroyWindow(state.gestures);
            state.gestures=nullptr;
            SetForegroundWindow(state.window);
        }

        void CheckAllGestures(HWND hwnd,bool on)
        {
            HWND list=GetDlgItem(hwnd,IdGestureList);
            const int count=static_cast<int>(SendMessageW(list,LVM_GETITEMCOUNT,0,0));
            for(int i=0;i<count;++i)ListView_SetCheckState(list,i,on);
        }

        LRESULT CALLBACK GesturesProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            switch(message)
            {
            case WM_COMMAND:
                switch(LOWORD(wParam))
                {
                case IDOK:
                {
                    HWND list=GetDlgItem(hwnd,IdGestureList);
                    auto& disabled=state.choice.settings.disabledGestures;
                    std::erase_if(disabled,[](const std::wstring& key){
                        return std::any_of(state.choice.gestures.begin(),state.choice.gestures.end(),[&](const auto& g){return g.first==key;});});
                    for(size_t i=0;i<state.choice.gestures.size();++i)
                        if(!ListView_GetCheckState(list,static_cast<int>(i)))disabled.push_back(state.choice.gestures[i].first);
                    CloseGestures();
                    return 0;
                }
                case IDCANCEL:
                    CloseGestures();
                    return 0;
                case IdGestureAll:
                    CheckAllGestures(hwnd,true);
                    return 0;
                case IdGestureNone:
                    CheckAllGestures(hwnd,false);
                    return 0;
                }
                break;
            case WM_CLOSE:
                CloseGestures();
                return 0;
            }
            return DefWindowProcW(hwnd,message,wParam,lParam);
        }

        void OpenGestures()
        {
            if(state.gestures){SetForegroundWindow(state.gestures);return;}
            static bool registered=false;
            const HINSTANCE instance=GetModuleHandleW(nullptr);
            if(!registered)
            {
                WNDCLASSEXW wc{sizeof(wc)};
                wc.lpfnWndProc=GesturesProc;
                wc.hInstance=instance;
                wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
                wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);
                wc.lpszClassName=GesturesClass;
                if(!RegisterClassExW(&wc))return;
                registered=true;
            }
            const DWORD style=WS_CAPTION|WS_SYSMENU;
            const DWORD exStyle=WS_EX_CONTROLPARENT|WS_EX_DLGMODALFRAME;
            RECT frame{0,0,Px(360),Px(520)};
            AdjustWindowRectExForDpi(&frame,style,FALSE,exStyle,state.dpi);
            RECT owner{};
            GetWindowRect(state.window,&owner);
            state.gestures=CreateWindowExW(exStyle,GesturesClass,T(L"待機中のしぐさ",L"Idle gestures"),style,owner.left+Px(50),owner.top+Px(40),
                                           frame.right-frame.left,frame.bottom-frame.top,state.window,nullptr,instance,nullptr);
            if(!state.gestures)return;
            const HWND p=state.gestures;
            AddTo(p,L"STATIC",T(L"立っているときに、チェックしたものの中からランダムに選びます。",L"While standing, she picks at random among the checked ones."),
                  SS_LEFT,16,12,328,36);
            HWND list=AddTo(p,WC_LISTVIEWW,L"",LVS_REPORT|LVS_NOCOLUMNHEADER|LVS_SINGLESEL|LVS_SHOWSELALWAYS|WS_BORDER|WS_TABSTOP,16,50,328,404,IdGestureList);
            ListView_SetExtendedListViewStyle(list,LVS_EX_CHECKBOXES|LVS_EX_FULLROWSELECT);
            LVCOLUMNW column{LVCF_WIDTH};
            column.cx=Px(300);
            ListView_InsertColumn(list,0,&column);
            const auto& disabled=state.choice.settings.disabledGestures;
            for(size_t i=0;i<state.choice.gestures.size();++i)
            {
                LVITEMW item{LVIF_TEXT};
                item.iItem=static_cast<int>(i);
                item.pszText=const_cast<wchar_t*>(state.choice.gestures[i].second.c_str());
                ListView_InsertItem(list,&item);
                ListView_SetCheckState(list,static_cast<int>(i),std::find(disabled.begin(),disabled.end(),state.choice.gestures[i].first)==disabled.end());
            }
            AddTo(p,L"BUTTON",T(L"すべて選ぶ",L"All"),BS_PUSHBUTTON|WS_TABSTOP,16,470,80,28,IdGestureAll);
            AddTo(p,L"BUTTON",T(L"すべて外す",L"None"),BS_PUSHBUTTON|WS_TABSTOP,102,470,80,28,IdGestureNone);
            AddTo(p,L"BUTTON",L"OK",BS_DEFPUSHBUTTON|WS_TABSTOP,198,470,70,28,IDOK);
            AddTo(p,L"BUTTON",T(L"キャンセル",L"Cancel"),BS_PUSHBUTTON|WS_TABSTOP,274,470,70,28,IDCANCEL);
            EnableWindow(state.window,FALSE);
            ShowWindow(p,SW_SHOWNORMAL);
            SetForegroundWindow(p);
            SetFocus(list);
        }

        // The memory dialog: what the character has learnt about the user, newest last.
        // Deleting and adding take effect (and are saved) at once.
        void FillMemoryList()
        {
            if(!state.memory||!state.choice.memory)return;
            HWND list=GetDlgItem(state.memory,IdMemoryList);
            const int top=static_cast<int>(SendMessageW(list,LB_GETTOPINDEX,0,0));
            SendMessageW(list,WM_SETREDRAW,FALSE,0);
            SendMessageW(list,LB_RESETCONTENT,0,0);
            int widest=0;
            HDC dc=GetDC(list);
            const HGDIOBJ old=SelectObject(dc,state.font);
            for(const auto& e:state.choice.memory->Entries())
            {
                const std::wstring line=(e.date.empty()?std::wstring(L"----------"):e.date)+L"　"+e.text;
                const auto index=SendMessageW(list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(line.c_str()));
                SendMessageW(list,LB_SETITEMDATA,index,e.id);
                SIZE size{};
                GetTextExtentPoint32W(dc,line.c_str(),static_cast<int>(line.size()),&size);
                widest=(std::max)(widest,static_cast<int>(size.cx));
            }
            SelectObject(dc,old);
            ReleaseDC(list,dc);
            SendMessageW(list,LB_SETHORIZONTALEXTENT,widest+Px(8),0);
            SendMessageW(list,LB_SETTOPINDEX,(std::max)(0,top),0);
            SendMessageW(list,WM_SETREDRAW,TRUE,0);
            InvalidateRect(list,nullptr,TRUE);
            const std::wstring count=Fmt(T(L"{0}件",L"{0} notes"),{std::to_wstring(state.choice.memory->Entries().size())});
            SetDlgItemTextW(state.memory,IdMemoryCount,count.c_str());
        }

        void SaveMemory()
        {
            if(state.choice.memory&&!state.choice.memory->Save())
                MessageBoxW(state.memory,T(L"記憶を保存できませんでした。",L"Could not save the memory."),L"MikuMikuDesktopPerch",MB_ICONERROR);
            FillMemoryList();
        }

        void CloseMemory()
        {
            if(!state.memory)return;
            EnableWindow(state.window,TRUE);
            DestroyWindow(state.memory);
            state.memory=nullptr;
            SetForegroundWindow(state.window);
        }

        LRESULT CALLBACK MemoryProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            CharacterMemory* memory=state.choice.memory;
            switch(message)
            {
            case WM_COMMAND:
                switch(LOWORD(wParam))
                {
                case IdMemoryAdd:
                {
                    std::wstring text(CharacterMemory::MaxLength+1,L'\0');
                    text.resize(static_cast<size_t>(GetDlgItemTextW(hwnd,IdMemoryNew,text.data(),static_cast<int>(text.size()))));
                    if(memory&&memory->Add(text,TodayDate()))
                    {
                        SetDlgItemTextW(hwnd,IdMemoryNew,L"");
                        SaveMemory();
                        HWND list=GetDlgItem(hwnd,IdMemoryList);
                        SendMessageW(list,LB_SETTOPINDEX,SendMessageW(list,LB_GETCOUNT,0,0)-1,0);
                    }
                    SetFocus(GetDlgItem(hwnd,IdMemoryNew));
                    return 0;
                }
                case IdMemoryDelete:
                {
                    HWND list=GetDlgItem(hwnd,IdMemoryList);
                    const int selected=static_cast<int>(SendMessageW(list,LB_GETSELCOUNT,0,0));
                    if(!memory||selected<=0)return 0;
                    std::vector<int> rows(static_cast<size_t>(selected));
                    SendMessageW(list,LB_GETSELITEMS,selected,reinterpret_cast<LPARAM>(rows.data()));
                    for(const int row:rows)memory->Remove(static_cast<int>(SendMessageW(list,LB_GETITEMDATA,row,0)));
                    SaveMemory();
                    return 0;
                }
                case IdMemoryClear:
                    if(memory&&!memory->Entries().empty()&&
                       MessageBoxW(hwnd,T(L"このモデルの記憶をすべて削除します。よろしいですか？",L"Delete everything this model remembers?"),L"MikuMikuDesktopPerch",MB_ICONWARNING|MB_OKCANCEL|MB_DEFBUTTON2)==IDOK)
                    {
                        memory->Clear();
                        SaveMemory();
                    }
                    return 0;
                case IDOK:
                    // Enter in the new-note box adds it; anywhere else it closes.
                    if(GetFocus()==GetDlgItem(hwnd,IdMemoryNew)){SendMessageW(hwnd,WM_COMMAND,IdMemoryAdd,0);return 0;}
                    CloseMemory();
                    return 0;
                case IDCANCEL:
                    CloseMemory();
                    return 0;
                }
                break;
            case WM_CLOSE:
                CloseMemory();
                return 0;
            }
            return DefWindowProcW(hwnd,message,wParam,lParam);
        }

        void OpenMemory()
        {
            if(state.memory){SetForegroundWindow(state.memory);return;}
            if(!state.choice.memory)return;
            static bool registered=false;
            const HINSTANCE instance=GetModuleHandleW(nullptr);
            if(!registered)
            {
                WNDCLASSEXW wc{sizeof(wc)};
                wc.lpfnWndProc=MemoryProc;
                wc.hInstance=instance;
                wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
                wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);
                wc.lpszClassName=MemoryClass;
                if(!RegisterClassExW(&wc))return;
                registered=true;
            }
            const DWORD style=WS_CAPTION|WS_SYSMENU;
            const DWORD exStyle=WS_EX_CONTROLPARENT|WS_EX_DLGMODALFRAME;
            RECT frame{0,0,Px(540),Px(420)};
            AdjustWindowRectExForDpi(&frame,style,FALSE,exStyle,state.dpi);
            RECT owner{};
            GetWindowRect(state.window,&owner);
            const std::wstring title=Fmt(T(L"記憶（{0}）",L"Memory ({0})"),{state.choice.character.name});
            state.memory=CreateWindowExW(exStyle,MemoryClass,title.c_str(),style,owner.left+Px(10),owner.top+Px(60),
                                         frame.right-frame.left,frame.bottom-frame.top,state.window,nullptr,instance,nullptr);
            if(!state.memory)return;
            const HWND m=state.memory;
            AddTo(m,L"STATIC",T(L"会話から覚えたことです。次の会話から、話に関係のあるものを思い出します。削除や追加はすぐ保存されます"
                  L"（このコンピューターの中だけに保存し、外部には送りません）。",
                  L"What she has learnt from your conversations; she recalls what fits the talk. Deleting and adding are saved at once "
                  L"(only on this computer, never sent anywhere)."),
                  SS_LEFT,16,12,508,40);
            AddTo(m,L"LISTBOX",L"",LBS_EXTENDEDSEL|LBS_NOINTEGRALHEIGHT|LBS_NOTIFY|WS_VSCROLL|WS_HSCROLL|WS_BORDER|WS_TABSTOP,16,56,508,236,IdMemoryList);
            AddTo(m,L"STATIC",L"",SS_LEFT,16,298,120,20,IdMemoryCount);
            AddTo(m,L"BUTTON",T(L"選んだものを削除",L"Delete selected"),BS_PUSHBUTTON|WS_TABSTOP,296,296,130,28,IdMemoryDelete);
            AddTo(m,L"BUTTON",T(L"すべて削除",L"Delete all"),BS_PUSHBUTTON|WS_TABSTOP,434,296,90,28,IdMemoryClear);
            HWND input=AddTo(m,L"EDIT",L"",ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,16,340,410,24,IdMemoryNew);
            SendMessageW(input,EM_SETLIMITTEXT,CharacterMemory::MaxLength,0);
            SendMessageW(input,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(T(L"覚えてほしいこと（例: 話し相手はコーヒーが好き）",L"Something to remember (e.g. The user likes coffee)")));
            AddTo(m,L"BUTTON",T(L"追加",L"Add"),BS_PUSHBUTTON|WS_TABSTOP,434,338,90,28,IdMemoryAdd);
            AddTo(m,L"BUTTON",T(L"閉じる",L"Close"),BS_PUSHBUTTON|WS_TABSTOP,434,380,90,28,IDCANCEL);
            FillMemoryList();
            HWND list=GetDlgItem(m,IdMemoryList);
            SendMessageW(list,LB_SETTOPINDEX,(std::max<LRESULT>)(0,SendMessageW(list,LB_GETCOUNT,0,0)-1),0);
            EnableWindow(state.window,FALSE);
            ShowWindow(m,SW_SHOWNORMAL);
            SetForegroundWindow(m);
            SetFocus(list);
        }

        SettingsChoice Read()
        {
            SettingsChoice c=state.choice;
            Settings& s=c.settings;
            CharacterSettings& own=c.mascot;
            s.autoBehavior=Checked(IdAuto);
            s.windowMode=Checked(IdWindowMode);
            s.physics=Checked(IdPhysics);
            s.blink=Checked(IdBlink);
            s.breath=Checked(IdBreath);
            s.lookAtMouse=Checked(IdLookAt);
            s.scale=ComboValue(IdScale,s.scale);
            s.edgeScale=ComboValue(IdEdge,static_cast<int>(std::lround(s.edgeScale*100)))/100.0f;
            s.topmost=Checked(IdTopmost);
            s.selfShadow=Checked(IdSelfShadow);
            s.lightAzimuth=SliderValue(IdSunAzimuth);
            s.lightElevation=SliderValue(IdSunElevation);
            c.startWithWindows=Checked(IdStartup);
            s.fps=ComboValue(IdFps,s.fps);
            s.motionBlur=Checked(IdMotionBlur);
            s.msaa=ComboValue(IdMsaa,s.msaa);
            s.maxTextureSize=ComboValue(IdTexture,s.maxTextureSize);
            s.physicsFps=ComboValue(IdPhysicsFps,s.physicsFps);
            s.physicsIterations=ComboValue(IdPhysicsIterations,s.physicsIterations);
            s.physicsFilter=ComboValue(IdPhysicsFilter,s.physicsFilter);
            wchar_t toon[MAX_PATH]{};
            GetDlgItemTextW(state.window,IdToonDir,toon,MAX_PATH);
            s.toonDir=toon;
            s.gpu=GpuValue();
            wchar_t endpoint[512]{},model[256]{};
            GetDlgItemTextW(state.window,IdChatEndpoint,endpoint,512);
            GetDlgItemTextW(state.window,IdChatModel,model,256);
            auto trim=[](std::wstring t){
                const size_t a=t.find_first_not_of(L" \t"),b=t.find_last_not_of(L" \t");
                return a==std::wstring::npos?std::wstring():t.substr(a,b-a+1);
            };
            s.chatEndpoint=trim(endpoint);
            s.chatModel=trim(model);
            wchar_t user[128]{};
            GetDlgItemTextW(state.window,IdUserName,user,128);
            s.userName=trim(user);
            s.chatMemory=Checked(IdChatMemory);
            s.bond=Checked(IdBond);
            s.chatInitiative=Checked(IdChatInitiative);
            s.chatterMinutes=ComboValue(IdChatterMinutes,s.chatterMinutes);
            s.banterMinutes=ComboValue(IdBanterMinutes,s.banterMinutes);
            s.updateCheck=Checked(IdUpdateCheck);
            s.api=Checked(IdApi);
            if(const int p=static_cast<int>(GetDlgItemInt(state.window,IdApiPort,nullptr,FALSE));p>=1024&&p<=65535)s.apiPort=p;
            s.morningSummary=Checked(IdMorningSummary);
            s.albumHotkey=static_cast<int>(SendDlgItemMessageW(state.window,IdAlbumHotkey,HKM_GETHOTKEY,0,0)&0xFFFF);
            s.shutterSound=Checked(IdShutterSound);
            {
                std::wstring folder(MAX_PATH*2,L'\0');
                folder.resize(GetDlgItemTextW(state.window,IdAlbumFolder,folder.data(),static_cast<int>(folder.size())));
                const size_t a=folder.find_first_not_of(L" \t\""),b=folder.find_last_not_of(L" \t\"");
                s.albumFolder=a==std::wstring::npos?std::wstring():folder.substr(a,b-a+1);
            }
            s.restMinutes=ComboValue(IdRestMinutes,s.restMinutes);
            s.focusMinutes=ComboValue(IdFocusMinutes,s.focusMinutes);
            s.focusBreakMinutes=ComboValue(IdFocusBreakMinutes,s.focusBreakMinutes);
            {
                std::wstring address(2048,L'\0');
                address.resize(static_cast<size_t>(GetDlgItemTextW(state.window,IdCalendarAddress,address.data(),static_cast<int>(address.size()))));
                s.calendarAddress=trim(address);
            }
            s.calendarLeadMinutes=ComboValue(IdCalendarLead,s.calendarLeadMinutes);
            {
                const int language=ComboValue(IdLanguage,0);
                s.language=language==1?L"ja":language==2?L"en":L"";
            }
            s.voice=Checked(IdVoice);
            {
                const VoiceSource source=CurrentVoiceSource();
                s.voiceDirect=source.direct;
                s.voiceEndpoint=source.endpoint;
                s.voiceStyleStrength=static_cast<int>(source.styleStrength*100.0+0.5);
                s.voiceFolder=source.folder;
            }
            own.voiceSpeaker=ComboValue(IdVoiceList,own.voiceSpeaker);
            {
                HWND list=GetDlgItem(state.window,IdVoiceList);
                const auto index=SendMessageW(list,CB_GETCURSEL,0,0);
                if(index!=CB_ERR)
                {
                    const auto length=static_cast<size_t>(SendMessageW(list,CB_GETLBTEXTLEN,index,0));
                    std::wstring name(length+1,L'\0');
                    SendMessageW(list,CB_GETLBTEXT,index,reinterpret_cast<LPARAM>(name.data()));
                    name.resize(length);
                    own.voiceName=name;
                }
            }
            return c;
        }

        LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            switch(message)
            {
            case WM_COMMAND:
                switch(LOWORD(wParam))
                {
                case IDOK:
                {
                    const SettingsChoice chosen=Read();
                    if(const auto problem=CheckLocalEndpoint(chosen.settings.chatEndpoint);!chosen.settings.chatEndpoint.empty()&&!problem.empty())
                    {
                        MessageBoxW(hwnd,problem.c_str(),L"MikuMikuDesktopPerch",MB_ICONWARNING);
                        SetFocus(GetDlgItem(hwnd,IdChatEndpoint));
                        return 0;
                    }
                    auto apply=state.onApply;
                    DestroyWindow(hwnd);
                    if(apply)apply(chosen);
                    return 0;
                }
                case IDCANCEL:
                    if(state.onLight)state.onLight(state.choice.settings.lightAzimuth,state.choice.settings.lightElevation);
                    DestroyWindow(hwnd);
                    return 0;
                case IdToonBrowse:
                    BrowseToonDir();
                    return 0;
                case IdChatList:
                    ListChatModels();
                    return 0;
                case IdVoiceFetch:
                    ListVoiceChoices();
                    return 0;
                case IdVoiceSample:
                    PlayVoiceSample();
                    return 0;
                case IdGestures:
                    OpenGestures();
                    return 0;
                case IdUpdateNow:
                    if(state.onCheckUpdate)state.onCheckUpdate();
                    return 0;
                case IdOpenAlbum:
                    if(const std::wstring folder=AlbumFolder(true);!folder.empty())
                        ShellExecuteW(state.window,L"open",folder.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
                    return 0;
                case IdVoiceMode:
                    if(HIWORD(wParam)==CBN_SELCHANGE)UpdateVoiceMode();
                    return 0;
                case IdChatInitiative:
                    if(HIWORD(wParam)==BN_CLICKED)UpdateInitiative();
                    return 0;
                case IdVoiceBrowse:
                    BrowseFolder(IdVoiceFolder,T(L"VOICEVOXをインストールしたフォルダ（VOICEVOX.exe のあるフォルダ）",L"The folder VOICEVOX is installed in (where VOICEVOX.exe is)"));
                    return 0;
                case IdAlbumBrowse:
                    BrowseFolder(IdAlbumFolder,T(L"アルバムの写真を保存するフォルダ",L"The folder to keep the album's pictures in"));
                    return 0;
                case IdPersona:
                    OpenPersona();
                    return 0;
                case IdMemory:
                    OpenMemory();
                    return 0;
                }
                break;
            case WM_NOTIFY:
                if(const auto* n=reinterpret_cast<const NMHDR*>(lParam);n->idFrom==IdTabs&&n->code==TCN_SELCHANGE)ShowPage(TabCtrl_GetCurSel(state.tabs));
                return 0;
            case WM_HSCROLL:
                if(state.onLight)state.onLight(SliderValue(IdSunAzimuth),SliderValue(IdSunElevation));
                return 0;
            case WM_CLOSE:
                if(state.onLight)state.onLight(state.choice.settings.lightAzimuth,state.choice.settings.lightElevation);
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                if(state.persona){DestroyWindow(state.persona);state.persona=nullptr;}
                if(state.memory){DestroyWindow(state.memory);state.memory=nullptr;}
                if(state.gestures){DestroyWindow(state.gestures);state.gestures=nullptr;}
                state.window=nullptr;
                state.tabs=nullptr;
                state.pages.clear();
                if(state.font){DeleteObject(state.font);state.font=nullptr;}
                state.onApply=nullptr;
                state.onLight=nullptr;
                state.onCheckUpdate=nullptr;
                if(!state.sample.empty()){PlaySoundW(nullptr,nullptr,0);state.sample.clear();}
                return 0;
            }
            return DefWindowProcW(hwnd,message,wParam,lParam);
        }
    }

    void ShowSettingsWindow(HWND owner,const SettingsChoice& current,std::function<void(const SettingsChoice&)> onApply,
                            std::function<void(int azimuth,int elevation)> onLight,std::function<void()> onCheckUpdate)
    {
        if(state.window)
        {
            ShowWindow(state.window,SW_SHOWNORMAL);
            SetForegroundWindow(state.window);
            return;
        }
        static bool registered=false;
        const HINSTANCE instance=GetModuleHandleW(nullptr);
        if(!registered)
        {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc=Proc;
            wc.hInstance=instance;
            wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
            wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);
            wc.lpszClassName=ClassName;
            wc.hIcon=reinterpret_cast<HICON>(GetClassLongPtrW(owner,GCLP_HICON));
            wc.hIconSm=reinterpret_cast<HICON>(GetClassLongPtrW(owner,GCLP_HICONSM));
            if(!RegisterClassExW(&wc))return;
            registered=true;
        }
        state.choice=current;
        state.onApply=std::move(onApply);
        state.onLight=std::move(onLight);
        state.onCheckUpdate=std::move(onCheckUpdate);
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_BAR_CLASSES|ICC_LISTVIEW_CLASSES|ICC_TAB_CLASSES|ICC_HOTKEY_CLASS};
        InitCommonControlsEx(&controls);
        state.dpi=static_cast<int>(GetDpiForSystem());
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0,state.dpi);
        state.font=CreateFontIndirectW(&metrics.lfMessageFont);

        // Client 460 x 400 logical pixels, centred on the screen the cursor is on.
        const DWORD style=WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
        const DWORD exStyle=WS_EX_CONTROLPARENT|WS_EX_DLGMODALFRAME;
        RECT frame{0,0,Px(460),Px(400)};
        AdjustWindowRectExForDpi(&frame,style,FALSE,exStyle,state.dpi);
        POINT cursor{};
        GetCursorPos(&cursor);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST),&monitor);
        const RECT& work=monitor.rcWork;
        const int w=frame.right-frame.left,h=frame.bottom-frame.top;
        // Owned by the mascot's window: kept above it (it is topmost) and gone with it.
        const std::wstring title=Fmt(T(L"MikuMikuDesktopPerch の設定（{0}）",L"MikuMikuDesktopPerch settings ({0})"),{current.character.name});
        state.window=CreateWindowExW(exStyle,ClassName,title.c_str(),style,
                                     work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,
                                     owner,nullptr,instance,nullptr);
        if(!state.window)return;
        Build();
        ShowWindow(state.window,SW_SHOWNORMAL);
        SetForegroundWindow(state.window);
        SetFocus(GetDlgItem(state.window,IdAuto));
    }

    HWND SettingsWindowHandle(){return state.persona?state.persona:state.memory?state.memory:state.gestures?state.gestures:state.window;}

    void RefreshMemoryWindow(){FillMemoryList();}
}
