#define NOMINMAX
#include "Updater.hpp"
#include "Text.hpp"
#include "HttpJson.hpp"
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#pragma comment(lib,"winhttp.lib")
#pragma comment(lib,"bcrypt.lib")
#pragma comment(lib,"version.lib")

namespace MikuMikuDesktopPerch
{
    using namespace Net;

    namespace
    {
        constexpr wchar_t LatestRelease[]=L"https://api.github.com/repos/closeberry/MikuMikuDesktopPerch/releases/latest";
        constexpr wchar_t ZipSuffix[]=L"-win-x64.zip";
        constexpr wchar_t AppFolder[]=L"MikuMikuDesktopPerch";   // the folder inside the zip
        constexpr size_t MaxApiBytes=1<<20,MaxZipBytes=64<<20;

        std::filesystem::path ExePath()
        {
            wchar_t exe[MAX_PATH]{};
            GetModuleFileNameW(nullptr,exe,MAX_PATH);
            return exe;
        }

        std::filesystem::path WorkFolder()
        {
            wchar_t temp[MAX_PATH]{};
            GetTempPathW(MAX_PATH,temp);
            return std::filesystem::path(temp)/L"MikuMikuDesktopPerch-update";
        }

        std::wstring FileVersion(const std::filesystem::path& file)
        {
            DWORD ignored=0;
            const DWORD size=GetFileVersionInfoSizeW(file.c_str(),&ignored);
            if(!size)return {};
            std::vector<BYTE> data(size);
            VS_FIXEDFILEINFO* info=nullptr;
            UINT length=0;
            if(!GetFileVersionInfoW(file.c_str(),0,size,data.data())||!VerQueryValueW(data.data(),L"\\",reinterpret_cast<void**>(&info),&length)||!info)return {};
            return std::to_wstring(HIWORD(info->dwFileVersionMS))+L"."+std::to_wstring(LOWORD(info->dwFileVersionMS))+L"."+
                   std::to_wstring(HIWORD(info->dwFileVersionLS));
        }

        // GET over HTTPS from GitHub (github.com and api.github.com; redirects to GitHub's
        // download hosts are followed), at most limit bytes.
        std::wstring HttpsGet(const std::wstring& address,const wchar_t* accept,size_t limit,std::string& body)
        {
            Url url;
            if(!Crack(address,url)||!url.secure||(url.host!=L"github.com"&&url.host!=L"api.github.com"))return T(L"更新の確認先が正しくありません。",L"The update address is not valid.");
            HINTERNET session=WinHttpOpen((L"MikuMikuDesktopPerch/"+CurrentVersion()).c_str(),WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                          WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
            if(!session)return T(L"インターネットに接続できませんでした。",L"Could not connect to the internet.");
            WinHttpSetTimeouts(session,10000,10000,15000,30000);
            std::wstring error;
            HINTERNET connection=WinHttpConnect(session,url.host.c_str(),url.port,0);
            HINTERNET request=connection?WinHttpOpenRequest(connection,L"GET",url.path.c_str(),nullptr,WINHTTP_NO_REFERER,
                                                            WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE):nullptr;
            const std::wstring headers=std::wstring(L"Accept: ")+accept+L"\r\n";
            if(!request||!WinHttpSendRequest(request,headers.c_str(),static_cast<DWORD>(-1),WINHTTP_NO_REQUEST_DATA,0,0,0)||
               !WinHttpReceiveResponse(request,nullptr))
                error=T(L"GitHubに接続できませんでした。",L"Could not connect to GitHub.");
            else
            {
                DWORD status=0,size=sizeof(status);
                WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX);
                if(status!=200)error=Fmt(T(L"GitHubから受け取れませんでした（{0}）。",L"Could not get it from GitHub ({0})."),{std::to_wstring(status)});
                for(DWORD available=0;error.empty()&&WinHttpQueryDataAvailable(request,&available)&&available>0;)
                {
                    const size_t at=body.size();
                    if(at+available>limit){error=T(L"受け取ったデータが大きすぎます。",L"What came back is too large.");break;}
                    body.resize(at+available);
                    DWORD read=0;
                    if(!WinHttpReadData(request,body.data()+at,available,&read)){error=T(L"受け取る途中で切れました。",L"The connection was lost on the way.");break;}
                    body.resize(at+read);
                }
            }
            if(request)WinHttpCloseHandle(request);
            if(connection)WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return error;
        }

