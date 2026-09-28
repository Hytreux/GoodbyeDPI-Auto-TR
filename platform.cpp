#include "platform.hpp"
#include <shlobj.h>
#include <shlwapi.h>
#include <winhttp.h>
#include <windns.h>
#include <wincrypt.h>
#include <aclapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <chrono>

namespace gd {
namespace fs=std::filesystem;
struct Handle {
    HANDLE h=nullptr;
    explicit Handle(HANDLE value=nullptr):h(value){}
    ~Handle(){ if(h && h!=INVALID_HANDLE_VALUE) CloseHandle(h); }
    Handle(const Handle&)=delete;
};
struct Service {
    SC_HANDLE h=nullptr;
    explicit Service(SC_HANDLE value=nullptr):h(value){}
    ~Service(){if(h)CloseServiceHandle(h);}
};
static void fail(const std::string& context,DWORD error) { throw std::runtime_error(context + " (Windows " + std::to_string(error) + ")"); }
static void fail(const std::string& context) { fail(context,GetLastError()); }
std::wstring wide(const std::string& input) {
    if(input.empty())return {};
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,input.data(),static_cast<int>(input.size()),nullptr,0);
    if(!n)throw std::runtime_error("UTF-8 metin okunamadı.");
    std::wstring out(n,0); MultiByteToWideChar(CP_UTF8,0,input.data(),static_cast<int>(input.size()),out.data(),n);return out;
}
std::string utf8(const std::wstring& input) {
    if(input.empty())return {};
    int n=WideCharToMultiByte(CP_UTF8,0,input.data(),static_cast<int>(input.size()),nullptr,0,nullptr,nullptr);
    std::string out(n,0);WideCharToMultiByte(CP_UTF8,0,input.data(),static_cast<int>(input.size()),out.data(),n,nullptr,nullptr);return out;
}
std::wstring quote(const std::wstring& input) {
    std::wstring out=L"\"";size_t slashes=0;
    for(wchar_t c:input) {
        if(c==L'\\'){++slashes;continue;}
        out.append(c==L'"'?slashes*2+1:slashes,L'\\');out+=c;slashes=0;
    }
    out.append(slashes*2,L'\\');out+=L'"';return out;
}
std::string base64(const void* data,size_t size) {
    if(size==0)return {};
    DWORD n=0;CryptBinaryToStringA(static_cast<const BYTE*>(data),static_cast<DWORD>(size),CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,nullptr,&n);
    std::string result(n,0);if(!CryptBinaryToStringA(static_cast<const BYTE*>(data),static_cast<DWORD>(size),CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,result.data(),&n))fail("Kodlama");
    while(!result.empty() && !result.back())result.pop_back();return result;
}
std::string unbase64(const std::wstring& data) {
    DWORD n=0;if(!CryptStringToBinaryW(data.c_str(),0,CRYPT_STRING_BASE64,nullptr,&n,nullptr,nullptr))fail("Hedef listesi");
    std::string result(n,0);if(!CryptStringToBinaryW(data.c_str(),0,CRYPT_STRING_BASE64,reinterpret_cast<BYTE*>(result.data()),&n,nullptr,nullptr))fail("Hedef listesi");return result;
}
static fs::path known(REFKNOWNFOLDERID id) { PWSTR text=nullptr; if(FAILED(SHGetKnownFolderPath(id,0,nullptr,&text)))throw std::runtime_error("Windows klasörü bulunamadı.");fs::path path(text);CoTaskMemFree(text);return path; }
fs::path install_root(){return known(FOLDERID_ProgramFiles)/L"GoodbyeDPI Auto";}
fs::path state_root(){return known(FOLDERID_ProgramData)/L"GoodbyeDPI Auto";}
static fs::path engine(){return install_root()/L"engine"/L"goodbyedpi.exe";}
static fs::path journal(){return state_root()/L"state.json";}
static fs::path system_path(const wchar_t* name){wchar_t buffer[MAX_PATH];GetSystemDirectoryW(buffer,MAX_PATH);return fs::path(buffer)/name;}
std::wstring exe_path(){std::wstring p(32768,0);DWORD n=GetModuleFileNameW(nullptr,p.data(),static_cast<DWORD>(p.size()));p.resize(n);return p;}
bool is_admin(){BOOL yes=FALSE;SID_IDENTIFIER_AUTHORITY auth=SECURITY_NT_AUTHORITY;PSID sid=nullptr;
    if(AllocateAndInitializeSid(&auth,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,0,0,0,0,0,0,&sid)){CheckTokenMembership(nullptr,sid,&yes);FreeSid(sid);}return yes!=FALSE;}
