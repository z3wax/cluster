#pragma once
// Platformaga xos amallar: avtostart, brauzer, disk joyi, foydalanuvchi faolligi
#include <string>
#include <cstdint>

// Dasturni kompyuter yoqilishi bilan ishga tushirishni sozlaydi
// Windows: HKCU Run | Linux: ~/.config/autostart | macOS: LaunchAgent
bool installAutostart(const std::string& exePath, const std::string& extraArgs, std::string& err);
void removeAutostart();
bool autostartInstalled();

// Dastur yo'li (o'z protsessi)
std::string currentExePath();

// UI ni standart brauzerda ochish
void openBrowser(const std::string& url);

// Yo'l joylashgan diskdagi bo'sh joy (yo'l hali bo'lmasa — eng yaqin mavjud ota papka bo'yicha)
bool diskFreeBytes(const std::string& path, int64_t& freeBytes);

// Foydalanuvchi oxirgi marta klaviatura/sichqonchadan foydalanganidan beri o'tgan soniya.
// Aniqlab bo'lmasa -1.
int64_t userIdleSeconds();

std::string platformId();   // windows | macos | linux
std::string osName();       // masalan "Ubuntu 24.04", "Windows", "macOS 14.5"
std::string archName();     // x64 | arm64 | x86
std::string hostName();
int cpuCores();

// Windows: bo'sh disk harflari (masalan "STUVWXYZ"), boshqa OS: bo'sh
std::string freeDriveLetters();
