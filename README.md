# SDrive Cluster

Foydalanuvchi kompyuterlaridagi bo'sh disk joyini va GPU/CPU quvvatini yagona taqsimlangan klasterga
birlashtiruvchi tizim. Server PC (bosh PC) fayllarni qabul qiladi, chunk'larga bo'ladi va
[shartnoma](SHARTNOMA.md) 2.3-bandiga ko'ra klasterdagi kompyuterlarning **30%** iga shifrlangan holda
nusxalaydi; shuningdek butun klaster GPU/CPU quvvatidan foydalanib **AI modellarini** (Gemma va shunga
o'xshash) ishga tushiradi. Foydalanuvchilar tasdiqlangan onlayn vaqt va saqlagan ma'lumot hajmi uchun
daromad oladi.

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

| 1.1, 2.5 — GPU/CPU hisoblash | Tugunlar ixtiyoriy ravishda hisoblash quvvatini ham beradi. Bosh PC OpenAI-mos API beradi va so'rovlarni tugunlarga ularning GPU/CPU/RAM/model imkoniyatiga qarab taqsimlaydi. Pastda **AI hisoblash klasteri** bo'limiga qarang. |

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
| `-compute-keys` | `$SDRIVE_COMPUTE_KEYS` | OpenAI API kalitlari (vergul bilan); bo'sh bo'lsa admin token |
| `-compute-job-timeout` | `10m` | bitta inference ishi uchun vaqt chegarasi |
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

## AI hisoblash klasteri (GPU/CPU)

Bosh PC klasterdagi barcha tugunlarning GPU/CPU quvvatidan foydalanib AI modellarini ishga tushiradi.
Arxitektura — **ishonchli va xavfsiz**:

- Koordinator (bosh PC) tugunlarga faqat **{model, matn, parametr}** turidagi inference ishlarini yuboradi —
  **ixtiyoriy buyruq emas**. Tugun faqat o'zidagi inference dvigatelini (ollama / llama.cpp) ishga tushiradi.
- Taqsimlash "pull" modelida: bo'sh tugun ish so'raydi, band tugun so'ramaydi — yuk tabiiy ravishda teng
  taqsimlanadi. Model marshrutlash: so'rov faqat kerakli modelga ega (yoki uni yuklab oladigan) tugunga boradi.
- Bosh PC **OpenAI-mos API** beradi, shuning uchun istalgan mavjud dastur (Python `openai`, `curl`,
  LangChain, Open WebUI, …) klasterni oddiy model sifatida ishlatadi.

```
   OpenAI mijozi ──► Bosh PC /v1/chat/completions ──► rejalashtiruvchi (GPU/CPU/model bo'yicha)
                                                          │ poll (uzoq)      ▲ token oqimi (SSE)
                                ┌─────────────────────────┼──────────────────┴────────────┐
                        ┌───────┴───────┐        ┌─────────┴────────┐        ┌─────────────┴──┐
                        │ tugun + GPU   │        │ tugun + ollama   │  ...   │ tugun (CPU)    │
                        │ (gemma, llama)│        │ (gemma:2b)       │        │ (qwen2:0.5b)   │
                        └───────────────┘        └──────────────────┘        └────────────────┘
```

### Tugunda AI dvigatelini yoqish

Tugunda inference dvigateli ishlab tursin, so'ng mijozni `--compute` bilan ishga tushiring:

```bash
# ollama (eng oson — modelni o'zi yuklab oladi):
ollama serve &          # va: ollama pull gemma:2b
sdrive --server http://BOSH-PC:8080 --compute --compute-engine ollama

# yoki llama.cpp server:
llama-server -m gemma-2b.gguf --port 8080 &
sdrive --server http://BOSH-PC:8080 --compute --compute-engine llama.cpp --llama-url http://127.0.0.1:8080
```

Muhit o'zgaruvchilari ham ishlaydi: `SDRIVE_COMPUTE=1`, `SDRIVE_COMPUTE_ENGINE=auto|ollama|llama.cpp`,
`SDRIVE_COMPUTE_MODELS=gemma:2b,llama3:8b`, `SDRIVE_COMPUTE_PARALLEL=2`, `SDRIVE_OLLAMA_URL`, `SDRIVE_LLAMA_URL`.
Mijoz GPU (`nvidia-smi` / Apple Metal / ROCm), RAM va yadrolar sonini avtomatik aniqlab serverga xabar qiladi.

| Parametr | Ma'nosi |
|---|---|
| `--compute` / `--no-compute` | hisoblash tuguni sifatida qo'shilish / o'chirish |
| `--compute-engine E` | `auto` (ollama → llama.cpp), `ollama`, `llama.cpp`, `echo` (test) |
| `--compute-models M` | modellarni cheklash yoki `echo` uchun ro'yxat (vergul bilan) |
| `--compute-parallel N` | bir vaqtda nechta inference ishi |

### Bosh PC orqali so'rov yuborish

Veb-paneldagi **🧠 AI Hisoblash** bo'limida tugunlar, modellar va chat ko'rinadi. Tashqi dasturlar uchun:

```bash
curl http://BOSH-PC:8080/v1/chat/completions \
  -H "Authorization: Bearer <ADMIN-TOKEN yoki -compute-keys qiymati>" \
  -H "Content-Type: application/json" \
  -d '{"model":"gemma:2b","stream":true,"messages":[{"role":"user","content":"Salom"}]}'
```

Python (rasmiy `openai` kutubxonasi — hech narsa o'zgartirilmaydi, faqat `base_url`):

```python
from openai import OpenAI
client = OpenAI(base_url="http://BOSH-PC:8080/v1", api_key="<ADMIN-TOKEN>")
r = client.chat.completions.create(model="gemma:2b",
        messages=[{"role": "user", "content": "Klaster qanday ishlaydi?"}])
print(r.choices[0].message.content)
```

Endpointlar: `POST /v1/chat/completions` (stream va oddiy), `POST /v1/embeddings`, `GET /v1/models`.
API kalitlarini `-compute-keys k1,k2` (yoki `SDRIVE_COMPUTE_KEYS`) bilan bering; admin token ham ishlaydi.

### SSH orqali: bosh PC va tugunlarni boshqarish

Grafik muhitsiz serverlar va masofaviy mashinalar uchun SSH ishlatiladi — operatorning **o'z** mashinalarida,
kalit bilan:

**1) Bosh PC'ni masofadan boshqarish.** Koordinator API'sini faqat loopback'da tinglang va SSH tunnel orqali kiring:

```bash
sdrive-server -addr 127.0.0.1:8080 -admin-token "$(openssl rand -hex 24)"
ssh -L 8080:127.0.0.1:8080 user@bosh-pc     # endi lokal http://127.0.0.1:8080 bosh PC paneliga ulanadi
```

**2) O'z mashinalaringizga tugunni SSH orqali o'rnatish** — `scripts/provision_worker.sh`:

