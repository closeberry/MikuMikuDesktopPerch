#pragma once
#include <initializer_list>
#include <string>

namespace MikuMikuDesktopPerch
{
    // The language the app speaks: its own text (menus, windows, messages, what the characters
    // say without the LLM) and what it asks the LLM to answer in. Japanese or English.
    // setting: "ja", "en", or empty for Windows' display language (Japanese only when that is
    // Japanese).
    void SetLanguage(const std::wstring& setting);
    bool English();

    // The text in the language in use: both are written where the text is used.
    inline const wchar_t* T(const wchar_t* japanese,const wchar_t* english){return English()?english:japanese;}

    // pattern with {0}, {1}, ... replaced by args (word order differs between the languages).
    std::wstring Fmt(std::wstring pattern,std::initializer_list<std::wstring> args);
}
