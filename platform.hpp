#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include "core.hpp"

namespace gd {
std::wstring wide(const std::string&);
std::string utf8(const std::wstring&);
std::wstring quote(const std::wstring&);
std::string base64(const void*,size_t);
std::string unbase64(const std::wstring&);
std::wstring exe_path();
bool is_admin();
std::filesystem::path install_root();
std::filesystem::path state_root();
Json read_state();
bool state_exists();
bool service_running();
void export_text(const std::filesystem::path&,const std::string&);
std::string resource(int id);
std::string sha256(const std::string&);
struct RunResult { int code=0; std::string output; };
RunResult run(const std::wstring&,const std::wstring&,DWORD,const std::atomic<bool>* cancel=nullptr);
int self_test(const std::filesystem::path&);
class WindowsPlatform final : public Platform {
    Events e; std::string adapter;
    Json snapshot();
    std::string powershell(const std::string&,const Json& = Json());
    bool probe(const std::string&,bool,const std::atomic<bool>&,Result&);
public:
    explicit WindowsPlatform(Events events): e(std::move(events)) {}
    void preflight() override;
    void prepare() override;
    Json capture() override;
    void save(const Json&) override;
    void apply(const Json&) override;
    void restore(const Json&) override;
    void trial(const Profile&) override;
    bool check(const std::vector<std::string>&,bool,const std::atomic<bool>&) override;
    void commit() override;
    void remove_service() override;
    void clear() override;
};
}