```bash
client/build.sh            # avval binar tayyorlang (yoki --remote-build bilan target'da build qiling)
scripts/provision_worker.sh --server http://bosh-pc:8080 --engine ollama \
    user@mac1 user@mac2 user@linux-gpu
```

Skript binarni scp bilan nusxalaydi (yoki target'da build qiladi) va mijozni fon rejimida ishga tushiradi.
Tugunni birinchi marta sozlash (disk/joy + shartnomaga rozilik) brauzer orqali bo'ladi; grafiksiz mashinada
`ssh -L 48673:127.0.0.1:48673 user@host` bilan tunnel ochib, `http://127.0.0.1:48673` ni oching.
Skript faqat sizning SSH ruxsatingiz bor hostlarga o'rnatadi va koordinator ularda faqat inference ishlarini
yuritadi — begona mashinalarga yoki ixtiyoriy kod bajarishga mo'ljallanmagan.

## Testlar

```bash
cd backend && go test -race ./...     # server: 30+ integratsion test (replikatsiya, audit, tiklash, to'lov, hisoblash, auth...)
client/build.sh                        # mijoz: kripto rasmiy vektorlari, JSON, HTTP (chunked stream), echo dvigatel, shifrlangan xotira
scripts/e2e.sh                         # xotira klasteri: haqiqiy server + 4 ta haqiqiy C++ mijoz (NODES=10 bilan ham)
scripts/e2e_compute.sh                 # AI klasteri: haqiqiy server + haqiqiy C++ ishchilar, OpenAI API orqali inference
```

`scripts/e2e.sh` (xotira): lokal UI orqali sozlash va rozilik, fayl yuklash va 30% replikatsiya, tugun diskida
ochiq matn yo'qligi, auditlar, `kill -9` bilan "elektr uzilishi" va nusxani ko'chirish, tugun qaytganda ortiqcha
nusxani olib tashlash, foydalanuvchi o'chirgan faylni qayta yuklash, server chunk'ini tugundan tiklash va
bayt-baybayt solishtirish, payout, uninstall, server qayta ishga tushganda holat saqlanishi.

`scripts/e2e_compute.sh` (AI): ishchilar imkoniyatini xabar qiladi, OpenAI `/v1/chat/completions` oddiy va
stream rejimida, 30 ta parallel so'rov butun klasterga taqsimlanishi, model marshrutlash, noma'lum model rad
etilishi, ishchi oflayn bo'lganda boshqasi davom ettirishi. Haqiqiy LLM uchun ishchida `ollama serve` ishlab
tursa, `SDRIVE_COMPUTE_ENGINE=ollama` bilan xuddi shu protokol Gemma kabi modellarda ishlaydi.

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
| `POST /api/nodes/{id}/compute/poll` | tugun | inference ishi so'rash (uzoq-poll) |
| `POST /api/compute/jobs/{jobId}/{chunk,done,fail}` | tugun | token oqimi va natija |
| `POST /api/compute/jobs` (`?stream=1`), `GET /api/compute/jobs/{id}[/stream]` | admin / kalit | ish yuborish va kuzatish |
| `GET /api/compute/workers`, `GET /api/compute/models` | hamma | hisoblash tugunlari va modellar |
| `POST /v1/chat/completions`, `POST /v1/embeddings`, `GET /v1/models` | kalit | OpenAI-mos gateway |

## Cheklovlar

- **AI hisoblash** inference (model ishlatish) uchun: tugun mavjud dvigatelni (ollama / llama.cpp) ishlatadi —
  bu xavfsiz, chunki koordinator ixtiyoriy kod emas, faqat {model, matn} yuboradi. Model **o'qitish** (training)
  yoki bitta modelni bir nechta mashina xotirasiga bo'lib yuklash (tensor/pipeline parallel) kiritilmagan;
  buning uchun llama.cpp RPC backend'i (`rpc-server`) alohida sozlanadi.
- **iOS** mijozi yo'q: iOS fon rejimida doimiy xizmat va ixtiyoriy disk joyidan foydalanishga ruxsat bermaydi.
- Windows'dagi `S:` disk — papkaga bog'langan virtual harf (`subst` kabi); hajm cheklovi dastur tomonidan
  ta'minlanadi, alohida formatlangan bo'lim (VHD) yaratilmaydi.
- To'lovlar — hisob-kitob yozuvlari (payout tarixi); haqiqiy pul o'tkazmasi (bank/karta) integratsiyasi yo'q.
- macOS paketi (`.dmg`) faqat macOS'da `client/build.sh` bilan yaratiladi.
