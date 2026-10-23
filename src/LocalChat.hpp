#pragma once
#include <windows.h>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace MikuMikuDesktopPerch
{
    // Talks to a local LLM server through the OpenAI-compatible chat API that Ollama, LM Studio
    // and llama.cpp's server provide (POST <endpoint>/chat/completions, streamed). Only servers
    // on this computer are accepted: nothing said to the mascot leaves the machine.
    struct ChatMessage
    {
        std::wstring role;      // "system", "user" or "assistant"
        std::wstring content;
    };

    // endpoint like http://127.0.0.1:11434/v1 must name localhost / 127.0.0.1 / ::1 over http
    // or https. Returns an explanation when it does not (empty when fine).
    std::wstring CheckLocalEndpoint(const std::wstring& endpoint);

    // The models the server offers (GET <endpoint>/models), waiting a few seconds at most.
    // Returns an explanation on failure (empty on success).
    std::wstring ListModels(const std::wstring& endpoint,std::vector<std::wstring>& models);

    class LocalChat
    {
    public:
        // Posted to the notify window while a reply streams in: wParam is a heap-allocated
        // std::wstring* the receiver deletes (a piece of text), lParam the request id.
        static constexpr UINT TextMessage=WM_APP+20;
        // The reply has ended: wParam is a std::wstring* error text (empty when it finished
        // normally), lParam the request id.
        static constexpr UINT DoneMessage=WM_APP+21;

        explicit LocalChat(HWND notify);
        ~LocalChat();
        LocalChat(const LocalChat&)=delete;
        LocalChat& operator=(const LocalChat&)=delete;

        // Start a reply to the conversation (the last message is the user's). A reply still
        // streaming is abandoned. Returns the request id the messages will carry.
        int Start(const std::wstring& endpoint,const std::wstring& model,std::vector<ChatMessage> messages);
        void Cancel();
        bool Busy() const {return busy;}

    private:
        struct Request;
        void Run(std::shared_ptr<Request> request);

        HWND notify;
        std::thread worker;
        std::shared_ptr<Request> current;
        std::atomic<bool> busy{false};
        int nextId=1;
    };
}
