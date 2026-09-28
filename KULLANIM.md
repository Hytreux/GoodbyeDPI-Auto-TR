# GoodbyeDPI Auto v0.3.1

GoodbyeDPI Auto, Windows 10/11 x64 için C++ ve Dear ImGui ile hazırlanmış tek dosyalık bir bağlantı ayarlayıcısıdır. Etkin ağ bağlantısının DNS ayarlarını yedekler, Cloudflare DNS'i uygular, seçilen sitelerde HTTPS erişimini sınar ve çalışan GoodbyeDPI profilini Windows hizmeti olarak kaydeder.

## Kullanım

1. `GoodbyeDPI-Auto.exe` dosyasını açın.
2. **Hedefler** ekranında test edilecek alan adlarını düzenleyin. Başlangıçta `discord.com` ve `pornhub.com` bulunur; en fazla altı hedef eklenebilir.
3. **Otomatik kurulumu başlat** düğmesine basın ve Windows yönetici iznini onaylayın.
4. Tarama bitene kadar aynı Wi-Fi veya Ethernet bağlantısında kalın.
5. Sonuçları ana ekranda, ayrıntıları **İşlem kaydı** ekranında izleyin. Raporu metin dosyası olarak dışa aktarabilirsiniz.

Ana ekran kurulumdan sonra çalışan hizmeti, otomatik seçilen profili ve tamamlanan adımları tek bakışta gösterir. **İşlem kaydını aç** güvenli ana eylemdir; **Kurulumu kaldır** ikincil düğmede tutulur. Sol menüdeki durum kartı hizmetin çalışıp çalışmadığını bildirir.

Uygulama açılırken Windows yönetici izni ister. Yalnızca açmak DNS veya hizmet ayarlarını değiştirmez; değişiklikler **Otomatik kurulumu başlat** ya da **Eski ayarlara dön** seçildiğinde yapılır. Kurulum sırasında internetten program indirilmez; GoodbyeDPI ve WinDivert dosyaları uygulamanın içinde bulunur. Dağıtılan tek kullanıcı dosyası `GoodbyeDPI-Auto.exe` olur. Windows hizmetinin yeniden başlatmalardan sonra çalışabilmesi için gömülü motor, DLL ve sürücü kurulum sırasında korumalı `%ProgramFiles%` klasörüne çıkarılır.

## İşleyiş

- Etkin fiziksel ağ kartını ve IPv4 varsayılan rotasını belirler. IPv4 ile IPv6 farklı kartlardan çıkıyorsa veya etkin bağlantı VPN/sanal kart ise işlemi durdurur.
- IPv4 ve kullanılabilir IPv6 DNS ayarlarını, otomatik/statik ayrımını koruyarak `%ProgramData%\GoodbyeDPI Auto\state.json` dosyasına kaydeder.
- Cloudflare adreslerini uygular: IPv4 için `1.1.1.1`, `1.0.0.1`; IPv6 için `2606:4700:4700::1111`, `2606:4700:4700::1001`.
- `goodbyedpi-0.2.3rc3-turkey` paketindeki yedi hizmet profilini değişmeden ve şu sırayla dener: ana profil, Alternative 1, Alternative 2, Alternative 3, Alternative 4, Alternative 5 ve Alternative 6.
- Her profil için seçili hedeflerin ve `www.microsoft.com` normal bağlantı kontrolünün iki ardışık turu geçmesini ister. İlk profil geçmezse ikinciye, o da geçmezse sıradaki alternatife ilerler.
- Başarılı profili otomatik başlayan Windows hizmetine çevirir, hizmeti yeniden başlatır ve iki tur daha doğrular.
- İptal veya başarısızlıkta kendi hizmetini kaldırıp eski DNS ayarlarını geri yüklemeye çalışır. Geri alma tamamlanamazsa kurtarma yedeğini silmez.

Başka bir GoodbyeDPI hizmeti veya çalışan `goodbyedpi`, `winws` ya da `winws2` süreci varsa uygulama mevcut kuruluma dokunmadan durur. Ağ bağlantısı test sırasında değişirse işlem iptal edilip geri alınır.

### Kullanılan profil ayarları

1. Ana profil: `-5 --set-ttl 5` ve paketin DNS yönlendirme parametreleri
2. Alternative 1: `--set-ttl 3`
3. Alternative 2: `-5`
4. Alternative 3: `--set-ttl 3` ve paketin DNS yönlendirme parametreleri
5. Alternative 4: `-5` ve paketin DNS yönlendirme parametreleri
6. Alternative 5: `-9` ve paketin DNS yönlendirme parametreleri
7. Alternative 6: `-9`

