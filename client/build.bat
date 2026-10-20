@echo off
rem SDrive Cluster — Windows build (MinGW-w64 g++ PATH da bo'lishi kerak, masalan WinLibs yoki MSYS2)
rem Natija: dist\sdrive-windows.exe va ..\backend\web\client\sdrive-windows.exe
setlocal
cd /d "%~dp0"
where g++ >nul 2>nul || (echo [XATO] g++ topilmadi. MinGW-w64 ni o'rnating va PATH ga qo'shing. & exit /b 1)
if not exist dist mkdir dist

set FLAGS=-std=c++17 -O2 -static -static-libgcc -static-libstdc++ -D_WIN32_WINNT=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Isrc
set LIBS=-lws2_32 -lwinhttp -lbcrypt -lshell32 -ladvapi32
set CORE=src\config.cpp src\crypto.cpp src\net.cpp src\storage.cpp src\platform.cpp src\compute.cpp

echo Unit testlar build qilinmoqda...
g++ %FLAGS% tests\test_main.cpp %CORE% %LIBS% -o dist\sdrive_tests.exe || exit /b 1
dist\sdrive_tests.exe || (echo [XATO] unit testlar o'tmadi & exit /b 1)

echo SDrive mijozi build qilinmoqda...
g++ %FLAGS% -s -mwindows src\main.cpp src\agent.cpp src\server.cpp src\compute_agent.cpp %CORE% %LIBS% -o dist\sdrive-windows.exe || exit /b 1

if not exist ..\backend\web\client mkdir ..\backend\web\client
copy /y dist\sdrive-windows.exe ..\backend\web\client\sdrive-windows.exe >nul
echo [OK] dist\sdrive-windows.exe tayyor (veb-saytga ham joylandi)
