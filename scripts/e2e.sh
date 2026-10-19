#!/usr/bin/env bash
# SDrive Cluster — to'liq end-to-end test: haqiqiy Go server + bir nechta haqiqiy C++ mijoz.
#
# Tekshiriladi:
#   1. mijozlar lokal UI orqali sozlanadi va shartnomani qabul qiladi
#   2. fayl yuklanadi va tugunlarning 30% iga shifrlangan holda nusxalanadi
#   3. tugun to'satdan o'chadi (elektr uzilishi) — nusxa boshqa tugunga ko'chiriladi
#   4. foydalanuvchi faylni o'chiradi — avtomatik qayta yuklanadi
#   5. server diskidagi chunk yo'qoladi — tugundagi nusxadan tiklanadi
#   6. payout (cash chiqarish) va uninstall (balans yo'qotilishi, 6.2-band)
#   7. server qayta ishga tushadi — holat saqlanadi, tugunlar ishlashda davom etadi
#
# Foydalanish:  scripts/e2e.sh            (go, cmake, g++, curl, jq kerak)
#               KEEP=1 scripts/e2e.sh     (vaqtinchalik papkani o'chirmaslik)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d -t sdrive-e2e-XXXXXX)"
PORT="${PORT:-18090}"
UI_BASE="${UI_BASE:-48700}"
NODES="${NODES:-4}"
TOKEN="e2e-admin-token-$(date +%s)"
SERVER="http://127.0.0.1:$PORT"
PIDS=()

log()  { printf '\033[1;36m[e2e]\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m  ✔\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m  ✘ %s\033[0m\n' "$*"; echo "--- server.log (oxiri) ---"; tail -n 30 "$WORK/server.log" || true; exit 1; }

cleanup() {
  for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
  if [[ "${KEEP:-0}" == "1" ]]; then echo "Vaqtinchalik papka: $WORK"; else rm -rf "$WORK"; fi
}
trap cleanup EXIT

admin() { curl -sS -H "Authorization: Bearer $TOKEN" "$@"; }
info()  { curl -sS "$SERVER/api/info"; }
dash()  { admin "$SERVER/api/dashboard"; }
ui()    { local n=$1; shift; curl -sS -H "X-SDrive-UI: 1" -H "Content-Type: application/json" "http://127.0.0.1:$((UI_BASE + n))$1" "${@:2}"; }

# wait_until "tavsif" timeout_sek buyruq...
wait_until() {
  local what=$1 timeout=$2; shift 2
  local end=$((SECONDS + timeout))
  while (( SECONDS < end )); do
    if "$@" >/dev/null 2>&1; then ok "$what"; return 0; fi
    sleep 0.5
  done
  die "kutish vaqti tugadi: $what"
}

file_field() { dash | jq -r --arg id "$1" ".files[] | select(.id==\$id) | $2"; }
file_healthy() { [[ "$(file_field "$1" .status)" == "healthy" ]]; }
online_holders() { dash | jq --arg id "$1" '[.nodes[] | select(.online and ((.storedFiles // []) | index($id)))] | length'; }

start_server() {
  "$WORK/sdrive-server" -addr "127.0.0.1:$PORT" -data "$WORK/data" -agreement "$ROOT/SHARTNOMA.md" \
    -admin-token "$TOKEN" -heartbeat 1s -offline-timeout 4s -repair-delay 6s -reconcile 1s -audit 5s \
    -rate 1000000 -payout-anytime >>"$WORK/server.log" 2>&1 &
  SERVER_PID=$!
  PIDS+=("$SERVER_PID")
  wait_until "server javob bermoqda" 15 curl -sf "$SERVER/api/info"
}

start_client() {
  local n=$1
  mkdir -p "$WORK/home$n"
  HOME="$WORK/home$n" XDG_CONFIG_HOME="$WORK/home$n/.config" SDRIVE_SCAN_INTERVAL=3 \
    "$WORK/client/sdrive" --config-dir "$WORK/c$n" --ui-port $((UI_BASE + n)) --server "$SERVER" --no-browser \
    >>"$WORK/client$n.log" 2>&1 &
  CLIENT_PID[$n]=$!
  PIDS+=("${CLIENT_PID[$n]}")
}

# ---------------- Build ----------------
log "Build: server (Go) va mijoz (C++)"
(cd "$ROOT/backend" && go build -o "$WORK/sdrive-server" .)
cmake -S "$ROOT/client" -B "$WORK/client" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$WORK/client" -j"$(nproc 2>/dev/null || echo 2)" >/dev/null
"$WORK/client/sdrive_tests" >/dev/null || die "mijoz unit testlari o'tmadi"
ok "build va unit testlar"

