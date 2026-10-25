#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include "ApiServer.hpp"
#include "Log.hpp"
#include "Text.hpp"
#include <algorithm>
#include <climits>
#include <cctype>
#include <string_view>
#pragma comment(lib,"ws2_32.lib")

namespace MikuMikuDesktopPerch
{
    namespace
    {
        constexpr size_t MaxHeader=16*1024,MaxBody=64*1024;
        constexpr size_t MaxLargeBody=16*1024*1024;   // a VMD sent as it is (POST /motion), or a line with its sound (POST /say)

        std::string Lower(std::string_view text)
        {
            std::string out(text);
            std::transform(out.begin(),out.end(),out.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
            return out;
        }

        std::string Trim(std::string_view text)
        {
            const size_t a=text.find_first_not_of(" \t"),b=text.find_last_not_of(" \t");
            return a==std::string_view::npos?std::string():std::string(text.substr(a,b-a+1));
        }

        const char* Reason(int status)
        {
            switch(status)
            {
            case 200:return "OK";
            case 400:return "Bad Request";
            case 403:return "Forbidden";
            case 404:return "Not Found";
            case 405:return "Method Not Allowed";
            case 409:return "Conflict";
            case 413:return "Payload Too Large";
            case 503:return "Service Unavailable";
            default:return "Error";
            }
        }

        void Send(SOCKET client,int status,const std::string& json)
        {
            std::string out="HTTP/1.1 "+std::to_string(status)+" "+Reason(status)+"\r\n"
                            "Content-Type: application/json; charset=utf-8\r\n"
                            "Content-Length: "+std::to_string(json.size())+"\r\n"
                            "Cache-Control: no-store\r\n"
                            "Connection: close\r\n\r\n"+json;
            for(size_t sent=0;sent<out.size();)
            {
                const int n=send(client,out.data()+sent,static_cast<int>(std::min<size_t>(out.size()-sent,INT_MAX)),0);
                if(n<=0)return;
                sent+=static_cast<size_t>(n);
            }
        }

        std::string Error(const char* text){return std::string("{\"ok\":false,\"error\":\"")+text+"\"}";}
    }

    ApiServer::~ApiServer(){Stop();}

    std::wstring ApiServer::Start(int listenPort)
    {
        Stop();
        WSADATA wsa{};
        if(WSAStartup(MAKEWORD(2,2),&wsa)!=0)return T(L"ネットワーク機能を初期化できませんでした。",L"Could not start networking.");
        SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        if(s==INVALID_SOCKET){WSACleanup();return T(L"ソケットを作成できませんでした。",L"Could not create a socket.");}
        // Another program already on the port must not be shared or taken over.
        BOOL exclusive=TRUE;
        setsockopt(s,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive));
        sockaddr_in address{};
        address.sin_family=AF_INET;
        address.sin_port=htons(static_cast<u_short>(listenPort));
        address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if(bind(s,reinterpret_cast<sockaddr*>(&address),sizeof(address))==SOCKET_ERROR||listen(s,8)==SOCKET_ERROR)
        {
            closesocket(s);
            WSACleanup();
            return Fmt(T(L"ポート {0} で待ち受けられませんでした（ほかのアプリが使っているかもしれません）。",
                         L"Could not listen on port {0} (another app may be using it)."),{std::to_wstring(listenPort)});
        }
        listener=s;
        port=listenPort;
        stopping=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        worker=std::thread([this]{Run();});
        Log(L"API: listening on 127.0.0.1:%d",port);
        return {};
    }

    void ApiServer::Stop()
    {
        if(!worker.joinable())return;
        SetEvent(stopping);
        closesocket(static_cast<SOCKET>(listener));   // accept returns at once
        worker.join();
        listener=INVALID_SOCKET;
        CloseHandle(stopping);
        stopping=nullptr;
        WSACleanup();
        Log(L"API: stopped");
    }

    void ApiServer::Run()
    {
        for(;;)
        {
            const SOCKET client=accept(static_cast<SOCKET>(listener),nullptr,nullptr);
            if(client==INVALID_SOCKET)
            {
                if(WaitForSingleObject(stopping,0)==WAIT_OBJECT_0)return;
                continue;
            }
            Serve(client);
            closesocket(client);
        }
    }

