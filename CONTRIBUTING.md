# Katkıda bulunma

Katkılarınızı issue veya pull request ile gönderebilirsiniz.

1. Değişikliğin kapsamını küçük ve açıklanabilir tutun.
2. Ağ, DNS ve hizmet işlemlerinde başarısızlık halinde geri alma yolunu koruyun.
3. Yeni profil ekliyorsanız kaynak paketi, tam parametreleri ve deneme sırasını belgeleyin.
4. Kod gizleme, paketleyici, güvenlik ürünü atlatma veya doğrulanamayan ikili dosya eklemeyin.
5. `build.ps1` ile derleyip yalıtılmış testleri çalıştırın.

```powershell
.\build.ps1
Start-Process .\dist\GoodbyeDPI-Auto.exe -ArgumentList '--self-test self-test.txt' -Wait
powershell.exe -NoProfile -File .\assets\Test-Dns.ps1
```

Pull request açıklamasında değişen davranışı, geri alma etkisini ve çalıştırdığınız kontrolleri belirtin.
