#pragma once
// Platformaga xos amallar: avtostart, ochiq brauzer
#include <string>
#include <cstdint>

// Dasturni kompyuter yoqilishi bilan ishga tushirishni sozlaydi
// Windows: HKCU Run | Linux: ~/.config/autostart | macOS: LaunchAgent
bool installAutostart(const std::string& exePath, std::string& err);

// Avtostartni olib tashlash
void removeAutostart();

// Dastur yo'lini topish (Windowsda o'z protsessi yo'li)
std::string currentExePath();

// UI ni brauzerda ochish
void openBrowser(const std::string& url);

// Disk bandligini qaytarish (windows: GetDiskFreeSpaceEx)
bool diskFreeBytes(const std::string& path, int64_t& freeBytes);
