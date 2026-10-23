#include "SettingsWindow.hpp"
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
            IdFps,IdMsaa,IdTexture,IdPhysicsFps,IdToonDir,IdToonBrowse,
            IdChatEndpoint,IdChatModel,IdChatList,IdPersona,IdPersonaName,IdUserName,
            IdPersonaText,IdPersonaReset,IdChatMemory,IdMemory,IdSelfShadow,IdChatInitiative,IdVoice,IdVoiceList,IdVoiceFetch,IdVoiceSample,IdVoiceMode,IdVoiceFolder,IdVoiceBrowse,IdSunAzimuth,IdSunElevation,
            IdMemoryList,IdMemoryNew,IdMemoryAdd,IdMemoryDelete,IdMemoryClear,IdMemoryCount
        };
        constexpr wchar_t PersonaClass[]=L"MikuMikuDesktopPerchPersona";
        constexpr wchar_t MemoryClass[]=L"MikuMikuDesktopPerchMemory";

        struct State
        {
            HWND window=nullptr;
            HWND persona=nullptr;   // the persona dialog, while open
            HWND memory=nullptr;    // the memory dialog, while open
            HFONT font=nullptr;
            SettingsChoice choice;
            std::function<void(const SettingsChoice&)> onApply;
            std::function<void(int,int)> onLight;
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
        {return AddTo(state.window,cls,text,style,x,y,w,h,id);}

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

        void Build()
        {
            const Settings& s=state.choice.settings;
            int y=10;
            Group(L"動き",y,128);
            CheckBox(IdAuto,L"おまかせ行動（ひとりで歩き回る）",28,y+22,400,s.autoBehavior);
            CheckBox(IdWindowMode,L"アクティブウィンドウに乗る",28,y+46,400,s.windowMode);
            CheckBox(IdPhysics,L"物理演算（髪やスカートの揺れ）",28,y+70,400,s.physics);
            CheckBox(IdBlink,L"まばたき",28,y+94,120,s.blink);
            CheckBox(IdBreath,L"呼吸",156,y+94,120,s.breath);
            CheckBox(IdLookAt,L"マウスの方を見る",284,y+94,150,s.lookAtMouse);
            y+=138;
            Group(L"表示",y,156);
            Label(L"大きさ",28,y+22,130);
            Combo(IdScale,170,y+22,120);
            Label(L"輪郭線の太さ",28,y+50,130);
            Combo(IdEdge,170,y+50,120);
            CheckBox(IdTopmost,L"最前面に表示",28,y+78,200,s.topmost);
            CheckBox(IdSelfShadow,L"セルフシャドウ（体に落ちる影）",250,y+78,190,s.selfShadow);
            CheckBox(IdStartup,L"Windowsの起動時に起動",28,y+100,300,state.choice.startWithWindows);
            Label(L"太陽の位置（左右・高さ）",28,y+128,140);
            Slider(IdSunAzimuth,170,y+126,130,-180,180,s.lightAzimuth);
            Slider(IdSunElevation,306,y+126,130,5,90,s.lightElevation);
            y+=166;
            Group(L"会話（このコンピューターのLLM：Ollama、LM Studio など）",y,224);
            Label(L"接続先",28,y+22,130);
            Add(L"EDIT",s.chatEndpoint.c_str(),ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,170,y+22,266,24,IdChatEndpoint);
            Label(L"モデル",28,y+52,130);
            Add(L"COMBOBOX",L"",CBS_DROPDOWN|CBS_AUTOHSCROLL|WS_VSCROLL|WS_TABSTOP,170,y+52,190,240,IdChatModel);
            SetDlgItemTextW(state.window,IdChatModel,s.chatModel.c_str());
            Add(L"BUTTON",L"一覧を取得",BS_PUSHBUTTON|WS_TABSTOP,366,y+51,70,26,IdChatList);
            Label(L"あなたの呼び名",28,y+82,130);
            Add(L"EDIT",s.userName.c_str(),ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,170,y+82,190,24,IdUserName);
            SendDlgItemMessageW(state.window,IdUserName,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"例: たろう"));
            CheckBox(IdChatMemory,L"会話の内容を覚える",28,y+112,140,s.chatMemory);
            Add(L"BUTTON",L"人物設定...",BS_PUSHBUTTON|WS_TABSTOP,170,y+110,110,26,IdPersona);
            Add(L"BUTTON",L"記憶...",BS_PUSHBUTTON|WS_TABSTOP,286,y+110,110,26,IdMemory);
            if(!state.choice.memory)EnableWindow(GetDlgItem(state.window,IdMemory),FALSE);
            CheckBox(IdChatInitiative,L"ときどき自分から話しかける（あいさつ、おかえり、ひと言）",28,y+138,410,s.chatInitiative);
            CheckBox(IdVoice,L"声で話す（VOICEVOX）",28,y+166,140,s.voice);
            Combo(IdVoiceList,170,y+164,128);
            {
                HWND list=GetDlgItem(state.window,IdVoiceList);
                const std::wstring label=s.voiceName.empty()?L"声 "+std::to_wstring(s.voiceSpeaker):s.voiceName;
                const auto index=SendMessageW(list,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
                SendMessageW(list,CB_SETITEMDATA,index,s.voiceSpeaker);
                SendMessageW(list,CB_SETCURSEL,index,0);
            }
            Add(L"BUTTON",L"試聴",BS_PUSHBUTTON|WS_TABSTOP,304,y+163,56,26,IdVoiceSample);
            Add(L"BUTTON",L"一覧を取得",BS_PUSHBUTTON|WS_TABSTOP,366,y+163,70,26,IdVoiceFetch);
            Combo(IdVoiceMode,46,y+192,120);
            FillCombo(IdVoiceMode,{{L"エンジンを使う",0},{L"ファイルを直接使う",1}},s.voiceDirect?1:0,L"");
            Add(L"EDIT",s.voiceFolder.c_str(),ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,170,y+192,190,24,IdVoiceFolder);
            SendDlgItemMessageW(state.window,IdVoiceFolder,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"VOICEVOXのフォルダ"));
            Add(L"BUTTON",L"参照...",BS_PUSHBUTTON|WS_TABSTOP,366,y+191,70,26,IdVoiceBrowse);
            UpdateVoiceMode();
            y+=234;
            Group(L"詳細（変更すると再起動します）",y,166);
            Label(L"描画のfps",28,y+22,130);
            Combo(IdFps,170,y+22,120);
            Label(L"アンチエイリアス",28,y+50,130);
            Combo(IdMsaa,170,y+50,120);
            Label(L"テクスチャの最大サイズ",28,y+78,140);
            Combo(IdTexture,170,y+78,120);
            Label(L"物理演算の更新",28,y+106,130);
            Combo(IdPhysicsFps,170,y+106,120);
            Label(L"共有トゥーンのフォルダ",28,y+134,140);
            Add(L"EDIT",s.toonDir.c_str(),ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,170,y+134,190,24,IdToonDir);
            Add(L"BUTTON",L"参照...",BS_PUSHBUTTON|WS_TABSTOP,366,y+133,70,26,IdToonBrowse);
            y+=176;
            Add(L"BUTTON",L"OK",BS_DEFPUSHBUTTON|WS_TABSTOP,276,y,82,28,IDOK);
            Add(L"BUTTON",L"キャンセル",BS_PUSHBUTTON|WS_TABSTOP,366,y,82,28,IDCANCEL);

            std::vector<std::pair<std::wstring,int>> scales;
            for(int p=Settings::MinScale;p<=Settings::MaxScale;p+=10)scales.push_back({std::to_wstring(p)+L"%",p});
            FillCombo(IdScale,scales,s.scale,std::to_wstring(s.scale)+L"%");
            const int edge=static_cast<int>(std::lround(s.edgeScale*100));
            FillCombo(IdEdge,{{L"なし",0},{L"細い（0.5倍）",50},{L"やや細い（0.75倍）",75},{L"標準",100},{L"やや太い（1.25倍）",125},{L"太い（1.5倍）",150},{L"とても太い（2倍）",200}},
                      edge,std::to_wstring(edge/100.0).substr(0,4)+L"倍");
            FillCombo(IdFps,{{L"15",15},{L"20",20},{L"30（標準）",30},{L"45",45},{L"60",60}},s.fps,std::to_wstring(s.fps));
            FillCombo(IdMsaa,{{L"なし",1},{L"2x",2},{L"4x（標準）",4},{L"8x",8}},s.msaa,std::to_wstring(s.msaa)+L"x");
            FillCombo(IdTexture,{{L"512",512},{L"1024（標準）",1024},{L"2048",2048},{L"4096",4096},{L"縮小しない",0}},s.maxTextureSize,std::to_wstring(s.maxTextureSize));
            FillCombo(IdPhysicsFps,{{L"30回/秒",30},{L"60回/秒（標準）",60},{L"90回/秒",90},{L"120回/秒",120}},s.physicsFps,std::to_wstring(s.physicsFps)+L"回/秒");
        }

        void BrowseToonDir()
        {
            Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
            if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog))))return;
            DWORD options=0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM);
            dialog->SetTitle(L"共有トゥーン（toon01.bmp〜toon10.bmp）のあるフォルダ");
            if(FAILED(dialog->Show(state.window)))return;
            Microsoft::WRL::ComPtr<IShellItem> item;
            PWSTR path=nullptr;
            if(SUCCEEDED(dialog->GetResult(&item))&&SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path)))
            {
                SetDlgItemTextW(state.window,IdToonDir,path);
                CoTaskMemFree(path);
            }
        }

        // Where the voice comes from, as the window shows it now (engine or files).
        VoiceSource CurrentVoiceSource()
        {
            wchar_t folder[MAX_PATH]{};
            GetDlgItemTextW(state.window,IdVoiceFolder,folder,MAX_PATH);
            return {ComboValue(IdVoiceMode,0)==1,state.choice.settings.voiceEndpoint,folder};
        }

        // The folder only matters when VOICEVOX's files are used directly.
        void UpdateVoiceMode()
        {
            const bool direct=ComboValue(IdVoiceMode,0)==1;
            EnableWindow(GetDlgItem(state.window,IdVoiceFolder),direct);
            EnableWindow(GetDlgItem(state.window,IdVoiceBrowse),direct);
        }

        void BrowseVoiceFolder()
        {
            Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
            if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog))))return;
            DWORD options=0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM);
            dialog->SetTitle(L"VOICEVOXをインストールしたフォルダ（VOICEVOX.exe のあるフォルダ）");
            if(FAILED(dialog->Show(state.window)))return;
            Microsoft::WRL::ComPtr<IShellItem> item;
            PWSTR path=nullptr;
            if(SUCCEEDED(dialog->GetResult(&item))&&SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path)))
            {
                SetDlgItemTextW(state.window,IdVoiceFolder,path);
                CoTaskMemFree(path);
            }
        }

        // The chosen voice says a short line.
        void PlayVoiceSample()
        {
            const int speaker=ComboValue(IdVoiceList,state.choice.settings.voiceSpeaker);
            SetCursor(LoadCursorW(nullptr,IDC_WAIT));
            PlaySoundW(nullptr,nullptr,0);
            std::string wav;
            const std::wstring error=SynthesizeVoice(CurrentVoiceSource(),speaker,L"こんにちは。今日もいっしょに、がんばろうね。",wav);
            SetCursor(LoadCursorW(nullptr,IDC_ARROW));
            if(!error.empty())
            {
                MessageBoxW(state.window,(error+L"\n\nVOICEVOX（または互換の音声エンジン）を起動してから、もう一度押してください。").c_str(),L"MikuMikuDesktopPerch",MB_ICONWARNING);
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
                const wchar_t* hint=CurrentVoiceSource().direct?L"\n\nVOICEVOXをインストールしたフォルダ（VOICEVOX.exe のあるフォルダ）を指定してください。"
                                                              :L"\n\nVOICEVOX（または互換の音声エンジン）を起動してから、もう一度押してください。";
                MessageBoxW(state.window,(error+hint).c_str(),L"MikuMikuDesktopPerch",MB_ICONWARNING);
                return;
            }
            HWND list=GetDlgItem(state.window,IdVoiceList);
            const int current=ComboValue(IdVoiceList,state.choice.settings.voiceSpeaker);
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

        LRESULT CALLBACK PersonaProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam)
        {
            switch(message)
            {
            case WM_COMMAND:
                switch(LOWORD(wParam))
                {
                case IDOK:
                {
                    std::wstring name(256,L'\0'),text(4096,L'\0');
                    name.resize(static_cast<size_t>(GetDlgItemTextW(hwnd,IdPersonaName,name.data(),256)));
                    text.resize(static_cast<size_t>(GetDlgItemTextW(hwnd,IdPersonaText,text.data(),4096)));
                    for(size_t r;(r=text.find(L'\r'))!=std::wstring::npos;)text.erase(r,1);
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
            state.persona=CreateWindowExW(exStyle,PersonaClass,L"人物設定（このモデル）",style,owner.left+Px(20),owner.top+Px(60),
                                          frame.right-frame.left,frame.bottom-frame.top,state.window,nullptr,instance,nullptr);
            if(!state.persona)return;
            const HWND p=state.persona;
            AddTo(p,L"STATIC",L"名前",SS_LEFT,16,19,80,20);
            AddTo(p,L"EDIT",state.choice.character.name.c_str(),ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,100,16,344,24,IdPersonaName);
            AddTo(p,L"STATIC",L"性格や話し方を自由に書いてください。返事が吹き出しに収まる短い話し言葉になるよう、アプリが指示を足します。",
                  SS_LEFT,16,50,428,40);
            std::wstring text=state.choice.character.persona;
            for(size_t n=0;(n=text.find(L'\n',n))!=std::wstring::npos;n+=2)text.insert(n,1,L'\r');
            HWND editor=AddTo(p,L"EDIT",text.c_str(),ES_MULTILINE|ES_WANTRETURN|ES_AUTOVSCROLL|WS_VSCROLL|WS_BORDER|WS_TABSTOP,16,92,428,180,IdPersonaText);
            SendMessageW(editor,EM_SETLIMITTEXT,4000,0);
            AddTo(p,L"BUTTON",L"既定に戻す",BS_PUSHBUTTON|WS_TABSTOP,16,286,100,28,IdPersonaReset);
            AddTo(p,L"BUTTON",L"OK",BS_DEFPUSHBUTTON|WS_TABSTOP,272,286,82,28,IDOK);
            AddTo(p,L"BUTTON",L"キャンセル",BS_PUSHBUTTON|WS_TABSTOP,362,286,82,28,IDCANCEL);
            EnableWindow(state.window,FALSE);
            ShowWindow(p,SW_SHOWNORMAL);
            SetForegroundWindow(p);
            SetFocus(GetDlgItem(p,IdPersonaName));
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
            const std::wstring count=std::to_wstring(state.choice.memory->Entries().size())+L"件";
            SetDlgItemTextW(state.memory,IdMemoryCount,count.c_str());
        }

        void SaveMemory()
        {
            if(state.choice.memory&&!state.choice.memory->Save())
                MessageBoxW(state.memory,L"記憶を保存できませんでした。",L"MikuMikuDesktopPerch",MB_ICONERROR);
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
                       MessageBoxW(hwnd,L"このモデルの記憶をすべて削除します。よろしいですか？",L"MikuMikuDesktopPerch",MB_ICONWARNING|MB_OKCANCEL|MB_DEFBUTTON2)==IDOK)
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
            const std::wstring title=L"記憶（"+state.choice.character.name+L"）";
            state.memory=CreateWindowExW(exStyle,MemoryClass,title.c_str(),style,owner.left+Px(10),owner.top+Px(60),
                                         frame.right-frame.left,frame.bottom-frame.top,state.window,nullptr,instance,nullptr);
            if(!state.memory)return;
            const HWND m=state.memory;
            AddTo(m,L"STATIC",L"会話から覚えたことです。次の会話から、話に関係のあるものを思い出します。削除や追加はすぐ保存されます"
                              L"（このコンピューターの中だけに保存し、外部には送りません）。",
                  SS_LEFT,16,12,508,40);
            AddTo(m,L"LISTBOX",L"",LBS_EXTENDEDSEL|LBS_NOINTEGRALHEIGHT|LBS_NOTIFY|WS_VSCROLL|WS_HSCROLL|WS_BORDER|WS_TABSTOP,16,56,508,236,IdMemoryList);
            AddTo(m,L"STATIC",L"",SS_LEFT,16,298,120,20,IdMemoryCount);
            AddTo(m,L"BUTTON",L"選んだものを削除",BS_PUSHBUTTON|WS_TABSTOP,296,296,130,28,IdMemoryDelete);
            AddTo(m,L"BUTTON",L"すべて削除",BS_PUSHBUTTON|WS_TABSTOP,434,296,90,28,IdMemoryClear);
            HWND input=AddTo(m,L"EDIT",L"",ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,16,340,410,24,IdMemoryNew);
            SendMessageW(input,EM_SETLIMITTEXT,CharacterMemory::MaxLength,0);
            SendMessageW(input,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"覚えてほしいこと（例: 話し相手はコーヒーが好き）"));
            AddTo(m,L"BUTTON",L"追加",BS_PUSHBUTTON|WS_TABSTOP,434,338,90,28,IdMemoryAdd);
            AddTo(m,L"BUTTON",L"閉じる",BS_PUSHBUTTON|WS_TABSTOP,434,380,90,28,IDCANCEL);
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
            s.msaa=ComboValue(IdMsaa,s.msaa);
            s.maxTextureSize=ComboValue(IdTexture,s.maxTextureSize);
            s.physicsFps=ComboValue(IdPhysicsFps,s.physicsFps);
            wchar_t toon[MAX_PATH]{};
            GetDlgItemTextW(state.window,IdToonDir,toon,MAX_PATH);
            s.toonDir=toon;
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
            s.chatInitiative=Checked(IdChatInitiative);
            s.voice=Checked(IdVoice);
            {
                const VoiceSource source=CurrentVoiceSource();
                s.voiceDirect=source.direct;
                s.voiceFolder=source.folder;
            }
            s.voiceSpeaker=ComboValue(IdVoiceList,s.voiceSpeaker);
            {
                HWND list=GetDlgItem(state.window,IdVoiceList);
                const auto index=SendMessageW(list,CB_GETCURSEL,0,0);
                if(index!=CB_ERR)
                {
                    const auto length=static_cast<size_t>(SendMessageW(list,CB_GETLBTEXTLEN,index,0));
                    std::wstring name(length+1,L'\0');
                    SendMessageW(list,CB_GETLBTEXT,index,reinterpret_cast<LPARAM>(name.data()));
                    name.resize(length);
                    s.voiceName=name;
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
                case IdVoiceMode:
                    if(HIWORD(wParam)==CBN_SELCHANGE)UpdateVoiceMode();
                    return 0;
                case IdVoiceBrowse:
                    BrowseVoiceFolder();
                    return 0;
                case IdPersona:
                    OpenPersona();
                    return 0;
                case IdMemory:
                    OpenMemory();
                    return 0;
                }
                break;
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
                state.window=nullptr;
                if(state.font){DeleteObject(state.font);state.font=nullptr;}
                state.onApply=nullptr;
                state.onLight=nullptr;
                if(!state.sample.empty()){PlaySoundW(nullptr,nullptr,0);state.sample.clear();}
                return 0;
            }
            return DefWindowProcW(hwnd,message,wParam,lParam);
        }
    }

    void ShowSettingsWindow(HWND owner,const SettingsChoice& current,std::function<void(const SettingsChoice&)> onApply,
                            std::function<void(int azimuth,int elevation)> onLight)
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
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_BAR_CLASSES};
        InitCommonControlsEx(&controls);
        state.dpi=static_cast<int>(GetDpiForSystem());
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0,state.dpi);
        state.font=CreateFontIndirectW(&metrics.lfMessageFont);

        // Client 460 x 782 logical pixels, centred on the screen the cursor is on.
        const DWORD style=WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
        const DWORD exStyle=WS_EX_CONTROLPARENT|WS_EX_DLGMODALFRAME;
        RECT frame{0,0,Px(460),Px(782)};
        AdjustWindowRectExForDpi(&frame,style,FALSE,exStyle,state.dpi);
        POINT cursor{};
        GetCursorPos(&cursor);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST),&monitor);
        const RECT& work=monitor.rcWork;
        const int w=frame.right-frame.left,h=frame.bottom-frame.top;
        // Owned by the mascot's window: kept above it (it is topmost) and gone with it.
        state.window=CreateWindowExW(exStyle,ClassName,L"MikuMikuDesktopPerch の設定",style,
                                     work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,
                                     owner,nullptr,instance,nullptr);
        if(!state.window)return;
        Build();
        ShowWindow(state.window,SW_SHOWNORMAL);
        SetForegroundWindow(state.window);
        SetFocus(GetDlgItem(state.window,IdAuto));
    }

    HWND SettingsWindowHandle(){return state.persona?state.persona:state.memory?state.memory:state.window;}

    void RefreshMemoryWindow(){FillMemoryList();}
}
