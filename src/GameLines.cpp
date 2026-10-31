#define NOMINMAX
#include "GameLines.hpp"
#include "AudioDecode.hpp"
#include "HttpJson.hpp"
#include "Text.hpp"
#include <windows.h>
#include <shlwapi.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <sstream>
#pragma comment(lib,"shlwapi.lib")

namespace MikuMikuDesktopPerch
{
    std::wstring ReadAnyText(const std::wstring& path,std::wstring& text,std::wstring* encoding)
    {
        text.clear();
        std::ifstream in(path,std::ios::binary);
        if(!in)return L"cannot open";
        const std::string bytes((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
        auto said=[&](const wchar_t* name){if(encoding)*encoding=name;};
        auto wide16=[&](size_t from,bool big){
            for(size_t i=from;i+1<bytes.size();i+=2)
            {
                const unsigned a=static_cast<unsigned char>(bytes[i]),b=static_cast<unsigned char>(bytes[i+1]);
                text+=static_cast<wchar_t>(big?(a<<8)|b:(b<<8)|a);
            }
        };
        auto multi=[&](UINT page,size_t from,DWORD flags)->bool{
            const char* p=bytes.data()+from;
            const int n=static_cast<int>(bytes.size()-from);
            if(n==0)return true;
            const int size=MultiByteToWideChar(page,flags,p,n,nullptr,0);
            if(size<=0)return false;
            text.assign(static_cast<size_t>(size),L'\0');
            MultiByteToWideChar(page,flags,p,n,text.data(),size);
            return true;
        };
        // A BOM says so.
        if(bytes.size()>=3&&bytes.compare(0,3,"\xEF\xBB\xBF")==0){said(L"UTF-8 (BOM)");multi(CP_UTF8,3,0);return {};}
        if(bytes.size()>=2&&bytes[0]=='\xFF'&&bytes[1]=='\xFE'){said(L"UTF-16 LE (BOM)");wide16(2,false);return {};}
        if(bytes.size()>=2&&bytes[0]=='\xFE'&&bytes[1]=='\xFF'){said(L"UTF-16 BE (BOM)");wide16(2,true);return {};}
        // UTF-16 without one: ASCII characters leave every other byte zero.
        size_t evenZero=0,oddZero=0;
        const size_t look=std::min<size_t>(bytes.size(),4096);
        for(size_t i=0;i<look;++i)if(bytes[i]==0)(i%2?oddZero:evenZero)++;
        if(look>=4&&oddZero>look/8&&evenZero<oddZero/8){said(L"UTF-16 LE");wide16(0,false);return {};}
        if(look>=4&&evenZero>look/8&&oddZero<evenZero/8){said(L"UTF-16 BE");wide16(0,true);return {};}
        // UTF-8 when it is valid as that (plain ASCII is too), else Shift-JIS.
        if(multi(CP_UTF8,0,MB_ERR_INVALID_CHARS)){said(L"UTF-8");return {};}
        if(multi(932,0,0)){said(L"Shift-JIS");return {};}
        return L"unreadable text";
    }

    namespace
    {
        // JSON as people write it by hand made into JSON: in a "voice" value (a Windows path)
        // every backslash is kept as written, and elsewhere one that starts no escape; // and
        // /* */ comments go, and so does a comma before a closing bracket. Line breaks are kept,
        // so a line number still points at the same place.
        std::wstring Forgive(const std::wstring& in)
        {
            std::wstring out;
            out.reserve(in.size()+16);
            bool inString=false,path=false;
            std::wstring string,last;   // the string being read, and the one before it
            bool afterColon=false;      // the string opening is a value
            for(size_t i=0;i<in.size();++i)
            {
                const wchar_t c=in[i];
                if(inString)
                {
                    if(c==L'\\')
                    {
                        const wchar_t next=i+1<in.size()?in[i+1]:0;
                        const bool escape=next==L'"'||next==L'\\'||(!path&&next&&wcschr(L"/bfnrt",next))||
                                          (!path&&next==L'u'&&i+5<in.size()&&std::all_of(in.begin()+static_cast<std::ptrdiff_t>(i)+2,in.begin()+static_cast<std::ptrdiff_t>(i)+6,iswxdigit));
                        // (A path's \" is its end: "C:\voice\" closes there.)
                        if(escape&&!(path&&next==L'"'&&(i+2>=in.size()||wcschr(L",}] \t\r\n",in[i+2]))))
                        {
                            out+=c;out+=next;string+=next;++i;continue;
                        }
                        out+=L"\\\\";
                        string+=c;
                        continue;
                    }
                    if(c==L'"'){inString=false;last=string;}
                    else string+=c;
                    out+=c;
                    continue;
                }
                if(c==L'"')
                {
                    inString=true;
                    path=afterColon&&last==L"voice";
                    string.clear();
                    out+=c;
                    continue;
                }
                if(c==L':')afterColon=true;
                else if(!iswspace(c))afterColon=false;
                if(c==L'/'&&i+1<in.size()&&in[i+1]==L'/'){while(i<in.size()&&in[i]!=L'\n')++i;if(i<in.size())out+=L'\n';continue;}
                if(c==L'/'&&i+1<in.size()&&in[i+1]==L'*')
                {
                    for(i+=2;i<in.size()&&!(in[i]==L'*'&&i+1<in.size()&&in[i+1]==L'/');++i)if(in[i]==L'\n')out+=L'\n';
                    ++i;
                    continue;
                }
                if(c==L',')
                {
                    size_t j=i+1;
                    while(j<in.size()&&iswspace(in[j]))++j;
                    if(j<in.size()&&(in[j]==L'}'||in[j]==L']'))continue;
                }
                out+=c==0xFEFF?L' ':c;
            }
            return out;
        }

        double Number(const Net::Json& j,double otherwise)
        {
            if(j.type!=Net::Json::Type::Number&&j.type!=Net::Json::Type::String)return otherwise;
            try{return std::stod(j.text);}catch(...){return otherwise;}
        }
    }

    void GameLines::Load(const wchar_t* resource,const std::wstring& game,const std::wstring& ownFile,
                         const std::wstring& stylesFile,const std::wstring& styleKey,bool english,const Asking& asking)
    {
        scenes.clear();
        report.clear();
        problem.clear();
        askingFor=-1;
        this->stylesFile=stylesFile;
        this->styleKey=styleKey;
        this->english=english;
        this->asking=asking;
        const HRSRC info=FindResourceW(nullptr,resource,L"LINES");
        const HGLOBAL data=info?LoadResource(nullptr,info):nullptr;
        const char* text=data?static_cast<const char*>(LockResource(data)):nullptr;
        if(text)
        {
            const std::wstring all=Net::Wide(std::string_view(text,SizeofResource(nullptr,info)));
            for(size_t start=0;start<all.size();)
            {
                size_t end=all.find(L'\n',start);
                if(end==std::wstring::npos)end=all.size();
                std::wstring line=all.substr(start,end-start);
                start=end+1;
                if(!line.empty()&&line.back()==L'\r')line.pop_back();
                if(!line.empty()&&line[0]==0xFEFF)line.erase(0,1);
                if(line.empty()||line[0]==L'#')continue;
                if(line[0]==L'[')
                {
                    const size_t close=line.find(L']');
                    if(close==std::wstring::npos)continue;
                    Scene scene;
                    scene.name=line.substr(1,close-1);
                    const size_t about=line.find_first_not_of(L" \t",close+1);
                    if(about!=std::wstring::npos)scene.about=line.substr(about);
                    scenes.push_back(std::move(scene));
                }
                else if(!scenes.empty())scenes.back().draft.push_back(line);
            }
        }
        LoadStyled();
        if(!ownFile.empty())LoadOwn(game,ownFile);
    }

    void GameLines::LoadOwn(const std::wstring& game,const std::wstring& file)
    {
        if(GetFileAttributesW(file.c_str())==INVALID_FILE_ATTRIBUTES)return;   // none: the usual
        std::wstring text,encoding;
        if(const std::wstring error=ReadAnyText(file,text,&encoding);!error.empty())
        {
            report+=L"lines file "+file+L": "+error+L"\n";
            problem+=Fmt(T(L"{0} を読めませんでした。",L"{0} could not be read."),{file})+L"\n";
            return;
        }
        const std::string utf8=Net::Utf8(Forgive(text));
        Net::Json root;
        Net::JsonParser parser(utf8);
        if(!parser.Parse(root)||root.type!=Net::Json::Type::Object)
        {
            const size_t at=std::min(parser.Stopped(),utf8.size());
            const long long line=1+std::count(utf8.begin(),utf8.begin()+static_cast<std::ptrdiff_t>(at),'\n');
            report+=L"lines file "+file+L" ("+encoding+L"): not JSON, near line "+std::to_wstring(line)+L"\n";
            problem+=Fmt(T(L"{0} の書き方に誤りがあります（{1}行目あたり）。かっこ・カンマ・\"の閉じ忘れがないか確かめてください。",
                           L"{0} is not written right (around line {1}). Check for a missing bracket, comma or closing \"."),{file,std::to_wstring(line)})+L"\n";
            return;
        }
        report+=L"lines file "+file+L" ("+encoding+L")\n";
        const Net::Json* mine=root.Member(Net::Utf8(game));
        if(!mine||mine->type!=Net::Json::Type::Object)return;
        wchar_t folder[MAX_PATH]{};
        wcsncpy_s(folder,file.c_str(),_TRUNCATE);
        PathRemoveFileSpecW(folder);
        auto lineOf=[&](const Net::Json& j,GameLine& out)->bool{
            out={};
            out.own=true;
            if(j.type==Net::Json::Type::String){out.text=Net::Wide(j.text);return !out.text.empty();}
            if(j.type!=Net::Json::Type::Object)return false;
            if(const Net::Json* t=j.Member("text");t&&t->type==Net::Json::Type::String)out.text=Net::Wide(t->text);
            if(const Net::Json* v=j.Member("voice");v&&v->type==Net::Json::Type::String&&!v->text.empty())
            {
                std::wstring voice=Net::Wide(v->text);
                std::replace(voice.begin(),voice.end(),L'/',L'\\');
                if(PathIsRelativeW(voice.c_str()))
                {
                    wchar_t joined[MAX_PATH]{};
                    if(PathCombineW(joined,folder,voice.c_str()))voice=joined;
                }
                out.voice=voice;
                if(GetFileAttributesW(voice.c_str())==INVALID_FILE_ATTRIBUTES)
                {
                    report+=L"  missing voice file: "+voice+L"\n";
                    problem+=Fmt(T(L"音声ファイルが見つかりません：{0}",L"Voice file not found: {0}"),{voice})+L"\n";
                }
            }
            return !out.text.empty()||!out.voice.empty();
        };
        for(const auto& [key,value]:mine->members)
        {
            Scene* scene=Named(Net::Wide(key));
            if(!scene)
            {
                report+=L"  no such scene: "+Net::Wide(key)+L"\n";
                problem+=Fmt(T(L"「{0}」という場面はありません（{1}）。",L"There is no scene \"{0}\" ({1})."),{Net::Wide(key),game})+L"\n";
                continue;
            }
            const Net::Json* lines=&value;
            if(value.type==Net::Json::Type::Object)
            {
                if(const Net::Json* c=value.Member("chance"))
                {
                    double chance=Number(*c,-1);
                    if(chance>1)chance/=100;   // a percentage
                    if(chance>=0)scene->chance=std::min(chance,1.0);
                }
                lines=value.Member("lines");
                if(!lines&&(value.Member("text")||value.Member("voice")))lines=&value;   // one line written alone
            }
            if(!lines)continue;
            GameLine line;
            if(lines->type==Net::Json::Type::Array){for(const auto& item:lines->items)if(lineOf(item,line))scene->own.push_back(line);}
            else if(lineOf(*lines,line))scene->own.push_back(line);
            report+=L"  "+scene->name+L": "+std::to_wstring(scene->own.size())+L" lines"+
                    (scene->chance>=0?L", chance "+std::to_wstring(scene->chance):L"")+L"\n";
        }
    }

    GameLines::Scene* GameLines::Named(const std::wstring& name)
    {
        for(auto& scene:scenes)if(scene.name==name)return &scene;
        return nullptr;
    }
    const GameLines::Scene* GameLines::Named(const std::wstring& name) const
    {
        for(const auto& scene:scenes)if(scene.name==name)return &scene;
        return nullptr;
    }

    bool GameLines::Pick(const wchar_t* name,const std::vector<std::wstring>& recent,std::mt19937& random,GameLine& out) const
    {
        out={};
        const Scene* scene=Named(name);
        if(!scene)return false;
        std::vector<GameLine> pool=scene->own;
        if(pool.empty())
            for(const auto& l:!scene->styled.empty()?scene->styled:english?scene->styled:scene->draft)pool.push_back({l,{}});
        if(pool.empty())return false;
        std::vector<const GameLine*> fresh;
        for(const auto& l:pool)
            if(l.text.empty()||std::find(recent.begin(),recent.end(),l.text)==recent.end())fresh.push_back(&l);
        if(fresh.empty())for(const auto& l:pool)fresh.push_back(&l);
        std::uniform_int_distribution<size_t> any(0,fresh.size()-1);
        out=*fresh[any(random)];
        return true;
    }

    bool GameLines::Roll(const wchar_t* name,double chance,std::mt19937& random) const
    {
        if(const Scene* scene=Named(name);scene&&scene->chance>=0)chance=scene->chance;
        if(chance>=1)return true;
        if(chance<=0)return false;
        return std::uniform_real_distribution<double>(0,1)(random)<chance;
    }

    // The key her styled lines are kept under: who she is and how she talks, the language,
    // and the draft (a change to any asks again).
    std::wstring GameLines::Key() const
    {
        std::wstring all=styleKey+(english?L"\nen":L"\nja");
        for(const auto& scene:scenes){all+=L"\n["+scene.name+L"]";for(const auto& l:scene.draft)all+=L"\n"+l;}
        unsigned long long hash=1469598103934665603ull;
        for(const wchar_t c:all){hash^=static_cast<unsigned long long>(c);hash*=1099511628211ull;}
        wchar_t hex[20]{};
        swprintf_s(hex,L"%016llx",hash);
        return hex;
    }

    // Her styled lines kept from before (the file: "# key", then "[name]" and a line each).
    void GameLines::LoadStyled()
    {
        if(stylesFile.empty())return;
        FILE* in=nullptr;
        if(_wfopen_s(&in,stylesFile.c_str(),L"r, ccs=UTF-8")!=0||!in)return;
        std::wstring all;
        wchar_t chunk[1024];
        while(fgetws(chunk,1024,in))all+=chunk;
        fclose(in);
        std::wstringstream lines(all);
        std::wstring line,key=L"# "+Key();
        if(!std::getline(lines,line)||line!=key)return;
        Scene* at=nullptr;
        while(std::getline(lines,line))
        {
            if(!line.empty()&&line.back()==L'\r')line.pop_back();
            if(line.empty())continue;
            if(line[0]==L'['){const size_t close=line.find(L']');at=close==std::wstring::npos?nullptr:Named(line.substr(1,close-1));continue;}
            if(at)at->styled.push_back(line);
        }
        for(auto& scene:scenes)scene.asked=!scene.styled.empty();
    }

    void GameLines::SaveStyled()
    {
        if(stylesFile.empty())return;
        FILE* out=nullptr;
        if(_wfopen_s(&out,stylesFile.c_str(),L"w, ccs=UTF-8")!=0||!out)return;
        fputws((L"# "+Key()+L"\n").c_str(),out);
        for(const auto& scene:scenes)
        {
            if(scene.styled.empty())continue;
            fputws((L"["+scene.name+L"]\n").c_str(),out);
            for(const auto& l:scene.styled)fputws((l+L"\n").c_str(),out);
        }
        fclose(out);
    }

    // The next scene not yet in her own words: its draft given to the LLM to say again her way,
    // a scene at a time (a small model keeps to the form so). Scenes the user gave lines of their
    // own are not asked.
    std::wstring GameLines::NextAsk()
    {
        Scene* want=nullptr;
        for(auto& scene:scenes)if(!scene.asked&&!scene.draft.empty()&&scene.own.empty()){want=&scene;break;}
        if(!want)return {};
        want->asked=true;
        askingFor=static_cast<int>(want-scenes.data());
        std::wstring numbered;
        for(size_t i=0;i<want->draft.size();++i)numbered+=std::to_wstring(i+1)+(english?L": ":L"：")+want->draft[i]+L"\n";
        const std::wstring count=std::to_wstring(want->draft.size());
        if(english)
            return Fmt(L"You are playing {3} with them. The moment: {0}.\n"
                       L"Say each of these {1} lines (Japanese) in English, the way you yourself talk to them (your way of speaking in every line). "
                       L"Keep what each says; add nothing, one sentence each.\n{2}"
                       L"Answer in {1} lines, numbered as these, in the form \"1: the line\". {4}"
                       L"Write nothing else. No feeling tags, names, anything in ( ) or emoji.",{want->about,count,numbered,asking.gameEn,asking.ruleEn});
        return Fmt(L"話し相手と{3}をしています。場面：{0}。台詞はどれも、あなたが話し相手に向かって言うものです。\n"
                   L"次の{1}個の台詞を、それぞれ、あなたがふだん話すときの口調に言い換えてください（一人称、語尾、口ぐせなど、あなたらしい話し方をどの行にも必ず入れる。元のままの行は作らない）。"
                   L"言っていることは変えず、言葉を足したり、2文にしたりしません。\n{2}"
                   L"答えは{1}行で、元と同じ番号を付けて「1：言い換えた台詞」の形で書きます。{4}"
                   L"この形の行のほかは何も書かないでください。気持ちの印、名前、（ ）書き、絵文字は使わないでください。",
                   {want->about,count,numbered,asking.gameJa,asking.ruleJa});
    }

    void GameLines::Answered(const std::wstring& answer)
    {
        const int at=askingFor;
        askingFor=-1;
        if(at<0||at>=static_cast<int>(scenes.size()))return;
        auto& scene=scenes[static_cast<size_t>(at)];
        std::vector<std::wstring> got;
        for(size_t from=0;from<answer.size();)
        {
            size_t end=answer.find(L'\n',from);
            if(end==std::wstring::npos)end=answer.size();
            std::wstring line=answer.substr(from,end-from);
            from=end+1;
            // "1：line" (a model may put 番号 or a dot before it): what follows the number.
            size_t i=0;
            auto skip=[&](const wchar_t* chars){while(i<line.size()&&wcschr(chars,line[i]))++i;};
            skip(L" \t\r　");
            if(line.compare(i,2,L"番号")==0)i+=2;
            skip(L"：: ");
            const size_t digits=i;
            skip(L"0123456789０１２３４５６７８９");
            if(i==digits)continue;   // no number: not one of the lines
            skip(L"：:.．)） \t　");
            std::wstring said=line.substr(i);
            for(const wchar_t* drop:{L"「",L"」",L"\"",L"*"})
                for(size_t p;(p=said.find(drop))!=std::wstring::npos;)said.erase(p,wcslen(drop));
            const size_t a=said.find_first_not_of(L" \t\r　"),b=said.find_last_not_of(L" \t\r　");
            if(a==std::wstring::npos)continue;
            said=said.substr(a,b-a+1);
            if(said.size()>60||std::find(got.begin(),got.end(),said)!=got.end())continue;
            got.push_back(said);
        }
        // Kept when most came back (a few short is fine; a stray answer is not).
        if(got.size()*2>=scene.draft.size())
        {
            scene.styled=std::move(got);
            SaveStyled();
        }
        report+=Fmt(L"  lines in her words: {0} ({1} of {2})\n",{scene.name,std::to_wstring(scene.styled.size()),std::to_wstring(scene.draft.size())});
    }

    bool GameVoice::Play(const GameLine& line,std::wstring& error)
    {
        Stop();
        if(line.voice.empty())return false;
        auto found=decoded.find(line.voice);
        if(found==decoded.end())
        {
            std::ifstream in(line.voice,std::ios::binary);
            if(!in){error=L"cannot open "+line.voice;return false;}
            in.seekg(0,std::ios::end);
            const std::streamoff size=in.tellg();
            if(size<=0||size>64ll*1024*1024){error=L"empty or too large: "+line.voice;return false;}
            in.seekg(0);
            std::string bytes(static_cast<size_t>(size),'\0');
            in.read(bytes.data(),size);
            std::string wav;
            if(error=DecodeAudio(bytes,wav);!error.empty()){error=line.voice+L": "+error;return false;}
            found=decoded.emplace(line.voice,std::move(wav)).first;
        }
        WAVEFORMATEX format{};
        const std::int16_t* samples=nullptr;
        size_t count=0;
        if(!ReadPcm(found->second,format,samples,count)||format.nChannels==0||format.nSamplesPerSec==0){error=L"not a sound: "+line.voice;return false;}
        seconds=static_cast<double>(count/format.nChannels)/format.nSamplesPerSec;
        speech.Say(line.text,found->second);
        speech.Finish();
        playing=true;
        return true;
    }
}

namespace MikuMikuDesktopPerch
{
    void ShowLinesProblem(HWND owner,const GameLines& lines)
    {
        if(lines.Problem().empty())return;
        std::wstring text=lines.Problem();
        if(text.size()>1500)text=text.substr(0,1500)+L"...\n";
        MessageBoxW(owner,(std::wstring(T(L"セリフファイルに問題があります。読めたところだけを使います。\n\n",
                                          L"There is a problem with the lines file. What could be read is used.\n\n"))+text).c_str(),
                    T(L"セリフファイル",L"Lines file"),MB_OK|MB_ICONWARNING);
    }
}