Buradaki DNS yönlendirme parametreleri, Türkiye paketindeki `77.88.8.8:1253` ve `2a02:6b8::feed:0ff:1253` değerleridir. Sistem DNS'i önce Cloudflare olarak ayarlanır; ilgili GoodbyeDPI profilinde bu yönlendirme parametreleri varsa motor paketle aynı biçimde kullanır.

## Erişim testi neyi kanıtlar?

Uygulama Windows'un `curl` bileşeniyle, sistem DNS'ini ve HTTP/1.1'i kullanarak yeni HTTPS bağlantıları açar. Geçerli TLS sertifikasıyla alınan HTTP 2xx yanıtları başarıdır. Yalnızca aynı alan adına veya onun alt alanlarına yapılan HTTPS yönlendirmeleri izlenir. HTTP'ye, başka bir alana veya farklı bir porta yönlendirme başarı sayılmaz. 403, 429, sertifika hatası, zaman aşımı ve sunucu hatası başarısız/belirsiz kabul edilir.

Bir HTTPS yanıtı; sayfadaki bütün içeriklerin, video sunucularının, Discord girişinin, WebSocket bağlantılarının veya sesli görüşmenin çalıştığını kanıtlamaz. Tarayıcının bağımsız DNS, proxy, HTTP/3 ya da TLS ayarları farklı sonuç verebilir. Site bot koruması gerçek erişimi yanlışlıkla başarısız gösterebilir.

Bu sürüm standart Cloudflare DNS kullanır; DNS-over-HTTPS kurmaz. Operatör Cloudflare DNS trafiğini engelliyor ya da DNS yanıtlarına müdahale ediyorsa uygun profil bulunamayabilir. Doğrudan IP engelleri ve bütün operatörler için çalışma garantisi yoktur.

## Geri alma

Ana ekrandaki **Eski ayarlara dön**, bu uygulamanın hizmetini kaldırır ve kurulum öncesindeki DNS ayarlarını geri yükler. Yeni bir bağlantıda profil aramadan önce mevcut kurulumu geri alın.

Bilgisayar zorla kapanırsa uygulamayı yeniden açın ve **Eski ayarlara dön** seçeneğini kullanın. DNS geri alınana kadar `state.json` dosyasını silmeyin. Ağ kartı sökülmüşse kartı yeniden bağlayıp işlemi tekrarlayın. Geri alma, kurulumdan sonra elle değiştirilmiş DNS ayarlarının yerine kurulum öncesindeki yedeği uygular.

- Motor: `%ProgramFiles%\GoodbyeDPI Auto\engine`
- Kurtarma yedeği: `%ProgramData%\GoodbyeDPI Auto\state.json`
- Son günlük: `%ProgramData%\GoodbyeDPI Auto\last-run-native.log`

## Doğrulama ve kaynaklar

Bu sürüm MSVC ile yerel x64 uygulama olarak derlenmiştir; .NET çalışma zamanı gerektirmez. Arayüz Dear ImGui ve DirectX 11 kullanır. Uygulama kod imzalı değildir; Windows ilk açılışta uyarı gösterebilir.

Geliştirme ortamında 30/30 yalıtılmış C++ mantık/paket testi, 9/9 taklit ağ kartı DNS testi ve 7/7 çevrimdışı ImGui etkileşim testi geçmiştir. Hatanın görüldüğü boş DNS yedeği kodlama yolu gerçek Windows API'siyle ayrıca doğrulanmıştır. IPv6 varsayılan rotası bulunmayan bağlantıda IPv6 DNS'e dokunulmadığı ve eksik hizmet sonucunun komut hazırlanırken kaybolmadığı ayrıca sınanmıştır. Ana ekran ve özel kullanım paneli görüntü olarak incelenmiştir. Bu kontroller gerçek DNS'i değiştirmedi, GoodbyeDPI hizmetini/sürücüsünü başlatmadı ve engelli sitelere istek göndermedi. Gerçek operatör hattındaki uçtan uca davranış saha testi gerektirir.

- [GoodbyeDPI 0.2.3rc3](https://github.com/ValdikSS/GoodbyeDPI/releases/tag/0.2.3rc3)
- [Dear ImGui 1.91.9b](https://github.com/ocornut/imgui/releases/tag/v1.91.9b)
- [WinDivert 2.2 kaynakları](https://github.com/basil00/WinDivert/tree/v2.2.0)
- [Cloudflare DNS adresleri](https://developers.cloudflare.com/1.1.1.1/ip-addresses/)

`GoodbyeDPI-Auto-Kaynak.zip` uygulamanın C++ kaynaklarını, derleme dosyasını, testleri, sabitlenmiş bağımlılıkları, orijinal motor dosyalarını ve lisans bildirimlerini içerir.
