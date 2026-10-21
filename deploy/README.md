# SDrive Cluster — serverga qo'yish (deploy)

Koordinator (bosh PC) serveri bitta statik Go binaridan iborat: veb-panel binarga joylangan,
ma'lumotlar `/data` papkasida. Quyidagi variantlardan birini tanlang.

> **Muhim:** koordinator doim ishlab turishi kerak (tugunlar unga ulanadi) va ma'lumotlari
> saqlanishi kerak. Shuning uchun "uxlab qoladigan" bepul rejimlardan ko'ra doimiy ishlaydigan
> (VPS / Fly / Render starter) variantni tanlang. Birinchi ishga tushishda **admin token**
> yaratiladi va loglarda ko'rinadi — uni saqlab qo'ying.

---

## 1. VPS yoki istalgan server — Docker (eng oson)

```bash
git clone https://github.com/z3wax/cluster && cd cluster
SDRIVE_ADMIN_TOKEN=$(openssl rand -hex 24) docker compose up -d --build
docker compose logs -f           # manzil va admin token
```

Panel: `http://SERVER-IP:8080`. Ma'lumotlar `sdrive-data` volume'ida saqlanadi.
To'xtatish: `docker compose down` (ma'lumotlar saqlanadi), tozalash: `docker compose down -v`.

## 2. VPS — Docker'siz (systemd)

```bash
# Go 1.22+ kerak
go build -o /usr/local/bin/sdrive-server ./backend
sudo useradd -r -s /usr/sbin/nologin sdrive || true
sudo mkdir -p /var/lib/sdrive && sudo cp SHARTNOMA.md /var/lib/sdrive/
sudo chown -R sdrive:sdrive /var/lib/sdrive
sudo cp deploy/sdrive-server.service /etc/systemd/system/
sudo systemctl enable --now sdrive-server
journalctl -u sdrive-server -f   # admin token
```

## 3. Fly.io

```bash
cp deploy/fly.toml ./fly.toml
fly volumes create sdrive_data -s 3
fly secrets set SDRIVE_ADMIN_TOKEN=$(openssl rand -hex 24)
fly deploy
```

Manzil: `https://sdrive-cluster.fly.dev` (HTTPS avtomatik).

## 4. Render.com

Repo'ni ulang → **New → Blueprint** → `deploy/render.yaml` ni tanlang.
Render $PORT, HTTPS va `/data` diskni avtomatik beradi; admin token Dashboard'da ko'rinadi.

## 5. Hugging Face Space (Docker)

HF Space'lar hisoblash uchun qulay, lekin **bepul rejimda disk vaqtincha** (qayta qurishda
nollanadi) — doimiy ma'lumot uchun yuqoridagi variantlarni tanlang. Qadamlar:

1. huggingface.co'da yangi **Space** yarating → SDK: **Docker** → **Blank**.
2. Shu repo fayllarini Space repo'siga push qiling (`Dockerfile` ildizda bo'lsin).
3. Space `README.md` boshiga qo'ying:
   ```yaml
   ---
   title: SDrive Cluster
   sdk: docker
   app_port: 7860
   ---
   ```
4. Space **Settings → Variables** da `PORT=7860` qo'shing (server shu portda tinglaydi).
5. Admin token uchun **Settings → Secrets** da `SDRIVE_ADMIN_TOKEN` bering.

Manzil: `https://<user>-<space>.hf.space`.

---

## Deploydan keyin

- **Admin panel:** `https://SERVER/` → istalgan admin bo'lim → token bilan kiring.
- **Tugunlarni ulash:** `sdrive --server https://SERVER` (xotira uchun) yoki
  `sdrive --server https://SERVER --compute --compute-engine ollama` (AI uchun).
- **AI API:** `POST https://SERVER/v1/chat/completions` (`Authorization: Bearer <ADMIN-TOKEN>`).
- **HTTPS:** Fly/Render/HF avtomatik beradi. VPS'da Caddy bitta qatorda:
  `caddy reverse-proxy --from https://domen.uz --to 127.0.0.1:8080`
  yoki server'ga to'g'ridan-to'g'ri `-tls-cert`/`-tls-key` bering.

## Resurslar

Koordinatorning o'zi yengil (RAM ~ fayllar metama'lumoti + chunk'lar diskda). 512 MB RAM va
bir nechta GB disk ko'p klaster uchun yetarli. Yuklangan fayl chunk'lari `/data/chunks` da
turadi — disk hajmini shunga qarab tanlang (yoki S3-mos obyekt xotirasini oldiga qo'ying).
