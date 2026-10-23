#pragma once
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch
{
    // What a character remembers from its conversations: short notes about the person it
    // talks with, each with the day it was learnt. Kept per model next to its profile, in
    // characters\<name>.memory.txt (UTF-8, one "YYYY-MM-DD<TAB>note" per line), and never sent
    // anywhere but the local LLM.
    struct MemoryEntry
    {
        int id=0;              // stable while the program runs (for the memory window)
        std::wstring date;     // YYYY-MM-DD
        std::wstring text;
    };

    class CharacterMemory
    {
    public:
        static constexpr size_t MaxEntries=300;   // the oldest go first beyond this
        static constexpr size_t MaxLength=200;    // characters per note

        // Reads the model's memory file (none yet: empty). Later saves go to the same file.
        void Load(const std::wstring& modelPath);
        bool Save() const;

        const std::vector<MemoryEntry>& Entries() const {return entries;}
        // Adds a note learnt on date (YYYY-MM-DD). Blank notes and ones already remembered
        // (the same words) are skipped: returns whether it was added.
        bool Add(std::wstring text,const std::wstring& date);
        void Remove(int id);
        void Clear();

        // The notes to put in the prompt, oldest first, as "- 2026年10月20日：..." lines within
        // maxChars: all of them while they fit, otherwise the ones sharing the most words with
        // query, then the most recent.
        std::wstring Recall(const std::wstring& query,size_t maxChars) const;

    private:
        std::wstring modelPath;
        std::vector<MemoryEntry> entries;
        int nextId=1;
    };

    // Today as YYYY-MM-DD (local time).
    std::wstring TodayDate();
}