        std::wstring Sha256(const std::string& data)
        {
            BCRYPT_ALG_HANDLE algorithm=nullptr;
            BCRYPT_HASH_HANDLE hash=nullptr;
            unsigned char digest[32]{};
            std::wstring hex;
            if(BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0))&&
               BCRYPT_SUCCESS(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0))&&
               BCRYPT_SUCCESS(BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),static_cast<ULONG>(data.size()),0))&&
               BCRYPT_SUCCESS(BCryptFinishHash(hash,digest,sizeof(digest),0)))
                for(const unsigned char b:digest){wchar_t two[3]{};swprintf_s(two,L"%02x",b);hex+=two;}
            if(hash)BCryptDestroyHash(hash);
            if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);
            return hex;
        }

        // failed: GitHub could not be asked, or its answer was not understood.
        std::unique_ptr<Release> LatestNewer(bool& failed)
        {
            failed=true;
            std::string body;
            if(!HttpsGet(LatestRelease,L"application/vnd.github+json",MaxApiBytes,body).empty())return nullptr;
            Json j;
            if(!JsonParser(body).Parse(j))return nullptr;
            const Json* tag=j.Member("tag_name");
            const Json* assets=j.Member("assets");
            if(!tag||tag->type!=Json::Type::String||!assets)return nullptr;
            failed=false;
            auto release=std::make_unique<Release>();
            release->version=Wide(tag->text);
            if(!release->version.empty()&&(release->version[0]==L'v'||release->version[0]==L'V'))release->version.erase(0,1);
            if(!IsNewer(release->version,CurrentVersion()))return nullptr;
            for(const Json& asset:assets->items)
            {
                const Json* name=asset.Member("name");
                const Json* url=asset.Member("browser_download_url");
                if(!name||!url||name->type!=Json::Type::String||url->type!=Json::Type::String)continue;
                const std::wstring n=Wide(name->text);
                if(n.size()<std::size(ZipSuffix)-1||n.compare(n.size()-(std::size(ZipSuffix)-1),std::wstring::npos,ZipSuffix)!=0)continue;
                release->zipUrl=Wide(url->text);
                if(const Json* digest=asset.Member("digest");digest&&digest->type==Json::Type::String&&digest->text.rfind("sha256:",0)==0)
                    release->sha256=Wide(digest->text.substr(7));
                return release;
            }
            return nullptr;
        }

        // Windows' own tar (System32) unpacks zip files.
        std::wstring Unpack(const std::filesystem::path& zip,const std::filesystem::path& into)
        {
            wchar_t system[MAX_PATH]{};
            GetSystemDirectoryW(system,MAX_PATH);
            const std::wstring tar=(std::filesystem::path(system)/L"tar.exe").wstring();
            std::wstring command=L"\""+tar+L"\" -xf \""+zip.wstring()+L"\" -C \""+into.wstring()+L"\"";
            STARTUPINFOW si{sizeof(si)};
            PROCESS_INFORMATION pi{};
            if(!CreateProcessW(tar.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))
                return T(L"ダウンロードした更新を展開できませんでした（tar.exe を起動できません）。",L"Could not unpack the update (tar.exe does not start).");
            const bool done=WaitForSingleObject(pi.hProcess,120000)==WAIT_OBJECT_0;
            DWORD code=1;
            GetExitCodeProcess(pi.hProcess,&code);
            if(!done)TerminateProcess(pi.hProcess,1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return done&&code==0?std::wstring():T(L"ダウンロードした更新を展開できませんでした。",L"Could not unpack the update.");
        }

        std::wstring DownloadAndUnpack(const Release& release)
        {
            std::string zip;
            if(std::wstring error=HttpsGet(release.zipUrl,L"application/octet-stream",MaxZipBytes,zip);!error.empty())return error;
            if(!release.sha256.empty()&&_wcsicmp(Sha256(zip).c_str(),release.sha256.c_str())!=0)
                return T(L"ダウンロードした更新が壊れていました（SHA-256が一致しません）。",L"The downloaded update is damaged (its SHA-256 does not match).");
            const std::filesystem::path work=WorkFolder();
            std::error_code ec;
            std::filesystem::remove_all(work,ec);
            std::filesystem::create_directories(work/L"files",ec);
            const std::filesystem::path zipPath=work/L"update.zip";
            {
                std::ofstream out(zipPath,std::ios::binary);
                out.write(zip.data(),static_cast<std::streamsize>(zip.size()));
                if(!out)return T(L"ダウンロードした更新を保存できませんでした。",L"Could not save the downloaded update.");
            }
            if(std::wstring error=Unpack(zipPath,work/L"files");!error.empty())return error;
            const std::filesystem::path exe=work/L"files"/AppFolder/L"MikuMikuDesktopPerch.exe";
            if(!std::filesystem::exists(exe,ec))return T(L"ダウンロードした更新に MikuMikuDesktopPerch.exe がありませんでした。",L"The update has no MikuMikuDesktopPerch.exe.");
            if(FileVersion(exe)!=release.version)return T(L"ダウンロードした更新のバージョンが違いました。",L"The update has the wrong version.");
            return {};
        }
    }

    namespace {std::wstring pretended;}

    std::wstring CurrentVersion(){return pretended.empty()?FileVersion(ExePath()):pretended;}
    void PretendVersion(const std::wstring& version){pretended=version;}

    bool IsNewer(const std::wstring& a,const std::wstring& b)
    {
        auto parts=[](const std::wstring& v){
            std::vector<int> n;
            for(size_t at=0;at<v.size();)
            {
                size_t end=v.find(L'.',at);
                if(end==std::wstring::npos)end=v.size();
                n.push_back(_wtoi(v.substr(at,end-at).c_str()));
                at=end+1;
            }
            return n;
        };
        std::vector<int> x=parts(a),y=parts(b);
        const size_t count=std::max(x.size(),y.size());
        x.resize(count);y.resize(count);
        return !a.empty()&&x>y;
    }

    Updater::~Updater()
    {
        if(worker.joinable())worker.join();
    }

    void Updater::Start(std::thread work)
    {
        if(worker.joinable())worker.join();
        worker=std::move(work);
    }

    void Updater::Check()
    {
        if(busy.exchange(true))return;
        Start(std::thread([this]{
            bool failed=false;
            Release* found=LatestNewer(failed).release();
            busy=false;
            if(!PostMessageW(notify,CheckedMessage,reinterpret_cast<WPARAM>(found),failed?1:0))delete found;
        }));
    }

    void Updater::Download(const Release& release)
    {
        if(busy.exchange(true))return;
        Start(std::thread([this,release]{
            auto* error=new std::wstring(DownloadAndUnpack(release));
            busy=false;
            if(!PostMessageW(notify,DownloadedMessage,reinterpret_cast<WPARAM>(error),0))delete error;
        }));
    }

    std::wstring Updater::Install()
    {
        namespace fs=std::filesystem;
        const fs::path exe=ExePath(),folder=exe.parent_path(),old=fs::path(exe).concat(L".old");
        const fs::path from=WorkFolder()/L"files"/AppFolder;
        std::error_code ec;
        fs::remove(old,ec);
        // A running exe cannot be overwritten, but it can be renamed.
        if(!MoveFileExW(exe.c_str(),old.c_str(),MOVEFILE_REPLACE_EXISTING))
            return T(L"このフォルダには書き込めないため、更新できませんでした。\n",L"Could not update: this folder cannot be written to.\n")+folder.wstring();
        if(!CopyFileW((from/L"MikuMikuDesktopPerch.exe").c_str(),exe.c_str(),FALSE))
        {
            MoveFileExW(old.c_str(),exe.c_str(),MOVEFILE_REPLACE_EXISTING);
            return T(L"新しい MikuMikuDesktopPerch.exe を置けなかったため、更新できませんでした。",L"Could not update: the new MikuMikuDesktopPerch.exe could not be put in place.");
        }
        // The rest (README, docs, licences) as well; one that cannot be replaced is left as it was.
        for(auto it=fs::recursive_directory_iterator(from,ec);!ec&&it!=fs::recursive_directory_iterator();it.increment(ec))
        {
            const fs::path relative=fs::relative(it->path(),from,ec);
            if(ec||relative==L"MikuMikuDesktopPerch.exe")continue;
            if(it->is_directory(ec))fs::create_directories(folder/relative,ec);
            else CopyFileW(it->path().c_str(),(folder/relative).c_str(),FALSE);
            ec.clear();
        }
        return {};
    }

    std::wstring Updater::UpdatedFrom()
    {
        const std::filesystem::path old=std::filesystem::path(ExePath()).concat(L".old");
        std::error_code ec;
        if(!std::filesystem::exists(old,ec))return {};
        const std::wstring version=FileVersion(old);
        return version!=CurrentVersion()?version:std::wstring();
    }

    void Updater::CleanUp()
    {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(ExePath()).concat(L".old"),ec);
        std::filesystem::remove_all(WorkFolder(),ec);
    }
}
