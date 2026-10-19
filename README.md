# SDrive Cluster

Foydalanuvchi kompyuterlaridagi bo'sh disk joyini yagona taqsimlangan xotira klasteriga birlashtiruvchi tizim.
Server PC fayllarni qabul qiladi, chunk'larga bo'ladi va [shartnoma](SHARTNOMA.md) 2.3-bandiga ko'ra
klasterdagi kompyuterlarning **30%** iga shifrlangan holda nusxalaydi. Foydalanuvchilar tasdiqlangan
onlayn vaqt va saqlagan ma'lumot hajmi uchun daromad oladi.

```
                    ┌────────────────────────── Server PC (Go) ──────────────────────────┐
  Admin brauzer ───►│ veb-panel  │  REST API  │ replikatsiya menejeri │ audit │ scrub │ db.json │
                    └──────▲────────────────────────▲───────────────────────────▲───────────┘
                           │ register/heartbeat     │ chunk yuklab olish        │ restore (PUT)
                           │ audit javoblari        │ (SHA-256 tekshiruvi)      │
              ┌────────────┴──────┐      ┌──────────┴────────┐      ┌───────────┴───────┐
              │  Mijoz (C++)      │      │  Mijoz (C++)      │ ...  │  Mijoz (C++)      │
              │  127.0.0.1:48673  │      │  ChaCha20+HMAC    │      │  S:\ (Windows)    │
              └───────────────────┘      └───────────────────┘      └───────────────────┘
```

## Imkoniyatlar

| Shartnoma bandi | Amalga oshirilishi |
|---|---|
| 1.2, 9.3 — o'rnatish, fon rejimi, rozilik | Mijoz brauzerda sozlash oynasini ochadi: disk/papka, hajm, server, keyin shartnoma. Rozilikdan so'ng avtostart o'rnatiladi (Windows `HKCU\…\Run`, macOS LaunchAgent, Linux XDG autostart). Roziliksiz server tugunni qabul qilmaydi (HTTP 412). |
| 2.1 — disk va hajm | Windows'da tanlangan harf (`S:` va h.k.) papkaga bog'lanadi (`DefineDosDevice`, administrator huquqisiz) va har ishga tushishda tiklanadi. Bo'sh joy tekshiriladi, kvota oshirilmaydi. |
| 2.2, 8 — shifrlash | Har chunk ChaCha20 (256-bit) bilan shifrlanadi va HMAC-SHA256 bilan himoyalanadi. Kalit faqat foydalanuvchi kompyuterida. |
| 2.3 — 30% replikatsiya | `ceil(0.3 × faol tugunlar)`: 10 → 3, 100 → 30, 1000 → 300. Klaster o'sganda nusxalar soni ham oshadi, tugun qaytganda ortiqchasi olib tashlanadi. |
| 2.4 — yuklamani kamaytirish | Foydalanuvchi kompyuterdan faol foydalanayotganda (oxirgi 60 s ichida klaviatura/sichqoncha) yuklab olish sekinlashtiriladi. |
| 3.2, 3.3 — o'chirilgan fayl | Mijoz o'chirilgan/buzilgan chunk'larni topadi, server qayta yuklash vazifasini beradi; qayta yuklanguncha to'lov to'xtaydi. |
| 4, 5.3 — to'lov va tasdiqlash | Daromad faqat **saqlash isboti** (audit: `sha256(nonce ‖ chunk)`) bilan tasdiqlangan fayllar uchun. Audit o'tmasa fayl hisobdan chiqadi va qayta yuklanadi. |
| 5.1, 5.2 — uzilishlar | Heartbeat kelmagan vaqt (internet/elektr) hisoblanmaydi. Uzoq uzilishda nusxalar boshqa tugunlarga ko'chiriladi. |
| 6 — payout va uninstall | Payout oyning oxirgi 3 kunida. Pul olinmasdan o'chirilsa — balans "forfeited" deb yoziladi, klaster fayllari va avtostart o'chiriladi. |

Qo'shimcha:
- **O'z-o'zini tiklash**: server diskidagi chunk yo'qolsa yoki buzilsa (SHA-256 scrub), u tugundagi nusxadan tiklanadi.
- **Xavfsizlik**: tugunlar uchun Bearer token, admin uchun token + HttpOnly/SameSite sessiya va CSRF himoyasi,
  veb-panelda CSP va XSS'dan himoya, lokal UI faqat `127.0.0.1` da (Host/Origin tekshiruvi, maxsus sarlavha).
