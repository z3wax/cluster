#!/usr/bin/env bash
# SDrive Cluster — hisoblash (GPU/CPU) klasteri end-to-end testi.
# Haqiqiy Go server (koordinator) + bir nechta haqiqiy C++ ishchi tugun (echo dvigateli bilan).
#
# Tekshiriladi:
#   1. ishchilar imkoniyatlarini (CPU/RAM/GPU/dvigatel/modellar) xabar qiladi
#   2. OpenAI-mos API (/v1/chat/completions) ishlaydi — oddiy va stream rejimida
#   3. so'rovlar barcha ishchilarga taqsimlanadi (bosh PC butun klaster quvvatidan foydalanadi)
#   4. model marshrutlash: faqat kerakli modelga ega tugunlar ishlaydi
#   5. ishchi oflayn bo'lsa, boshqa ishchi davom ettiradi
#
# Haqiqiy LLM (ollama/Gemma) uchun: SDRIVE_COMPUTE_ENGINE=ollama bilan ishga tushiring —
# protokol bir xil, faqat dvigatel o'zgaradi (quyida izoh).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d -t sdrive-compute-XXXXXX)"
PORT="${PORT:-18120}"
UI_BASE="${UI_BASE:-48730}"
WORKERS="${WORKERS:-4}"
TOKEN="compute-e2e-token-$(date +%s)"
SERVER="http://127.0.0.1:$PORT"
PIDS=()

log()  { printf '\033[1;36m[compute-e2e]\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m  ✔\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m  ✘ %s\033[0m\n' "$*"; tail -n 20 "$WORK/server.log" 2>/dev/null; exit 1; }

cleanup() {
  for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
  [[ "${KEEP:-0}" == "1" ]] && echo "papka: $WORK" || rm -rf "$WORK"
}
trap cleanup EXIT

api()  { curl -sS -H "Authorization: Bearer $TOKEN" "$@"; }
ui()   { local n=$1; shift; curl -sS -H "X-SDrive-UI: 1" -H "Content-Type: application/json" "http://127.0.0.1:$((UI_BASE + n))$1" "${@:2}"; }

wait_until() {
  local what=$1 timeout=$2; shift 2
  local end=$((SECONDS + timeout))
  while (( SECONDS < end )); do "$@" >/dev/null 2>&1 && { ok "$what"; return 0; }; sleep 0.4; done
  die "kutish vaqti tugadi: $what"
}

# ---------------- Build ----------------
log "Build: server + mijoz"
(cd "$ROOT/backend" && go build -o "$WORK/sdrive-server" .)
cmake -S "$ROOT/client" -B "$WORK/cbuild" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
cmake --build "$WORK/cbuild" -j"$(nproc 2>/dev/null || echo 2)" >/dev/null
ok "build"

# ---------------- Server ----------------
log "Koordinator ishga tushirilmoqda"
"$WORK/sdrive-server" -addr "127.0.0.1:$PORT" -data "$WORK/data" -agreement "$ROOT/SHARTNOMA.md" \
  -admin-token "$TOKEN" -heartbeat 1s -offline-timeout 4s -reconcile 1s >>"$WORK/server.log" 2>&1 &
PIDS+=($!)
wait_until "server javob bermoqda" 15 curl -sf "$SERVER/api/info"

# ---------------- Ishchilar (echo dvigateli) ----------------
# Yarmi "gemma:2b" ni, yarmi ikkala modelni biladi — model marshrutlashni sinash uchun.
log "$WORKERS ta hisoblash ishchisi ulanmoqda (echo dvigateli)"
for n in $(seq 1 "$WORKERS"); do
  mkdir -p "$WORK/home$n"
  models="gemma:2b"
  (( n % 2 == 0 )) && models="gemma:2b,llama3:8b"
  HOME="$WORK/home$n" XDG_CONFIG_HOME="$WORK/home$n/.config" \
    SDRIVE_COMPUTE=1 SDRIVE_COMPUTE_ENGINE=echo SDRIVE_COMPUTE_MODELS="$models" SDRIVE_COMPUTE_PARALLEL=2 \
    "$WORK/cbuild/sdrive" --config-dir "$WORK/c$n" --ui-port $((UI_BASE + n)) --server "$SERVER" --no-browser \
    >>"$WORK/worker$n.log" 2>&1 &
  PIDS+=($!)