static void no_links(fs::path p){
    while(!p.empty()){
        DWORD attr=GetFileAttributesW(p.c_str());if(attr!=INVALID_FILE_ATTRIBUTES && (attr&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Kurulum konumunda dosya yönlendirmesi var.");
        auto parent=p.parent_path();if(parent==p)break;p=parent;
    }
}
static void secure_dir(const fs::path& path){
    no_links(path);PSECURITY_DESCRIPTOR descriptor=nullptr;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)",SDDL_REVISION_1,&descriptor,nullptr))fail("Klasör izinleri");
    SECURITY_ATTRIBUTES attr={sizeof(attr),descriptor,FALSE};
    if(!CreateDirectoryW(path.c_str(),&attr) && GetLastError()!=ERROR_ALREADY_EXISTS){LocalFree(descriptor);fail("Klasör oluşturma");}
    BOOL present,def;PACL dacl=nullptr;GetSecurityDescriptorDacl(descriptor,&present,&dacl,&def);
    DWORD error=SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,dacl,nullptr);
    LocalFree(descriptor);if(error!=ERROR_SUCCESS){SetLastError(error);fail("Klasör koruması");}
}
void export_text(const fs::path& path,const std::string& text){std::ofstream out(path,std::ios::binary|std::ios::trunc);if(!out)throw std::runtime_error("Dosya yazılamadı.");out.write(text.data(),text.size());if(!out)throw std::runtime_error("Dosya yazma tamamlanamadı.");}
static void durable(const fs::path& path,const std::string& data){
    no_links(path);fs::path temp=path;temp+=L".new";no_links(temp);
    {Handle file(CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr));
    if(file.h==INVALID_HANDLE_VALUE)fail("Yedek açılamadı");DWORD n=0;
    if(!WriteFile(file.h,data.data(),static_cast<DWORD>(data.size()),&n,nullptr)||n!=data.size()||!FlushFileBuffers(file.h))fail("Yedek kaydedilemedi");}
    if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))fail("Yedek tamamlanamadı");
}
bool state_exists(){return fs::exists(journal());}
Json read_state(){
    if(!state_exists())return Json();no_links(journal());
    if(fs::file_size(journal())>128*1024)throw std::runtime_error("Yedek dosyası geçersiz.");
    std::ifstream in(journal());Json state=Json::parse(in);
    if(!state.is_object()||state.value("Schema",0)!=1||!state.contains("Dns")||!state["Dns"].is_object())throw std::runtime_error("DNS yedeği okunamadı. state.json dosyasını koruyun.");return state;
}
std::string resource(int id){HRSRC res=FindResourceW(nullptr,MAKEINTRESOURCEW(id),RT_RCDATA);if(!res)fail("Gömülü dosya bulunamadı");DWORD n=SizeofResource(nullptr,res);const char* p=static_cast<const char*>(LockResource(LoadResource(nullptr,res)));if(!p)fail("Gömülü dosya açılamadı");return std::string(p,n);}
std::string sha256(const std::string& data){
    HCRYPTPROV provider=0;HCRYPTHASH hash=0;BYTE result[32];DWORD size=32;
    if(!CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT))fail("Hash başlatılamadı");
    bool ok=CryptCreateHash(provider,CALG_SHA_256,0,0,&hash) && CryptHashData(hash,reinterpret_cast<const BYTE*>(data.data()),static_cast<DWORD>(data.size()),0) && CryptGetHashParam(hash,HP_HASHVAL,result,&size,0);
    if(hash)CryptDestroyHash(hash);CryptReleaseContext(provider,0);if(!ok)fail("Hash kontrolü");
    std::ostringstream out;for(BYTE b:result)out<<std::hex<<std::uppercase<<std::setfill('0')<<std::setw(2)<<static_cast<int>(b);return out.str();
}
RunResult run(const std::wstring& path,const std::wstring& args,DWORD timeout,const std::atomic<bool>* cancel){
    SECURITY_ATTRIBUTES sa={sizeof(sa),nullptr,TRUE};HANDLE r=nullptr,w=nullptr;if(!CreatePipe(&r,&w,&sa,0))fail("İşlem kanalı");Handle reader(r),writer(w);
    SetHandleInformation(reader.h,HANDLE_FLAG_INHERIT,0);
    STARTUPINFOW si={sizeof(si)};si.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;si.hStdOutput=writer.h;si.hStdError=writer.h;
    Handle input(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr));si.hStdInput=input.h;
    PROCESS_INFORMATION pi={};std::wstring command=quote(path)+L" "+args;
    std::vector<wchar_t> env;LPWCH block=GetEnvironmentStringsW();
    for(const wchar_t* entry=block;entry&&*entry;entry+=wcslen(entry)+1){std::wstring v(entry);auto key=v.substr(0,v.find(L'='));
        if(_wcsicmp(key.c_str(),L"CURL_CA_BUNDLE")&&_wcsicmp(key.c_str(),L"SSL_CERT_FILE")&&_wcsicmp(key.c_str(),L"SSL_CERT_DIR")){env.insert(env.end(),v.begin(),v.end());env.push_back(0);}}
    FreeEnvironmentStringsW(block);env.push_back(0);
    Handle job(CreateJobObjectW(nullptr,nullptr));JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit={};limit.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job.h||!SetInformationJobObject(job.h,JobObjectExtendedLimitInformation,&limit,sizeof(limit)))fail("İşlem denetimi");
    if(!CreateProcessW(path.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED|CREATE_UNICODE_ENVIRONMENT,env.data(),nullptr,&si,&pi))fail("İşlem başlatılamadı");
    Handle process(pi.hProcess),thread(pi.hThread);
    if(!AssignProcessToJobObject(job.h,process.h)){TerminateProcess(process.h,1);fail("İşlem sınırı");}
    CloseHandle(writer.h);writer.h=nullptr;ResumeThread(thread.h);
    RunResult result;ULONGLONG began=GetTickCount64();
    auto drain=[&]{DWORD available=0;while(PeekNamedPipe(reader.h,nullptr,0,nullptr,&available,nullptr)&&available){char buf[4096];DWORD read=0;if(!ReadFile(reader.h,buf,std::min<DWORD>(available,sizeof(buf)),&read,nullptr)||!read)break;result.output.append(buf,read);if(result.output.size()>1024*1024)throw std::runtime_error("İşlem çıktısı beklenenden büyük.");}};
    try {
        for(;;){drain();if(cancel)cancellation(*cancel);if(WaitForSingleObject(process.h,25)==WAIT_OBJECT_0)break;
            if(GetTickCount64()-began>timeout)throw std::runtime_error("İşlem zaman aşımına uğradı.");}
        drain();DWORD code=0;GetExitCodeProcess(process.h,&code);result.code=static_cast<int>(code);return result;
    }catch(...){TerminateJobObject(job.h,1);WaitForSingleObject(process.h,5000);throw;}
}
static Service manager(){SC_HANDLE h=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_ALL_ACCESS);if(!h)fail("Hizmet yöneticisi");return Service(h);}
static bool exists_service(){Service m(OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT));if(!m.h)fail("Hizmet durumu");SC_HANDLE handle=OpenServiceW(m.h,L"GoodbyeDPI",SERVICE_QUERY_STATUS);const DWORD error=handle?ERROR_SUCCESS:GetLastError();Service s(handle);if(s.h)return true;if(error!=ERROR_SERVICE_DOES_NOT_EXIST)fail("Hizmet durumu",error);return false;}
bool service_running(){Service m(OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT));if(!m.h)return false;Service s(OpenServiceW(m.h,L"GoodbyeDPI",SERVICE_QUERY_STATUS));SERVICE_STATUS state={};return s.h&&QueryServiceStatus(s.h,&state)&&state.dwCurrentState==SERVICE_RUNNING;}
static void owned(){
    if(!state_exists())throw std::runtime_error("Hizmet işlemi için kurulum yedeği bulunamadı.");
    auto m=manager();SC_HANDLE handle=OpenServiceW(m.h,L"GoodbyeDPI",SERVICE_QUERY_CONFIG);const DWORD error=handle?ERROR_SUCCESS:GetLastError();Service s(handle);
    if(!s.h){if(error==ERROR_SERVICE_DOES_NOT_EXIST)return;fail("Hizmet denetimi",error);}
    DWORD bytes=0;QueryServiceConfigW(s.h,nullptr,0,&bytes);std::vector<BYTE> buffer(bytes);
    auto config=reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());if(!QueryServiceConfigW(s.h,config,bytes,&bytes))fail("Hizmet yapılandırması");
    const auto expected=quote(engine().wstring())+L" ";std::wstring actual=config->lpBinaryPathName;
    if(actual.size()<expected.size()||_wcsnicmp(actual.c_str(),expected.c_str(),expected.size()))throw std::runtime_error("GoodbyeDPI hizmeti bu uygulamaya ait değil. Değiştirilmedi.");
}
static void wait_service(SC_HANDLE service,DWORD target){
    ULONGLONG began=GetTickCount64();SERVICE_STATUS status={};
    while(QueryServiceStatus(service,&status)){
        if(status.dwCurrentState==target)return;
        if(target==SERVICE_RUNNING&&status.dwCurrentState==SERVICE_STOPPED)throw std::runtime_error("GoodbyeDPI başlatılamadı. Sürücü veya hizmet hatası.");
        if(GetTickCount64()-began>15000)throw std::runtime_error("Hizmet zaman aşımı.");Sleep(100);
    }fail("Hizmet durumu");
}
static void stop_service(){
    auto m=manager();SC_HANDLE handle=OpenServiceW(m.h,L"GoodbyeDPI",SERVICE_STOP|SERVICE_QUERY_STATUS);const DWORD error=handle?ERROR_SUCCESS:GetLastError();Service s(handle);if(!s.h){if(error==ERROR_SERVICE_DOES_NOT_EXIST)return;fail("Hizmet durdurma",error);}
    SERVICE_STATUS status={};if(!QueryServiceStatus(s.h,&status))fail("Hizmet sorgusu");if(status.dwCurrentState==SERVICE_STOPPED)return;
    if(status.dwCurrentState!=SERVICE_STOP_PENDING&&!ControlService(s.h,SERVICE_CONTROL_STOP,&status))fail("Hizmet durdurma");wait_service(s.h,SERVICE_STOPPED);
}
static void configure_service(const std::string& args,bool automatic){
    auto m=manager();SC_HANDLE handle=OpenServiceW(m.h,L"GoodbyeDPI",SERVICE_ALL_ACCESS);const DWORD open_error=handle?ERROR_SUCCESS:GetLastError();Service s(handle);const auto cmd=quote(engine().wstring())+L" "+wide(args);
    if(!s.h){
        if(open_error!=ERROR_SERVICE_DOES_NOT_EXIST)fail("Hizmet açma",open_error);
        s.h=CreateServiceW(m.h,L"GoodbyeDPI",L"GoodbyeDPI Auto",SERVICE_ALL_ACCESS,SERVICE_WIN32_OWN_PROCESS,automatic?SERVICE_AUTO_START:SERVICE_DEMAND_START,SERVICE_ERROR_NORMAL,cmd.c_str(),nullptr,nullptr,nullptr,nullptr,nullptr);
        if(!s.h)fail("Hizmet oluşturma");
    } else if(!ChangeServiceConfigW(s.h,SERVICE_NO_CHANGE,automatic?SERVICE_AUTO_START:SERVICE_DEMAND_START,SERVICE_NO_CHANGE,cmd.c_str(),nullptr,nullptr,nullptr,nullptr,nullptr,nullptr))fail("Hizmet ayarı");
    SERVICE_DESCRIPTIONW desc={const_cast<wchar_t*>(L"GoodbyeDPI Auto — automatically selected connection profile")};ChangeServiceConfig2W(s.h,SERVICE_CONFIG_DESCRIPTION,&desc);
}
static void start_service(){auto m=manager();SC_HANDLE handle=OpenServiceW(m.h,L"GoodbyeDPI",SERVICE_START|SERVICE_QUERY_STATUS);const DWORD error=handle?ERROR_SUCCESS:GetLastError();Service s(handle);if(!s.h)fail("Hizmet başlatma",error);if(!StartServiceW(s.h,0,nullptr))fail("Hizmet başlatma");wait_service(s.h,SERVICE_RUNNING);Sleep(1500);}
void WindowsPlatform::preflight(){
    if(!is_admin())throw std::runtime_error("Yönetici izni gerekiyor.");
    SYSTEM_INFO info;GetNativeSystemInfo(&info);if(info.wProcessorArchitecture!=PROCESSOR_ARCHITECTURE_AMD64)throw std::runtime_error("Intel/AMD 64 bit Windows gerekiyor.");
    if(!fs::exists(system_path(L"curl.exe")))throw std::runtime_error("Windows curl bileşeni bulunamadı.");
    if(state_exists())throw std::runtime_error("Önceki kurulum var. Önce 'Eski ayarlara dön' seçeneğini kullanın.");
    if(exists_service())throw std::runtime_error("Başka bir GoodbyeDPI hizmeti zaten kurulu.");
    Handle processes(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));PROCESSENTRY32W entry={sizeof(entry)};
    if(processes.h==INVALID_HANDLE_VALUE||!Process32FirstW(processes.h,&entry))fail("Çalışan uygulamalar kontrol edilemedi");
    do{if(!_wcsicmp(entry.szExeFile,L"goodbyedpi.exe")||!_wcsicmp(entry.szExeFile,L"winws.exe")||!_wcsicmp(entry.szExeFile,L"winws2.exe"))throw std::runtime_error("GoodbyeDPI/zapret zaten çalışıyor. Önce mevcut uygulamayı kapatın.");}while(Process32NextW(processes.h,&entry));
    secure_dir(state_root());
}
void WindowsPlatform::prepare(){
    secure_dir(install_root());secure_dir(install_root()/L"engine");secure_dir(install_root()/L"licenses");
    struct Asset{int id;const wchar_t* name;const char* hash;};
    const Asset assets[]={{100,L"goodbyedpi.exe","8D412B094BB9C137FF25BA9A794D1122ECC84BB776DEBFF6C249723A13CC31CD"},{101,L"WinDivert.dll","6110BFA44667405179C3E15E12AF1B62037E447ED59B054B19042032995E6C7E"},{102,L"WinDivert64.sys","E69B5BA3F0CD6CFB2983E442636E7F0B342B61B15264B0328317D4559C82CF50"}};
    for(const auto& a:assets){std::string data=resource(a.id);if(sha256(data)!=a.hash)throw std::runtime_error("Motor dosyasının bütünlük kontrolü başarısız.");auto path=install_root()/L"engine"/a.name;no_links(path);durable(path,data);}
    for(int i=0;i<4;++i){const wchar_t* names[]={L"GoodbyeDPI.txt",L"WinDivert.txt",L"getline.txt",L"uthash.txt"};durable(install_root()/L"licenses"/names[i],resource(110+i));}
}
std::string WindowsPlatform::powershell(const std::string& action,const Json& dns){
    std::string data=dns.is_null()?"":dns.dump();std::string script="$ProgressPreference='SilentlyContinue'\n& {\n"+resource(103)+"\n} -Action '"+action+"' -BackupBase64 '"+base64(data.data(),data.size())+"'";
    std::wstring u=wide(script);auto result=run(system_path(L"WindowsPowerShell\\v1.0\\powershell.exe").wstring(),L"-NoLogo -NoProfile -NonInteractive -OutputFormat Text -EncodedCommand "+wide(base64(u.data(),u.size()*sizeof(wchar_t))),60000);
    if(result.code){
        struct Known{const char* raw;const char* clear;};const Known known[]={
            {"Etkin IPv4 internet baglantisi bulunamadi","Etkin IPv4 bağlantısı bulunamadı. Wi-Fi veya Ethernet bağlantısını açıp yeniden deneyin."},
            {"Etkin baglanti sanal/VPN ag karti","Etkin bağlantı VPN veya sanal ağ kartı. Normal Wi-Fi ya da Ethernet bağlantısına geçin."},
            {"IPv4 ve IPv6 farkli ag kartlarindan cikiyor","IPv4 ve IPv6 farklı ağ kartlarından çıkıyor. Tek bir etkin bağlantı kullanın."},
            {"Yedeklenen ag karti bulunamadi","Yedeklenen ağ kartı bulunamadı. Kartı yeniden bağlayıp geri almayı deneyin."},
            {"IPv4: DNS ayari dogrulanamadi","Cloudflare IPv4 DNS ayarı Windows tarafından doğrulanamadı."},
            {"IPv6: DNS ayari dogrulanamadi","Cloudflare IPv6 DNS ayarı Windows tarafından doğrulanamadı."},
            {"DNS ayari dogrulanamadi","Cloudflare DNS ayarı Windows tarafından doğrulanamadı."},
            {"Statik DNS geri yuklemesi dogrulanamadi","Eski statik DNS ayarı geri yüklenemedi. Yedek korunuyor."},
            {"Otomatik DNS geri yuklemesi dogrulanamadi","Eski otomatik DNS ayarı geri yüklenemedi. Yedek korunuyor."}};
        for(const auto& item:known)if(result.output.find(item.raw)!=std::string::npos)throw std::runtime_error(item.clear);
        if(result.output.size()>1200)result.output.resize(1200);throw std::runtime_error("DNS işlemi tamamlanamadı: "+result.output);
    }return result.output;
}
Json WindowsPlatform::snapshot(){auto data=Json::parse(powershell("Snapshot"));if(!data.contains("AdapterGuid")||!data["AdapterGuid"].is_string())throw std::runtime_error("Ağ kartı belirlenemedi.");return data;}
Json WindowsPlatform::capture(){Json data=snapshot();adapter=data["AdapterGuid"].get<std::string>();e.log("Etkin bağlantı: "+data.value("Name",std::string("Ağ")));return data;}
void WindowsPlatform::save(const Json& data){durable(journal(),data.dump(2));}
void WindowsPlatform::apply(const Json& data){powershell("Apply",data);}
void WindowsPlatform::restore(const Json& data){powershell("Restore",data);}
void WindowsPlatform::clear(){no_links(journal());if(!DeleteFileW(journal().c_str())&&GetLastError()!=ERROR_FILE_NOT_FOUND)fail("Yedek kapatılamadı");}
void WindowsPlatform::trial(const Profile& p){owned();stop_service();configure_service(p.args,false);start_service();}
void WindowsPlatform::commit(){owned();stop_service();auto m=manager();{Service s(OpenServiceW(m.h,L"GoodbyeDPI",SERVICE_CHANGE_CONFIG));if(!s.h||!ChangeServiceConfigW(s.h,SERVICE_NO_CHANGE,SERVICE_AUTO_START,SERVICE_NO_CHANGE,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr))fail("Otomatik başlangıç");}start_service();}
void WindowsPlatform::remove_service(){
    if(!exists_service())return;owned();stop_service();{auto m=manager();Service s(OpenServiceW(m.h,L"GoodbyeDPI",DELETE));if(!s.h||!DeleteService(s.h))fail("Hizmet kaldırma");}
    ULONGLONG began=GetTickCount64();while(exists_service()){if(GetTickCount64()-began>6000)throw std::runtime_error("Hizmet silinmeyi bekliyor. Hizmetler penceresini kapatıp tekrar deneyin.");Sleep(100);}
}
static std::string redirect(const std::string& root,const std::string& from,const std::string& location){
    std::wstring buffer(16384,0);DWORD n=static_cast<DWORD>(buffer.size());if(FAILED(UrlCombineW(wide(from).c_str(),wide(location).c_str(),buffer.data(),&n,0)))throw std::runtime_error("Yönlendirme okunamadı.");buffer.resize(n);
    URL_COMPONENTS url={sizeof(url)};url.dwHostNameLength=url.dwUserNameLength=url.dwPasswordLength=static_cast<DWORD>(-1);
    if(!WinHttpCrackUrl(buffer.c_str(),0,0,&url)||url.nScheme!=INTERNET_SCHEME_HTTPS||url.nPort!=443||url.dwUserNameLength||url.dwPasswordLength||!same_domain(root,utf8(std::wstring(url.lpszHostName,url.dwHostNameLength))))throw std::runtime_error("Hedef dışı yönlendirme başarı sayılmadı.");
    return utf8(buffer);
}
bool WindowsPlatform::probe(const std::string& host,bool ipv6,const std::atomic<bool>& cancel,Result& view){
    std::string url="https://"+host+"/";bool get=false;ULONGLONG began=GetTickCount64();
    const std::string family=ipv6?"IPv6":"IPv4";
    try{
        for(int hop=0;hop<6;++hop){
            auto args=L"-q --silent --show-error --http1.1 --noproxy \"*\" --proto =https --connect-timeout 5 --max-time 9 "+std::wstring(ipv6?L"--ipv6 ":L"--ipv4 ")+L"--header \"Cache-Control: no-cache\" --user-agent \"Mozilla/5.0 GoodbyeDPIAuto/0.3.1\" --dump-header - --output NUL "+(get?L"--request GET --range 0-0 --max-filesize 16384 ":L"--head ")+quote(wide(url));
            auto result=run(system_path(L"curl.exe").wstring(),args,13000,&cancel);
            std::regex status(R"((?:^|\n)HTTP/\S+\s+(\d{3}))");int code=0;
            for(auto it=std::sregex_iterator(result.output.begin(),result.output.end(),status);it!=std::sregex_iterator();++it)code=std::stoi((*it)[1]);
            view.http=code;view.milliseconds=static_cast<int>(GetTickCount64()-began);
            if(result.code && !(get&&result.code==63&&success(code))){view.detail=family+" · bağlantı kurulamadı ("+std::to_string(result.code)+")";e.log(host+" · "+view.detail);return false;}
            if(code==405&&!get){get=true;continue;}
            if(success(code)){view.detail=family+" · HTTPS "+std::to_string(code);e.log(host+" · "+view.detail);return true;}
            if(code==301||code==302||code==303||code==307||code==308){std::smatch match;std::regex header(R"((?:^|\n)Location:\s*([^\r\n]+))",std::regex::icase);if(!std::regex_search(result.output,match,header))throw std::runtime_error("Yönlendirme adresi yok.");url=redirect(host,url,match[1]);continue;}
            view.detail=family+" · HTTP "+std::to_string(code)+(code==403||code==429?" / site sınırlaması olabilir":" / doğrulanamadı");e.log(host+" · "+view.detail);return false;
        }
        view.detail="Çok fazla yönlendirme";
    }catch(const Cancelled&){throw;}catch(const std::exception& ex){view.detail=ex.what();}
    e.log(host+" · "+view.detail);return false;
}
bool WindowsPlatform::check(const std::vector<std::string>& targets,bool ipv6,const std::atomic<bool>& cancel){
    cancellation(cancel);auto net=snapshot();if(net.value("AdapterGuid",std::string())!=adapter||net.value("Ipv6",false)!=ipv6)throw std::runtime_error("Test sırasında etkin bağlantı değişti. Yeni bağlantıda yeniden deneyin.");
    auto hosts=targets;if(std::find(hosts.begin(),hosts.end(),"www.microsoft.com")==hosts.end())hosts.push_back("www.microsoft.com");bool all=true;
    for(const auto& host:hosts){
        cancellation(cancel);Result view;view.host=host;view.reach=Reach::Testing;view.detail="HTTPS yanıtı bekleniyor";e.result(view);
        bool ok=probe(host,false,cancel,view);std::string ipv4detail=view.detail;
        if(ipv6){
            PDNS_RECORD records=nullptr;DNS_STATUS status=DnsQuery_W(wide(host).c_str(),DNS_TYPE_AAAA,DNS_QUERY_STANDARD,nullptr,&records,nullptr);bool has=false;
            for(auto* r=records;r;r=r->pNext)if(r->wType==DNS_TYPE_AAAA)has=true;if(records)DnsRecordListFree(records,DnsFreeRecordList);
            if(has){bool v6=probe(host,true,cancel,view);ok=ok&&v6;view.detail=ipv4detail+" | "+view.detail;}
            else if(status==DNS_INFO_NO_RECORDS||status==ERROR_SUCCESS){e.log(host+" · IPv6 AAAA kaydı yok; IPv4 test edildi.");}
            else{ok=false;view.detail="IPv6 DNS yanıtı doğrulanamadı";}
        }
        cancellation(cancel);view.reach=ok?Reach::Passed:Reach::Failed;e.result(view);all=all&&ok;
    }
    if(exists_service()){owned();if(!service_running())throw std::runtime_error("Hizmet test sırasında durdu.");}
    return all;
}
}