- **Ishonchlilik**: `db.json` atomik yoziladi (fsync + rename), server to'xtatilganda holat saqlanadi.
- **Simulyator**: admin paneldan 10–100 ta virtual tugun qo'shish, ularni "uzish" va o'z-o'zini tiklashni kuzatish.
  Virtual tugunlar haqiqiy mijoz bilan bir xil HTTP protokolida ishlaydi.

## Tez boshlash

### 1. Server

Go 1.22+ kerak.

```bash
cd backend
go run . -addr :8080
# yoki: go build -o sdrive-server . && ./sdrive-server -addr :8080 -data /var/lib/sdrive
```

Birinchi ishga tushishda admin token yaratiladi va logda ko'rsatiladi (`data/admin_token` faylida saqlanadi).
Veb-panel: `http://localhost:8080` → istalgan admin bo'limi → token bilan kirish.

HTTPS uchun: `-tls-cert cert.pem -tls-key key.pem` (yoki server oldiga nginx/Caddy qo'ying).

| Flag | Standart | Ma'nosi |
|---|---|---|
| `-addr` | `:8080` | tinglash manzili |
| `-data` | `data` | `db.json`, chunk'lar, admin token |
| `-admin-token` | `$SDRIVE_ADMIN_TOKEN` | admin token (bo'sh bo'lsa yaratiladi) |
| `-tls-cert`, `-tls-key` | — | HTTPS |
| `-heartbeat` | `30s` | mijoz heartbeat oralig'i |
| `-offline-timeout` | `90s` | shu vaqt heartbeat kelmasa — uzilish |
| `-repair-delay` | `5m` | oflayn tugundagi nusxa shu vaqtdan keyin boshqa tugunga ko'chiriladi |
| `-audit` | `10m` | har bir tugunni saqlash isboti bilan tekshirish oralig'i |
| `-scrub` | `6h` | server chunk'larini to'liq SHA-256 tekshiruvi |
| `-rate` | `0.05` | 1 GB × 1 soat uchun to'lov (sent) |
| `-max-upload` | 16 GiB | maksimal fayl hajmi |
| `-payout-anytime` | `false` | payout'ni oy oxirini kutmasdan ruxsat berish (test) |
| `-web`, `-downloads`, `-agreement` | avtomatik | veb papka (aks holda binarga joylangan nusxa), mijoz dasturlari, shartnoma |

### 2. Mijoz (build)

```bash
# Linux / macOS: build + unit testlar + paket (.tar.gz/.deb yoki .dmg)
client/build.sh
# Linux'da Windows .exe ham (MinGW kerak: apt install g++-mingw-w64-x86-64-posix)
client/build.sh --windows
```

Windows'da: `client\build.bat` (PATH'da MinGW-w64 `g++`). Paketlar `client/dist/` ga va
`backend/web/client/` ga nusxalanadi — veb-saytdagi "Yuklab olish" tugmalari ularni avtomatik ko'rsatadi.

CMake bilan: `cmake -S client -B build && cmake --build build && ctest --test-dir build`.
HTTPS: Linux/macOS'da OpenSSL (topilsa avtomatik), Windows'da tizimning WinHTTP'i.

### 3. Mijozni ishga tushirish

```bash
sdrive --server http://SERVER:8080
```