    void ApiServer::Serve(std::uintptr_t handle)
    {
        const SOCKET client=static_cast<SOCKET>(handle);
        const DWORD timeout=5000;
        setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
        setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));

        // The head, up to the blank line.
        std::string data;
        size_t headEnd=std::string::npos;
        char buffer[4096];
        while((headEnd=data.find("\r\n\r\n"))==std::string::npos)
        {
            if(data.size()>MaxHeader){Send(client,413,Error("request too large"));return;}
            const int n=recv(client,buffer,sizeof(buffer),0);
            if(n<=0)return;
            data.append(buffer,static_cast<size_t>(n));
        }
        auto request=std::make_shared<Request>();
        const std::string_view head(data.data(),headEnd);
        const size_t lineEnd=head.find("\r\n");
        const std::string_view line=head.substr(0,lineEnd);
        const size_t a=line.find(' '),b=a==std::string_view::npos?a:line.find(' ',a+1);
        if(b==std::string_view::npos){Send(client,400,Error("bad request line"));return;}
        request->method=std::string(line.substr(0,a));
        request->path=std::string(line.substr(a+1,b-a-1));
        if(const size_t q=request->path.find('?');q!=std::string::npos){request->query=request->path.substr(q+1);request->path.resize(q);}

        size_t length=0;
        bool hostOk=false,origin=false;
        for(size_t p=lineEnd==std::string_view::npos?head.size():lineEnd+2;p<head.size();)
        {
            size_t e=head.find("\r\n",p);
            if(e==std::string_view::npos)e=head.size();
            const std::string_view header=head.substr(p,e-p);
            p=e+2;
            const size_t colon=header.find(':');
            if(colon==std::string_view::npos)continue;
            const std::string name=Lower(Trim(header.substr(0,colon))),value=Trim(header.substr(colon+1));
            if(name=="content-length")length=static_cast<size_t>(std::strtoull(value.c_str(),nullptr,10));
            else if(name=="content-type")request->contentType=Trim(Lower(value.substr(0,value.find(';'))));
            else if(name=="origin")origin=true;
            else if(name=="host")
            {
                // Only this computer by name: a web page reaching here through DNS rebinding
                // carries its own host name.
                const std::string host=Lower(value);
                const std::string suffix=":"+std::to_string(port);
                const std::string bare=host.size()>suffix.size()&&host.ends_with(suffix)?host.substr(0,host.size()-suffix.size()):host;
                hostOk=bare=="127.0.0.1"||bare=="localhost"||bare=="[::1]";
            }
        }
        if(origin||!hostOk||request->method=="OPTIONS"){Send(client,403,Error("only programs on this computer may use the API, not web pages"));return;}
        const bool large=(request->path=="/motion"&&request->contentType=="application/octet-stream")||request->path=="/say";
        if(length>(large?MaxLargeBody:MaxBody))
        {
            // Read (and drop) what is being sent first, up to a point: a client still sending
            // when the connection closes sees it cut, not the answer.
            for(size_t left=length-std::min(length,data.size()-headEnd-4);left>0&&length<=4*MaxLargeBody;)
            {
                const int n=recv(client,buffer,sizeof(buffer),0);
                if(n<=0)break;
                left-=std::min(left,static_cast<size_t>(n));
            }
            Send(client,413,Error("request too large"));
            return;
        }
        request->body.reserve(length);
        request->body=data.substr(headEnd+4);
        while(request->body.size()<length)
        {
            const int n=recv(client,buffer,sizeof(buffer),0);
            if(n<=0)return;
            request->body.append(buffer,static_cast<size_t>(n));
        }
        request->body.resize(length);

        // Done on the window's thread; given up when the app is quitting or does not answer.
        if(!PostMessageW(notify,RequestMessage,reinterpret_cast<WPARAM>(new std::shared_ptr<Request>(request)),0))
        {
            Send(client,503,Error("the app is not ready"));
            return;
        }
        const HANDLE waits[]={request->done,stopping};
        if(WaitForMultipleObjects(2,waits,FALSE,10000)!=WAIT_OBJECT_0){Send(client,503,Error("the app did not answer"));return;}
        Send(client,request->status,request->response);
    }
}
