#define NOMINMAX
#include "VoicevoxCore.hpp"
#include "Text.hpp"
#include "HttpJson.hpp"
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>

namespace MikuMikuDesktopPerch::VoicevoxCore
{
    using namespace Net;

    namespace
    {
        // The parts of VOICEVOX CORE 0.16's C API used here (voicevox_core.h).
        using ResultCode=std::int32_t;
        constexpr ResultCode Ok=0;
        struct Onnxruntime;struct OpenJtalkRc;struct Synthesizer;struct VoiceModelFile;
        struct LoadOnnxruntimeOptions {const char* filename;};
        struct InitializeOptions {std::int32_t accelerationMode;std::uint16_t cpuNumThreads;};
        struct SynthesisOptions {bool enableInterrogativeUpspeak;};
        constexpr std::int32_t AccelerationCpu=1;

        struct Api
        {
            const char* (*getVersion)();
            const char* (*errorMessage)(ResultCode);
            LoadOnnxruntimeOptions (*defaultLoadOnnxruntimeOptions)();
            ResultCode (*onnxruntimeLoadOnce)(LoadOnnxruntimeOptions,const Onnxruntime**);
            ResultCode (*openJtalkNew)(const char*,OpenJtalkRc**);
            InitializeOptions (*defaultInitializeOptions)();
            ResultCode (*synthesizerNew)(const Onnxruntime*,const OpenJtalkRc*,InitializeOptions,Synthesizer**);
            ResultCode (*modelOpen)(const char*,VoiceModelFile**);
            char* (*modelMetas)(const VoiceModelFile*);
            void (*modelDelete)(VoiceModelFile*);
            ResultCode (*loadModel)(const Synthesizer*,const VoiceModelFile*);
            ResultCode (*audioQuery)(const Synthesizer*,const char*,std::uint32_t,char**);
            SynthesisOptions (*defaultSynthesisOptions)();
            ResultCode (*synthesis)(const Synthesizer*,const char*,std::uint32_t,SynthesisOptions,std::uintptr_t*,std::uint8_t**);
            void (*jsonFree)(char*);
            void (*wavFree)(std::uint8_t*);
        };

        std::string Path8(const std::filesystem::path& p){return Utf8(p.wstring());}

        // The Open JTalk dictionary: in the CORE package's dict folder, or where VOICEVOX's
        // engine keeps it (engine_internal\pyopenjtalk).
        std::filesystem::path FindDictionary(const std::filesystem::path& core)
        {
            std::error_code ec;
            for(const auto& base:{core/L"dict",core/L"engine_internal"/L"pyopenjtalk",core})
            {
                if(!std::filesystem::is_directory(base,ec))continue;
                for(const auto& entry:std::filesystem::directory_iterator(base,ec))
                    if(entry.is_directory(ec)&&entry.path().filename().wstring().rfind(L"open_jtalk_dic_utf_8",0)==0)return entry.path();
            }
            return {};
        }

        class Core
        {
        public:
            static Core& Instance(){static Core core;return core;}

            std::wstring Voices(const std::wstring& folder,std::vector<std::pair<std::wstring,int>>& out)
            {
                std::lock_guard lock(mutex);
                if(auto e=Open(folder);!e.empty())return e;
                out=voices;
                return voices.empty()?T(L"VOICEVOXのフォルダに声のモデル（model\\*.vvm）がありません。",L"The VOICEVOX folder has no voice models (model\\*.vvm)."):std::wstring();
            }

            std::wstring AudioQuery(const std::wstring& folder,const std::wstring& text,int style,std::string& json)
            {
                std::lock_guard lock(mutex);
                if(auto e=Open(folder);!e.empty())return e;
                if(auto e=Load(style);!e.empty())return e;
                char* query=nullptr;
                if(const ResultCode r=api.audioQuery(synthesizer,Utf8(text).c_str(),static_cast<std::uint32_t>(style),&query);r!=Ok)return Error(r);
                json=query;
                api.jsonFree(query);
                return {};
            }

            std::wstring Synthesis(const std::wstring& folder,const std::string& json,int style,std::string& wav)
            {
                std::lock_guard lock(mutex);
                if(auto e=Open(folder);!e.empty())return e;
                if(auto e=Load(style);!e.empty())return e;
                std::uintptr_t length=0;
                std::uint8_t* data=nullptr;
                if(const ResultCode r=api.synthesis(synthesizer,json.c_str(),static_cast<std::uint32_t>(style),api.defaultSynthesisOptions(),&length,&data);r!=Ok)return Error(r);
                wav.assign(reinterpret_cast<const char*>(data),static_cast<size_t>(length));
                api.wavFree(data);
                return {};
            }

