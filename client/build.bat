@echo off
rem SDrive Cluster — Windows static build script
set GXX=C:\Users\HPPC~1\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\g++.exe
set B1=-B"C:/Users/HPPC~1/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/lib/gcc/x86_64-w64-mingw32/16.1.0/"
set B2=-B"C:/Users/HPPC~1/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/x86_64-w64-mingw32/lib/"

echo Compiling statically linked SDrive Client...
%GXX% %B1% %B2% -O2 -std=c++17 -static -static-libgcc -static-libstdc++ -Isrc src/main.cpp src/config.cpp src/net.cpp src/storage.cpp src/platform.cpp src/server.cpp -lws2_32 -o sdrive.exe
%GXX% %B1% %B2% -O2 -std=c++17 -static -static-libgcc -static-libstdc++ -Isrc src/test_net.cpp src/net.cpp -lws2_32 -o test_net.exe

if exist sdrive.exe (
  echo [OK] Statically compiled sdrive.exe ready!
)