Brauzerda `http://127.0.0.1:48673` ochiladi: disk/papka va hajm → shartnoma → ishlash holati
(saqlanayotgan fayllar, balans, tasdiqlangan vaqt, cash chiqarish, dasturni o'chirish).

| Parametr | Ma'nosi |
|---|---|
| `--server URL` | klaster serveri |
| `--ui-port N` | lokal panel porti (standart 48673) |
| `--config-dir DIR` | sozlamalar papkasi (standart: `%APPDATA%\SDrive`, `~/.config/sdrive`, `~/Library/Application Support/SDrive`) |
| `--background` | avtostart rejimi (brauzer ochilmaydi) |
| `--no-browser` | brauzerni ochmaslik |

Bitta kompyuterda faqat bitta nusxa ishlaydi (lokal port band bo'lsa ikkinchisi panelni ochib chiqib ketadi).

## Testlar

```bash
cd backend && go test -race ./...     # server: 20+ integratsion test (replikatsiya, audit, tiklash, to'lov, auth...)
client/build.sh                        # mijoz: SHA-256/HMAC/ChaCha20 rasmiy vektorlari, JSON, HTTP, shifrlangan xotira
scripts/e2e.sh                         # haqiqiy server + 4 ta haqiqiy C++ mijoz (NODES=10 bilan ham)
```

`scripts/e2e.sh` quyidagilarni haqiqiy jarayonlar bilan tekshiradi: lokal UI orqali sozlash va rozilik,
fayl yuklash va 30% replikatsiya, tugun diskida ochiq matn yo'qligi, auditlar, `kill -9` bilan "elektr
uzilishi" va nusxani ko'chirish, tugun qaytganda ortiqcha nusxani olib tashlash, foydalanuvchi o'chirgan faylni
qayta yuklash, server chunk'ini tugundan tiklash va bayt-baybayt solishtirish, payout, uninstall (balans
yo'qotilishi, fayllar va avtostart o'chirilishi), server qayta ishga tushganda holat saqlanishi.

## API

Tugun so'rovlari `Authorization: Bearer <token>` bilan (token `register` javobida bir marta beriladi).

| So'rov | Kim | Vazifasi |
|---|---|---|
| `GET /api/info`, `GET /api/agreement`, `GET /api/downloads` | hamma | klaster statistikasi, shartnoma, mijoz dasturlari |
| `POST /api/nodes/register` | mijoz | ro'yxatdan o'tish (shartnoma versiyasi talab qilinadi) |
| `POST /api/nodes/{id}/heartbeat` | tugun | onlayn vaqt, daromad, vazifalar soni, auditlar |
| `GET /api/nodes/{id}/tasks` | tugun | `replica`, `repair`, `restore`, `delete` vazifalari |
| `GET /api/nodes/{id}/files/{fileId}/chunks/{i}` | tugun | vazifadagi chunk'ni yuklab olish |
| `PUT /api/nodes/{id}/tasks/{taskId}/chunks/{i}` | tugun | server nusxasini tiklash |
| `POST /api/nodes/{id}/tasks/{taskId}/done` · `/fail` | tugun | vazifa natijasi |
| `POST /api/nodes/{id}/audits` | tugun | saqlash isboti javoblari |
| `POST /api/nodes/{id}/repair` | tugun | lokal yo'qolgan fayllar |
| `POST /api/nodes/{id}/payout` · `/uninstall` | tugun | cash chiqarish, dasturni o'chirish |
| `POST /api/admin/login` · `/logout`, `GET /api/admin/me` | admin | sessiya |
| `GET /api/dashboard` | admin | tugunlar, fayllar, vazifalar, to'lovlar |
| `POST /api/files/upload?name=&size=` | admin | fayl yuklash (tana — xom baytlar) |
| `GET /api/files/{id}/download`, `DELETE /api/files/{id}` | admin | yuklab olish (SHA-256 tekshiruvi bilan), o'chirish |
| `POST /api/admin/sim/{start,offline,online,remove}`, `POST /api/admin/reconcile` | admin | simulyator |

## Cheklovlar

- Shartnomada (1.1, 2.5) tilga olingan **GPU/CPU hisoblash klasteri** amalga oshirilmagan — tizim xotira
  (storage) klasteri sifatida ishlaydi. Begona kodni foydalanuvchi kompyuterida bajarish alohida xavfsizlik
  (sandbox) loyihasini talab qiladi.
- **iOS** mijozi yo'q: iOS fon rejimida doimiy xizmat va ixtiyoriy disk joyidan foydalanishga ruxsat bermaydi.
- Windows'dagi `S:` disk — papkaga bog'langan virtual harf (`subst` kabi); hajm cheklovi dastur tomonidan
  ta'minlanadi, alohida formatlangan bo'lim (VHD) yaratilmaydi.
- To'lovlar — hisob-kitob yozuvlari (payout tarixi); haqiqiy pul o'tkazmasi (bank/karta) integratsiyasi yo'q.
- macOS paketi (`.dmg`) faqat macOS'da `client/build.sh` bilan yaratiladi.
