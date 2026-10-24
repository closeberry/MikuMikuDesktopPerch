#define NOMINMAX
#include "Planner.hpp"
#include "Memory.hpp"
#include "Text.hpp"
#include <windows.h>
#include <algorithm>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace MikuMikuDesktopPerch
{
    namespace
    {
        constexpr std::uintmax_t MaxFileSize=1u<<20;
        constexpr std::int64_t Day=24*60*60;

        std::wstring FromUtf8(const std::string& text)
        {
            if(text.empty())return {};
            const int n=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
            std::wstring out(static_cast<size_t>(n),L'\0');
            MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),out.data(),n);
            return out;
        }

        std::string ToUtf8(const std::wstring& text)
        {
            if(text.empty())return {};
            const int n=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
            std::string out(static_cast<size_t>(n),'\0');
            WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),out.data(),n,nullptr,nullptr);
            return out;
        }

        std::filesystem::path PlannerFile(bool create)
        {
            wchar_t base[MAX_PATH]{};
            if(!GetEnvironmentVariableW(L"LOCALAPPDATA",base,MAX_PATH))return {};
            const std::filesystem::path folder=std::filesystem::path(base)/L"MikuMikuDesktopPerch";
            if(create){std::error_code ec;std::filesystem::create_directories(folder,ec);}
            return folder/L"planner.txt";
        }

        bool IsSpace(wchar_t c){return c==L' '||c==L'\t'||c==L'\r'||c==L'\n'||c==L'　';}

        // One line, no tabs, trimmed, at most MaxText characters.
        std::wstring Tidy(std::wstring text)
        {
            for(auto& c:text)if(c==L'\t'||c==L'\r'||c==L'\n')c=L' ';
            size_t a=0,b=text.size();
            while(a<b&&IsSpace(text[a]))++a;
            while(b>a&&IsSpace(text[b-1]))--b;
            text=text.substr(a,b-a);
            if(text.size()>Planner::MaxText)text.resize(Planner::MaxText);
            return text;
        }

        std::tm Local(std::int64_t at)
        {
            std::tm tm{};
            const std::time_t t=static_cast<std::time_t>(at);
            localtime_s(&tm,&t);
            return tm;
        }
        std::int64_t Make(std::tm tm){tm.tm_isdst=-1;return static_cast<std::int64_t>(std::mktime(&tm));}
        // The same clock time, days later (daylight saving changes included).
        std::int64_t AddDays(std::int64_t at,int days){std::tm tm=Local(at);tm.tm_mday+=days;return Make(tm);}

        // Full-width letters, digits and signs as ASCII; kanji numbers before a unit
        // (三十分, 十五時) as digits.
        std::wstring Normalize(const std::wstring& said)
        {
            std::wstring s;
            for(wchar_t c:said)
            {
                if(c>=0xFF01&&c<=0xFF5E)c=static_cast<wchar_t>(c-0xFEE0);
                else if(c==L'　')c=L' ';
                s+=c;
            }
            static const std::wstring Kanji=L"〇一二三四五六七八九十百";
            std::wstring out;
            for(size_t i=0;i<s.size();)
            {
                size_t j=i;
                while(j<s.size()&&Kanji.find(s[j])!=std::wstring::npos)++j;
                if(j==i){out+=s[i++];continue;}
                size_t k=j;
                while(k<s.size()&&s[k]==L' ')++k;
                if(k<s.size()&&std::wstring(L"時分秒月日つ").find(s[k])!=std::wstring::npos)
                {
                    int total=0,current=0;
                    for(size_t p=i;p<j;++p)
                    {
                        const size_t d=Kanji.find(s[p]);
                        if(d<=9)current=static_cast<int>(d);
                        else if(s[p]==L'十'){total+=(current?current:1)*10;current=0;}
                        else {total+=(current?current:1)*100;current=0;}
                    }
                    out+=std::to_wstring(total+current);
                }
                else out+=s.substr(i,j-i);
                i=j;
            }
            return out;
        }

        // Particles and request words around what is left once the times are taken out.
        std::wstring TrimJapanese(std::wstring t)
        {
            static constexpr const wchar_t* Heads[]={L"には",L"に",L"の",L"、",L",",L"は",L"で",L"まで",L"から",L"って",L"を",L"と",L"!",L"。",L"."};
            static constexpr const wchar_t* Tails[]={
                L"って教えて",L"と教えて",L"を教えて",L"教えて",L"おしえて",L"って知らせて",L"を知らせて",L"知らせて",L"しらせて",L"お知らせして",L"お知らせ",
                L"って言って",L"と言って",L"言って",L"って声かけて",L"って声をかけて",L"声をかけて",L"声かけて",L"で起こして",L"に起こして",L"起こして",L"おこして",
                L"って呼んで",L"呼んで",L"をリマインドして",L"をリマインド",L"リマインドして",L"リマインド",L"を通知して",L"通知して",L"アラームを鳴らして",L"アラームをセットして",
                L"アラーム",L"鳴らして",L"をセットして",L"セットして",L"をかけて",L"かけて",L"セット",L"計って",L"はかって",L"測って",L"お願いします",L"お願い",L"おねがい",
                L"ください",L"下さい",L"くれる?",L"くれる",L"くれない?",L"してね",L"して",L"ね",L"よ",L"な",L"ー",L"〜",L"。",L"!",L"?",L"、",L",",L".",
                L"って",L"を",L"と",L"に",L"の",L"で",L"は"};
            for(bool changed=true;changed;)
            {
                changed=false;
                t=Tidy(t);
                for(const wchar_t* h:Heads)if(t.rfind(h,0)==0){t.erase(0,wcslen(h));changed=true;break;}
                for(const wchar_t* tail:Tails)
                {
                    const size_t n=wcslen(tail);
                    if(t.size()>=n&&t.compare(t.size()-n,n,tail)==0){t.resize(t.size()-n);changed=true;break;}
                }
            }
            for(const wchar_t* q:{L"「",L"『",L"\""})if(t.rfind(q,0)==0)t.erase(0,1);
            while(!t.empty()&&(t.back()==L'」'||t.back()==L'』'||t.back()==L'"'))t.pop_back();
            return Tidy(t);
        }

        std::wstring TrimEnglish(std::wstring t)
        {
            static const std::wregex Head(LR"(^\s*(?:(?:please|hey|ok|okay|so|and|can you|could you|would you|will you|remind me|set (?:a |an |the |my )?(?:timer|alarm|reminder)|start (?:a |the )?timer|timer|alarm|tell me|let me know|wake me(?: up)?|alert me|ping me|notify me|to|about|of|that|for|in|at|on|,)\b\s*)+)",std::regex_constants::icase);
            static const std::wregex Tail(LR"((?:\s*\b(?:please|thanks|thank you|in|at|on|for|to|and|then)\b|\s*[,.!?])+\s*$)",std::regex_constants::icase);
            t=std::regex_replace(t,Head,L"");
            t=std::regex_replace(t,Tail,L"");
            return Tidy(t);
        }

        bool HasJapanese(const std::wstring& s)
        {
            return std::any_of(s.begin(),s.end(),[](wchar_t c){return (c>=0x3040&&c<=0x30FF)||(c>=0x4E00&&c<=0x9FFF);});
        }

        int Weekday(const std::wstring& w)
        {
            static constexpr const wchar_t* Japanese=L"日月火水木金土";
            if(w.size()==1)if(const wchar_t* p=wcschr(Japanese,w[0]))return static_cast<int>(p-Japanese);
            static constexpr const wchar_t* English[]={L"sun",L"mon",L"tue",L"wed",L"thu",L"fri",L"sat"};
            for(int i=0;i<7;++i)if(_wcsnicmp(w.c_str(),English[i],3)==0)return i;
            return -1;
        }

        int Month(const std::wstring& m)
        {
            static constexpr const wchar_t* Names[]={L"jan",L"feb",L"mar",L"apr",L"may",L"jun",L"jul",L"aug",L"sep",L"oct",L"nov",L"dec"};
            for(int i=0;i<12;++i)if(_wcsnicmp(m.c_str(),Names[i],3)==0)return i+1;
            return 0;
        }

        int Number(const std::wssub_match& m){return m.matched?_wtoi(m.str().c_str()):0;}

        // A todo item's words without the quotes and particles around them.
        std::wstring ItemWords(std::wstring t)
        {
            t=Tidy(t);
            for(const wchar_t* q:{L"「",L"『",L"\""})if(t.rfind(q,0)==0)t.erase(0,1);
            while(!t.empty()&&(t.back()==L'」'||t.back()==L'』'||t.back()==L'"'))t.pop_back();
            return Tidy(t);
        }
    }

    std::int64_t Now(){return static_cast<std::int64_t>(std::time(nullptr));}

    // ---- Keeping them --------------------------------------------------------------------

    void Planner::Load()
    {
        alarms.clear();
        todos.clear();
        const auto path=PlannerFile(false);
        std::error_code ec;
        if(path.empty()||!std::filesystem::exists(path,ec)||std::filesystem::file_size(path,ec)>MaxFileSize)return;
        std::ifstream in(path,std::ios::binary);
        std::stringstream buffer;
        buffer<<in.rdbuf();
        std::wstringstream lines(FromUtf8(buffer.str()));
        for(std::wstring line;std::getline(lines,line);)
        {
            if(!line.empty()&&line.back()==L'\r')line.pop_back();
            std::vector<std::wstring> f;
            for(size_t at=0;;)
            {
                const size_t tab=line.find(L'\t',at);
                f.push_back(line.substr(at,tab==std::wstring::npos?std::wstring::npos:tab-at));
                if(tab==std::wstring::npos)break;
                at=tab+1;
            }
            if(f[0]==L"alarm"&&f.size()>=5&&alarms.size()<MaxAlarms)
            {
                Alarm a;
                a.id=nextId++;
                a.due=_wtoi64(f[1].c_str());
                a.timerSeconds=std::max(0,_wtoi(f[2].c_str()));
                a.daily=f[3]==L"1";
                a.text=Tidy(f[4]);
                if(a.due>0)alarms.push_back(a);
            }
            else if(f[0]==L"todo"&&f.size()>=3&&todos.size()<MaxTodos)
            {
                const std::wstring text=Tidy(f[2]);
                if(!text.empty())todos.push_back({nextId++,text,f[1]});
            }
        }
        Sort();
    }

    bool Planner::Save() const
    {
        const auto path=PlannerFile(true);
        if(path.empty())return false;
        std::wstring all;
        for(const auto& a:alarms)
            all+=L"alarm\t"+std::to_wstring(a.due)+L"\t"+std::to_wstring(a.timerSeconds)+L"\t"+(a.daily?L"1":L"0")+L"\t"+a.text+L"\n";
        for(const auto& t:todos)all+=L"todo\t"+t.added+L"\t"+t.text+L"\n";
        const auto temp=path.wstring()+L".tmp";
        {
            std::ofstream out(temp,std::ios::binary|std::ios::trunc);
            const std::string bytes=ToUtf8(all);
            out.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
            if(!out)return false;
        }
        return MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING)!=0;
    }

    void Planner::Sort(){std::stable_sort(alarms.begin(),alarms.end(),[](const Alarm& a,const Alarm& b){return a.due<b.due;});}

    int Planner::AddAlarm(Alarm alarm)
    {
        if(alarms.size()>=MaxAlarms||alarm.due<=0)return 0;
        alarm.text=Tidy(alarm.text);
        alarm.id=nextId++;
        alarms.push_back(alarm);
        Sort();
        return alarm.id;
    }

    void Planner::RemoveAlarm(int id){std::erase_if(alarms,[id](const Alarm& a){return a.id==id;});}

    int Planner::RemoveTimers(){return static_cast<int>(std::erase_if(alarms,[](const Alarm& a){return a.timerSeconds>0;}));}

    int Planner::AddTodo(const std::wstring& text,const std::wstring& date)
    {
        const std::wstring t=Tidy(text);
        if(t.empty()||todos.size()>=MaxTodos)return 0;
        todos.push_back({nextId++,t,date});
        return todos.back().id;
    }

    void Planner::RemoveTodo(int id){std::erase_if(todos,[id](const TodoItem& t){return t.id==id;});}

    int Planner::FindTodo(const std::wstring& words) const
    {
        auto squeeze=[](const std::wstring& s){
            std::wstring out;
            for(wchar_t c:Normalize(s))if(!IsSpace(c)&&!std::iswpunct(c)&&std::wstring(L"「」『』、。").find(c)==std::wstring::npos)out+=static_cast<wchar_t>(std::towlower(c));
            return out;
        };
        const std::wstring w=squeeze(words);
        if(w.size()<2)return 0;
        int best=0;
        size_t bestLength=0;
        for(const auto& t:todos)
        {
            const std::wstring item=squeeze(t.text);
            if(item.empty())continue;
            // The item named in the words (宿題 in "宿題終わった"), or the words a part of the
            // item ("牛乳" for "牛乳を買う"): the longest such item wins.
            if(w.find(item)!=std::wstring::npos||(item.find(w)!=std::wstring::npos))
                if(item.size()>bestLength){best=t.id;bestLength=item.size();}
        }
        return best;
    }

    std::vector<Alarm> Planner::TakeDue(std::int64_t now)
    {
        std::vector<Alarm> due;
        bool changed=false;
        for(auto it=alarms.begin();it!=alarms.end();)
        {
            if(it->due>now){++it;continue;}
            changed=true;
            if(now-it->due<=Day)due.push_back(*it);
            if(it->daily)
            {
                while(it->due<=now)it->due=AddDays(it->due,1);
                ++it;
            }
            else it=alarms.erase(it);
        }
        if(changed)Sort();
        return due;
    }

    // ---- Reading requests ----------------------------------------------------------------

    PlanRequest ReadPlanRequest(const std::wstring& said,std::int64_t now,bool loose)
    {
        PlanRequest r;
        const std::wstring s=Normalize(said);
        if(Tidy(s).empty()||s.size()>300)return r;
        using Kind=PlanRequest::Kind;
        const auto flags=std::regex_constants::ECMAScript|std::regex_constants::icase;
        std::wsmatch m;
        auto find=[&](const wchar_t* pattern){return std::regex_search(s,m,std::wregex(pattern,flags));};
        const bool japanese=HasJapanese(s);
        constexpr const wchar_t* TodoWord=LR"((?:ToDo|To-Do|やること|やる事|タスク)(?:リスト)?)";
        auto with=[&](const wchar_t* pattern){return std::wstring(pattern).replace(std::wstring(pattern).find(L"TODO"),4,TodoWord);};

        if(!loose)
        {
            // Stopping timers.
            if(find(LR"((タイマー|アラーム)\s*(を|は)?\s*(止めて|とめて|止める|消して|けして|キャンセル|やめて|解除|ストップ|オフ|切って))")||
               find(LR"(\b(cancel|stop|clear|delete|turn off|kill)\b.{0,12}\b(timers?|alarms?)\b)"))
            {r.kind=Kind::CancelTimers;return r;}

            // Adding to the to-do list.
            if(std::regex_search(s,m,std::wregex(with(LR"(TODO\s*(?:に|へ)\s*(.+?)\s*(?:を|って|と)?\s*(?:追加|入れて|いれて|入れと|足して|たして|登録|加えて|書いて|メモ))"),flags))||
               std::regex_search(s,m,std::wregex(with(LR"(^(.+?)\s*(?:を|って|と|も)\s*TODO\s*(?:に|へ)\s*(?:追加|入れ|いれ|足し|たし|登録|加え|書い|メモ))"),flags))||
               std::regex_search(s,m,std::wregex(with(LR"(^\s*TODO\s*[:：]\s*(.+)$)"),flags))||
               find(LR"(\badd\s+(.+?)\s+to\s+(?:my\s+|the\s+)?(?:to-?do|task)s?(?:\s*list)?\b)")||
               find(LR"(\bput\s+(.+?)\s+on\s+(?:my\s+|the\s+)?(?:to-?do\s*|task\s*)?list\b)")||
               find(LR"(^\s*(?:to-?do|task)\s*:\s*(.+)$)"))
            {
                r.text=ItemWords(m[1].str());
                if(japanese)r.text=ItemWords(TrimJapanese(r.text));
                if(!r.text.empty()){r.kind=Kind::TodoAdd;return r;}
            }
            // Taking an item off it.
            if(std::regex_search(s,m,std::wregex(with(LR"(TODO\s*(?:から|の)\s*(.+?)\s*(?:を|は)?\s*(?:消して|けして|削除|外して|はずして|済み|完了|チェック))"),flags))||
               find(LR"(\b(?:remove|delete|cross off|check off|tick off|take)\s+(.+?)\s+(?:from|off)\s+(?:my\s+|the\s+)?(?:to-?do\s*|task\s*)?list\b)")||
               find(LR"(\bmark\s+(.+?)\s+(?:as\s+)?(?:done|finished|complete)\b)"))
            {
                r.text=ItemWords(m[1].str());
                if(!r.text.empty()){r.kind=Kind::TodoDone;return r;}
            }
        }

        // ---- A time to remind at, or a length of time ----
        std::vector<std::pair<size_t,size_t>> spans;
        auto take=[&](const std::wsmatch& match){spans.push_back({static_cast<size_t>(match.position(0)),static_cast<size_t>(match.position(0)+match.length(0))});};
        auto overlaps=[&](const std::wsmatch& match){
            const size_t a=static_cast<size_t>(match.position(0)),b=a+static_cast<size_t>(match.length(0));
            return std::any_of(spans.begin(),spans.end(),[&](const auto& sp){return a<sp.second&&sp.first<b;});
        };
        int afternoon=0;   // 1: afternoon/evening said, -1: morning said
        bool daily=false;
        if(find(LR"(毎日|毎朝|毎晩|毎夜|\bevery\s*day\b|\bdaily\b|\bevery\s+(morning|evening|night)\b)"))
        {
            daily=true;
            const std::wstring w=m[0].str();
            if(w.find(L"晩")!=std::wstring::npos||w.find(L"夜")!=std::wstring::npos||_wcsicmp(m[1].str().c_str(),L"evening")==0||_wcsicmp(m[1].str().c_str(),L"night")==0)afternoon=1;
            if(w.find(L"朝")!=std::wstring::npos||_wcsicmp(m[1].str().c_str(),L"morning")==0)afternoon=-1;
            take(m);
        }

        int hour=-1,minute=0;
        if(find(LR"((午前|午後|朝の?|夜の?|夕方の?|夜中の?|深夜の?|昼の?)?\s*(\d{1,2})\s*時(?!間)\s*(?:(\d{1,2})\s*分|(半))?)")||
           find(LR"((午前|午後|朝の?|夜の?|夕方の?|夜中の?|深夜の?|昼の?)?\s*(\d{1,2}):(\d{2})())"))
        {
            const std::wstring period=m[1].str();
            hour=Number(m[2]);
            minute=m[4].matched&&m[4].length()>0?30:Number(m[3]);
            if(period.find(L"午後")!=std::wstring::npos||period.find(L"夕方")!=std::wstring::npos||(period.find(L"夜")!=std::wstring::npos&&period.find(L"夜中")==std::wstring::npos))afternoon=1;
            else if(!period.empty())afternoon=-1;
            take(m);
        }
        else if(find(LR"(正午)")){hour=12;afternoon=-1;take(m);}
        else if(find(LR"((?:\bat\s+)?\b(\d{1,2})(?::(\d{2}))?\s*(a\.?m\.?|p\.?m\.?)(?=\W|$))")||
                find(LR"(\b(\d{1,2})(?::(\d{2}))?\s*o'?clock\b())")||
                find(LR"(\bat\s+(\d{1,2})(?::(\d{2}))?\b(?!\s*(?:min|hour|sec|h\b|m\b|s\b))())")||
                find(LR"(\b(\d{1,2}):(\d{2})\b())"))
        {
            hour=Number(m[1]);
            minute=Number(m[2]);
            const std::wstring ampm=m[3].str();
            if(!ampm.empty())afternoon=(ampm[0]==L'p'||ampm[0]==L'P')?1:-1;
            take(m);
        }
        else if(find(LR"((?:\bat\s+)?\b(noon|midday|midnight)\b)"))
        {
            hour=_wcsicmp(m[1].str().c_str(),L"midnight")==0?0:12;
            afternoon=-1;
            take(m);
        }
        // "in the evening" with a time (a part of the day alone is no time).
        if(hour>=0&&find(LR"(\b(?:in the|this)\s+(morning|afternoon|evening)\b|\b(tonight|at night)\b)"))
        {
            const std::wstring w=m[1].matched?m[1].str():m[2].str();
            afternoon=_wcsicmp(w.c_str(),L"morning")==0?-1:1;
            take(m);
        }

        // The day: today unless said.
        const std::tm today=Local(now);
        int dayOffset=-1,month=0,dayOfMonth=0;
        if(find(LR"(明後日|あさって|\bday after tomorrow\b)")){dayOffset=2;take(m);}
        else if(find(LR"(明日|あした|あす|\btomorrow\b)")){dayOffset=1;take(m);}
        else if(find(LR"(今日|きょう|今夜|今晩|こんや|\btoday\b)")){dayOffset=0;if(m[0].str().find(L"夜")!=std::wstring::npos||m[0].str().find(L"晩")!=std::wstring::npos||m[0].str()==L"こんや")afternoon=1;take(m);}
        else if(find(LR"((\d{1,2})\s*月\s*(\d{1,2})\s*日)")||find(LR"(\b(\d{1,2})/(\d{1,2})\b)")){month=Number(m[1]);dayOfMonth=Number(m[2]);take(m);}
        else if(find(LR"(\b(?:on\s+)?(jan|feb|mar|apr|may|jun|jul|aug|sep|oct|nov|dec)[a-z]*\.?\s+(\d{1,2})(?:st|nd|rd|th)?\b)")){month=Month(m[1].str());dayOfMonth=Number(m[2]);take(m);}
        else if(find(LR"((来週|今週)?の?\s*(月|火|水|木|金|土|日)曜日?)")||find(LR"(\b(?:on\s+)?(next\s+|this\s+)?(sun|mon|tues?|wed(?:nes)?|thu(?:rs)?|fri|sat(?:ur)?)(?:day)?\b)"))
        {
            const int wd=Weekday(m[2].str());
            const std::wstring which=m[1].str();
            if(wd>=0)
            {
                if(which.find(L"来週")!=std::wstring::npos||_wcsnicmp(which.c_str(),L"next",4)==0)
                {
                    const int sinceMonday=(today.tm_wday+6)%7;
                    dayOffset=7-sinceMonday+(wd+6)%7;
                }
                else dayOffset=(wd-today.tm_wday+7)%7;
                take(m);
            }
        }

        // Lengths of time (not the clock time already read).
        int seconds=0;
        bool relative=false;
        for(const wchar_t* pattern:{LR"((\d+)\s*時間\s*(半)?)",LR"((\d+)\s*分\s*(半)?)",LR"((\d+)\s*秒)",
                                    LR"(\b(\d+(?:\.\d+)?)\s*(hours?|hrs?|h)\b)",LR"(\b(\d+)\s*(minutes?|mins?|m)\b)",LR"(\b(\d+)\s*(seconds?|secs?|s)\b)",
                                    LR"(\b(half)\s+an\s+hour\b)",LR"(\b(an)\s+hour\b)",LR"(\b(a)\s+minute\b)"})
        {
            const std::wregex re(pattern,flags);
            for(auto it=std::wsregex_iterator(s.begin(),s.end(),re);it!=std::wsregex_iterator();++it)
            {
                const std::wsmatch& d=*it;
                if(overlaps(d))continue;
                const std::wstring unit=d[0].str(),n=d[1].str();
                double value=_wtof(n.c_str());
                if(n==L"half")seconds+=30*60;
                else if(n==L"an"||n==L"An")seconds+=60*60;
                else if(n==L"a"||n==L"A")seconds+=60;
                else if(unit.find(L"時間")!=std::wstring::npos||(d[2].matched&&std::towlower(d[2].str()[0])==L'h'))seconds+=static_cast<int>(value*3600)+(d[2].str()==L"半"?1800:0);
                else if(unit.find(L"分")!=std::wstring::npos||(d[2].matched&&std::towlower(d[2].str()[0])==L'm'))seconds+=static_cast<int>(value*60)+(d[2].str()==L"半"?30:0);
                else seconds+=static_cast<int>(value);
                take(d);
                // "後に", "たったら", "in ...", "after ...": from now.
                const size_t end=static_cast<size_t>(d.position(0)+d.length(0));
                std::wsmatch after;
                const std::wstring rest=s.substr(end);
                if(std::regex_search(rest,after,std::wregex(LR"(^\s*(後|ご|経ったら|たったら|経って|たって|経過したら|したら|で|later\b))",flags)))
                {
                    relative=true;
                    spans.push_back({end,end+static_cast<size_t>(after.length(0))});
                }
                const std::wstring before=s.substr(0,static_cast<size_t>(d.position(0)));
                if(std::regex_search(before,std::wregex(LR"(\b(in|after|for)\s*$)",flags)))relative=true;
            }
        }
        seconds=std::clamp(seconds,0,99*3600);

        const bool verb=find(LR"(教えて|おしえて|知らせて|しらせて|お知らせ|呼んで|よんで|声を?かけて|声掛けて|って言って|と言って|起こして|おこして|リマインド|アラーム|鳴らして|通知|忘れないよう|お願い|頼む|頼んだ|\bremind\b|\blet me know\b|\btell me\b|\bwake me\b|\balert me\b|\bping me\b|\balarm\b|\bnotify\b)");
        const bool timerWord=find(LR"(タイマー|計って|はかって|測って|\btimer\b)");

        auto rest=[&]{
            std::wstring out;
            for(size_t i=0;i<s.size();++i)
                if(std::none_of(spans.begin(),spans.end(),[&](const auto& sp){return i>=sp.first&&i<sp.second;}))out+=s[i];
            // A timer word is no part of what to remind of.
            out=std::regex_replace(out,std::wregex(LR"(タイマー|アラーム)",flags),L"");
            return japanese?TrimJapanese(out):TrimEnglish(out);
        };

        if(hour>=0&&hour<=24&&minute>=0&&minute<60&&(verb||timerWord||loose))
        {
            if(afternoon==1&&hour<12)hour+=12;
            if(afternoon==-1&&hour==12&&s.find(L"午前")!=std::wstring::npos)hour=0;
            std::tm at=today;
            at.tm_hour=hour%24;at.tm_min=minute;at.tm_sec=0;
            if(month>0)
            {
                at.tm_mon=month-1;at.tm_mday=dayOfMonth;
                std::int64_t t=Make(at);
                if(t<=now){at.tm_year+=1;t=Make(at);}   // a date gone by this year is next year's
                r.due=t;
            }
            else
            {
                at.tm_mday+=std::max(0,dayOffset);
                std::int64_t t=Make(at);
                // A bare "3時" at one o'clock in the afternoon is three o'clock this afternoon;
                // a time already gone (with no day said) is tomorrow's.
                if(t<=now&&afternoon==0&&hour<12&&dayOffset<=0&&!daily){std::tm pm=at;pm.tm_hour+=12;if(Make(pm)>now){t=Make(pm);}}
                if(t<=now&&dayOffset<=0)t=AddDays(t,1);
                r.due=t;
            }
            if(r.due<=now)return {};
            r.kind=Kind::Remind;
            r.daily=daily;
            r.text=rest();
            return r;
        }
        if(seconds>0&&(timerWord||(relative&&verb)||loose))
        {
            r.kind=Kind::Timer;
            r.seconds=seconds;
            r.due=now+seconds;
            r.text=rest();
            return r;
        }

        if(loose)return r;
        // Asking what is set.
        if(std::regex_search(s,m,std::wregex(with(LR"(TODO\s*(?:は|を|って|に)?\s*(?:見せて|みせて|教えて|おしえて|一覧|ある\?|何|なに|確認|読んで|ちょうだい|は\?))"),flags))||
           std::regex_search(s,m,std::wregex(with(LR"(^\s*(?:今日の)?TODO\s*(?:は)?\s*\??\s*$)"),flags))||
           find(LR"(\b(show|what'?s|what is|what are|read|list|check)\b.{0,15}\b(to-?dos?|to-?do list|tasks)\b)")||
           find(LR"(^\s*(my\s+)?(to-?dos?|to-?do list|tasks)\s*\??\s*$)"))
        {r.kind=Kind::TodoList;return r;}
        if(find(LR"((タイマー|リマインダー|アラーム)\s*(は|を|って|に)?\s*(あと何分|あとどれ|残り|のこり|教えて|おしえて|一覧|ある|見せて|みせて|何|なに|確認|\?|$))")||
           find(LR"(予定\s*(は|を|って)?\s*(教えて|おしえて|一覧|見せて|みせて|確認|ある\?|は\?|\?))")||
           find(LR"(あと何分|あとどれくらい)")||
           find(LR"(\b(how (long|much time)|time left)\b)")||
           find(LR"(\b(show|list|what are|what's|whats|read|check)\b.{0,15}\b(reminders|timers|alarms)\b)")||
           find(LR"(^\s*(my\s+)?(reminders|timers|alarms)\s*\??\s*$)"))
        {r.kind=Kind::AlarmList;return r;}
        // "宿題終わった": a request only if 宿題 is on the list (the caller looks).
        if(find(LR"(^(.+?)\s*(?:は|を|も)?\s*(?:終わった|おわった|終えた|済んだ|すんだ|済ませた|すませた|やった|やりました|完了|できた|片付いた|かたづいた|終わりました|済みました))")||
           find(LR"(\b(?:i\s+)?(?:finished|completed|did|done with|i'?m done with|i have done|i've done)\s+(?:the\s+|my\s+)?(.+?)\s*[.!]*$)"))
        {
            r.text=ItemWords(m[1].str());
            if(!r.text.empty()){r.kind=Kind::TodoDone;r.sure=false;}
        }
        return r;
    }

    bool MightBePlanRequest(const std::wstring& said)
    {
        const std::wstring s=Normalize(said);
        const auto flags=std::regex_constants::ECMAScript|std::regex_constants::icase;
        auto has=[&](const wchar_t* pattern){return std::regex_search(s,std::wregex(pattern,flags));};
        if(has(LR"(タイマー|リマインド|リマインダー|アラーム|ToDo|To-Do|やること|やる事|タスク|\btimer|\bremind|\balarm|\bto-?do|\btasks?\b)"))return true;
        const bool time=has(LR"(\d|分|時|秒|明日|あした|毎日|毎朝|毎晩|朝|夜|夕方|午前|午後|曜|\bminutes?\b|\bhours?\b|\btomorrow\b|\btonight\b|\bo'?clock\b|\bnoon\b|\b(am|pm)\b)");
        const bool ask=has(LR"(教え|おしえ|知らせ|しらせ|声かけ|声を?かけ|起こ|おこして|呼んで|言って|忘れ|\blet me know\b|\btell me\b|\bwake me\b|\bremember\b|\bdon'?t let me forget\b)");
        return time&&ask;
    }

    std::wstring PlanCommandPrompt(const std::wstring& said,std::int64_t now)
    {
        const std::tm t=Local(now);
        wchar_t clock[64]{};
        static constexpr const wchar_t* Days[]={L"Sun",L"Mon",L"Tue",L"Wed",L"Thu",L"Fri",L"Sat"};
        swprintf_s(clock,L"%04d-%02d-%02d (%s) %02d:%02d",t.tm_year+1900,t.tm_mon+1,t.tm_mday,Days[t.tm_wday%7],t.tm_hour,t.tm_min);
        if(English())return
            std::wstring(L"It is now ")+clock+L".\n"
            L"If the message below asks to set a timer, to be reminded of something at a time, or to do something with the to-do list, "
            L"answer with exactly one of these lines and nothing else. Otherwise answer only: none\n"
            L"timer <seconds> | <what to remind of, or nothing>\n"
            L"remind <YYYY-MM-DD HH:MM> | <what to remind of>\n"
            L"daily <HH:MM> | <what to remind of>\n"
            L"todo-add | <the task>\n"
            L"todo-done | <the task that is done>\n"
            L"todo-list\n"
            L"reminders\n"
            L"cancel-timers\n"
            L"none\n\n"
            L"Message: \""+said+L"\"";
        return
            std::wstring(L"いまは ")+clock+L" です。\n"
            L"次の発言が、タイマー、時刻に知らせるリマインダー、やることリストの操作を頼んでいるなら、次のどれか1行だけを書いてください（ほかには何も書かないでください）。"
            L"頼んでいなければ none とだけ書いてください。\n"
            L"timer <秒数> | <知らせる内容（なければ空）>\n"
            L"remind <YYYY-MM-DD HH:MM> | <知らせる内容>\n"
            L"daily <HH:MM> | <知らせる内容>\n"
            L"todo-add | <やること>\n"
            L"todo-done | <済んだやること>\n"
            L"todo-list\n"
            L"reminders\n"
            L"cancel-timers\n"
            L"none\n\n"
            L"発言：「"+said+L"」";
    }

    PlanRequest ReadPlanCommand(const std::wstring& answer,std::int64_t now)
    {
        using Kind=PlanRequest::Kind;
        std::wstringstream lines(Normalize(answer));
        for(std::wstring line;std::getline(lines,line);)
        {
            line=Tidy(line);
            while(!line.empty()&&std::wstring(L"`*-・>").find(line[0])!=std::wstring::npos)line=Tidy(line.substr(1));
            if(line.empty())continue;
            std::wstring head=line,text;
            if(const size_t bar=line.find(L'|');bar!=std::wstring::npos){head=Tidy(line.substr(0,bar));text=ItemWords(line.substr(bar+1));}
            std::transform(head.begin(),head.end(),head.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});
            PlanRequest r;
            r.text=text;
            if(head.rfind(L"none",0)==0)return {};
            if(head.rfind(L"timer",0)==0)
            {
                const std::wstring n=Tidy(head.substr(5));
                int seconds=_wtoi(n.c_str());
                if(!n.empty()&&(n.back()==L'm'))seconds*=60;
                if(!n.empty()&&(n.back()==L'h'))seconds*=3600;
                if(seconds<=0||seconds>99*3600)return {};
                r.kind=Kind::Timer;r.seconds=seconds;r.due=now+seconds;
                return r;
            }
            if(head.rfind(L"remind",0)==0||head.rfind(L"daily",0)==0)
            {
                const bool daily=head.rfind(L"daily",0)==0;
                int y=0,mo=0,d=0,h=-1,mi=-1;
                std::tm at=Local(now);
                if(daily){if(swscanf_s(head.c_str()+5,L" %d:%d",&h,&mi)!=2)return {};}
                else
                {
                    if(swscanf_s(head.c_str()+6,L" %d-%d-%d %d:%d",&y,&mo,&d,&h,&mi)!=5)return {};
                    at.tm_year=y-1900;at.tm_mon=mo-1;at.tm_mday=d;
                }
                if(h<0||h>23||mi<0||mi>59)return {};
                at.tm_hour=h;at.tm_min=mi;at.tm_sec=0;
                std::int64_t t=Make(at);
                if(daily)while(t<=now)t=AddDays(t,1);
                if(t<=now)return {};
                r.kind=Kind::Remind;r.due=t;r.daily=daily;
                return r;
            }
            if(head.rfind(L"todo-add",0)==0){if(text.empty())return {};r.kind=Kind::TodoAdd;return r;}
            if(head.rfind(L"todo-done",0)==0){if(text.empty())return {};r.kind=Kind::TodoDone;return r;}
            if(head.rfind(L"todo-list",0)==0){r.kind=Kind::TodoList;return r;}
            if(head.rfind(L"reminders",0)==0){r.kind=Kind::AlarmList;return r;}
            if(head.rfind(L"cancel-timer",0)==0){r.kind=Kind::CancelTimers;return r;}
            return {};   // the first line says it; anything else is not a command
        }
        return {};
    }

    // ---- Words for times -----------------------------------------------------------------

    std::wstring DurationText(int seconds)
    {
        const int h=seconds/3600,m=seconds/60%60,s=seconds%60;
        std::wstring out;
        if(English())
        {
            auto part=[&](int n,const wchar_t* unit){if(n){if(!out.empty())out+=L" ";out+=std::to_wstring(n)+L" "+unit+(n==1?L"":L"s");}};
            part(h,L"hour");part(m,L"minute");part(s,L"second");
            return out;
        }
        if(h)out+=std::to_wstring(h)+L"時間";
        if(m)out+=std::to_wstring(m)+L"分";
        if(s)out+=std::to_wstring(s)+L"秒";
        return out;
    }

    std::wstring TimeText(std::int64_t at,std::int64_t now,bool daily)
    {
        const std::tm t=Local(at),n=Local(now);
        std::tm a0=t,n0=n;
        a0.tm_hour=n0.tm_hour=12;a0.tm_min=n0.tm_min=a0.tm_sec=n0.tm_sec=0;
        const int days=static_cast<int>((Make(a0)-Make(n0)+Day/2)/Day);
        wchar_t clock[32]{};
        if(English())
        {
            swprintf_s(clock,L"%d:%02d %s",(t.tm_hour+11)%12+1,t.tm_min,t.tm_hour<12?L"AM":L"PM");
            if(daily)return std::wstring(L"every day at ")+clock;
            if(days==0)return clock;
            if(days==1)return std::wstring(L"tomorrow at ")+clock;
            static constexpr const wchar_t* Days[]={L"Sun",L"Mon",L"Tue",L"Wed",L"Thu",L"Fri",L"Sat"};
            static constexpr const wchar_t* Months[]={L"Jan",L"Feb",L"Mar",L"Apr",L"May",L"Jun",L"Jul",L"Aug",L"Sep",L"Oct",L"Nov",L"Dec"};
            wchar_t date[48]{};
            swprintf_s(date,L"%s, %s %d at ",Days[t.tm_wday%7],Months[t.tm_mon%12],t.tm_mday);
            return date+std::wstring(clock);
        }
        if(t.tm_min)swprintf_s(clock,L"%d時%d分",t.tm_hour,t.tm_min);
        else swprintf_s(clock,L"%d時",t.tm_hour);
        if(daily)return std::wstring(L"毎日")+clock;
        if(days==0)return clock;
        if(days==1)return std::wstring(L"明日の")+clock;
        if(days==2)return std::wstring(L"あさっての")+clock;
        static constexpr const wchar_t* Days[]={L"日",L"月",L"火",L"水",L"木",L"金",L"土"};
        wchar_t date[48]{};
        swprintf_s(date,L"%d月%d日（%s）の",t.tm_mon+1,t.tm_mday,Days[t.tm_wday%7]);
        return date+std::wstring(clock);
    }
}
