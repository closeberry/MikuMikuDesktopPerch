#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

// A hash of what a game's 2D layer shows. Over the 3D view the layer is drawn again only when this
// changes: drawing it costs far more than a frame in a large window.
class ShownKey
{
public:
    template<class... T> ShownKey& operator()(const T&... values){(Add(values),...);return *this;}
    // Her words' bubble: the text, how much of it is out, and where (it follows her head, in steps).
    ShownKey& Words(const std::wstring& text,double shown,float x,float y,float step)
    {
        Add(text);
        if(!text.empty())
        {
            Add(static_cast<std::size_t>(shown));
            Add(std::lround(x/step));
            Add(std::lround(y/step));
        }
        return *this;
    }
    std::uint64_t Value() const {return hash;}
private:
    std::uint64_t hash=14695981039346656037ull;
    void Mix(std::uint64_t v){hash=(hash^v)*1099511628211ull;}
    template<class T> requires std::is_integral_v<T>||std::is_enum_v<T> void Add(T v){Mix(static_cast<std::uint64_t>(v));}
    void Add(const std::wstring& s){Mix(s.size());for(wchar_t c:s)Mix(static_cast<std::uint64_t>(c));}
    template<class T> void Add(const std::vector<T>& list){Mix(list.size());for(const T& item:list)Add(item);}
    template<class T,std::size_t N> void Add(const std::array<T,N>& list){for(const T& item:list)Add(item);}
};