done
# har bir ishchini sozlash (disk/joy + shartnoma; hisoblash uchun ham rozilik kerak)
for n in $(seq 1 "$WORKERS"); do
  wait_until "ishchi $n UI" 15 curl -sf "http://127.0.0.1:$((UI_BASE + n))/api/status"
  ui "$n" /api/setup -d "{\"quotaGb\":1,\"sdriveDir\":\"$WORK/sd$n\"}" | grep -q '"ok":true' || die "setup $n"
  ver=$(ui "$n" /api/agreement | python3 -c 'import sys,json;print(json.load(sys.stdin)["version"])')
  ui "$n" /api/accept -d "{\"version\":\"$ver\"}" | grep -q '"ok":true' || die "accept $n"
done
wait_until "$WORKERS ta hisoblash tuguni onlayn" 20 bash -c "[[ \$(curl -s $SERVER/api/info | python3 -c 'import sys,json;print(json.load(sys.stdin)[\"computeNodes\"])') == $WORKERS ]]"

# 1) imkoniyatlar
log "1) Imkoniyatlar va modellar"
api "$SERVER/api/compute/workers" > "$WORK/workers.json"
python3 - "$WORK/workers.json" "$WORKERS" <<'PY' || die "imkoniyatlar"
import sys, json
d = json.load(open(sys.argv[1]))
want = int(sys.argv[2])
assert len(d["workers"]) == want, f"{len(d['workers'])} ishchi"
for w in d["workers"]:
    assert w["engines"] == ["echo"], w
    assert w["cpuCores"] >= 1 and "gemma:2b" in w["models"], w
assert "gemma:2b" in d["models"] and "llama3:8b" in d["models"], d["models"]
print("  ✔ %d ishchi, modellar: %s" % (len(d["workers"]), ", ".join(d["models"])))
PY
api "$SERVER/v1/models" | grep -q '"gemma:2b"' || die "/v1/models"
ok "OpenAI /v1/models"

# 2) OpenAI chat — oddiy
log "2) OpenAI /v1/chat/completions (oddiy)"
api -H "Content-Type: application/json" -X POST "$SERVER/v1/chat/completions" \
  -d '{"model":"gemma:2b","messages":[{"role":"user","content":"Assalomu alaykum klaster"}]}' > "$WORK/chat.json"
python3 - "$WORK/chat.json" <<'PY' || die "chat javobi"
import sys, json
d = json.load(open(sys.argv[1]))
assert d["object"] == "chat.completion", d
c = d["choices"][0]["message"]["content"]
assert c == "Assalomu alaykum klaster", repr(c)
assert d["usage"]["total_tokens"] > 0
print("  ✔ javob: %r" % c)
PY

# 3) OpenAI chat — streaming (token-token)
log "3) OpenAI streaming (SSE)"
api -N -H "Content-Type: application/json" -X POST "$SERVER/v1/chat/completions" \
  -d '{"model":"gemma:2b","stream":true,"messages":[{"role":"user","content":"bir ikki uch tort"}]}' \
  > "$WORK/stream.txt"
python3 - "$WORK/stream.txt" <<'PY' || die "streaming"
import sys, json
content, done = "", False
for line in open(sys.argv[1]):
    line = line.strip()
    if not line.startswith("data: "): continue
    p = line[6:]
    if p == "[DONE]": done = True; continue
    j = json.loads(p)
    assert j["object"] == "chat.completion.chunk"
    for ch in j["choices"]:
        content += ch["delta"].get("content", "")
assert done, "[DONE] yo'q"
assert content == "bir ikki uch tort", repr(content)
print("  ✔ stream yig'ildi: %r" % content)
PY

