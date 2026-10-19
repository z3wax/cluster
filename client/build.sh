#!/usr/bin/env bash
# SDrive Cluster mijozini build qilish (Linux / macOS) va veb-saytga joylash.
#
#   ./build.sh            — joriy OS uchun build + unit testlar + paket
#   ./build.sh --windows  — qo'shimcha: MinGW bilan Windows .exe (Linux'da x86_64-w64-mingw32-g++ kerak)
#
# Natija: client/dist/ va backend/web/client/ (server /client/... orqali beradi)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BUILD="$HERE/build"
DIST="$HERE/dist"
WEB="$ROOT/backend/web/client"
mkdir -p "$DIST" "$WEB"

echo "==> build ($(uname -s))"
cmake -S "$HERE" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
echo "==> unit testlar"
"$BUILD/sdrive_tests"

case "$(uname -s)" in
  Linux)
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/sdrive"
    cp "$BUILD/sdrive" "$tmp/sdrive/"
    cp "$HERE/scripts/sdrive.service" "$ROOT/SHARTNOMA.md" "$tmp/sdrive/"
    tar -C "$tmp" -czf "$DIST/sdrive-linux.tar.gz" sdrive
    rm -rf "$tmp"
    cp "$DIST/sdrive-linux.tar.gz" "$WEB/"
    if command -v dpkg-deb >/dev/null; then
      "$HERE/scripts/package_deb.sh" "$BUILD/sdrive" "$DIST/sdrive-linux.deb"
      cp "$DIST/sdrive-linux.deb" "$WEB/"
    fi
    ;;
  Darwin)
    tmp="$(mktemp -d)"
    app="$tmp/SDrive Cluster.app/Contents/MacOS"
    mkdir -p "$app"
    cp "$BUILD/sdrive" "$app/sdrive"
    cat > "$tmp/SDrive Cluster.app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>SDrive Cluster</string>
  <key>CFBundleIdentifier</key><string>com.sdrive.cluster</string>
  <key>CFBundleExecutable</key><string>sdrive</string>
  <key>CFBundleVersion</key><string>2.0.0</string>
  <key>LSUIElement</key><true/>
</dict></plist>
PLIST
    rm -f "$DIST/sdrive-macos.dmg"
    hdiutil create -volname "SDrive Cluster" -srcfolder "$tmp" -ov -format UDZO "$DIST/sdrive-macos.dmg"
    rm -rf "$tmp"
    cp "$DIST/sdrive-macos.dmg" "$WEB/"
    ;;
esac

if [[ "${1:-}" == "--windows" ]]; then
  CXX_WIN="$(command -v x86_64-w64-mingw32-g++-posix || command -v x86_64-w64-mingw32-g++ || true)"
  [[ -n "$CXX_WIN" ]] || { echo "MinGW topilmadi (apt install g++-mingw-w64-x86-64-posix)"; exit 1; }
  echo "==> Windows .exe ($CXX_WIN)"
  (cd "$HERE/src" && "$CXX_WIN" -std=c++17 -O2 -s -mwindows -static -static-libgcc -static-libstdc++ \
      -D_WIN32_WINNT=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -I. \
      main.cpp agent.cpp server.cpp config.cpp crypto.cpp net.cpp storage.cpp platform.cpp \
      -lws2_32 -lwinhttp -lbcrypt -lshell32 -ladvapi32 -o "$DIST/sdrive-windows.exe")
  cp "$DIST/sdrive-windows.exe" "$WEB/"
fi

echo "==> tayyor:"
ls -la "$DIST"
