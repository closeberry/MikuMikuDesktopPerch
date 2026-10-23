#include "LocalChat.hpp"
#include <winhttp.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwctype>
#include <mutex>
#include <string_view>

#pragma comment(lib,"winhttp.lib")

namespace MikuMikuDesktopPerch
{
    namespace
    {
        std::string Utf8(const std::wstring& text)
        {
            if(text.empty())return {};
            const int size=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
            std::string out(static_cast<size_t>(size),'\0');
            WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),out.data(),size,nullptr,nullptr);
            return out;
        }

        std::wstring Wide(std::string_view text)
        {
            if(text.empty())return {};
            const int size=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
            std::wstring out(static_cast<size_t>(size),L'\0');
            MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),out.data(),size);
            return out;
        }

        std::string JsonString(const std::wstring& text)
        {
            std::string out="\"";
            for(const unsigned char c:Utf8(text))
            {
                switch(c)
                {
                case '"':out+="\\\"";break;
                case '\\':out+="\\\\";break;
                case '\n':out+="\\n";break;
                case '\r':out+="\\r";break;
                case '\t':out+="\\t";break;
                default:
                    if(c<0x20){char hex[8];sprintf_s(hex,"\\u%04x",c);out+=hex;}
                    else out+=static_cast<char>(c);
                }
            }
            return out+"\"";
        }

        // Just enough JSON to read the server's replies: values are parsed fully (with a depth
        // limit), strings come back as UTF-8.
        struct Json
        {
            enum class Type {Null,Bool,Number,String,Array,Object} type=Type::Null;
            std::string text;
            std::vector<Json> items;
            std::vector<std::pair<std::string,Json>> members;

            const Json* Member(std::string_view name) const
            {
                for(const auto& [key,value]:members)if(key==name)return &value;
                return nullptr;
            }
            const Json* Item(size_t index) const {return index<items.size()?&items[index]:nullptr;}
        };

        class JsonParser
        {
        public:
            explicit JsonParser(std::string_view text):s(text){}
            bool Parse(Json& out){Space();if(!Value(out,0))return false;Space();return p==s.size();}

        private:
            std::string_view s;
            size_t p=0;

            void Space(){while(p<s.size()&&(s[p]==' '||s[p]=='\t'||s[p]=='\n'||s[p]=='\r'))++p;}
            bool Literal(std::string_view word){if(s.substr(p,word.size())!=word)return false;p+=word.size();return true;}

            static void Append(std::string& out,unsigned code)
            {
                if(code<0x80)out+=static_cast<char>(code);
                else if(code<0x800){out+=static_cast<char>(0xC0|(code>>6));out+=static_cast<char>(0x80|(code&0x3F));}
                else if(code<0x10000){out+=static_cast<char>(0xE0|(code>>12));out+=static_cast<char>(0x80|((code>>6)&0x3F));out+=static_cast<char>(0x80|(code&0x3F));}
                else{out+=static_cast<char>(0xF0|(code>>18));out+=static_cast<char>(0x80|((code>>12)&0x3F));out+=static_cast<char>(0x80|((code>>6)&0x3F));out+=static_cast<char>(0x80|(code&0x3F));}
            }

            bool Hex4(unsigned& code)
            {
                if(p+4>s.size())return false;
                code=0;
                for(int i=0;i<4;++i)
                {
                    const char c=s[p++];
                    code<<=4;
                    if(c>='0'&&c<='9')code|=c-'0';
                    else if(c>='a'&&c<='f')code|=c-'a'+10;
                    else if(c>='A'&&c<='F')code|=c-'A'+10;
                    else return false;
                }
                return true;
            }

            bool String(std::string& out)
            {
                if(p>=s.size()||s[p]!='"')return false;
                ++p;
                while(p<s.size())
                {
                    const char c=s[p++];
                    if(c=='"')return true;
                    if(c!='\\'){out+=c;continue;}
                    if(p>=s.size())return false;
                    const char e=s[p++];
                    switch(e)
                    {
                    case '"':out+='"';break;
                    case '\\':out+='\\';break;
                    case '/':out+='/';break;
                    case 'b':out+='\b';break;
                    case 'f':out+='\f';break;
                    case 'n':out+='\n';break;
                    case 'r':out+='\r';break;
                    case 't':out+='\t';break;
                    case 'u':
                    {
                        unsigned code=0;
                        if(!Hex4(code))return false;
                        if(code>=0xD800&&code<0xDC00&&p+6<=s.size()&&s[p]=='\\'&&s[p+1]=='u')
                        {
                            p+=2;
                            unsigned low=0;
                            if(!Hex4(low))return false;
                            if(low>=0xDC00&&low<0xE000)code=0x10000+((code-0xD800)<<10)+(low-0xDC00);
                        }
                        Append(out,code);
                        break;
                    }
                    default:return false;
                    }
                }
                return false;
            }

            bool Value(Json& out,int depth)
            {
                if(depth>32||p>=s.size())return false;
                const char c=s[p];
                if(c=='{')
                {
                    out.type=Json::Type::Object;
                    ++p;Space();
                    if(p<s.size()&&s[p]=='}'){++p;return true;}
                    for(;;)
                    {
                        Space();
                        std::string key;
                        if(!String(key))return false;
                        Space();
                        if(p>=s.size()||s[p++]!=':')return false;
                        Space();
                        Json value;
                        if(!Value(value,depth+1))return false;
                        out.members.emplace_back(std::move(key),std::move(value));
                        Space();
                        if(p<s.size()&&s[p]==','){++p;continue;}
                        if(p<s.size()&&s[p]=='}'){++p;return true;}
                        return false;
                    }
                }
                if(c=='[')
                {
                    out.type=Json::Type::Array;
                    ++p;Space();
                    if(p<s.size()&&s[p]==']'){++p;return true;}
                    for(;;)
                    {
                        Space();
                        Json value;
                        if(!Value(value,depth+1))return false;
                        out.items.push_back(std::move(value));
                        Space();
                        if(p<s.size()&&s[p]==','){++p;continue;}
                        if(p<s.size()&&s[p]==']'){++p;return true;}
                        return false;
                    }
                }
                if(c=='"'){out.type=Json::Type::String;return String(out.text);}
                if(Literal("true")||Literal("false")){out.type=Json::Type::Bool;return true;}
                if(Literal("null")){out.type=Json::Type::Null;return true;}
                const size_t start=p;
                while(p<s.size()&&(std::isdigit(static_cast<unsigned char>(s[p]))||s[p]=='-'||s[p]=='+'||s[p]=='.'||s[p]=='e'||s[p]=='E'))++p;
                if(p==start)return false;
                out.type=Json::Type::Number;
                out.text=std::string(s.substr(start,p-start));
                return true;
            }
        };

        const Json* Path(const Json* j,std::initializer_list<std::string_view> keys)
        {
            for(const auto key:keys)
            {
                if(!j)return nullptr;
                if(key=="0")j=j->Item(0);
                else j=j->Member(key);
            }
            return j;
        }

        // The error a server explains in its body ({"error":{"message":...}} or {"error":"..."}).
        std::wstring ServerError(std::string_view body)
        {
            Json j;
            if(JsonParser(body).Parse(j))
            {
                if(const Json* m=Path(&j,{"error","message"});m&&m->type==Json::Type::String)return Wide(m->text);
                if(const Json* m=Path(&j,{"error"});m&&m->type==Json::Type::String)return Wide(m->text);
            }
            return Wide(body.substr(0,200));
        }

        struct Url
        {
            bool secure=false;
            std::wstring host,path;
            INTERNET_PORT port=0;
        };

        bool Crack(const std::wstring& text,Url& url)
        {
            URL_COMPONENTS parts{sizeof(parts)};
            wchar_t host[256]{},path[1024]{};
            parts.lpszHostName=host;parts.dwHostNameLength=static_cast<DWORD>(std::size(host));
            parts.lpszUrlPath=path;parts.dwUrlPathLength=static_cast<DWORD>(std::size(path));
            if(!WinHttpCrackUrl(text.c_str(),static_cast<DWORD>(text.size()),0,&parts))return false;
            if(parts.nScheme!=INTERNET_SCHEME_HTTP&&parts.nScheme!=INTERNET_SCHEME_HTTPS)return false;
            url.secure=parts.nScheme==INTERNET_SCHEME_HTTPS;
            url.host=host;
            url.path=path;
            url.port=parts.nPort;
            while(!url.path.empty()&&url.path.back()==L'/')url.path.pop_back();
            return true;
        }

        bool IsLocalHost(std::wstring host)
        {
            std::transform(host.begin(),host.end(),host.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});
            return host==L"localhost"||host==L"127.0.0.1"||host==L"::1"||host==L"[::1]";
        }

        void Post(HWND window,UINT message,const std::wstring& text,int id)
        {
            auto* copy=new std::wstring(text);
            if(!PostMessageW(window,message,reinterpret_cast<WPARAM>(copy),id))delete copy;
        }
    }

    std::wstring CheckLocalEndpoint(const std::wstring& endpoint)
    {
        Url url;
        if(!Crack(endpoint,url))return L"接続先のURLが正しくありません（例: http://127.0.0.1:11434/v1）。";
        if(!IsLocalHost(url.host))return L"接続先には、このコンピューター（localhost、127.0.0.1）のサーバーだけを指定できます。";
        return {};
    }

    std::wstring ListModels(const std::wstring& endpoint,std::vector<std::wstring>& models)
    {
        models.clear();
        if(auto problem=CheckLocalEndpoint(endpoint);!problem.empty())return problem;
        Url url;
        Crack(endpoint,url);
        std::wstring error;
        HINTERNET session=WinHttpOpen(L"MikuMikuDesktopPerch",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
        HINTERNET connection=nullptr,request=nullptr;
        std::string body;
        DWORD status=0;
        if(session)
        {
            WinHttpSetTimeouts(session,3000,3000,3000,5000);
            connection=WinHttpConnect(session,url.host.c_str(),url.port,0);
        }
        if(connection)
            request=WinHttpOpenRequest(connection,L"GET",(url.path+L"/models").c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,url.secure?WINHTTP_FLAG_SECURE:0);
        if(request&&WinHttpSendRequest(request,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)&&WinHttpReceiveResponse(request,nullptr))
        {
            DWORD size=sizeof(status);
            WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX);
            char buffer[4096];
            for(DWORD read=0;WinHttpReadData(request,buffer,sizeof(buffer),&read)&&read>0&&body.size()<(1u<<20);)body.append(buffer,read);
        }
        else error=L"LLMのサーバーに接続できませんでした。Ollama や LM Studio などが起動しているか確かめてください。";
        if(request)WinHttpCloseHandle(request);
        if(connection)WinHttpCloseHandle(connection);
        if(session)WinHttpCloseHandle(session);
        if(!error.empty())return error;
        if(status!=200)return L"モデルの一覧を取得できませんでした（"+std::to_wstring(status)+L"）: "+ServerError(body);
        Json j;
        const Json* data=JsonParser(body).Parse(j)?Path(&j,{"data"}):nullptr;
        if(!data||data->type!=Json::Type::Array)return L"モデルの一覧を読み取れませんでした。";
        for(const auto& item:data->items)
            if(const Json* id=item.Member("id");id&&id->type==Json::Type::String)models.push_back(Wide(id->text));
        if(models.empty())return L"サーバーにモデルがありません（Ollama なら ollama pull でモデルを入れてください）。";
        return {};
    }

    struct LocalChat::Request
    {
        int id=0;
        std::wstring endpoint,model;
        std::vector<ChatMessage> messages;
        std::atomic<bool> cancelled{false};
        std::mutex lock;          // guards the handles against Cancel closing them
        HINTERNET session=nullptr,connection=nullptr,request=nullptr;

        void Close()
        {
            std::lock_guard guard(lock);
            if(request){WinHttpCloseHandle(request);request=nullptr;}
            if(connection){WinHttpCloseHandle(connection);connection=nullptr;}
            if(session){WinHttpCloseHandle(session);session=nullptr;}
        }
    };

    LocalChat::LocalChat(HWND notify):notify(notify){}

    LocalChat::~LocalChat()
    {
        Cancel();
        if(worker.joinable())worker.join();
    }

    void LocalChat::Cancel()
    {
        if(!current)return;
        current->cancelled=true;
        current->Close();   // unblocks a read in progress
        current.reset();
    }

    int LocalChat::Start(const std::wstring& endpoint,const std::wstring& model,std::vector<ChatMessage> messages)
    {
        Cancel();
        if(worker.joinable())worker.join();
        auto request=std::make_shared<Request>();
        request->id=nextId++;
        request->endpoint=endpoint;
        request->model=model;
        request->messages=std::move(messages);
        current=request;
        busy=true;
        worker=std::thread([this,request]{Run(request);});
        return request->id;
    }

    void LocalChat::Run(std::shared_ptr<Request> r)
    {
        std::wstring error;
        // Hard limits: a reply this long is cut, a line this long is not a server's.
        constexpr size_t MaxReplyBytes=64*1024,MaxLineBytes=256*1024;
        auto fail=[&](const std::wstring& text){if(error.empty())error=text;};
        Url url;
        if(!Crack(r->endpoint,url)||!IsLocalHost(url.host))fail(L"接続先のURLが正しくないか、このコンピューターのサーバーではありません。");
        if(error.empty())
        {
            std::lock_guard guard(r->lock);
            // No proxy: the server is on this machine.
            r->session=WinHttpOpen(L"MikuMikuDesktopPerch",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
            if(r->session)
            {
                WinHttpSetTimeouts(r->session,5000,5000,30000,180000);
                r->connection=WinHttpConnect(r->session,url.host.c_str(),url.port,0);
            }
            if(r->connection)
                r->request=WinHttpOpenRequest(r->connection,L"POST",(url.path+L"/chat/completions").c_str(),nullptr,WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES,url.secure?WINHTTP_FLAG_SECURE:0);
            if(!r->request)fail(L"接続の準備に失敗しました。");
        }
        // Thinking first (Gemma 4, Qwen 3 and others) only delays a chat reply: llama.cpp's
        // server turns it off with this template flag; servers that do not know it ignore it.
        std::string body="{\"model\":"+JsonString(r->model)+",\"stream\":true,\"chat_template_kwargs\":{\"enable_thinking\":false},\"messages\":[";
        for(size_t i=0;i<r->messages.size();++i)
            body+=(i?",":"")+std::string("{\"role\":")+JsonString(r->messages[i].role)+",\"content\":"+JsonString(r->messages[i].content)+"}";
        body+="]}";
        HINTERNET request=nullptr;
        {
            std::lock_guard guard(r->lock);
            request=r->request;
        }
        if(error.empty()&&!r->cancelled)
        {
            const wchar_t* headers=L"Content-Type: application/json\r\n";
            if(!WinHttpSendRequest(request,headers,static_cast<DWORD>(-1),body.data(),static_cast<DWORD>(body.size()),static_cast<DWORD>(body.size()),0)||
               !WinHttpReceiveResponse(request,nullptr))
                fail(L"LLMのサーバーに接続できませんでした。Ollama や LM Studio などが起動しているか、接続先が合っているか確かめてください。");
        }
        DWORD status=0;
        if(error.empty()&&!r->cancelled)
        {
            DWORD size=sizeof(status);
            WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX);
        }
        std::string pending,all;
        size_t replied=0;
        bool done=false,streamed=false;
        auto handleLine=[&](std::string_view line){
            while(!line.empty()&&(line.back()=='\r'||line.back()==' '))line.remove_suffix(1);
            if(line.substr(0,5)!="data:")return;
            streamed=true;
            line.remove_prefix(5);
            while(!line.empty()&&line.front()==' ')line.remove_prefix(1);
            if(line=="[DONE]"){done=true;return;}
            Json j;
            if(!JsonParser(line).Parse(j))return;
            if(const Json* e=Path(&j,{"error"}))
            {
                fail(L"LLMのサーバーがエラーを返しました: "+(e->type==Json::Type::String?Wide(e->text):ServerError(line)));
                done=true;
                return;
            }
            const Json* text=Path(&j,{"choices","0","delta","content"});
            if(text&&text->type==Json::Type::String&&!text->text.empty()&&replied<MaxReplyBytes)
            {
                replied+=text->text.size();
                Post(notify,TextMessage,Wide(text->text),r->id);
            }
            if(const Json* finish=Path(&j,{"choices","0","finish_reason"});finish&&finish->type==Json::Type::String)done=true;
        };
        while(error.empty()&&!r->cancelled&&!done)
        {
            char buffer[4096];
            DWORD read=0;
            if(!WinHttpReadData(request,buffer,sizeof(buffer),&read)){if(!r->cancelled)fail(L"返事の受信中に接続が切れました。");break;}
            if(read==0)break;
            if(status!=200){if(all.size()<4096)all.append(buffer,read);continue;}
            pending.append(buffer,read);
            if(all.size()<MaxLineBytes)all.append(buffer,read);
            for(size_t nl;(nl=pending.find('\n'))!=std::string::npos;)
            {
                handleLine(std::string_view(pending).substr(0,nl));
                pending.erase(0,nl+1);
                if(done)break;
            }
            if(pending.size()>MaxLineBytes){fail(L"LLMのサーバーの返事を読み取れませんでした。");break;}
        }
        if(error.empty()&&!r->cancelled)
        {
            if(status!=200&&status!=0)fail(L"LLMのサーバーがエラーを返しました（"+std::to_wstring(status)+L"）: "+ServerError(all));
            else if(!streamed)
            {
                // A server that ignored "stream": one JSON body with the whole message.
                Json j;
                const Json* text=JsonParser(all).Parse(j)?Path(&j,{"choices","0","message","content"}):nullptr;
                if(text&&text->type==Json::Type::String)Post(notify,TextMessage,Wide(text->text.substr(0,MaxReplyBytes)),r->id);
                else fail(L"LLMのサーバーの返事を読み取れませんでした。");
            }
        }
        r->Close();
        if(!r->cancelled)Post(notify,DoneMessage,error,r->id);
        busy=false;
    }
}
