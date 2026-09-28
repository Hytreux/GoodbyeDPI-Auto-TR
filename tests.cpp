#include "platform.hpp"
#include <sstream>

namespace gd {
struct Mock final:Platform {
    std::vector<std::string> events;
    Json state;
    int checks=0,trials=0,commits=0;
    bool preflight_error=false,apply_error=false,start_error=false,restore_error=false,cleanup_error=false,save_error=false;
    std::function<bool(int)> response=[](int){return false;};
    std::function<void()> on_apply=[]{};
    void preflight()override{events.push_back("preflight");if(preflight_error)throw std::runtime_error("preflight");}
    void prepare()override{events.push_back("prepare");}
    Json capture()override{return {{"AdapterGuid","{11111111-1111-1111-1111-111111111111}"},{"Name","Test"},{"Ipv6",false},{"ManageV6",true},{"V4Static",false},{"V4Servers",Json::array()},{"V6Static",true},{"V6Servers",{"2001:db8::1"}}};}
    void save(const Json& data)override{events.push_back("save");if(save_error)throw std::runtime_error("save");state=data;}
    void apply(const Json&)override{events.push_back("apply");on_apply();if(apply_error)throw std::runtime_error("partial DNS");}
    void restore(const Json&)override{events.push_back("restore");if(restore_error)throw std::runtime_error("restore");}
    void trial(const Profile&)override{events.push_back("trial");++trials;if(start_error)throw std::runtime_error("driver");}
    bool check(const std::vector<std::string>&,bool,const std::atomic<bool>& stop)override{events.push_back("check");cancellation(stop);return response(++checks);}
    void commit()override{events.push_back("commit");++commits;}
    void remove_service()override{events.push_back("cleanup");if(cleanup_error)throw std::runtime_error("cleanup");}
    void clear()override{events.push_back("clear");state=nullptr;}
    bool did(const std::string& name){return std::find(events.begin(),events.end(),name)!=events.end();}
};
static void require(bool yes){if(!yes)throw std::runtime_error("Assertion failed");}
static void must_throw(const std::function<void()>& action){try{action();}catch(...){return;}throw std::runtime_error("Expected exception");}
int self_test(const std::filesystem::path& output){
    std::vector<std::pair<std::string,std::function<void()>>> tests;
    const std::vector<std::string> hosts={"discord.com","pornhub.com"};std::atomic<bool> stop{false};Events events;
    auto test=[&](std::string name,std::function<void()> body){tests.push_back({std::move(name),std::move(body)});};
    test("Normalize and deduplicate domains",[&]{require(parse_targets("HTTPS://Discord.com/\npornhub.com discord.com")==hosts);});
    test("Reject URL credentials, injection, IP and local targets",[]{for(const char* input:{"discord.com&whoami","https://user@discord.com/","https://discord.com:443/","https://discord.com/a","localhost","127.0.0.1","x.local","x.localhost"})must_throw([&]{parse_targets(input);});});
    test("Target count boundaries",[]{must_throw([]{parse_targets("");});must_throw([]{parse_targets("a.com b.com c.com d.com e.com f.com g.com");});});
    test("Domain boundaries on redirects",[]{require(same_domain("discord.com","www.discord.com"));require(!same_domain("discord.com","discord.com.evil.example"));require(!same_domain("discord.com","evil-discord.com"));});
    test("403/429/redirects are not success",[]{require(success(200)&&success(204));for(int code:{0,199,300,302,403,429,503})require(!success(code));});
    test("Windows argument quoting",[]{require(quote(L"C:\\a b\\")==L"\"C:\\a b\\\\\"");require(quote(L"a\"b")==L"\"a\\\"b\"");});
    test("Captured service error survives command construction",[]{SetLastError(ERROR_SERVICE_DOES_NOT_EXIST);const DWORD captured=GetLastError();const auto command=quote(L"C:\\Program Files\\GoodbyeDPI Auto\\engine\\goodbyedpi.exe")+L" "+wide(profiles.front().args);SetLastError(ERROR_NO_TOKEN);require(captured==ERROR_SERVICE_DOES_NOT_EXIST&&!command.empty());});
    test("UTF-8 Turkish roundtrip",[]{require(utf8(wide("İıŞşĞğÜüÖöÇç"))=="İıŞşĞğÜüÖöÇç");});
    test("Base64 roundtrip",[]{std::string data="discord.com\npornhub.com";require(unbase64(wide(base64(data.data(),data.size())))==data);});
    test("Empty Base64 payload for DNS snapshot",[]{require(base64(nullptr,0).empty());});
    test("SHA256 known answer",[]{require(sha256("abc")=="BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");});
    test("Exact GoodbyeDPI-Turkey profile order",[]{
        const std::vector<std::string> expected={
            "-5 --set-ttl 5 --dns-addr 77.88.8.8 --dns-port 1253 --dnsv6-addr 2a02:6b8::feed:0ff --dnsv6-port 1253",
            "--set-ttl 3", "-5",
            "--set-ttl 3 --dns-addr 77.88.8.8 --dns-port 1253 --dnsv6-addr 2a02:6b8::feed:0ff --dnsv6-port 1253",
            "-5 --dns-addr 77.88.8.8 --dns-port 1253 --dnsv6-addr 2a02:6b8::feed:0ff --dnsv6-port 1253",
            "-9 --dns-addr 77.88.8.8 --dns-port 1253 --dnsv6-addr 2a02:6b8::feed:0ff --dnsv6-port 1253",
            "-9"};
        require(profiles.size()==expected.size());for(size_t i=0;i<expected.size();++i)require(profiles[i].args==expected[i]);
    });
    test("First Turkey profile persists after four checks",[&]{Mock p;p.response=[](int){return true;};Installer(p,events).install(hosts,stop);require(p.checks==4&&p.trials==1&&p.commits==1&&p.state["Status"]=="Installed");});
    test("Backup before partial DNS mutation",[&]{Mock p;p.apply_error=true;must_throw([&]{Installer(p,events).install(hosts,stop);});require(std::find(p.events.begin(),p.events.end(),"save")<std::find(p.events.begin(),p.events.end(),"apply"));require(p.did("restore")&&p.state.is_null());});
    test("Transient first profile advances to alternative 1",[&]{Mock p;p.response=[](int n){return n!=2;};Installer(p,events).install(hosts,stop);require(p.trials==2&&p.commits==1&&p.checks==6);});
    test("Profile reverified after real restart boundary",[&]{Mock p;p.response=[](int){return true;};Installer(p,events).install(hosts,stop);require(p.trials==1&&p.checks==4&&p.commits==1);});
    test("Failed final check advances to next profile",[&]{Mock p;p.response=[](int n){return n!=4;};Installer(p,events).install(hosts,stop);require(p.trials==2&&p.commits==2&&p.checks==8);});
    test("Exhausted seven-profile search rolls back",[&]{Mock p;must_throw([&]{Installer(p,events).install(hosts,stop);});require(p.trials==7&&p.checks==7&&p.commits==0&&p.did("restore")&&p.state.is_null());});
    test("Driver failure rolls back",[&]{Mock p;p.start_error=true;must_throw([&]{Installer(p,events).install(hosts,stop);});require(p.did("cleanup")&&p.did("restore")&&p.state.is_null());});
    test("Cancellation rolls back",[&]{Mock p;std::atomic<bool> cancel{false};p.on_apply=[&]{cancel=true;};must_throw([&]{Installer(p,events).install(hosts,cancel);});require(p.did("restore")&&p.state.is_null());});
    test("Preflight collision cannot mutate network",[&]{Mock p;p.preflight_error=true;must_throw([&]{Installer(p,events).install(hosts,stop);});require(p.events==std::vector<std::string>{"preflight"});});
    test("Backup write failure cannot mutate network",[&]{Mock p;p.save_error=true;must_throw([&]{Installer(p,events).install(hosts,stop);});require(!p.did("apply")&&!p.did("trial"));});
    test("Cleanup failure still attempts DNS restore",[&]{Mock p;p.cleanup_error=true;must_throw([&]{Installer(p,events).install(hosts,stop);});require(p.did("restore")&&!p.state.is_null()&&!p.did("clear"));});
    test("Recovery failure retains durable journal",[&]{Mock p;p.restore_error=true;must_throw([&]{Installer(p,events).install(hosts,stop);});require(!p.state.is_null()&&p.state["Status"]=="Pending");});
    test("Preserve IPv4 DHCP and IPv6 static independently",[&]{Mock p;p.response=[](int){return true;};Installer(p,events).install(hosts,stop);require(p.state["Dns"]["V4Static"]==false&&p.state["Dns"]["V6Static"]==true);Installer(p,events).recover(p.state);require(p.state.is_null());});
    test("Invalid recovery state cannot mutate network",[&]{Mock p;must_throw([&]{Installer(p,events).recover(nullptr);});require(p.events.empty());});
    test("Embedded GoodbyeDPI executable fingerprint",[]{require(sha256(resource(100))=="8D412B094BB9C137FF25BA9A794D1122ECC84BB776DEBFF6C249723A13CC31CD");});
    test("Embedded WinDivert library fingerprint",[]{require(sha256(resource(101))=="6110BFA44667405179C3E15E12AF1B62037E447ED59B054B19042032995E6C7E");});
    test("Embedded signed driver fingerprint",[]{require(sha256(resource(102))=="E69B5BA3F0CD6CFB2983E442636E7F0B342B61B15264B0328317D4559C82CF50");});
    test("Native child process execution is bounded",[]{wchar_t dir[MAX_PATH];GetSystemDirectoryW(dir,MAX_PATH);auto r=run((std::filesystem::path(dir)/L"cmd.exe").wstring(),L"/d /c exit 7",4000);require(r.code==7);});
    int failed=0;std::ostringstream out;out<<"GoodbyeDPI Auto v0.3.0 — C++ isolated tests\nNo DNS/service/driver changes or website requests.\n";
    for(const auto& t:tests){try{t.second();out<<"PASS: "<<t.first<<'\n';}catch(const std::exception& ex){++failed;out<<"FAIL: "<<t.first<<" — "<<ex.what()<<'\n';}}
    out<<(tests.size()-failed)<<"/"<<tests.size()<<" passed.\n";export_text(output,out.str());return failed?1:0;
}
}
