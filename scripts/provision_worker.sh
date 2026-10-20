#!/usr/bin/env bash
# SDrive Cluster — ishchi tugunni SSH orqali O'RNATISH (operator o'z mashinalariga).
#
# Bu skript operatorning o'z kompyuterlariga (ssh kaliti bilan kiradigan) SDrive mijozini
# o'rnatadi va uni bosh PC (koordinator) ga ulaydi. BEGONA mashinalarga emas — faqat
# sizning ssh ruxsatingiz bor hostlarga. Koordinator keyinchalik bu tugunlarda faqat
# {model, prompt} turidagi inference ishlarini yuritadi, ixtiyoriy buyruq emas.
#
# Foydalanish:
#   scripts/provision_worker.sh --server http://BOSH-PC:8080 user@host1 user@host2 ...
#   scripts/provision_worker.sh --server ... --engine ollama --model gemma:2b user@mac1
#
# Talablar: operator mashinasida ssh/scp va target hostlarga kalit bilan kirish.
#           Linux/macOS target uchun avval `client/build.sh` bilan binar tayyorlang,
#           yoki --remote-build bilan target'da build qiling (git, cmake, g++ kerak).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERVER=""
ENGINE="auto"       # auto | ollama | llama.cpp
MODEL=""            # ixtiyoriy: modelni cheklash yoki echo uchun
PARALLEL="1"
BIN=""              # tayyor binar yo'li (bo'sh: client/dist/sdrive)
REMOTE_BUILD=0
HOSTS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --server) SERVER="$2"; shift 2;;
    --engine) ENGINE="$2"; shift 2;;
    --model) MODEL="$2"; shift 2;;
    --parallel) PARALLEL="$2"; shift 2;;
    --bin) BIN="$2"; shift 2;;
    --remote-build) REMOTE_BUILD=1; shift;;
    -h|--help)
      sed -n '2,20p' "$0"; exit 0;;
    *) HOSTS+=("$1"); shift;;
  esac
done

[[ -n "$SERVER" ]] || { echo "xato: --server BOSH-PC manzili kerak"; exit 2; }
[[ ${#HOSTS[@]} -gt 0 ]] || { echo "xato: kamida bitta user@host kerak"; exit 2; }

if [[ "$REMOTE_BUILD" == 0 && -z "$BIN" ]]; then
  BIN="$ROOT/client/dist/sdrive"
  [[ -f "$BIN" ]] || { echo "binar topilmadi: $BIN — avval client/build.sh ishga tushiring yoki --remote-build bering"; exit 2; }
fi

compute_env="SDRIVE_COMPUTE=1 SDRIVE_COMPUTE_ENGINE=$ENGINE SDRIVE_COMPUTE_PARALLEL=$PARALLEL"
[[ -n "$MODEL" ]] && compute_env="$compute_env SDRIVE_COMPUTE_MODELS=$MODEL"

for host in "${HOSTS[@]}"; do
  echo "==> $host"
  if [[ "$REMOTE_BUILD" == 1 ]]; then
    # target'da git clone + build (git, cmake, g++ kerak)
    ssh "$host" "bash -s" <<REMOTE
set -e
dir="\$HOME/.sdrive-src"
if [ -d "\$dir/.git" ]; then git -C "\$dir" pull --quiet; else git clone --quiet ${SDRIVE_REPO:-https://github.com/z3wax/cluster} "\$dir"; fi
cmake -S "\$dir/client" -B "\$dir/client/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "\$dir/client/build" -j\$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2) >/dev/null
install -m755 "\$dir/client/build/sdrive" "\$HOME/.local/bin/sdrive" 2>/dev/null || { mkdir -p "\$HOME/bin"; install -m755 "\$dir/client/build/sdrive" "\$HOME/bin/sdrive"; }
REMOTE
    remote_bin="\$HOME/.local/bin/sdrive"
  else
    ssh "$host" 'mkdir -p "$HOME/.local/bin"'
    scp -q "$BIN" "$host:.local/bin/sdrive"
    ssh "$host" 'chmod +x "$HOME/.local/bin/sdrive"'
    remote_bin="\$HOME/.local/bin/sdrive"
  fi

  # Fon rejimida ishga tushirish. --background avtostartni o'rnatadi (foydalanuvchi
  # mashinada brauzer orqali sozlab, shartnomaga rozilik bergandan keyingina ishlaydi).
  # Grafiksiz serverlarda: avval `ssh -L 48673:127.0.0.1:48673 $host` bilan tunnel ochib,
  # brauzerda http://127.0.0.1:48673 orqali sozlang.
  ssh "$host" "env $compute_env nohup $remote_bin --background --no-browser --server '$SERVER' >\$HOME/.sdrive.out 2>&1 & echo ishga_tushdi PID=\$!"
  echo "   ✔ o'rnatildi va ishga tushdi"
  echo "   Sozlash (bir marta): ssh -L 48673:127.0.0.1:48673 $host  → brauzerda http://127.0.0.1:48673"
done

echo
echo "Barcha tugunlar koordinatorга ($SERVER) ulanmoqda."
echo "Holat:  curl -s $SERVER/api/compute/workers | jq ."
