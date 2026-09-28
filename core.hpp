#pragma once
#include "vendor/json.hpp"
#include <algorithm>
#include <atomic>
#include <functional>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace gd {
using Json = nlohmann::json;
struct Cancelled : std::runtime_error { Cancelled() : runtime_error("İşlem iptal edildi.") {} };
inline void cancellation(const std::atomic<bool>& flag) { if (flag.load()) throw Cancelled(); }
struct Profile { std::string name, args; };
inline const std::vector<Profile> profiles = {
    {"Türkiye ana profil", "-5 --set-ttl 5 --dns-addr 77.88.8.8 --dns-port 1253 --dnsv6-addr 2a02:6b8::feed:0ff --dnsv6-port 1253"},
    {"Alternatif 1 · TTL 3", "--set-ttl 3"},
    {"Alternatif 2 · Modern", "-5"},
    {"Alternatif 3 · TTL 3 + DNS", "--set-ttl 3 --dns-addr 77.88.8.8 --dns-port 1253 --dnsv6-addr 2a02:6b8::feed:0ff --dnsv6-port 1253"},
    {"Alternatif 4 · Modern + DNS", "-5 --dns-addr 77.88.8.8 --dns-port 1253 --dnsv6-addr 2a02:6b8::feed:0ff --dnsv6-port 1253"},
    {"Alternatif 5 · Gelişmiş + DNS", "-9 --dns-addr 77.88.8.8 --dns-port 1253 --dnsv6-addr 2a02:6b8::feed:0ff --dnsv6-port 1253"},
    {"Alternatif 6 · Gelişmiş", "-9"}
};
enum class Stage { Ready, Prepare, Dns, Scan, Save, Complete, Restore, Error };
enum class Reach { Waiting, Testing, Passed, Failed };
struct Result { std::string host, detail; Reach reach = Reach::Waiting; int http = 0, milliseconds = 0; };
struct Events {
    std::function<void(const std::string&)> log = [](auto&) {};
    std::function<void(Stage, const std::string&, int, int)> stage = [](auto, auto&, int, int) {};
    std::function<void(const Result&)> result = [](auto&) {};
};
inline std::vector<std::string> parse_targets(std::string input) {
    std::transform(input.begin(), input.end(), input.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::regex split("[\\s,;]+"), domain(R"(^(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.)+[a-z]{2,63}$)");
    std::vector<std::string> result;
    for (auto it = std::sregex_token_iterator(input.begin(), input.end(), split, -1); it != std::sregex_token_iterator(); ++it) {
        std::string value = *it;
        if (value.empty()) continue;
        if (value.rfind("https://", 0) == 0) { value.erase(0,8); if (!value.empty() && value.back() == '/') value.pop_back(); }
        auto suffix = [&](const std::string& s) { return value.size() >= s.size() && value.compare(value.size()-s.size(),s.size(),s)==0; };
        if (value.size()>253 || !std::regex_match(value,domain) || suffix(".local") || suffix(".localhost"))
            throw std::runtime_error("Geçersiz alan adı: " + value + ". Örnek: discord.com");
        if (std::find(result.begin(),result.end(),value)==result.end()) result.push_back(value);
    }
    if (result.empty() || result.size()>6) throw std::runtime_error("1–6 alan adı ekleyin.");
    return result;
}
inline bool success(int status) { return status>=200 && status<300; }
inline bool same_domain(std::string root,std::string host) {
    std::transform(host.begin(),host.end(),host.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    return host==root || (host.size()>root.size() && host.compare(host.size()-root.size()-1,root.size()+1,"."+root)==0);
}
struct Platform {
    virtual ~Platform() = default;
    virtual void preflight()=0;
    virtual void prepare()=0;
    virtual Json capture()=0;
    virtual void save(const Json&)=0;
    virtual void apply(const Json&)=0;
    virtual void restore(const Json&)=0;
    virtual void trial(const Profile&)=0;
    virtual bool check(const std::vector<std::string>&,bool,const std::atomic<bool>&)=0;
    virtual void commit()=0;
    virtual void remove_service()=0;
    virtual void clear()=0;
};
class Installer {
    Platform& p; Events e;
    bool confirm(const std::vector<std::string>& hosts,bool ipv6,const std::atomic<bool>& cancel) {
        for(int pass=1;pass<=2;++pass) {
            cancellation(cancel); e.log("Doğrulama " + std::to_string(pass) + "/2");
            if(!p.check(hosts,ipv6,cancel)) return false;
        }
        return true;
    }
public:
    Installer(Platform& platform,Events events) : p(platform),e(std::move(events)) {}
    void recover(const Json& state) {
        if(!state.is_object() || state.value("Schema",0)!=1 || !state.contains("Dns") || !state["Dns"].is_object())
            throw std::runtime_error("Geçerli DNS yedeği bulunamadı.");
        e.stage(Stage::Restore,"Önceki ayarlar geri yükleniyor",0,0);
        std::string errors;
        try { p.remove_service(); } catch(const std::exception& ex) { errors += ex.what(); }
        try { p.restore(state["Dns"]); } catch(const std::exception& ex) { errors += std::string(" ")+ex.what(); }
        if(!errors.empty()) throw std::runtime_error(errors + " Yedek korunuyor; geri almayı yeniden deneyin.");
        p.clear(); e.log("Hizmet kaldırıldı. Önceki DNS ayarları geri yüklendi.");
    }
    std::string install(const std::vector<std::string>& hosts,const std::atomic<bool>& cancel) {
        p.preflight(); cancellation(cancel);
        e.stage(Stage::Prepare,"Paket ve bağlantı hazırlanıyor",0,0); p.prepare();
        Json state = {{"Schema",1},{"Status","Pending"},{"Dns",p.capture()},{"Targets",hosts},{"Profile",""}};
        p.save(state);
        try {
            cancellation(cancel); e.stage(Stage::Dns,"Cloudflare DNS ayarlanıyor",0,0); p.apply(state["Dns"]);
            e.log("DNS yedeği kaydedildi. Cloudflare uygulandı.");
            const bool ipv6=state["Dns"].value("Ipv6",false);
            for(size_t i=0;i<profiles.size();++i) {
                cancellation(cancel); const auto& profile=profiles[i];
                e.stage(Stage::Scan,profile.name + " deneniyor",static_cast<int>(i+1),static_cast<int>(profiles.size()));
                e.log("Profil " + std::to_string(i+1) + "/" + std::to_string(profiles.size()) + " · " + profile.name);
                state["Profile"]=profile.args; p.save(state); p.trial(profile); cancellation(cancel);
                if(!confirm(hosts,ipv6,cancel)) continue;
                e.stage(Stage::Save,"Hizmet yeniden başlatılıp doğrulanıyor",static_cast<int>(i+1),static_cast<int>(profiles.size()));
                p.commit();
                if(!confirm(hosts,ipv6,cancel)) { e.log("Son kontrol geçmedi. Sonraki profil deneniyor."); continue; }
                cancellation(cancel); state["Status"]="Installed"; p.save(state);
                return profile.name + " profili hazır. Windows açıldığında otomatik başlayacak.";
            }
            throw std::runtime_error("Türkiye paketindeki yedi profilin hiçbiri bütün hedefleri açamadı.");
        } catch(...) {
            const auto original=std::current_exception();
            try { recover(state); }
            catch(const std::exception& ex) { throw std::runtime_error(std::string("İşlem tamamlanamadı. Geri alma: ")+ex.what()); }
            std::rethrow_exception(original);
        }
    }
};
}
