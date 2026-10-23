// Copied from MotionTraceEditor (LipSyncCore.hpp, the author's own tool): turns timed kana
// (lyric events) into keys for the あ い う え お ん mouth morphs.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace MotionTraceEditor::LipSyncCore
{
    struct LyricEvent
    {
        int frame=0;
        std::wstring text;
        int phrase=0;
        std::wstring word;
        int wordIndex=-1;
    };

    inline std::vector<std::wstring> SplitMorae(const std::wstring& text)
    {
        std::vector<std::wstring> result;
        const std::wstring smallKana=L"ゃゅょぁぃぅぇぉゎャュョァィゥェォヮ";
        for(const wchar_t ch:text)
        {
            if(std::iswspace(ch)) continue;
            const bool previousRest=!result.empty()&&
                (result.back()==L"、"||result.back()==L"。"||
                 result.back()==L","||result.back()==L"."||
                 result.back()==L"_"||result.back()==L"|");
            if(smallKana.find(ch)!=std::wstring::npos&&!result.empty()&&
               !previousRest)
            {
                result.back().push_back(ch);
                continue;
            }
            result.emplace_back(1,ch);
        }
        return result;
    }

    enum class Vowel:int
    {
        A=0,I,U,E,O,N,Rest
    };

    struct MorphPoint
    {
        int frame=0;
        Vowel vowel=Vowel::Rest;
        float value=0.0f;
    };

    inline bool IsRestText(const std::wstring& text) noexcept
    {
        return text.empty()||text==L"、"||text==L"。"||
            text==L","||text==L"."||text==L"!"||text==L"?"||
            text==L";"||text==L":"||text==L"_"||text==L"|"||
            (!text.empty()&&text.front()==L'?');
    }

    inline Vowel VowelOf(const std::wstring& source) noexcept
    {
        if(IsRestText(source)) return Vowel::Rest;
        if(source==L"ん"||source==L"ン"||
           source==L"っ"||source==L"ッ") return Vowel::N;

        std::wstring phone=source;
        std::transform(phone.begin(),phone.end(),phone.begin(),
            [](wchar_t ch)
            { return static_cast<wchar_t>(std::towupper(ch)); });
        while(!phone.empty()&&std::iswdigit(phone.back()))
            phone.pop_back();
        if(phone==L"B"||phone==L"M"||phone==L"P") return Vowel::N;
        if(phone==L"AA"||phone==L"AE"||phone==L"AH") return Vowel::A;
        if(phone==L"IH"||phone==L"IY") return Vowel::I;
        if(phone==L"UH"||phone==L"UW") return Vowel::U;
        if(phone==L"EH"||phone==L"ER"||phone==L"EY") return Vowel::E;
        if(phone==L"AO"||phone==L"OW") return Vowel::O;

        // The final kana determines the vowel for contracted sounds such as
        // きゃ, しゅ and ティ. Long marks retain the preceding event and are
        // handled by GenerateMorphPoints.
        for(auto it=source.rbegin();it!=source.rend();++it)
        {
            const wchar_t ch=*it;
            if(ch==L'ー'||std::iswspace(ch)) continue;
            const std::wstring a=L"あかがさざただなはばぱまゃやらゎわぁアカガサザタダナハバパマャヤラヮワァファヴァクァグァツァ";
            const std::wstring i=L"いきぎしじちぢにひびぴみりぃイキギシジチヂニヒビピミリィフィヴィティディキィギィ";
            const std::wstring u=L"うゔくぐすずつづぬふぶぷむゅゆるぅウヴクグスズツヅヌフブプムュユルゥトゥドゥクゥグゥ";
            const std::wstring e=L"えけげせぜてでねへべぺめれぇエケゲセゼテデネヘベペメレェフェヴェシェジェチェ";
            const std::wstring o=L"おこごそぞとどのほぼぽもょよろをぉオコゴソゾトドノホボポモョヨロヲォフォヴォクォグォ";
            if(a.find(ch)!=std::wstring::npos) return Vowel::A;
            if(i.find(ch)!=std::wstring::npos) return Vowel::I;
            if(u.find(ch)!=std::wstring::npos) return Vowel::U;
            if(e.find(ch)!=std::wstring::npos) return Vowel::E;
            if(o.find(ch)!=std::wstring::npos) return Vowel::O;
        }
        return Vowel::Rest;
    }

    inline bool IsLongMark(const std::wstring& text) noexcept
    {
        return text==L"ー"||text==L"～"||text==L"〜"||text==L"~";
    }

    inline bool IsSupportedLyricToken(const std::wstring& text) noexcept
    {
        return IsLongMark(text)||IsRestText(text)||
            VowelOf(text)!=Vowel::Rest;
    }

    inline std::wstring MouthShapeLabel(const std::wstring& text)
    {
        if(IsLongMark(text)) return L"Hold";
        if(text==L"_"||text==L"|") return L"Close";
        if(IsRestText(text))
            return !text.empty()&&text.front()==L'?'
                ?L"?"
                :L"Pause";
        switch(VowelOf(text))
        {
        case Vowel::A: return L"A";
        case Vowel::I: return L"I";
        case Vowel::U: return L"U";
        case Vowel::E: return L"E";
        case Vowel::O: return L"O";
        case Vowel::N: return L"Close";
        default: return text;
        }
    }

    inline int RestLeadFrames(const std::wstring& text) noexcept
    {
        if(text==L"。"||text==L"."||text==L"!"||text==L"?") return 4;
        if(text==L"、"||text==L",") return 2;
        return 1;
    }

    inline float ClosureDepth(const std::wstring& nextText) noexcept
    {
        if(nextText.empty()) return 0.0f;
        std::wstring phone=nextText;
        std::transform(phone.begin(),phone.end(),phone.begin(),
            [](wchar_t value)
            { return static_cast<wchar_t>(std::towupper(value)); });
        while(!phone.empty()&&std::iswdigit(phone.back()))
            phone.pop_back();
        if(phone==L"B"||phone==L"M"||phone==L"P") return 0.0f;
        if(phone==L"AA"||phone==L"AE"||phone==L"AH"||
           phone==L"IH"||phone==L"IY"||phone==L"UH"||
           phone==L"UW"||phone==L"EH"||phone==L"ER"||
           phone==L"AO") return 0.42f;
        const wchar_t ch=nextText.front();
        const std::wstring full=L"まみむめもばびぶべぼぱぴぷぺぽマミムメモバビブベボパピプペポ";
        const std::wstring light=L"あいうえおアイウエオぁぃぅぇぉァィゥェォ";
        if(full.find(ch)!=std::wstring::npos) return 0.0f;
        if(light.find(ch)!=std::wstring::npos) return 0.42f;
        return 0.18f;
    }

    inline std::vector<MorphPoint> GenerateMorphPoints(
        std::vector<LyricEvent> events,
        float maximum=1.0f)
    {
        std::erase_if(events,[](const LyricEvent& event)
        {
            return event.frame<0||event.text.empty();
        });
        std::stable_sort(events.begin(),events.end(),
            [](const LyricEvent& left,const LyricEvent& right)
            { return left.frame<right.frame; });

        std::vector<MorphPoint> result;
        Vowel previous=Vowel::Rest;
        for(size_t index=0;index<events.size();++index)
        {
            const auto& event=events[index];
            Vowel current=VowelOf(event.text);
            if(IsLongMark(event.text)) current=previous;
            const int nextFrame=index+1<events.size()
                ?events[index+1].frame:event.frame+8;

            if(current==Vowel::Rest)
            {
                for(int channel=0;channel<6;++channel)
                    result.push_back({event.frame,static_cast<Vowel>(channel),0.0f});
                previous=Vowel::Rest;
                continue;
            }

            // At each onset only one mouth shape is active.
            for(int channel=0;channel<6;++channel)
                result.push_back({event.frame,static_cast<Vowel>(channel),
                    channel==static_cast<int>(current)?maximum:0.0f});

            if(index+1<events.size()&&nextFrame>event.frame)
            {
                const auto& next=events[index+1];
                Vowel nextVowel=VowelOf(next.text);
                if(IsLongMark(next.text)) nextVowel=current;
                const int preClose=std::max(event.frame,nextFrame-
                    (nextVowel==Vowel::Rest?RestLeadFrames(next.text):2));
                const int closeFrame=std::max(event.frame,nextFrame-1);

                if(nextVowel==Vowel::Rest)
                {
                    result.push_back({preClose,current,maximum*0.55f});
                    result.push_back({nextFrame,current,0.0f});
                }
                else if(!IsLongMark(next.text))
                {
                    result.push_back({preClose,current,
                        maximum*ClosureDepth(next.text)});
                    result.push_back({closeFrame,current,0.0f});
                }
            }
            else if(index+1==events.size())
            {
                // The final event has no following onset to close it. Give it
                // the same eight-frame tail used by the editor range, then
                // return the active morph to the model's neutral mouth.
                result.push_back({event.frame+6,current,maximum*0.55f});
                result.push_back({event.frame+8,current,0.0f});
            }
            previous=current;
        }

        // Keep the last value for duplicate channel/frame pairs.
        std::map<std::pair<int,int>,float> unique;
        for(const auto& point:result)
            unique[{static_cast<int>(point.vowel),point.frame}]=point.value;
        result.clear();
        result.reserve(unique.size());
        for(const auto& [key,value]:unique)
            result.push_back({key.second,static_cast<Vowel>(key.first),value});
        std::sort(result.begin(),result.end(),[](const MorphPoint& a,const MorphPoint& b)
        {
            return a.frame!=b.frame?a.frame<b.frame:
                static_cast<int>(a.vowel)<static_cast<int>(b.vowel);
        });
        return result;
    }

    // Remove only the interior of an unchanged run. Its first and last keys
    // are retained because they delimit the preceding/following interpolation;
    // dropping the last plateau key would make the next transition start too
    // early. This runs before registration, so generation remains one Undo.
    inline std::vector<MorphPoint> RemoveRedundantMorphPoints(
        const std::vector<MorphPoint>& points,
        float epsilon=1.0e-5f)
    {
        std::array<std::vector<MorphPoint>,6> channels;
        for(const auto& point:points)
        {
            const int channel=static_cast<int>(point.vowel);
            if(channel>=0&&channel<static_cast<int>(channels.size()))
                channels[static_cast<size_t>(channel)].push_back(point);
        }
        std::vector<MorphPoint> result;
        result.reserve(points.size());
        for(auto& channelPoints:channels)
        {
            size_t first=0;
            while(first<channelPoints.size())
            {
                size_t last=first;
                while(last+1<channelPoints.size()&&
                      std::abs(channelPoints[last+1].value-
                               channelPoints[first].value)<=epsilon)
                    ++last;
                result.push_back(channelPoints[first]);
                if(last>first)
                    result.push_back(channelPoints[last]);
                first=last+1;
            }
        }
        std::sort(result.begin(),result.end(),[](const auto& left,const auto& right)
        {
            return left.frame!=right.frame?left.frame<right.frame:
                static_cast<int>(left.vowel)<static_cast<int>(right.vowel);
        });
        return result;
    }

    using MorphPointValueMap=std::map<std::pair<int,int>,float>;

    inline MorphPointValueMap BuildMorphPointValueMap(
        const std::vector<LyricEvent>& events)
    {
        MorphPointValueMap result;
        for(const auto& point:RemoveRedundantMorphPoints(
                GenerateMorphPoints(events)))
        {
            result[{
                static_cast<int>(point.vowel),
                point.frame}]=point.value;
        }
        return result;
    }

    inline std::set<std::pair<int,int>> ChangedMorphPointKeys(
        const MorphPointValueMap& before,
        const MorphPointValueMap& after,
        float epsilon=1.0e-5f)
    {
        std::set<std::pair<int,int>> result;
        for(const auto& [key,value]:before)
        {
            const auto found=after.find(key);
            if(found==after.end()||std::abs(found->second-value)>epsilon)
                result.insert(key);
        }
        for(const auto& [key,value]:after)
        {
            const auto found=before.find(key);
            if(found==before.end()||std::abs(found->second-value)>epsilon)
                result.insert(key);
        }
        return result;
    }
}
