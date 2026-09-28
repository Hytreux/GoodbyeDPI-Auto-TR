#include "platform.hpp"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include <d3d11.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <commdlg.h>
#include <shellapi.h>
#include <cmath>
#include <mutex>
#include <thread>
#include <sstream>
#include <ctime>

using namespace gd;
namespace fs=std::filesystem;
static HWND window_handle=nullptr;
static ID3D11Device* device=nullptr;
static ID3D11DeviceContext* gpu_context=nullptr;
static IDXGISwapChain* swapchain=nullptr;
static ID3D11RenderTargetView* render_target=nullptr;
static UINT resize_width=0,resize_height=0;
static bool want_close=false;

struct LogLine {std::string time,text;};
struct Model {
    bool busy=false,installed=false,recovery=false,dns_only=false,service_active=false;
    Stage stage=Stage::Ready;
    std::string headline="Bağlantına uygun ayarları otomatik bul.",note;
    std::string profile_name;
    int profile=0;
    std::vector<std::string> hosts={"discord.com","pornhub.com"};
    std::vector<Result> results;
    std::vector<LogLine> logs;
};
class Controller {
    std::mutex mutex;
    Model data;
    std::thread worker;
    std::atomic<bool> cancelled{false};
public:
    bool preview=false;
    ~Controller(){cancelled=true;if(worker.joinable())worker.join();}
    Model get(){std::lock_guard<std::mutex> lock(mutex);return data;}
    void preview_installed(){std::lock_guard<std::mutex> lock(mutex);data.installed=true;data.service_active=true;data.stage=Stage::Complete;data.profile=1;data.profile_name="Türkiye ana profil";for(auto& item:data.results){item.reach=Reach::Passed;item.detail="IPv4 · HTTPS 200";item.http=200;item.milliseconds=84;}}
    void log(const std::string& message){
        SYSTEMTIME time;GetLocalTime(&time);char stamp[16];sprintf_s(stamp,"%02u:%02u:%02u",time.wHour,time.wMinute,time.wSecond);
        std::lock_guard<std::mutex> lock(mutex);data.logs.push_back({stamp,message});if(data.logs.size()>2000)data.logs.erase(data.logs.begin());
    }
    void set_stage(Stage stage,const std::string& title,int index,int){std::lock_guard<std::mutex> lock(mutex);data.stage=stage;data.headline=title;if(index)data.profile=index;}
    void result(const Result& result){std::lock_guard<std::mutex> lock(mutex);auto it=std::find_if(data.results.begin(),data.results.end(),[&](const auto& r){return r.host==result.host;});if(it!=data.results.end())*it=result;else data.results.push_back(result);}
    Events events(){Events out;out.log=[this](auto& line){log(line);};out.stage=[this](auto s,auto& t,int i,int n){set_stage(s,t,i,n);};out.result=[this](auto& r){result(r);};return out;}
    void set_targets(std::vector<std::string> hosts){
        std::lock_guard<std::mutex> lock(mutex);if(data.busy)return;data.hosts=std::move(hosts);data.results.clear();
        for(const auto& host:data.hosts)data.results.push_back({host,"Test bekleniyor"});
        if(std::find(data.hosts.begin(),data.hosts.end(),"www.microsoft.com")==data.hosts.end())data.results.push_back({"www.microsoft.com","Normal bağlantı kontrolü"});
    }
    void refresh(){
        if(preview)return;
        try{
            auto saved=read_state();const bool installed=!saved.is_null()&&saved.value("Status",std::string())=="Installed";const bool running=installed&&service_running();std::lock_guard<std::mutex> lock(mutex);
            data.installed=installed;data.service_active=running;data.profile_name.clear();data.profile=0;
            data.recovery=!saved.is_null()&&!data.installed;
            data.dns_only=!saved.is_null()&&(saved.value("Profile",std::string())=="DNS only"||saved.value("Profile",std::string())=="Yalnızca Cloudflare DNS");
            if(data.installed&&!data.dns_only){const auto chosen=saved.value("Profile",std::string());for(size_t i=0;i<profiles.size();++i)if(profiles[i].args==chosen){data.profile=static_cast<int>(i+1);data.profile_name=profiles[i].name;break;}}
            if(!data.busy&&!saved.is_null()&&saved.contains("Targets")&&saved["Targets"].is_array()){
                std::string input;for(const auto& host:saved["Targets"])input+=host.get<std::string>()+"\n";
                data.hosts=parse_targets(input);data.results.clear();for(auto& host:data.hosts)data.results.push_back({host,"Kayıtlı hedef; henüz yeniden test edilmedi"});
                if(std::find(data.hosts.begin(),data.hosts.end(),"www.microsoft.com")==data.hosts.end())data.results.push_back({"www.microsoft.com","Normal bağlantı kontrolü"});
            }
        }catch(const std::exception& ex){std::lock_guard<std::mutex> lock(mutex);data.recovery=true;data.stage=Stage::Error;data.note=ex.what();}
    }
    void cancel(){cancelled=true;log("İptal istendi. Ayarlar geri alınacak.");}
    std::string report(){auto copy=get();std::string out="GoodbyeDPI Auto v0.3.1 — C++ / Dear ImGui\r\nTuğrul tarafından geliştirildi.\r\nHTTPS erişim testi; tam uygulama işlev testi değildir.\r\n\r\n";for(auto& line:copy.logs)out+=line.time+"  "+line.text+"\r\n";return out;}
    void start(bool restore){
        auto initial=get();if(initial.busy||preview)return;
        if(!is_admin()){
            std::string hosts;for(auto& h:initial.hosts)hosts+=h+"\n";
            std::wstring params=restore?L"--restore":L"--install "+wide(base64(hosts.data(),hosts.size()));
            SHELLEXECUTEINFOW execute={sizeof(execute)};execute.fMask=SEE_MASK_NOCLOSEPROCESS;execute.hwnd=window_handle;execute.lpVerb=L"runas";std::wstring path=exe_path();execute.lpFile=path.c_str();execute.lpParameters=params.c_str();execute.nShow=SW_SHOWNORMAL;
            if(ShellExecuteExW(&execute)){if(execute.hProcess)CloseHandle(execute.hProcess);want_close=true;}
            else{log("Yönetici izni verilmedi. Ayarlar değiştirilmedi.");std::lock_guard<std::mutex> lock(mutex);data.note="Başlatmak için Windows yönetici izni gerekiyor.";}
            return;
        }
        if(worker.joinable())worker.join();cancelled=false;
        set_targets(initial.hosts);
        {std::lock_guard<std::mutex> lock(mutex);data.busy=true;data.stage=restore?Stage::Restore:Stage::Prepare;data.profile=0;data.note.clear();data.headline=restore?"Önceki ayarlar geri yükleniyor":"Kurulum hazırlanıyor";}
        worker=std::thread([this,restore,hosts=initial.hosts]{
            HANDLE gate=CreateMutexW(nullptr,FALSE,L"Global\\GoodbyeDPIAuto.Setup");bool acquired=false;
            try{
                if(!gate)throw std::runtime_error("Kurulum kilidi açılamadı.");DWORD status=WaitForSingleObject(gate,0);acquired=status==WAIT_OBJECT_0||status==WAIT_ABANDONED;
                if(!acquired)throw std::runtime_error("Başka bir kurulum veya geri alma işlemi sürüyor.");
                WindowsPlatform platform(events());Installer engine(platform,events());std::string message;
                if(restore){engine.recover(read_state());message="Önceki ayarların geri yüklendi.";}
                else message=engine.install(hosts,cancelled);
                log(message);{std::lock_guard<std::mutex> lock(mutex);data.stage=restore?Stage::Ready:Stage::Complete;data.headline=message;data.note=message;}
            }catch(const Cancelled&){log("İptal tamamlandı. Önceki ayarlar geri yüklendi.");std::lock_guard<std::mutex> lock(mutex);data.stage=Stage::Ready;data.note="İptal edildi. Önceki ayarlarına dönüldü.";}
            catch(const std::exception& ex){log(ex.what());std::lock_guard<std::mutex> lock(mutex);data.stage=Stage::Error;data.headline="İşlem tamamlanamadı";data.note=ex.what();}
            if(acquired)ReleaseMutex(gate);if(gate)CloseHandle(gate);refresh();
            try{if(fs::exists(state_root()))export_text(state_root()/L"last-run-native.log",report());}catch(...){}
            {std::lock_guard<std::mutex> lock(mutex);data.busy=false;}
        });
    }
};
static Controller controller;

