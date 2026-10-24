#define NOMINMAX
#include "Calendar.hpp"
#include "HttpJson.hpp"
#include "Text.hpp"
#include <wincrypt.h>
#include <algorithm>
#include <ctime>
#include <map>
#include <set>
#include <sstream>
#pragma comment(lib,"winhttp.lib")
#pragma comment(lib,"crypt32.lib")

namespace MikuMikuDesktopPerch
{
    using namespace Net;

    namespace
    {
        constexpr wchar_t Host[]=L"calendar.google.com";
        constexpr size_t MaxBytes=8u<<20;
        constexpr size_t MaxEvents=300;
        constexpr std::int64_t Day=24*60*60;

        std::wstring FromUtf8(const std::string& text)
        {
            if(text.empty())return {};
            const int n=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
            std::wstring out(static_cast<size_t>(n),L'\0');
            MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),out.data(),n);
            return out;
        }

        std::tm Local(std::int64_t at)
        {
            std::tm tm{};
            const std::time_t t=static_cast<std::time_t>(at);
            localtime_s(&tm,&t);
            return tm;
        }
        std::int64_t Make(std::tm tm){tm.tm_isdst=-1;return static_cast<std::int64_t>(std::mktime(&tm));}

        std::wstring Trim(std::wstring s)
        {
            const size_t a=s.find_first_not_of(L" \t\r\n　"),b=s.find_last_not_of(L" \t\r\n　");
            return a==std::wstring::npos?std::wstring():s.substr(a,b-a+1);
        }

        // GET the address from calendar.google.com over HTTPS, redirects not followed (the
        // address goes nowhere else), at most MaxBytes.
        std::wstring Get(const std::wstring& address,std::string& body)
        {
            if(const std::wstring problem=CheckCalendarAddress(address);!problem.empty())return problem;
            Url url;
            Crack(Trim(address),url);
            HINTERNET session=WinHttpOpen(L"MikuMikuDesktopPerch",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
            if(!session)return T(L"インターネットに接続できませんでした。",L"Could not connect to the internet.");
            WinHttpSetTimeouts(session,10000,10000,15000,30000);
            std::wstring error;
            HINTERNET connection=WinHttpConnect(session,url.host.c_str(),url.port,0);
            HINTERNET request=connection?WinHttpOpenRequest(connection,L"GET",url.path.c_str(),nullptr,WINHTTP_NO_REFERER,
                                                            WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE):nullptr;
            if(request)
            {
                DWORD policy=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
                WinHttpSetOption(request,WINHTTP_OPTION_REDIRECT_POLICY,&policy,sizeof(policy));
            }
            if(!request||!WinHttpSendRequest(request,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(request,nullptr))
                error=T(L"Googleカレンダーに接続できませんでした。",L"Could not connect to Google Calendar.");
            else
            {
                DWORD status=0,size=sizeof(status);
                WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX);
                if(status==404||status==403||status==401)
                    error=T(L"カレンダーが見つかりませんでした。アドレスが正しいか（Googleカレンダーで作り直していないか）確かめてください。",
                            L"The calendar was not found. Check the address (it changes when it is reset in Google Calendar).");
                else if(status!=200)error=Fmt(T(L"Googleカレンダーから受け取れませんでした（{0}）。",L"Could not get the calendar ({0})."),{std::to_wstring(status)});
                for(DWORD available=0;error.empty()&&WinHttpQueryDataAvailable(request,&available)&&available>0;)
                {
                    const size_t at=body.size();
                    if(at+available>MaxBytes){error=T(L"カレンダーが大きすぎます。",L"The calendar is too large.");break;}
                    body.resize(at+available);
                    DWORD read=0;
                    if(!WinHttpReadData(request,body.data()+at,available,&read)){error=T(L"受け取る途中で切れました。",L"The connection was lost on the way.");break;}
                    body.resize(at+read);
                }
            }
            if(request)WinHttpCloseHandle(request);
            if(connection)WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return error;
        }

        // ---- iCal ----

        struct Property
        {
            std::string name,value;
            std::map<std::string,std::string> params;
        };

        Property ReadProperty(const std::string& line)
        {
            Property p;
            // name;param=value;...:value (a colon inside a quoted parameter does not count)
            size_t colon=std::string::npos;
            bool quoted=false;
            for(size_t i=0;i<line.size();++i)
            {
                if(line[i]=='"')quoted=!quoted;
                else if(line[i]==':'&&!quoted){colon=i;break;}
            }
            if(colon==std::string::npos)return p;
            p.value=line.substr(colon+1);
            std::stringstream head(line.substr(0,colon));
            std::string part;
            bool first=true;
            while(std::getline(head,part,';'))
            {
                if(first){p.name=part;first=false;continue;}
                const size_t eq=part.find('=');
                if(eq!=std::string::npos)p.params[part.substr(0,eq)]=part.substr(eq+1);
            }
            for(auto& c:p.name)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return p;
        }

        // 20261024 (a day: local midnight, allDay), 20261024T150000 (local time; with a time
        // zone, taken as local too) or 20261024T060000Z (UTC).
        bool ReadTime(const std::string& value,std::int64_t& at,bool& allDay)
        {
            int y=0,mo=0,d=0,h=0,mi=0,s=0;
            if(value.size()>=8&&sscanf_s(value.c_str(),"%4d%2d%2d",&y,&mo,&d)==3&&value.size()<15)
            {
                std::tm tm{};
                tm.tm_year=y-1900;tm.tm_mon=mo-1;tm.tm_mday=d;
                at=Make(tm);
                allDay=true;
                return at>0;
            }
            if(value.size()<15||sscanf_s(value.c_str(),"%4d%2d%2dT%2d%2d%2d",&y,&mo,&d,&h,&mi,&s)!=6)return false;
            std::tm tm{};
            tm.tm_year=y-1900;tm.tm_mon=mo-1;tm.tm_mday=d;tm.tm_hour=h;tm.tm_min=mi;tm.tm_sec=s;
            at=value.back()=='Z'?static_cast<std::int64_t>(_mkgmtime(&tm)):Make(tm);
            allDay=false;
            return at>0;
        }

        // P1D, PT1H30M, P1W: seconds.
        std::int64_t ReadDuration(const std::string& value)
        {
            std::int64_t total=0,number=0;
            for(const char c:value)
            {
                if(c>='0'&&c<='9'){number=number*10+(c-'0');continue;}
                if(c=='W')total+=number*7*Day;
                else if(c=='D')total+=number*Day;
                else if(c=='H')total+=number*3600;
                else if(c=='M')total+=number*60;
                else if(c=='S')total+=number;
                number=0;
            }
            return value.rfind('-',0)==0?-total:total;
        }

        std::wstring Unescape(const std::string& value)
        {
            std::string out;
            for(size_t i=0;i<value.size();++i)
            {
                if(value[i]=='\\'&&i+1<value.size())
                {
                    const char next=value[++i];
                    out+=next=='n'||next=='N'?' ':next;
                }
                else out+=value[i];
            }
            return Trim(FromUtf8(out));
        }

        struct Raw
        {
            std::string uid,rule;
            std::wstring title;
            std::int64_t start=0,end=-1,duration=-1,recurrenceId=0;
            bool allDay=false,cancelled=false;
            std::vector<std::int64_t> exdates;
        };

        int WeekdayCode(const std::string& code)
        {
            static constexpr const char* Codes[]={"SU","MO","TU","WE","TH","FR","SA"};
            for(int i=0;i<7;++i)if(code==Codes[i])return i;
            return -1;
        }

        // The occurrences of a repeating event from its first start on, until one starts after
        // to (or the rule ends), each passed to emit (which also gets excluded ones: COUNT
        // counts them).
        template<class Emit> void Expand(const Raw& e,std::int64_t to,Emit emit)
        {
            std::map<std::string,std::string> rule;
            std::stringstream parts(e.rule);
            for(std::string part;std::getline(parts,part,';');)
                if(const size_t eq=part.find('=');eq!=std::string::npos)rule[part.substr(0,eq)]=part.substr(eq+1);
            const std::string freq=rule["FREQ"];
            const int interval=std::max(1,rule.count("INTERVAL")?atoi(rule["INTERVAL"].c_str()):1);
            const int count=rule.count("COUNT")?atoi(rule["COUNT"].c_str()):-1;
            std::int64_t until=0;
            bool untilDay=false;
            if(rule.count("UNTIL"))ReadTime(rule["UNTIL"],until,untilDay);
            if(untilDay)until+=Day-1;
            std::vector<std::pair<int,int>> byDay;   // (nth or 0, weekday)
            if(rule.count("BYDAY"))
            {
                std::stringstream days(rule["BYDAY"]);
                for(std::string d;std::getline(days,d,',');)
                {
                    if(d.size()<2)continue;
                    const int wd=WeekdayCode(d.substr(d.size()-2));
                    if(wd>=0)byDay.push_back({d.size()>2?atoi(d.substr(0,d.size()-2).c_str()):0,wd});
                }
            }
            const std::tm base=Local(e.start);
            int emitted=0;
            for(int k=0;k<60000;++k)
            {
                std::vector<std::int64_t> starts;
                auto day=[&](int months,int years,int days){
                    std::tm tm=base;
                    tm.tm_year+=years;tm.tm_mon+=months;tm.tm_mday+=days;
                    const int wantedDay=tm.tm_mday;
                    const std::int64_t t=Make(tm);
                    // The 31st in a short month, 29 February in another year: none that time.
                    if(days==0&&Local(t).tm_mday!=wantedDay)return;
                    starts.push_back(t);
                };
                if(freq=="DAILY")day(0,0,k*interval);
                else if(freq=="WEEKLY")
                {
                    if(byDay.empty())day(0,0,7*k*interval);
                    else
                    {
                        const int sinceMonday=(base.tm_wday+6)%7;
                        for(const auto& [nth,wd]:byDay)day(0,0,7*k*interval-sinceMonday+(wd+6)%7);
                    }
                }
                else if(freq=="MONTHLY")
                {
                    const auto numbered=std::find_if(byDay.begin(),byDay.end(),[](const auto& b){return b.first!=0;});
                    if(numbered==byDay.end())day(k*interval,0,0);
                    else
                    {
                        // The nth (or nth from last) such weekday of the month.
                        std::tm first=base;
                        first.tm_mday=1;first.tm_mon+=k*interval;
                        const std::int64_t f=Make(first);
                        const std::tm fm=Local(f);
                        std::tm last=fm;last.tm_mon+=1;last.tm_mday=0;
                        const int daysIn=Local(Make(last)).tm_mday;
                        const int firstMatch=1+(numbered->second-fm.tm_wday+7)%7;
                        int dom=numbered->first>0?firstMatch+7*(numbered->first-1):0;
                        if(numbered->first<0){dom=firstMatch;while(dom+7<=daysIn)dom+=7;dom-=7*(-numbered->first-1);}
                        if(dom>=1&&dom<=daysIn){std::tm at=fm;at.tm_mday=dom;at.tm_hour=base.tm_hour;at.tm_min=base.tm_min;at.tm_sec=base.tm_sec;starts.push_back(Make(at));}
                    }
                }
                else if(freq=="YEARLY")day(0,k*interval,0);
                else {emit(e.start);return;}   // a rule not understood: just the first time
                std::sort(starts.begin(),starts.end());
                bool past=false;
                for(const std::int64_t t:starts)
                {
                    if(t<e.start)continue;
                    if((until&&t>until)||(count>=0&&emitted>=count)||t>to){past=true;break;}
                    ++emitted;
                    emit(t);
                }
                if(past)return;
            }
        }
    }

    std::wstring CheckCalendarAddress(const std::wstring& address)
    {
        const std::wstring text=Trim(address);
        Url url;
        std::wstring host=url.host;
        if(!Crack(text,url)||!url.secure)return T(L"Googleカレンダーの「iCal形式の非公開アドレス」（https://calendar.google.com/... で始まるもの）を入れてください。",
                                                    L"Enter the calendar's \"Secret address in iCal format\" (starting with https://calendar.google.com/).");
        host=url.host;
        std::transform(host.begin(),host.end(),host.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});
        if(host!=Host||url.path.rfind(L"/calendar/ical/",0)!=0||url.path.size()<20||url.path.substr(url.path.size()-4)!=L".ics")
            return T(L"Googleカレンダーの「iCal形式の非公開アドレス」（https://calendar.google.com/calendar/ical/... .ics）だけを使えます。",
                     L"Only a Google Calendar \"Secret address in iCal format\" (https://calendar.google.com/calendar/ical/... .ics) can be used.");
        return {};
    }

    std::vector<CalendarEvent> ReadCalendar(const std::string& ics,std::int64_t from,std::int64_t to)
    {
        // Lines folded with a leading space or tab are joined first.
        std::vector<std::string> lines;
        {
            std::stringstream in(ics);
            for(std::string line;std::getline(in,line);)
            {
                if(!line.empty()&&line.back()=='\r')line.pop_back();
                if(!line.empty()&&(line[0]==' '||line[0]=='\t')&&!lines.empty())lines.back()+=line.substr(1);
                else lines.push_back(line);
            }
        }
        std::vector<Raw> raws;
        Raw current;
        bool inEvent=false;
        int depth=0;   // components inside the event (VALARM)
        for(const std::string& line:lines)
        {
            if(line=="BEGIN:VEVENT"){current=Raw{};inEvent=true;depth=0;continue;}
            if(!inEvent)continue;
            if(line.rfind("BEGIN:",0)==0){++depth;continue;}
            if(line.rfind("END:",0)==0)
            {
                if(depth>0){--depth;continue;}
                if(line=="END:VEVENT"){inEvent=false;if(current.start>0)raws.push_back(current);}
                continue;
            }
            if(depth>0)continue;
            const Property p=ReadProperty(line);
            bool allDay=false;
            if(p.name=="UID")current.uid=p.value;
            else if(p.name=="SUMMARY")current.title=Unescape(p.value);
            else if(p.name=="DTSTART"){ReadTime(p.value,current.start,allDay);current.allDay=allDay;}
            else if(p.name=="DTEND"){std::int64_t t=0;if(ReadTime(p.value,t,allDay))current.end=t;}
            else if(p.name=="DURATION")current.duration=ReadDuration(p.value);
            else if(p.name=="RRULE")current.rule=p.value;
            else if(p.name=="RECURRENCE-ID")ReadTime(p.value,current.recurrenceId,allDay);
            else if(p.name=="STATUS")current.cancelled=p.value=="CANCELLED";
            else if(p.name=="EXDATE")
            {
                std::stringstream dates(p.value);
                for(std::string d;std::getline(dates,d,',');){std::int64_t t=0;if(ReadTime(d,t,allDay))current.exdates.push_back(t);}
            }
        }

        // Single changed or cancelled occurrences of a repeating event come as events of their
        // own (RECURRENCE-ID: the start they replace).
        std::set<std::pair<std::string,std::int64_t>> replaced;
        for(const Raw& r:raws)if(r.recurrenceId)replaced.insert({r.uid,r.recurrenceId});

        std::vector<CalendarEvent> events;
        auto add=[&](const Raw& r,std::int64_t start){
            const std::int64_t length=r.end>=0?r.end-r.start:r.duration>=0?r.duration:(r.allDay?Day:0);
            const std::int64_t end=start+std::max<std::int64_t>(0,length);
            if(start>=to||(end<=from&&start<from)||r.title.empty())return;
            CalendarEvent e;
            e.title=r.title;
            e.start=start;
            e.end=end;
            e.allDay=r.allDay;
            e.key=FromUtf8(r.uid)+L"@"+std::to_wstring(start);
            events.push_back(e);
        };
        for(const Raw& r:raws)
        {
            if(r.cancelled)continue;
            if(r.rule.empty()||r.recurrenceId){add(r,r.start);continue;}
            Expand(r,to,[&](std::int64_t start){
                if(std::find(r.exdates.begin(),r.exdates.end(),start)!=r.exdates.end())return;
                if(replaced.count({r.uid,start}))return;
                add(r,start);
            });
        }
        std::sort(events.begin(),events.end(),[](const CalendarEvent& a,const CalendarEvent& b){return a.start<b.start;});
        if(events.size()>MaxEvents)events.resize(MaxEvents);
        return events;
    }

    std::wstring ProtectText(const std::wstring& text)
    {
        if(text.empty())return {};
        DATA_BLOB in{static_cast<DWORD>(text.size()*sizeof(wchar_t)),reinterpret_cast<BYTE*>(const_cast<wchar_t*>(text.data()))},out{};
        if(!CryptProtectData(&in,L"MikuMikuDesktopPerch",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&out))return {};
        DWORD n=0;
        std::wstring base64;
        if(CryptBinaryToStringW(out.pbData,out.cbData,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,nullptr,&n))
        {
            base64.resize(n);
            if(CryptBinaryToStringW(out.pbData,out.cbData,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,base64.data(),&n))base64.resize(n);
            else base64.clear();
        }
        LocalFree(out.pbData);
        return base64.empty()?std::wstring():L"dpapi:"+base64;
    }

    std::wstring UnprotectText(const std::wstring& stored)
    {
        if(stored.rfind(L"dpapi:",0)!=0)return {};
        const std::wstring base64=stored.substr(6);
        DWORD n=0;
        if(!CryptStringToBinaryW(base64.c_str(),static_cast<DWORD>(base64.size()),CRYPT_STRING_BASE64,nullptr,&n,nullptr,nullptr))return {};
        std::vector<BYTE> bytes(n);
        if(!CryptStringToBinaryW(base64.c_str(),static_cast<DWORD>(base64.size()),CRYPT_STRING_BASE64,bytes.data(),&n,nullptr,nullptr))return {};
        DATA_BLOB in{n,bytes.data()},out{};
        if(!CryptUnprotectData(&in,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&out))return {};
        std::wstring text(reinterpret_cast<const wchar_t*>(out.pbData),out.cbData/sizeof(wchar_t));
        SecureZeroMemory(out.pbData,out.cbData);
        LocalFree(out.pbData);
        return text;
    }

    CalendarFeed::~CalendarFeed(){if(worker.joinable())worker.join();}

    void CalendarFeed::Fetch(const std::wstring& address,bool report)
    {
        if(busy)return;
        if(worker.joinable())worker.join();
        busy=true;
        worker=std::thread([this,address,report,notify=notify]{
            auto* result=new CalendarResult;
            result->report=report;
            std::string body;
            result->error=Get(address,body);
            if(result->error.empty())
            {
                std::tm today=Local(static_cast<std::int64_t>(std::time(nullptr)));
                today.tm_hour=today.tm_min=today.tm_sec=0;
                const std::int64_t from=Make(today);
                today.tm_mday+=3;
                result->events=ReadCalendar(body,from,Make(today));
            }
            busy=false;
            if(!PostMessageW(notify,FetchedMessage,reinterpret_cast<WPARAM>(result),0))delete result;
        });
    }
}