# ---------------- 1. Klaster ----------------
log "1) Server va $NODES ta mijoz ishga tushirilmoqda"
start_server
declare -a CLIENT_PID
for n in $(seq 1 "$NODES"); do start_client "$n"; done
for n in $(seq 1 "$NODES"); do
  wait_until "mijoz $n lokal UI" 15 curl -sf "http://127.0.0.1:$((UI_BASE + n))/api/status"
  [[ "$(curl -s -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$((UI_BASE + n))/api/accept")" == "403" ]] \
    || die "lokal UI CSRF himoyasi ishlamadi"
  ui "$n" /api/setup -d "{\"quotaGb\":1,\"sdriveDir\":\"$WORK/sdrive$n\",\"driveLetter\":\"S\"}" | jq -e '.ok' >/dev/null \
    || die "mijoz $n setup"
  ver=$(ui "$n" /api/agreement | jq -r .version)
  ui "$n" /api/accept -d "{\"version\":\"$ver\"}" | jq -e '.ok' >/dev/null || die "mijoz $n shartnoma"
done
ok "barcha mijozlar sozlandi va shartnoma v$ver qabul qilindi"
wait_until "$NODES ta tugun onlayn" 20 bash -c "[[ \$(curl -s $SERVER/api/info | jq .onlineNodes) == $NODES ]]"
[[ -f "$WORK/home1/.config/autostart/sdrive.desktop" ]] && ok "avtostart o'rnatildi" || die "avtostart yo'q"

# ---------------- 2. Yuklash va replikatsiya ----------------
log "2) Fayllar yuklanmoqda"
head -c 9437184 /dev/urandom > "$WORK/big.bin"
printf 'SDRIVE-MAXFIY-MATN-%.0s' {1..200} > "$WORK/secret.txt"
BIG=$(admin -X POST --data-binary @"$WORK/big.bin" "$SERVER/api/files/upload?name=big.bin&size=$(stat -c %s "$WORK/big.bin")" | jq -r .fileId)
SEC=$(admin -X POST --data-binary @"$WORK/secret.txt" "$SERVER/api/files/upload?name=secret.txt" | jq -r .fileId)
[[ "$BIG" == f_* && "$SEC" == f_* ]] || die "yuklash muvaffaqiyatsiz"
EXPECT=$(awk -v n="$NODES" 'BEGIN { d = int(n * 0.3 + 0.9999999); if (d < 1) d = 1; print d }')
TARGET=$(file_field "$BIG" .replicaTarget)
[[ "$TARGET" == "$EXPECT" ]] || die "$NODES tugunning 30% i = $EXPECT kutilgan, $TARGET"
wait_until "big.bin $EXPECT ta tugunga nusxalandi (sog'lom)" 60 file_healthy "$BIG"
wait_until "secret.txt $EXPECT ta tugunga nusxalandi" 60 file_healthy "$SEC"
if grep -rq "SDRIVE-MAXFIY-MATN" "$WORK"/sdrive*/chunks; then die "tugun diskida ochiq matn topildi!"; fi
ok "tugunlar diskida faqat shifrlangan ma'lumot (ochiq matn topilmadi)"
wait_until "auditlar (saqlash isboti) o'tdi" 30 bash -c "[[ \$(curl -s -H 'Authorization: Bearer $TOKEN' $SERVER/api/dashboard | jq '[.nodes[].auditsPassed] | add') -ge 4 ]]"

# ---------------- 3. Tugun o'chadi ----------------
log "3) Nusxa saqlovchi tugun to'satdan o'chirilmoqda (kill -9)"
VICTIM=$(dash | jq -r --arg id "$BIG" '[.nodes[] | select((.storedFiles // []) | index($id))][0].id')
for n in $(seq 1 "$NODES"); do
  if [[ "$(jq -r .nodeId "$WORK/c$n/config.json")" == "$VICTIM" ]]; then VN=$n; fi
done
kill -9 "${CLIENT_PID[$VN]}"
wait_until "tugun $VN oflayn deb belgilandi" 20 bash -c "[[ \$(curl -s $SERVER/api/info | jq .onlineNodes) == $((NODES - 1)) ]]"
wait_until "nusxa boshqa tugunga ko'chirildi ($EXPECT ta onlayn nusxa)" 60 bash -c "[[ \$(curl -s -H 'Authorization: Bearer $TOKEN' $SERVER/api/dashboard | jq --arg id $BIG '[.nodes[] | select(.online and ((.storedFiles // []) | index(\$id)))] | length') -ge $EXPECT ]]"
start_client "$VN"
wait_until "tugun $VN qaytib keldi" 20 bash -c "[[ \$(curl -s $SERVER/api/info | jq .onlineNodes) == $NODES ]]"
wait_until "ortiqcha nusxa olib tashlandi (aniq $EXPECT ta)" 60 bash -c "[[ \$(curl -s -H 'Authorization: Bearer $TOKEN' $SERVER/api/dashboard | jq --arg id $BIG '[.nodes[] | select((.storedFiles // []) | index(\$id))] | length') == $EXPECT ]]"

# ---------------- 4. Foydalanuvchi faylni o'chiradi ----------------
log "4) Foydalanuvchi SDrive papkasidan faylni o'chiradi"
HOLDER_DIR=$(ls -d "$WORK"/sdrive*/chunks/"$SEC" | head -1)
rm -rf "$HOLDER_DIR"
wait_until "o'chirilgan fayl avtomatik qayta yuklandi" 60 test -d "$HOLDER_DIR"
wait_until "fayl yana sog'lom" 30 file_healthy "$SEC"

# ---------------- 5. Server nusxasi yo'qoladi ----------------
log "5) Server diskidagi chunk o'chirilmoqda"
rm -f "$WORK/data/chunks/$BIG/1.bin"
code=$(admin -s -o /dev/null -w '%{http_code}' "$SERVER/api/files/$BIG/download")
[[ "$code" == "503" ]] || die "yo'q chunk bilan yuklab berildi ($code)"
ok "to'liq bo'lmagan fayl berilmadi (503 — tiklanmoqda)"
wait_until "server chunk'i tugundan tiklandi" 60 test -f "$WORK/data/chunks/$BIG/1.bin"
wait_until "fayl yuklab olinadi" 30 bash -c "[[ \$(curl -s -o /dev/null -w '%{http_code}' -H 'Authorization: Bearer $TOKEN' $SERVER/api/files/$BIG/download) == 200 ]]"
admin -o "$WORK/big.down" "$SERVER/api/files/$BIG/download"
cmp -s "$WORK/big.bin" "$WORK/big.down" || die "yuklab olingan fayl asl nusxadan farq qiladi"
ok "yuklab olingan fayl bayt-baybayt bir xil ($(sha256sum "$WORK/big.bin" | cut -c1-16)...)"

# ---------------- 6. Payout va uninstall ----------------
log "6) Cash chiqarish va dasturni o'chirish"
PAYER=$(dash | jq -r --arg id "$BIG" '[.nodes[] | select((.storedFiles // []) | index($id))][0].id')
for n in $(seq 1 "$NODES"); do [[ "$(jq -r .nodeId "$WORK/c$n/config.json")" == "$PAYER" ]] && PN=$n; done
sleep 2
AMOUNT=$(ui "$PN" /api/claim -X POST | jq -r '.payout.amountCents')
[[ "$AMOUNT" != "null" ]] && awk "BEGIN{exit !($AMOUNT > 0)}" || die "payout muvaffaqiyatsiz"
ok "mijoz $PN payout oldi: $AMOUNT sent"
sleep 2
FORF=$(ui "$PN" /api/uninstall -X POST | jq -r '.forfeitedCents')
awk "BEGIN{exit !($FORF > 0)}" || die "uninstall: yo'qotilgan balans 0"
ok "uninstall: olinmagan $FORF sent yo'qotildi (6.2-band)"
wait_until "mijoz jarayoni to'xtadi" 15 bash -c "! kill -0 ${CLIENT_PID[$PN]}"
[[ -z "$(ls -A "$WORK/sdrive$PN/chunks" 2>/dev/null)" ]] || die "uninstall'dan keyin klaster fayllari qoldi"
[[ ! -f "$WORK/home$PN/.config/autostart/sdrive.desktop" ]] || die "avtostart olib tashlanmadi"
ok "lokal klaster fayllari va avtostart o'chirildi"
[[ "$(dash | jq -r --arg id "$PAYER" '.nodes[] | select(.id==$id) | .status')" == "uninstalled" ]] || die "serverda tugun o'chirilmadi"
wait_until "o'chirilgan tugundagi nusxalar ko'chirildi" 60 bash -c "[[ \$(curl -s -H 'Authorization: Bearer $TOKEN' $SERVER/api/dashboard | jq '[.files[] | select(.status==\"healthy\")] | length') == 2 ]]"

# ---------------- 7. Server qayta ishga tushadi ----------------
log "7) Server qayta ishga tushirilmoqda (SIGTERM)"
kill -TERM "$SERVER_PID"
wait "$SERVER_PID" 2>/dev/null || true
start_server
wait_until "tugunlar eski token bilan qayta ulandi" 20 bash -c "[[ \$(curl -s $SERVER/api/info | jq .onlineNodes) == $((NODES - 1)) ]]"
[[ "$(info | jq .totalFiles)" == "2" ]] || die "fayllar yo'qoldi"
admin -o "$WORK/big.down2" "$SERVER/api/files/$BIG/download"
cmp -s "$WORK/big.bin" "$WORK/big.down2" || die "qayta ishga tushgandan keyin fayl mos emas"
ok "holat saqlandi, fayl to'g'ri"

# ---------------- 8. Fayl o'chirish ----------------
log "8) Admin faylni klasterdan o'chiradi"
admin -X DELETE "$SERVER/api/files/$SEC" | jq -e .ok >/dev/null
wait_until "barcha tugunlardan nusxa o'chirildi" 30 bash -c "! ls -d $WORK/sdrive*/chunks/$SEC 2>/dev/null | grep -q ."

echo
printf '\033[1;32mBARCHA E2E TESTLAR MUVAFFAQIYATLI O'"'"'TDI\033[0m\n'