namespace ui {
static float scale=1,offset_x=0,offset_y=0;
static ImDrawList* draw=nullptr;
static ImFont *fontSmall=nullptr,*body=nullptr,*medium=nullptr,*title=nullptr,*hero=nullptr,*brand=nullptr;
static int tab=0;
static bool help_open=false;
static float help_anim=0,nav_indicator_y=180;
static char new_host[256]="";
static std::string target_error;
static ImU32 bg=IM_COL32(7,15,20,255),side=IM_COL32(10,22,28,255),panel=IM_COL32(15,29,36,255),border=IM_COL32(33,55,64,255);
static ImU32 ink=IM_COL32(240,248,246,255),muted=IM_COL32(143,170,176,255),dim=IM_COL32(82,111,120,255),mint=IM_COL32(100,230,190,255),warn=IM_COL32(242,184,103,255),red=IM_COL32(246,132,137,255),blue=IM_COL32(103,185,229,255);
static ImU32 fade(ImU32 color,float amount){amount=std::clamp(amount,0.f,1.f);return (color&~IM_COL32_A_MASK)|(static_cast<ImU32>(((color>>IM_COL32_A_SHIFT)&0xff)*amount)<<IM_COL32_A_SHIFT);}
static ImVec2 point(float x,float y){return {offset_x+x*scale,offset_y+y*scale};}
static void text(float x,float y,const std::string& value,ImFont* font=nullptr,ImU32 color=0,float wrap=0){if(!font)font=body;draw->AddText(font,font->FontSize*scale,point(x,y),color?color:ink,value.c_str(),nullptr,wrap*scale);}
static void card(float x,float y,float w,float h,ImU32 fill=0,float rounding=12){draw->AddRectFilled(point(x+2,y+6),point(x+w+2,y+h+6),IM_COL32(0,0,0,42),rounding*scale);draw->AddRectFilled(point(x,y),point(x+w,y+h),fill?fill:panel,rounding*scale);draw->AddRect(point(x,y),point(x+w,y+h),border,rounding*scale);draw->AddLine(point(x+rounding,y+1),point(x+w-rounding,y+1),IM_COL32(71,105,109,26),scale);}
static void line(float x,float y,float xx,float yy,ImU32 color=0,float weight=1){draw->AddLine(point(x,y),point(xx,yy),color?color:border,weight*scale);}
static void dot(float x,float y,ImU32 color,float r=3){draw->AddCircleFilled(point(x,y),r*scale,color);}
static void label(float x,float y,const std::string& value){text(x,y,value,fontSmall,muted);}
static void icon(float x,float y,int kind,ImU32 color,float size=20){
    float a=size/20;auto q=[&](float u,float v){return point(x+u*a,y+v*a);};float t=1.65f*scale;
    if(kind==0){draw->AddCircle(q(10,10),8*a*scale,color,32,t);draw->AddEllipse(q(10,10),{4*a*scale,8*a*scale},color,0,32,t);draw->AddLine(q(2,10),q(18,10),color,t);}
    if(kind==1){for(int i=0;i<3;++i){draw->AddCircle(q(4,4.f+i*6.f),1.6f*a*scale,color,16,t);draw->AddLine(q(9,4.f+i*6.f),q(18,4.f+i*6.f),color,t);}}
    if(kind==2){draw->AddRect(q(3,2),q(17,18),color,2*scale,0,t);for(int i=0;i<3;++i)draw->AddLine(q(6,6.f+i*4.f),q(14,6.f+i*4.f),color,t);}
    if(kind==3){draw->AddCircle(q(10,10),8*a*scale,color,32,t);draw->AddLine(q(10,8),q(10,14),color,t);draw->AddCircleFilled(q(10,5),1*a*scale,color);}
    if(kind==4){draw->AddLine(q(3,10),q(16,10),color,t);draw->AddLine(q(11,5),q(16,10),color,t);draw->AddLine(q(11,15),q(16,10),color,t);}
    if(kind==5){draw->AddLine(q(4,10),q(8,14),color,t);draw->AddLine(q(8,14),q(16,5),color,t);}
}
static bool button(const char* id,const std::string& caption,float x,float y,float w,float h,bool primary=false,bool enabled=true){
    if(primary&&enabled)draw->AddRectFilled(point(x+2,y+5),point(x+w+2,y+h+5),IM_COL32(18,113,88,55),8*scale);
    ImGui::SetCursorScreenPos(point(x,y));ImGui::PushID(id);ImGui::PushFont(body);
    ImGui::PushStyleColor(ImGuiCol_Button,ImGui::ColorConvertU32ToFloat4(primary?mint:IM_COL32(30,42,49,255)));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImGui::ColorConvertU32ToFloat4(primary?IM_COL32(137,243,211,255):IM_COL32(41,59,67,255)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImGui::ColorConvertU32ToFloat4(primary?IM_COL32(86,205,171,255):IM_COL32(48,68,74,255)));
    ImGui::PushStyleColor(ImGuiCol_Text,ImGui::ColorConvertU32ToFloat4(primary?IM_COL32(10,38,31,255):ink));
    ImGui::BeginDisabled(!enabled);bool pressed=ImGui::Button(caption.c_str(),{w*scale,h*scale});ImGui::EndDisabled();
    ImGui::PopStyleColor(4);ImGui::PopFont();ImGui::PopID();return pressed;
}
static bool hit(const char* id,float x,float y,float w,float h){ImGui::SetCursorScreenPos(point(x,y));ImGui::PushID(id);bool pressed=ImGui::InvisibleButton("##hit",{w*scale,h*scale});ImGui::PopID();return pressed;}
static void orbit(float x,float y,const Model& m){
    const float time=static_cast<float>(ImGui::GetTime()*(m.busy?.72:.16));const ImU32 glow=m.stage==Stage::Error?red:m.service_active?mint:blue;
    for(int r=112;r>=48;r-=8){float opacity=(112-r)/64.f;draw->AddCircleFilled(point(x,y),r*scale,fade(glow,.007f+opacity*.006f),72);}
    for(int r:{66,96})draw->AddCircle(point(x,y),r*scale,fade(glow,.20f),80,scale);
    for(int i=0;i<2;++i){float start=time*(i?-.7f:1.f)+i*2.f;draw->PathArcTo(point(x,y),(82.f+i*15)*scale,start,start+1.15f,28);draw->PathStroke(fade(glow,.72f-i*.18f),0,(2.f-i*.5f)*scale);}
    draw->AddCircleFilled(point(x,y),46*scale,fade(glow,.10f),64);draw->AddCircleFilled(point(x,y),34*scale,IM_COL32(21,55,52,255),64);draw->AddCircle(point(x,y),34*scale,fade(glow,.65f),64,1.5f*scale);
    if(m.service_active)icon(x-14,y-14,5,mint,28);else icon(x-14,y-14,0,glow,28);
    for(int i=0;i<3;++i){float angle=time+i*2.094f;float radius=i==1?96.f:66.f;float xx=x+std::cos(angle)*radius,yy=y+std::sin(angle)*radius;
        draw->AddCircleFilled(point(xx,yy),10*scale,IM_COL32(17,39,42,255),24);draw->AddCircle(point(xx,yy),10*scale,fade(glow,.55f),24,scale);dot(xx,yy,glow,3);}
}
static void sidebar(const Model& m){
    draw->AddRectFilled(point(0,0),point(210,760),side);draw->AddCircleFilled(point(50,28),105*scale,IM_COL32(51,181,148,11),72);line(210,0,210,760,IM_COL32(38,65,73,255));
    draw->AddRectFilled(point(22,56),point(64,98),mint,13*scale);draw->AddCircleFilled(point(41,75),24*scale,IM_COL32(255,255,255,15),40);icon(32,66,0,IM_COL32(8,48,37,255),22);
    text(76,55,"goodbye",brand);text(77,84,"DPI · AUTO",fontSmall,muted);text(77,102,"BY TUĞRUL",fontSmall,fade(mint,.82f));
    label(24,144,"ANA MENÜ");
    const char* names[]={"Bağlantı","Hedefler","İşlem kaydı"};
    const float target_y=180.f+tab*54.f;nav_indicator_y+=(target_y-nav_indicator_y)*std::min(1.f,ImGui::GetIO().DeltaTime*13.f);
    draw->AddRectFilled(point(16,nav_indicator_y),point(194,nav_indicator_y+44),IM_COL32(29,51,48,255),8*scale);
    draw->AddRectFilled(point(16,nav_indicator_y+11),point(19,nav_indicator_y+33),mint,2*scale);
    for(int i=0;i<3;++i){float y=180.f+i*54;
        ImGui::SetCursorScreenPos(point(16,y));ImGui::PushID(i);if(ImGui::InvisibleButton("##nav",{178*scale,44*scale}))tab=i;bool hovered=ImGui::IsItemHovered();ImGui::PopID();
        if(hovered&&tab!=i)draw->AddRectFilled(point(16,y),point(194,y+44),IM_COL32(25,37,42,255),8*scale);
        icon(30,y+12,i,tab==i?mint:hovered?ink:muted,19);text(62,y+12,names[i],body,tab==i?mint:hovered?ink:muted);
    }
    card(18,574,174,110,m.service_active?IM_COL32(14,39,36,255):IM_COL32(16,31,38,255),12);icon(32,591,m.service_active?5:3,m.service_active?mint:blue,19);
    text(58,589,m.service_active?"Bağlantı hazır":m.busy?"Bağlantı ayarlanıyor":"Kontrol sende",body);
    text(32,620,m.service_active?"Otomatik hizmet çalışıyor.\nAyarların güvenle kayıtlı.":"Değişikliklerden önce yedek\nalınır; dilediğinde dönersin.",fontSmall,muted);
    ImGui::SetCursorScreenPos(point(20,700));ImGui::PushID("help-card");bool help_pressed=ImGui::InvisibleButton("##help",{170*scale,40*scale});bool help_hovered=ImGui::IsItemHovered();ImGui::PopID();
    card(20,700,170,40,help_hovered?IM_COL32(31,49,51,255):IM_COL32(22,34,40,255),9);icon(32,710,3,help_hovered?mint:muted,18);text(58,710,"Kullanım ve bilgiler",fontSmall,help_hovered?ink:muted);icon(163,711,4,help_hovered?mint:dim,16);
    if(help_pressed)help_open=true;
    (void)m;
}
static void header(const Model& m){
    const char* titles[]={"Bağlantı merkezi","Test hedefleri","İşlem kaydı"};
    const char* subs[]={"Bağlantına uygun ayarlar. Tek bir yerden.","Erişimini doğrulayacağımız siteleri seç.","Her adımı ve sonucu buradan takip et."};
    label(238,49,"GOODBYE DPI  ·  AKILLI BAĞLANTI");text(236,75,titles[tab],title);text(238,117,subs[tab],body,muted);
    std::string badge=m.busy?"Bağlantı ayarlanıyor":m.recovery?"Geri alma bekliyor":m.installed?(m.service_active?"Hizmet çalışıyor":"Hizmet durmuş"):"Kuruluma hazır";
    ImU32 color=m.busy?blue:m.recovery||m.installed&&!m.service_active?warn:m.installed?mint:muted;float pill_w=m.busy?174.f:m.installed?151.f:145.f;
    draw->AddRectFilled(point(1058-pill_w,47),point(1058,80),IM_COL32(16,31,38,245),17*scale);draw->AddRect(point(1058-pill_w,47),point(1058,80),fade(color,.35f),17*scale);
    if(m.busy){float pulse=.5f+.5f*std::sin(static_cast<float>(ImGui::GetTime()*4));draw->AddCircleFilled(point(1077-pill_w,63),(7+5*pulse)*scale,fade(color,.13f*(1-pulse)));}
    dot(1077-pill_w,63,color,3.5f);text(1089-pill_w,54,badge,fontSmall,color);
}
static void target_row(const Result& r,float x,float y,float width,bool control=false){
    ImU32 color=r.reach==Reach::Passed?mint:r.reach==Reach::Failed?red:r.reach==Reach::Testing?warn:dim;
    draw->AddRectFilled(point(x,y+1),point(x+30,y+31),IM_COL32(29,42,49,255),8*scale);icon(x+6,y+7,0,r.reach==Reach::Waiting?muted:color,18);
    text(x+42,y,r.host,body);text(x+42,y+21,control?"Normal bağlantı kontrolü":r.reach==Reach::Waiting?"HTTPS erişim testi":r.detail,fontSmall,muted,260);
    std::string status=r.reach==Reach::Passed?"Doğrulandı":r.reach==Reach::Failed?"Yanıt yok":r.reach==Reach::Testing?"Deneniyor":"Bekliyor";
    float right=x+width-99;dot(right,y+10,color);text(right+11,y+1,status,fontSmall,color);
    if(r.reach==Reach::Passed&&r.milliseconds>0)text(right+11,y+21,std::to_string(r.milliseconds)+" ms",fontSmall,dim);
}
static void home(const Model& m){
    const ImU32 hero_fill=m.stage==Stage::Error?IM_COL32(37,29,31,255):m.installed?IM_COL32(14,42,39,255):IM_COL32(14,34,39,255);card(236,154,836,226,hero_fill,18);
    draw->AddRectFilled(point(237,170),point(240,364),m.stage==Stage::Error?red:m.installed?mint:blue,2*scale);draw->AddCircleFilled(point(915,198),118*scale,m.installed?IM_COL32(68,225,178,8):IM_COL32(78,174,225,8),72);orbit(944,267,m);
    std::string badge=m.busy?"BAĞLANTI AYARLANIYOR":m.recovery?"GÜVENLİ GERİ ALMA":m.installed?(m.service_active?"KORUMA ETKİN":"HİZMET KONTROLÜ GEREKİYOR"):m.stage==Stage::Error?"İŞLEM TAMAMLANAMADI":"AKILLI KURULUM";
    ImU32 state_color=m.stage==Stage::Error?warn:m.installed&&m.service_active?mint:m.busy?blue:m.recovery?warn:mint;float badge_w=m.installed&&!m.service_active?229.f:m.busy?194.f:158.f;
    draw->AddRectFilled(point(260,178),point(260+badge_w,204),fade(state_color,.14f),7*scale);draw->AddRect(point(260,178),point(260+badge_w,204),fade(state_color,.22f),7*scale);dot(273,191,state_color,2.5f);text(283,183,badge,fontSmall,state_color);
    const char* heading=m.busy?"Bağlantın hazırlanıyor":m.recovery?"Ayarlarını güvenle geri al":m.installed?(m.service_active?"Bağlantın hazır":"Hizmeti yeniden kur"):m.stage==Stage::Error?"Birlikte tekrar deneyelim":"Daha rahat bir bağlantı";
    text(258,218,heading,hero);
    std::string desc=m.busy?m.headline:m.recovery?"Yarım kalan işlem güvenli yedekte.\nTek dokunuşla önceki ayarlarına dön.":m.installed?(m.service_active?"Seçilen Türkiye profili arka planda çalışıyor.\nBilgisayar açıldığında otomatik olarak hazır olacak.":"Kayıtlı hizmet şu an çalışmıyor.\nKurulumu kaldırıp yeniden başlatabilirsin."):m.stage==Stage::Error?"Ayarların geri alındı; bağlantın güvende.\nKaydı inceleyebilir veya yeniden deneyebilirsin.":"Cloudflare DNS ve bağlantına özel profil seçimi.\nYedekleme, deneme ve kurulum tek akışta.";
    text(260,267,desc,body,muted,510);
    if(m.busy){if(button("cancel","Güvenle iptal et",260,327,238,38))controller.cancel();}
    else if(m.recovery){if(button("restore","Ayarlarımı geri al",260,327,238,38,true))controller.start(true);}
    else if(m.installed){if(button("openlogs","İşlem kaydını aç",260,327,238,38,true))tab=2;if(button("restore","Kurulumu kaldır",512,327,180,38))controller.start(true);}
    else if(m.stage==Stage::Error){if(button("retry","Yeniden dene",260,327,238,38,true))controller.start(false);if(button("herohelp","Kaydı incele",512,327,180,38))tab=2;}
    else{if(button("install","Akıllı kurulumu başlat",260,327,238,38,true))controller.start(false);if(button("herohelp","Nasıl çalışır?",512,327,180,38))help_open=true;}
    struct Info{float x;const char* label;std::string value,sub;int glyph;};
    const std::string profile_value=!m.profile_name.empty()?m.profile_name:(m.profile?std::to_string(m.profile)+" / 7":"7 akıllı profil");
    Info infos[]={{236,"DNS TEMELİ","Cloudflare","1.1.1.1  ·  1.0.0.1",0},{520,m.installed?"SEÇİLEN PROFİL":"TÜRKİYE PROFİLLERİ",profile_value,m.busy?"En iyi ayar aranıyor":m.installed?"Otomatik seçildi":"Ana profil + 6 alternatif",1},{804,"OTOMATİK BAŞLANGIÇ",m.service_active?"Etkin":m.installed?"Kontrol gerekli":"Kurulumdan sonra",m.service_active?"Windows ile hazır":"Hizmet olarak kaydedilir",2}};
    for(const auto& info:infos){card(info.x,400,268,82,IM_COL32(14,27,34,255),13);label(info.x+18,412,info.label);text(info.x+18,433,info.value,medium);text(info.x+18,459,info.sub,fontSmall,muted);draw->AddCircleFilled(point(info.x+237,441),18*scale,IM_COL32(27,56,55,255),32);icon(info.x+226,430,info.glyph,mint,22);}
    card(236,502,480,199,IM_COL32(13,26,33,255),15);text(256,520,"Erişim hedefleri",medium);text(256,545,"Kurulumun doğruladığı bağlantılar",fontSmall,muted);if(button("edit","Düzenle",614,515,82,30))tab=1;
    for(size_t i=0;i<std::min<size_t>(3,m.results.size());++i){target_row(m.results[i],256,571+static_cast<float>(i)*43,438,m.results[i].host=="www.microsoft.com");if(i<2)line(296,609+static_cast<float>(i)*43,693,609+static_cast<float>(i)*43,IM_COL32(28,48,56,255));}
    card(736,502,336,199,IM_COL32(13,27,33,255),15);text(756,520,m.installed?"Hazır ve güvende":"Kurulum yolculuğu",medium);text(756,545,m.installed?"Üç adım da tamamlandı":"Her adım geri alınabilir",fontSmall,muted);
    int active=m.stage==Stage::Prepare||m.stage==Stage::Dns?0:m.stage==Stage::Scan?1:m.stage==Stage::Save?2:m.installed?3:-1;
    const char* steps[]={"DNS ayarlarını yedekle","Çalışan profili bul","Başlangıca kaydet"};
    for(int i=0;i<3;++i){float y=579.f+i*37;bool complete=active>i;ImU32 c=complete||active==i?mint:dim;draw->AddCircleFilled(point(767,y+9),11*scale,complete?IM_COL32(31,72,59,255):IM_COL32(25,42,49,255),24);if(complete)icon(759,y+1,5,mint,16);else{text(763,y,std::to_string(i+1),fontSmall,c);}if(i<2)line(767,y+22,767,y+27,fade(c,.45f));text(790,y,steps[i],body,active==i||complete?ink:muted);}
    if(!m.note.empty()&&!m.busy){std::string note=m.note;auto pos=note.find('\n');if(pos!=std::string::npos)note=note.substr(0,pos);if(note.size()>110)note=note.substr(0,107)+"...";draw->AddRectFilled(point(236,711),point(1072,739),IM_COL32(14,28,34,230),8*scale);dot(251,725,m.stage==Stage::Error?warn:mint,3);text(262,716,note,fontSmall,m.stage==Stage::Error?warn:muted,790);}
}
static void targets_page(const Model& m){
    card(236,154,520,548,IM_COL32(13,27,34,255),16);text(260,179,"Doğrulama hedefleri",medium);text(260,207,"Bağlantının gerçekten açıldığını birlikte kontrol ederiz.",fontSmall,muted);
    draw->AddRectFilled(point(638,174),point(731,204),IM_COL32(20,45,45,255),15*scale);dot(653,189,mint,3);text(664,179,std::to_string(m.hosts.size())+" / 6 hedef",fontSmall,mint);
    for(size_t i=0;i<m.hosts.size();++i){float y=255.f+static_cast<float>(i)*54;draw->AddRectFilled(point(258,y),point(732,y+44),IM_COL32(11,23,29,255),10*scale);draw->AddRect(point(258,y),point(732,y+44),IM_COL32(29,49,57,255),10*scale);draw->AddCircleFilled(point(281,y+22),14*scale,IM_COL32(23,53,50,255),24);icon(272,y+13,0,mint,18);text(306,y+12,m.hosts[i],body);
        if(button(("del"+std::to_string(i)).c_str(),"×",684,y+6,34,31,false,!m.busy&&m.hosts.size()>1)){auto copy=m.hosts;copy.erase(copy.begin()+i);controller.set_targets(copy);target_error.clear();}}
    float input_y=255.f+static_cast<float>(m.hosts.size())*54;
    if(m.hosts.size()<6){ImGui::SetCursorScreenPos(point(260,input_y+6));ImGui::SetNextItemWidth(366*scale);ImGui::BeginDisabled(m.busy);ImGui::PushFont(body);ImGui::InputTextWithHint("##domain","ornek.com",new_host,sizeof(new_host));ImGui::PopFont();ImGui::EndDisabled();
        if(button("addtarget","Ekle",640,input_y+3,91,34,true,!m.busy)){try{auto parsed=parse_targets(new_host);std::string combined;for(auto& h:m.hosts)combined+=h+"\n";for(auto& h:parsed)combined+=h+"\n";controller.set_targets(parse_targets(combined));new_host[0]=0;target_error.clear();}catch(const std::exception& ex){target_error=ex.what();}}}
    draw->AddRectFilled(point(258,637),point(734,674),target_error.empty()?IM_COL32(16,34,39,255):IM_COL32(47,35,30,255),9*scale);icon(270,646,target_error.empty()?5:3,target_error.empty()?mint:warn,18);text(298,646,target_error.empty()?"Değişiklikler bir sonraki taramada kullanılır.":target_error,fontSmall,target_error.empty()?muted:warn,420);
    card(776,154,296,211,IM_COL32(14,31,37,255),16);draw->AddCircleFilled(point(812,195),24*scale,IM_COL32(24,59,53,255),36);icon(801,184,3,mint,22);text(846,181,"Neyi ölçüyoruz?",medium);
    text(798,230,"Geçerli sertifikayla alınan HTTPS yanıtını ve güvenli yönlendirmeleri kontrol ediyoruz.",body,muted,248);
    card(776,383,296,144,IM_COL32(14,28,35,255),14);label(798,405,"BAĞLANTI REFERANSI");text(798,435,"www.microsoft.com",medium);text(798,474,"Her profil için internetin genel durumu ayrıca doğrulanır.",fontSmall,muted,248);
    draw->AddRectFilled(point(776,544),point(1072,626),IM_COL32(31,28,25,255),12*scale);draw->AddRect(point(776,544),point(1072,626),IM_COL32(70,57,43,255),12*scale);icon(794,561,3,warn,18);text(824,558,"Küçük bir not",body,warn);text(794,586,"HTTPS testi, Discord ses gibi uygulama içi özellikleri tek başına kanıtlamaz.",fontSmall,muted,254);
    if(button("targetback","Bağlantı merkezine dön",796,651,255,38,true))tab=0;
}
static void save_report(){wchar_t path[32768]=L"GoodbyeDPI-Auto-rapor.txt";OPENFILENAMEW ofn={sizeof(ofn)};ofn.hwndOwner=window_handle;ofn.lpstrFilter=L"Metin raporu\0*.txt\0\0";ofn.lpstrFile=path;ofn.nMaxFile=32768;ofn.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;ofn.lpstrDefExt=L"txt";if(GetSaveFileNameW(&ofn)){try{export_text(path,controller.report());controller.log("Rapor seçilen dosyaya kaydedildi.");}catch(const std::exception& ex){controller.log(ex.what());}}}
static void logs_page(const Model& m){
    card(236,154,836,547,IM_COL32(12,26,33,255),16);text(258,179,"Canlı işlem günlüğü",medium);text(258,207,"Her adım cihazında, anlaşılır ve zaman damgalı.",fontSmall,muted);
    draw->AddRectFilled(point(493,177),point(493.f+(m.busy?106.f:82.f),207),IM_COL32(19,42,43,255),15*scale);dot(510,192,m.busy?blue:m.installed?mint:dim,3);text(521,182,m.busy?"Canlı":"Hazır",fontSmall,m.busy?blue:m.installed?mint:muted);
    if(button("copy","Kopyala",794,177,107,33))ImGui::SetClipboardText(controller.report().c_str());
    if(button("export","Dışa aktar",915,177,135,33,true))save_report();
    line(258,237,1050,237,IM_COL32(35,58,66,255));
    if(m.logs.empty()){draw->AddCircleFilled(point(654,382),36*scale,IM_COL32(28,43,48,255),40);icon(638,366,2,mint,32);text(542,438,"Henüz bir işlem yok.",medium);text(457,476,"Kurulum başladığında her adım burada görünecek.",body,muted);}
    else {
        ImGui::SetCursorScreenPos(point(254,253));ImGui::BeginChild("logscroll",{800*scale,420*scale},ImGuiChildFlags_None,ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushFont(body);
        for(const auto& entry:m.logs){ImGui::PushStyleColor(ImGuiCol_Text,ImGui::ColorConvertU32ToFloat4(dim));ImGui::TextUnformatted(entry.time.c_str());ImGui::PopStyleColor();ImGui::SameLine(91*scale);ImGui::PushTextWrapPos(776*scale);ImGui::TextUnformatted(entry.text.c_str());ImGui::PopTextWrapPos();ImGui::Dummy({0,6*scale});}
        if(m.busy&&ImGui::GetScrollY()>=ImGui::GetScrollMaxY()-70*scale)ImGui::SetScrollHereY(1);
        ImGui::PopFont();ImGui::EndChild();
    }
    text(238,716,"Raporlar bu cihazda kalır. Otomatik olarak paylaşılmaz.",fontSmall,muted);
}
static void help(){
    const float wanted=help_open?1.f:0.f;
    help_anim+=(wanted-help_anim)*std::min(1.f,ImGui::GetIO().DeltaTime*11.f);
    if(!help_open&&help_anim<.012f){help_anim=0;return;}
    const float a=1.f-std::pow(1.f-help_anim,3.f),x=305.f+(1.f-a)*32.f,y=74.f+(1.f-a)*10.f,w=680.f,h=612.f;
    if(ImGui::IsKeyPressed(ImGuiKey_Escape))help_open=false;
    draw->AddRectFilled(point(0,0),point(1100,760),IM_COL32(4,8,11,static_cast<int>(188*a)));
    draw->AddRectFilled(point(x+9,y+13),point(x+w+9,y+h+13),IM_COL32(0,0,0,static_cast<int>(80*a)),18*scale);
    draw->AddRectFilled(point(x,y),point(x+w,y+h),fade(IM_COL32(18,27,33,255),a),18*scale);
    draw->AddRect(point(x,y),point(x+w,y+h),fade(IM_COL32(49,69,76,255),a),18*scale,0,scale);
    draw->AddRectFilled(point(x+28,y+26),point(x+151,y+51),fade(IM_COL32(32,66,56,255),a),7*scale);
    text(x+40,y+31,"v0.3.1  ·  WINDOWS x64",fontSmall,fade(mint,a));
    text(x+28,y+70,"Kurulum nasıl çalışır?",title,fade(ink,a));
    text(x+28,y+108,"Tek uygulama, bağlantına göre seçilen kalıcı Türkiye profili.",body,fade(muted,a));
    const auto step=[&](float sy,const char* number,const char* heading,const char* detail,int glyph){
        draw->AddRectFilled(point(x+28,sy),point(x+w-28,sy+70),fade(IM_COL32(22,34,40,255),a),11*scale);
        draw->AddRect(point(x+28,sy),point(x+w-28,sy+70),fade(border,a),11*scale,0,scale);
        draw->AddCircleFilled(point(x+57,sy+35),17*scale,fade(IM_COL32(35,72,61,255),a),32);text(x+52,sy+25,number,medium,fade(mint,a));
        text(x+89,sy+13,heading,medium,fade(ink,a));text(x+89,sy+40,detail,fontSmall,fade(muted,a),475);
        icon(x+w-63,sy+25,glyph,fade(mint,a),20);
    };
    step(y+146,"1","DNS yedeği ve Cloudflare","Etkin bağlantının ayarları korunur; 1.1.1.1 ve 1.0.0.1 uygulanır.",0);
    step(y+228,"2","Türkiye profillerini sırayla dene","Ana profil, ardından Alternative 1–6; Discord ve seçili hedeflerle iki tur test.",1);
    step(y+310,"3","Çalışan profili kalıcı kur","Kazanan ayar Windows hizmetine alınır, yeniden başlatılır ve tekrar doğrulanır.",2);
    draw->AddRectFilled(point(x+28,y+405),point(x+329,y+518),fade(IM_COL32(20,39,38,255),a),12*scale);draw->AddRect(point(x+28,y+405),point(x+329,y+518),fade(IM_COL32(42,77,68,255),a),12*scale,0,scale);
    icon(x+48,y+427,5,fade(mint,a),22);text(x+82,y+423,"Güvenli geri alma",medium,fade(ink,a));text(x+48,y+461,"İptal veya hatada DNS ve hizmet değişiklikleri geri alınır. Tamamlanmazsa yedek korunur.",fontSmall,fade(muted,a),252);
    draw->AddRectFilled(point(x+341,y+405),point(x+w-28,y+518),fade(IM_COL32(25,34,41,255),a),12*scale);draw->AddRect(point(x+341,y+405),point(x+w-28,y+518),fade(border,a),12*scale,0,scale);
    icon(x+361,y+427,3,fade(warn,a),22);text(x+395,y+423,"Testin kapsamı",medium,fade(ink,a));text(x+361,y+461,"HTTPS erişimi doğrulanır. Discord ses ve görüntü gibi uygulama içi özellikler ayrıca sınanmalıdır.",fontSmall,fade(muted,a),252);
    line(x+28,y+545,x+w-28,y+545,fade(border,a));text(x+28,y+558,"GoodbyeDPI Auto",fontSmall,fade(ink,a));text(x+28,y+577,"Tuğrul tarafından geliştirildi  ·  MIT Lisansı",fontSmall,fade(mint,a));
    bool close=hit("help-close",x+w-55,y+24,32,32);bool close_hovered=ImGui::IsItemHovered();draw->AddCircleFilled(point(x+w-39,y+40),16*scale,fade(close_hovered?IM_COL32(48,67,73,255):IM_COL32(29,43,49,255),a),32);text(x+w-44,y+28,"×",medium,fade(close_hovered?ink:muted,a));
    bool done=hit("help-done",x+w-145,y+558,117,36);bool done_hovered=ImGui::IsItemHovered();draw->AddRectFilled(point(x+w-145,y+558),point(x+w-28,y+594),fade(done_hovered?IM_COL32(138,243,211,255):mint,a),8*scale);text(x+w-113,y+567,"Anladım",body,fade(IM_COL32(9,39,30,255),a));
    if(close||done)help_open=false;
}
static void draw_app(){
    auto& io=ImGui::GetIO();scale=std::min(io.DisplaySize.x/1100.f,io.DisplaySize.y/760.f);offset_x=(io.DisplaySize.x-1100*scale)*.5f;offset_y=(io.DisplaySize.y-760*scale)*.5f;io.FontGlobalScale=scale;
    ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize(io.DisplaySize);ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,0);
    ImGui::Begin("GoodbyeDPI Auto",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBringToFrontOnFocus);
    draw=ImGui::GetWindowDrawList();draw->AddRectFilled({0,0},io.DisplaySize,bg);draw->AddCircleFilled(point(1015,92),190*scale,IM_COL32(54,164,143,9),96);draw->AddCircleFilled(point(750,760),260*scale,IM_COL32(62,127,174,6),96);draw->AddLine(point(210,0),point(1100,0),IM_COL32(89,226,186,35),scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{10*scale,8*scale});ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,7*scale);ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{8*scale,8*scale});
    auto m=controller.get();const bool help_blocks=help_open||help_anim>.012f;ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha,1.f);ImGui::BeginDisabled(help_blocks);
    sidebar(m);header(m);if(tab==0)home(m);else if(tab==1)targets_page(m);else logs_page(m);
    if(tab==0&&m.note.empty())text(238,722,"Test raporları cihazında kalır.",fontSmall,dim);
    text(800,742,"GoodbyeDPI Auto by Tuğrul  ·  v0.3.1",fontSmall,dim);
    ImGui::SetCursorScreenPos({io.DisplaySize.x-88,8});ImGui::PushStyleColor(ImGuiCol_Button,{0,0,0,0});if(ImGui::Button("—##min",{34,23}))ShowWindow(window_handle,SW_MINIMIZE);ImGui::SameLine();if(ImGui::Button("×##close",{34,23})){if(m.busy)controller.cancel();else want_close=true;}ImGui::PopStyleColor();
    ImGui::EndDisabled();ImGui::PopStyleVar();help();ImGui::PopStyleVar(3);ImGui::End();ImGui::PopStyleVar(2);
}
static void fonts(){
    auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    wchar_t windows[MAX_PATH];GetWindowsDirectoryW(windows,MAX_PATH);const auto normal=utf8((fs::path(windows)/L"Fonts"/L"segoeui.ttf").wstring());const auto bold=utf8((fs::path(windows)/L"Fonts"/L"segoeuib.ttf").wstring());
    static const ImWchar ranges[]={0x0020,0x024f,0x2000,0x206f,0};
    auto load=[&](const std::string& path,float size){ImFontConfig config;config.OversampleH=2;config.OversampleV=2;ImFont* f=io.Fonts->AddFontFromFileTTF(path.c_str(),size,&config,ranges);return f?f:io.Fonts->AddFontDefault();};
    body=load(normal,16);fontSmall=load(normal,13);medium=load(bold,18);title=load(bold,30);hero=load(bold,32);brand=load(bold,23);io.FontDefault=body;
    auto& s=ImGui::GetStyle();ImGui::StyleColorsDark();s.WindowRounding=12;s.WindowPadding={20,20};s.PopupRounding=10;s.FrameRounding=7;s.ChildRounding=8;s.ScrollbarSize=9;s.ScrollbarRounding=8;
    s.Colors[ImGuiCol_WindowBg]=ImGui::ColorConvertU32ToFloat4(panel);s.Colors[ImGuiCol_PopupBg]=s.Colors[ImGuiCol_WindowBg];s.Colors[ImGuiCol_Text]=ImGui::ColorConvertU32ToFloat4(ink);s.Colors[ImGuiCol_FrameBg]=ImGui::ColorConvertU32ToFloat4(IM_COL32(13,22,27,255));s.Colors[ImGuiCol_Border]=ImGui::ColorConvertU32ToFloat4(border);s.Colors[ImGuiCol_CheckMark]=ImGui::ColorConvertU32ToFloat4(mint);s.Colors[ImGuiCol_ScrollbarGrab]=ImGui::ColorConvertU32ToFloat4(IM_COL32(57,78,85,255));s.Colors[ImGuiCol_NavCursor]=ImGui::ColorConvertU32ToFloat4(mint);
}
}