        private:
            std::mutex mutex;
            HMODULE library=nullptr;
            Api api{};
            const Onnxruntime* onnxruntime=nullptr;
            OpenJtalkRc* openJtalk=nullptr;
            Synthesizer* synthesizer=nullptr;
            std::wstring opened;                          // the folder named, once open
            std::map<int,std::filesystem::path> modelOf;  // style id -> its model file
            std::set<std::filesystem::path> loaded;
            std::vector<std::pair<std::wstring,int>> voices;

            std::wstring Error(ResultCode r) const
            {
                const char* text=api.errorMessage?api.errorMessage(r):nullptr;
                return T(L"VOICEVOX CORE のエラー: ",L"VOICEVOX CORE error: ")+(text?Wide(text):std::to_wstring(r));
            }

            // Load the library, ONNX Runtime and the dictionary, make the synthesizer, and read
            // which voices each model holds. Once open, it stays open (the library is never
            // unloaded); another folder needs a restart.
            std::wstring Open(const std::wstring& folder)
            {
                if(synthesizer)
                    return opened==folder?std::wstring():T(L"VOICEVOXのフォルダを変えたときは、アプリを再起動してください。",L"After changing the VOICEVOX folder, restart the app.");
                const std::wstring found=FindCoreFolder(folder);
                if(found.empty())return T(L"指定したフォルダに voicevox_core.dll が見つかりません（VOICEVOXをインストールしたフォルダを指定してください）: ",L"voicevox_core.dll is not in that folder (choose the folder VOICEVOX is installed in): ")+folder;
                const std::filesystem::path core=found;
                if(!library)
                {
                    // By full path, with its own folder (and System32) for what it needs in turn:
                    // never from the current folder or PATH.
                    library=LoadLibraryExW((core/L"voicevox_core.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
                    if(!library)return T(L"voicevox_core.dll を読み込めませんでした。",L"Could not load voicevox_core.dll.");
                    bool all=true;
                    auto get=[&](auto& slot,const char* name){slot=reinterpret_cast<std::remove_reference_t<decltype(slot)>>(GetProcAddress(library,name));all=all&&slot;};
                    get(api.getVersion,"voicevox_get_version");
                    get(api.errorMessage,"voicevox_error_result_to_message");
                    get(api.defaultLoadOnnxruntimeOptions,"voicevox_make_default_load_onnxruntime_options");
                    get(api.onnxruntimeLoadOnce,"voicevox_onnxruntime_load_once");
                    get(api.openJtalkNew,"voicevox_open_jtalk_rc_new");
                    get(api.defaultInitializeOptions,"voicevox_make_default_initialize_options");
                    get(api.synthesizerNew,"voicevox_synthesizer_new");
                    get(api.modelOpen,"voicevox_voice_model_file_open");
                    get(api.modelMetas,"voicevox_voice_model_file_create_metas_json");
                    get(api.modelDelete,"voicevox_voice_model_file_delete");
                    get(api.loadModel,"voicevox_synthesizer_load_voice_model");
                    get(api.audioQuery,"voicevox_synthesizer_create_audio_query");
                    get(api.defaultSynthesisOptions,"voicevox_make_default_synthesis_options");
                    get(api.synthesis,"voicevox_synthesizer_synthesis");
                    get(api.jsonFree,"voicevox_json_free");
                    get(api.wavFree,"voicevox_wav_free");
                    const std::string version=all&&api.getVersion()?api.getVersion():"";
                    if(!all||version.rfind("0.16.",0)!=0)
                    {
                        api={};
                        FreeLibrary(library);
                        library=nullptr;
                        return T(L"この voicevox_core.dll には対応していません（対応しているのは VOICEVOX CORE 0.16 です）",L"This voicevox_core.dll is not supported (VOICEVOX CORE 0.16 is)")+
                               (version.empty()?std::wstring():T(L"：バージョン ",L": version ")+Wide(version))+T(L"。エンジン方式を使ってください。",L". Use the engine instead.");
                    }
                }
                if(!onnxruntime)
                {
                    LoadOnnxruntimeOptions options=api.defaultLoadOnnxruntimeOptions();
                    const std::string runtime=Path8(core/L"voicevox_onnxruntime.dll");
                    options.filename=runtime.c_str();
                    if(const ResultCode r=api.onnxruntimeLoadOnce(options,&onnxruntime);r!=Ok){onnxruntime=nullptr;return Error(r);}
                }
                if(!openJtalk)
                {
                    const std::filesystem::path dictionary=FindDictionary(core);
                    if(dictionary.empty())return T(L"Open JTalk の辞書（open_jtalk_dic_utf_8-…）が見つかりません。",L"The Open JTalk dictionary (open_jtalk_dic_utf_8-…) is missing.");
                    if(const ResultCode r=api.openJtalkNew(Path8(dictionary).c_str(),&openJtalk);r!=Ok){openJtalk=nullptr;return Error(r);}
                }
                // On the CPU: the GPU is left to the mascot (and the LLM).
                InitializeOptions options=api.defaultInitializeOptions();
                options.accelerationMode=AccelerationCpu;
                if(const ResultCode r=api.synthesizerNew(onnxruntime,openJtalk,options,&synthesizer);r!=Ok){synthesizer=nullptr;return Error(r);}
                opened=folder;
                ReadModels(core);
                return {};
            }

            void ReadModels(const std::filesystem::path& core)
            {
                std::error_code ec;
                std::filesystem::path models=core/L"model";
                if(!std::filesystem::is_directory(models,ec))models=core/L"models"/L"vvms";   // the CORE package's layout
                std::vector<std::filesystem::path> files;
                for(const auto& entry:std::filesystem::directory_iterator(models,ec))
                    if(entry.is_regular_file(ec)&&entry.path().extension()==L".vvm")files.push_back(entry.path());
                std::sort(files.begin(),files.end());
                std::vector<std::pair<int,std::wstring>> found;
                for(const auto& file:files)
                {
                    VoiceModelFile* model=nullptr;
                    if(api.modelOpen(Path8(file).c_str(),&model)!=Ok||!model)continue;
                    if(char* metas=api.modelMetas(model))
                    {
                        Json j;
                        if(JsonParser(metas).Parse(j)&&j.type==Json::Type::Array)
                            for(const auto& speaker:j.items)
                            {
                                const Json* name=speaker.Member("name");
                                const Json* styles=speaker.Member("styles");
                                if(!name||name->type!=Json::Type::String||!styles||styles->type!=Json::Type::Array)continue;
                                for(const auto& style:styles->items)
                                {
                                    const Json* styleName=style.Member("name");
                                    const Json* id=style.Member("id");
                                    const Json* type=style.Member("type");
                                    if(!id||id->type!=Json::Type::Number)continue;
                                    if(type&&type->type==Json::Type::String&&type->text!="talk")continue;   // singing voices
                                    const int styleId=std::atoi(id->text.c_str());
                                    modelOf[styleId]=file;
                                    found.push_back({styleId,Wide(name->text)+L"（"+(styleName&&styleName->type==Json::Type::String?Wide(styleName->text):std::wstring())+L"）"});
                                }
                            }
                        api.jsonFree(metas);
                    }
                    api.modelDelete(model);
                }
                voices.clear();
                for(const auto& [id,name]:found)voices.push_back({name,id});
            }

            // The model holding a voice, loaded into the synthesizer on first use.
            std::wstring Load(int style)
            {
                const auto it=modelOf.find(style);
                if(it==modelOf.end())return Fmt(T(L"VOICEVOXのフォルダにこの声（{0}）のモデルがありません。",L"The VOICEVOX folder has no model for this voice ({0})."),{std::to_wstring(style)});
                if(loaded.count(it->second))return {};
                VoiceModelFile* model=nullptr;
                if(const ResultCode r=api.modelOpen(Path8(it->second).c_str(),&model);r!=Ok)return Error(r);
                const ResultCode r=api.loadModel(synthesizer,model);
                api.modelDelete(model);
                if(r!=Ok)return Error(r);
                loaded.insert(it->second);
                return {};
            }
        };
    }

    std::wstring FindCoreFolder(const std::wstring& folder)
    {
        if(folder.empty())return {};
        std::error_code ec;
        for(const auto& dir:{std::filesystem::path(folder),std::filesystem::path(folder)/L"vv-engine"})
            if(std::filesystem::is_regular_file(dir/L"voicevox_core.dll",ec))return dir.wstring();
        return {};
    }

    std::wstring Voices(const std::wstring& folder,std::vector<std::pair<std::wstring,int>>& voices)
    {return Core::Instance().Voices(folder,voices);}

    std::wstring AudioQuery(const std::wstring& folder,const std::wstring& text,int style,std::string& json)
    {return Core::Instance().AudioQuery(folder,text,style,json);}

    std::wstring Synthesis(const std::wstring& folder,const std::string& json,int style,std::string& wav)
    {return Core::Instance().Synthesis(folder,json,style,wav);}
}
