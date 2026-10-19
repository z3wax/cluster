#!/usr/bin/env bash
# Debian/Ubuntu paketi: package_deb.sh <sdrive binar> <chiqish .deb>
set -euo pipefail
BIN="$1"
OUT="$2"
HERE="$(cd "$(dirname "$0")" && pwd)"
VERSION="$("$BIN" --version)"
ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)"
PKG="$(mktemp -d)"
trap 'rm -rf "$PKG"' EXIT

install -Dm755 "$BIN" "$PKG/usr/bin/sdrive"
install -Dm644 "$HERE/sdrive.service" "$PKG/usr/lib/systemd/user/sdrive.service"
install -Dm644 "$HERE/../../SHARTNOMA.md" "$PKG/usr/share/doc/sdrive/SHARTNOMA.md"
mkdir -p "$PKG/usr/share/applications" "$PKG/DEBIAN"
cat > "$PKG/usr/share/applications/sdrive.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=SDrive Cluster
Comment=Bo'sh disk joyini SDrive klasteriga ulash
Exec=/usr/bin/sdrive
Terminal=false
Categories=Network;Utility;
EOF
cat > "$PKG/DEBIAN/control" <<EOF
Package: sdrive
Version: $VERSION
Section: net
Priority: optional
Architecture: $ARCH
Depends: libc6, libstdc++6, libssl3 | libssl3t64
Maintainer: SDrive Cluster <support@sdrive.local>
Description: SDrive Cluster mijozi
 Kompyuterning bo'sh disk joyini SDrive taqsimlangan xotira klasteriga ulaydi.
 Fayl nusxalari ChaCha20 + HMAC-SHA256 bilan shifrlangan holda saqlanadi.
EOF
dpkg-deb --root-owner-group --build "$PKG" "$OUT" >/dev/null
echo "deb: $OUT"