static void create_target(){ID3D11Texture2D* back=nullptr;if(SUCCEEDED(swapchain->GetBuffer(0,IID_PPV_ARGS(&back)))){device->CreateRenderTargetView(back,nullptr,&render_target);back->Release();}}
static void free_target(){if(render_target){render_target->Release();render_target=nullptr;}}
static bool create_device(HWND window){DXGI_SWAP_CHAIN_DESC desc={};desc.BufferCount=2;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.OutputWindow=window;desc.SampleDesc.Count=1;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_0},got;
    HRESULT status=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,2,D3D11_SDK_VERSION,&desc,&swapchain,&device,&got,&gpu_context);
    if(FAILED(status))status=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,levels,2,D3D11_SDK_VERSION,&desc,&swapchain,&device,&got,&gpu_context);
    if(FAILED(status))return false;create_target();return render_target!=nullptr;}
static bool save_frame(const fs::path& path){
    ID3D11Texture2D *back=nullptr,*staging=nullptr;IWICImagingFactory* factory=nullptr;IWICStream* stream=nullptr;IWICBitmapEncoder* encoder=nullptr;IWICBitmapFrameEncode* frame=nullptr;
    bool ok=false;D3D11_MAPPED_SUBRESOURCE mapped={};bool is_mapped=false;
    do{
        if(FAILED(swapchain->GetBuffer(0,IID_PPV_ARGS(&back))))break;D3D11_TEXTURE2D_DESC desc;back->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
        if(FAILED(device->CreateTexture2D(&desc,nullptr,&staging)))break;gpu_context->CopyResource(staging,back);
        if(FAILED(gpu_context->Map(staging,0,D3D11_MAP_READ,0,&mapped)))break;is_mapped=true;
        if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))))break;
        if(FAILED(factory->CreateStream(&stream))||FAILED(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE)))break;
        if(FAILED(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder))||FAILED(encoder->Initialize(stream,WICBitmapEncoderNoCache)))break;
        if(FAILED(encoder->CreateNewFrame(&frame,nullptr))||FAILED(frame->Initialize(nullptr))||FAILED(frame->SetSize(desc.Width,desc.Height)))break;
        WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;if(FAILED(frame->SetPixelFormat(&format)))break;
        std::vector<BYTE> pixels(static_cast<size_t>(desc.Width)*desc.Height*4);
        for(UINT y=0;y<desc.Height;++y){auto src=static_cast<const BYTE*>(mapped.pData)+y*mapped.RowPitch;auto dst=pixels.data()+static_cast<size_t>(y)*desc.Width*4;for(UINT x=0;x<desc.Width;++x){dst[x*4]=src[x*4+2];dst[x*4+1]=src[x*4+1];dst[x*4+2]=src[x*4];dst[x*4+3]=255;}}
        if(FAILED(frame->WritePixels(desc.Height,desc.Width*4,static_cast<UINT>(pixels.size()),pixels.data()))||FAILED(frame->Commit())||FAILED(encoder->Commit()))break;ok=true;
    }while(false);
    if(is_mapped)gpu_context->Unmap(staging,0);if(frame)frame->Release();if(encoder)encoder->Release();if(stream)stream->Release();if(factory)factory->Release();if(staging)staging->Release();if(back)back->Release();return ok;
}
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);
static LRESULT WINAPI wndproc(HWND h,UINT message,WPARAM w,LPARAM l){
    if(ImGui_ImplWin32_WndProcHandler(h,message,w,l))return true;
    switch(message){
    case WM_NCCALCSIZE:if(w)return 0;break;
    case WM_NCHITTEST:{POINT p={static_cast<short>(LOWORD(l)),static_cast<short>(HIWORD(l))};ScreenToClient(h,&p);RECT r;GetClientRect(h,&r);bool left=p.x<6,right=p.x>r.right-6,top=p.y<6,bottom=p.y>r.bottom-6;
        if(top&&left)return HTTOPLEFT;if(top&&right)return HTTOPRIGHT;if(bottom&&left)return HTBOTTOMLEFT;if(bottom&&right)return HTBOTTOMRIGHT;if(left)return HTLEFT;if(right)return HTRIGHT;if(top)return HTTOP;if(bottom)return HTBOTTOM;if(p.y<38&&p.x<r.right-100)return HTCAPTION;break;}
    case WM_GETMINMAXINFO:{auto info=reinterpret_cast<MINMAXINFO*>(l);info->ptMinTrackSize={880,608};return 0;}
    case WM_SIZE:if(w!=SIZE_MINIMIZED){resize_width=LOWORD(l);resize_height=HIWORD(l);}return 0;
    case WM_SYSCOMMAND:if((w&0xfff0)==SC_KEYMENU)return 0;break;
    case WM_CLOSE:if(controller.get().busy)controller.cancel();else want_close=true;return 0;
    case WM_DESTROY:PostQuitMessage(0);return 0;
    }return DefWindowProcW(h,message,w,l);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int){
    int count=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&count);std::vector<std::wstring> args;for(int i=1;i<count;++i)args.emplace_back(argv[i]);LocalFree(argv);
    if(args.size()==2&&args[0]==L"--self-test")return self_test(args[1]);
    if(args.size()==2&&args[0]==L"--snapshot-test"){
        try{WindowsPlatform platform(Events{});auto dns=platform.capture();if(!dns.contains("AdapterGuid")||!dns.contains("V4Servers"))throw std::runtime_error("DNS yedeği eksik döndü.");export_text(args[1],"PASS: Etkin ağ kartı DNS yedeği salt okunur olarak alındı.\r\nPASS: Boş yedek kodlaması Windows 87 hatası vermedi.\r\nNo DNS/service/driver changes performed.\r\n");return 0;}
        catch(const std::exception& ex){const std::string message=ex.what();const bool no_route=message.find("Etkin IPv4 bağlantısı bulunamadı")!=std::string::npos;try{export_text(args[1],(no_route?"PASS: Boş yedek kodlaması Windows 87 hatası vermedi.\r\nSKIP: Etkin ağ kartı yedeği — canlı IPv4 varsayılan rotası yok.\r\n":std::string("FAIL: ")+message+"\r\n")+"No DNS/service/driver changes performed.\r\n");}catch(...){}return no_route?0:1;}
    }
    bool ui_test=args.size()==2&&args[0]==L"--ui-test";
    bool preview=args.size()>=2&&(args[0]==L"--preview"||ui_test);controller.preview=preview;
    std::wstring preview_path=preview?args[1]:L"";const bool installed_preview=preview&&args.size()>2&&args[2]==L"installed";if(preview&&args.size()>2){ui::tab=args[2]==L"targets"?1:args[2]==L"logs"?2:0;if(args[2]==L"help"){ui::help_open=true;ui::help_anim=1;}}
    controller.set_targets({"discord.com","pornhub.com"});if(installed_preview)controller.preview_installed();controller.refresh();
    bool auto_install=!args.empty()&&args[0]==L"--install",auto_restore=!args.empty()&&args[0]==L"--restore";
    if(auto_install){try{if(args.size()!=2)throw std::runtime_error("Hedef listesi eksik.");controller.set_targets(parse_targets(unbase64(args[1])));}catch(const std::exception& ex){MessageBoxW(nullptr,wide(ex.what()).c_str(),L"GoodbyeDPI Auto",MB_ICONERROR);return 1;}}
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc={sizeof(wc),CS_CLASSDC,wndproc,0,0,instance,LoadIconW(instance,MAKEINTRESOURCEW(1)),LoadCursorW(nullptr,IDC_ARROW),nullptr,nullptr,L"GoodbyeDPINative",nullptr};RegisterClassExW(&wc);
    RECT work;SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);int width=1100,height=760;if(work.right-work.left<width)width=work.right-work.left-20;if(work.bottom-work.top<height)height=work.bottom-work.top-20;
    if(preview){width=1100;height=760;}
    window_handle=CreateWindowExW(0,wc.lpszClassName,L"GoodbyeDPI Auto",WS_POPUP|WS_THICKFRAME|WS_SYSMENU|WS_MINIMIZEBOX,(work.right-width)/2,(work.bottom-height)/2,width,height,nullptr,nullptr,instance,nullptr);
    BOOL dark=TRUE;DwmSetWindowAttribute(window_handle,20,&dark,sizeof(dark));DWORD corners=2;DwmSetWindowAttribute(window_handle,33,&corners,sizeof(corners));
    if(!create_device(window_handle)){MessageBoxW(nullptr,L"DirectX 11 başlatılamadı.",L"GoodbyeDPI Auto",MB_ICONERROR);return 1;}
    if(!preview){ShowWindow(window_handle,SW_SHOW);UpdateWindow(window_handle);}
    IMGUI_CHECKVERSION();ImGui::CreateContext();ui::fonts();ImGui_ImplWin32_Init(window_handle);ImGui_ImplDX11_Init(device,gpu_context);
    int frames=0,exit_code=0;
    while(!want_close){MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);if(message.message==WM_QUIT)want_close=true;}if(want_close)break;
        if(resize_width&&resize_height){free_target();if(FAILED(swapchain->ResizeBuffers(0,resize_width,resize_height,DXGI_FORMAT_UNKNOWN,0))){exit_code=1;break;}resize_width=resize_height=0;create_target();}
        if(!preview&&IsIconic(window_handle)){Sleep(60);continue;}
        ImGui_ImplDX11_NewFrame();ImGui_ImplWin32_NewFrame();
        if(ui_test){
            auto& input=ImGui::GetIO();input.AddFocusEvent(true);
            auto click=[&](float x,float y,bool down){input.AddMousePosEvent(x,y);input.AddMouseButtonEvent(0,down);};
            if(frames==2||frames==3)click(80,255,frames==2);
            if(frames==5||frames==6)click(400,383,frames==5);
            if(frames==7)input.AddInputCharactersUTF8("example.com");
            if(frames==8||frames==9)click(682,382,frames==8);
            if(frames==11||frames==12)click(704,384,frames==11);
            if(frames==14||frames==15)click(80,308,frames==14);
            if(frames==17||frames==18)click(80,202,frames==17);
            if(frames==21||frames==22)click(100,720,frames==21);
            if(frames==25||frames==26)click(946,114,frames==25);
        }
        ImGui::NewFrame();ui::draw_app();ImGui::Render();
        const float clear[]={.05f,.07f,.09f,1};gpu_context->OMSetRenderTargets(1,&render_target,nullptr);gpu_context->ClearRenderTargetView(render_target,clear);ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if(ui_test){
            bool good=true;std::string check;
            if(frames==4){good=ui::tab==1;check="Targets navigation";}
            if(frames==10){good=controller.get().hosts.size()==3&&controller.get().hosts.back()=="example.com";check="Domain entry and add";}
            if(frames==13){good=controller.get().hosts.size()==2;check="Domain removal";}
            if(frames==16){good=ui::tab==2;check="Log navigation";}
            if(frames==19){good=ui::tab==0;check="Home navigation";}
            if(frames==23){good=ui::help_open;check="Animated help panel opens";}
            if(frames==27){good=!ui::help_open;check="Animated help panel closes";}
            if(!check.empty())controller.log(std::string(good?"PASS: ":"FAIL: ")+check);
            if(!good){exit_code=1;export_text(args[1],controller.report());save_frame(fs::path(args[1]).replace_extension(L".png"));break;}
            if(frames==28){export_text(args[1],controller.report()+"\r\n7/7 offline ImGui interaction checks passed. No installation performed.\r\n");break;}
        }
        ++frames;if(preview&&!ui_test&&frames==4){exit_code=save_frame(preview_path)?0:1;break;}
        HRESULT shown=swapchain->Present(1,0);if(shown==DXGI_STATUS_OCCLUDED)Sleep(40);
        if(frames==3&&!preview&&(auto_install||auto_restore))controller.start(auto_restore);
    }
    ImGui_ImplDX11_Shutdown();ImGui_ImplWin32_Shutdown();ImGui::DestroyContext();free_target();if(swapchain)swapchain->Release();if(gpu_context)gpu_context->Release();if(device)device->Release();DestroyWindow(window_handle);UnregisterClassW(wc.lpszClassName,instance);CoUninitialize();return exit_code;
}
