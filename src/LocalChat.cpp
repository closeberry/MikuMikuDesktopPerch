#include "LocalChat.hpp"
#include "Text.hpp"
#include "HttpJson.hpp"
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
    using namespace Net;

    namespace
    {
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

        void Post(HWND window,UINT message,const std::wstring& text,int id)
        {
            auto* copy=new std::wstring(text);
            if(!PostMessageW(window,message,reinterpret_cast<WPARAM>(copy),id))delete copy;
        }
    }

    std::wstring CheckLocalEndpoint(const std::wstring& endpoint)
    {
        Url url;
        if(!Crack(endpoint,url))return T(T(L"接続先のURLが正しくありません（例: http://127.0.0.1:11434/v1）。",L"The server address is not a valid URL (for example http://127.0.0.1:11434/v1)."),L"The server address is not a valid URL (for example http://127.0.0.1:11434/v1).");
        if(!IsLocalHost(url.host))return T(T(L"接続先には、このコンピューター（localhost、127.0.0.1）のサーバーだけを指定できます。",L"Only a server on this computer (localhost, 127.0.0.1) can be used."),L"Only a server on this computer (localhost, 127.0.0.1) can be used.");
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
        else error=T(T(L"LLMのサーバーに接続できませんでした。Ollama や LM Studio などが起動しているか確かめてください。",L"Could not connect to the LLM server. Check that Ollama, LM Studio or the like is running."),L"Could not connect to the LLM server. Check that Ollama, LM Studio or the like is running.");
        if(request)WinHttpCloseHandle(request);
        if(connection)WinHttpCloseHandle(connection);
        if(session)WinHttpCloseHandle(session);
        if(!error.empty())return error;
        if(status!=200)return Fmt(T(L"モデルの一覧を取得できませんでした（{0}）: ",L"Could not get the list of models ({0}): "),{std::to_wstring(status)})+ServerError(body);
        Json j;
        const Json* data=JsonParser(body).Parse(j)?Path(&j,{"data"}):nullptr;
        if(!data||data->type!=Json::Type::Array)return T(T(L"モデルの一覧を読み取れませんでした。",L"Could not read the list of models."),L"Could not read the list of models.");
        for(const auto& item:data->items)
            if(const Json* id=item.Member("id");id&&id->type==Json::Type::String)models.push_back(Wide(id->text));
        if(models.empty())return T(T(L"サーバーにモデルがありません（Ollama なら ollama pull でモデルを入れてください）。",L"The server has no models (with Ollama, add one with ollama pull)."),L"The server has no models (with Ollama, add one with ollama pull).");
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
        if(!Crack(r->endpoint,url)||!IsLocalHost(url.host))fail(T(T(L"接続先のURLが正しくないか、このコンピューターのサーバーではありません。",L"The server address is not valid, or not on this computer."),L"The server address is not valid, or not on this computer."));
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
            if(!r->request)fail(T(T(L"接続の準備に失敗しました。",L"Could not prepare the connection."),L"Could not prepare the connection."));
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
                fail(T(T(L"LLMのサーバーに接続できませんでした。Ollama や LM Studio などが起動しているか、接続先が合っているか確かめてください。",L"Could not connect to the LLM server. Check that Ollama, LM Studio or the like is running and the address is right."),L"Could not connect to the LLM server. Check that Ollama, LM Studio or the like is running and the address is right."));
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
                fail(T(L"LLMのサーバーがエラーを返しました: ",L"The LLM server returned an error: ")+(e->type==Json::Type::String?Wide(e->text):ServerError(line)));
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
            if(!WinHttpReadData(request,buffer,sizeof(buffer),&read)){if(!r->cancelled)fail(T(T(L"返事の受信中に接続が切れました。",L"The connection was lost while the reply came in."),L"The connection was lost while the reply came in."));break;}
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
            if(pending.size()>MaxLineBytes){fail(T(T(L"LLMのサーバーの返事を読み取れませんでした。",L"Could not read the LLM server's reply."),L"Could not read the LLM server's reply."));break;}
        }
        if(error.empty()&&!r->cancelled)
        {
            if(status!=200&&status!=0)fail(Fmt(T(L"LLMのサーバーがエラーを返しました（{0}）: ",L"The LLM server returned an error ({0}): "),{std::to_wstring(status)})+ServerError(all));
            else if(!streamed)
            {
                // A server that ignored "stream": one JSON body with the whole message.
                Json j;
                const Json* text=JsonParser(all).Parse(j)?Path(&j,{"choices","0","message","content"}):nullptr;
                if(text&&text->type==Json::Type::String)Post(notify,TextMessage,Wide(text->text.substr(0,MaxReplyBytes)),r->id);
                else fail(T(T(L"LLMのサーバーの返事を読み取れませんでした。",L"Could not read the LLM server's reply."),L"Could not read the LLM server's reply."));
            }
        }
        r->Close();
        if(!r->cancelled)Post(notify,DoneMessage,error,r->id);
        busy=false;
    }
}
