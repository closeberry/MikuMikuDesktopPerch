#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace MikuMikuDesktopPerch
{
    // Lets other programs on this computer drive the characters: a small HTTP server on
    // 127.0.0.1 only (never reachable from other machines). Requests are answered one at a
    // time; each is handed to the notify window's thread, which does it and fills the answer.
    // Requests from web pages are refused (they carry an Origin header, or a Host other than
    // this computer's), so a site open in a browser cannot make the characters do anything.
    class ApiServer
    {
    public:
        struct Request
        {
            std::string method;      // "GET", "POST"
            std::string path;        // "/say", without the query
            std::string query;       // after the '?', as sent ("character=2")
            std::string contentType; // lower case, without parameters ("application/json")
            std::string body;        // UTF-8 JSON (up to 64 KB; 16 MB for /say), or a VMD's bytes (POST /motion as application/octet-stream)
            int status=200;          // filled by the receiver
            std::string response;    // JSON, filled by the receiver
            HANDLE done=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            Request()=default;
            Request(const Request&)=delete;
            Request& operator=(const Request&)=delete;
            ~Request(){if(done)CloseHandle(done);}
        };
        // A request to answer: wParam is a heap-allocated std::shared_ptr<Request>* the receiver
        // deletes after filling its status and response and calling Answered.
        static constexpr UINT RequestMessage=WM_APP+50;

        explicit ApiServer(HWND notify):notify(notify){}
        ~ApiServer();
        ApiServer(const ApiServer&)=delete;
        ApiServer& operator=(const ApiServer&)=delete;

        // Listens on 127.0.0.1:port. Returns an explanation when it cannot (empty on success).
        std::wstring Start(int port);
        void Stop();
        bool Running() const {return worker.joinable();}
        int Port() const {return port;}

        // The receiver has filled the request: its answer goes back to the caller.
        static void Answered(Request& request){SetEvent(request.done);}

    private:
        void Run();
        void Serve(std::uintptr_t client);

        HWND notify;
        int port=0;
        std::uintptr_t listener=~std::uintptr_t(0);   // a SOCKET (INVALID_SOCKET when none)
        HANDLE stopping=nullptr;
        std::thread worker;
    };
}