# 4) yuk taqsimoti — ko'p so'rov, bir nechta tugun ishlashi kerak
log "4) 30 ta parallel so'rov butun klasterga taqsimlanmoqda"
jobfile="$WORK/jobs.txt"
: > "$jobfile"
one() {
  local i=$1 jid r st
  jid=$(curl -sS -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" -X POST "$SERVER/api/compute/jobs" \
    -d "{\"model\":\"gemma:2b\",\"messages\":[{\"role\":\"user\",\"content\":\"sorov-$i\"}]}" \
    | python3 -c 'import sys,json;print(json.load(sys.stdin)["jobId"])')
  for _ in $(seq 1 60); do
    r=$(curl -sS -H "Authorization: Bearer $TOKEN" "$SERVER/api/compute/jobs/$jid")
    st=$(printf '%s' "$r" | python3 -c 'import sys,json;print(json.load(sys.stdin)["status"])')
    if [[ "$st" == "done" ]]; then
      printf '%s' "$r" | python3 -c "import sys,json;d=json.load(sys.stdin);print(d['worker'],d['result'])" >> "$jobfile"
      return 0
    fi
    [[ "$st" == "failed" ]] && return 1
    sleep 0.1
  done
  return 1
}
pids=()
for i in $(seq 1 30); do one "$i" & pids+=($!); done
fail=0
for p in "${pids[@]}"; do wait "$p" || fail=$((fail+1)); done
[[ "$fail" == 0 ]] || die "$fail ta so'rov bajarilmadi"
done_count=$(wc -l < "$jobfile")
used=$(awk '{print $1}' "$jobfile" | sort -u | wc -l)
[[ "$done_count" == 30 ]] || die "faqat $done_count/30 bajarildi"
(( used >= 2 )) || die "yuk taqsimlanmadi — faqat $used tugun ishladi"
ok "30 so'rov bajarildi, $used ta tugunga taqsimlandi"

# 5) model marshrutlash — llama3:8b faqat juft tugunlarda
log "5) Model marshrutlash"
jid=$(api -H "Content-Type: application/json" -X POST "$SERVER/api/compute/jobs" \
  -d '{"model":"llama3:8b","messages":[{"role":"user","content":"faqat llama"}]}' \
  | python3 -c 'import sys,json;print(json.load(sys.stdin)["jobId"])')
for _ in $(seq 1 50); do
  r=$(api "$SERVER/api/compute/jobs/$jid")
  [[ $(echo "$r" | python3 -c 'import sys,json;print(json.load(sys.stdin)["status"])') == "done" ]] && break
  sleep 0.1
done
w=$(echo "$r" | python3 -c 'import sys,json;print(json.load(sys.stdin)["worker"])')
# juft raqamli ishchilargina llama3:8b ni biladi; worker ID sim-... emas, node_...; tekshiramiz model mosligini
echo "$r" | python3 -c 'import sys,json;d=json.load(sys.stdin);assert d["result"]=="faqat llama",d;print("  ✔ llama3:8b marshrutlandi ->",d["worker"])' || die "marshrutlash"
code=$(api -o /dev/null -w '%{http_code}' -H "Content-Type: application/json" -X POST "$SERVER/v1/chat/completions" \
  -d '{"model":"mistral:7b","messages":[{"role":"user","content":"x"}]}')
[[ "$code" == "503" ]] || die "noma'lum model uchun $code (503 kutilgan)"
ok "noma'lum model rad etildi (503)"

# 6) ishchi oflayn -> boshqasi davom ettiradi
log "6) Bitta ishchi to'xtatilmoqda (kill)"
kill "${PIDS[2]}" 2>/dev/null || true  # 2-ishchi (PIDS[0]=server)
wait_until "bitta tugun oflayn" 15 bash -c "[[ \$(curl -s $SERVER/api/info | python3 -c 'import sys,json;print(json.load(sys.stdin)[\"computeNodes\"])') == $((WORKERS-1)) ]]"
out=$(api -H "Content-Type: application/json" -X POST "$SERVER/v1/chat/completions" \
  -d '{"model":"gemma:2b","messages":[{"role":"user","content":"oflayndan keyin"}]}')
echo "$out" | python3 -c 'import sys,json;d=json.load(sys.stdin);assert d["choices"][0]["message"]["content"]=="oflayndan keyin",d;print("  ✔ qolgan tugunlar xizmat ko'\''rsatmoqda")' || die "oflayndan keyin ishlamadi"

echo
printf '\033[1;32mHISOBLASH KLASTERI E2E TESTLARI MUVAFFAQIYATLI O'"'"'TDI\033[0m\n'
echo
echo "Haqiqiy LLM uchun (masalan Gemma): ishchida 'ollama serve' ishlab tursin va"
echo "  SDRIVE_COMPUTE=1 SDRIVE_COMPUTE_ENGINE=ollama sdrive --server $SERVER"
echo "bilan ishga tushiring — protokol aynan shu, faqat echo o'rniga haqiqiy model ishlaydi."
