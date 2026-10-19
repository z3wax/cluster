#include "platform.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#include <sys/statvfs.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

std::string currentExePath() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(NULL, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n > 0) return fs::path(std::wstring(buf, n)).u8string();
    return "sdrive.exe";
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        std::error_code ec;
        fs::path p = fs::canonical(buf, ec);
        return ec ? std::string(buf) : p.string();
    }
    return "sdrive";
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0) return "sdrive";
    buf[n] = 0;
    return std::string(buf);
#endif
}

#ifndef _WIN32
static std::string homeDir() {
    const char* h = getenv("HOME");
    return h ? h : "";
}

#ifdef __APPLE__
static std::string xmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

#else
// .desktop Exec qatori uchun argumentni qo'shtirnoqqa olish (freedesktop spetsifikatsiyasi)
static std::string desktopQuote(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '`' || c == '$' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}
#endif

static std::string runCmd(const char* cmd) {
    std::string out;
    FILE* p = popen(cmd, "r");
    if (!p) return out;
    char buf[512];
    while (fgets(buf, sizeof(buf), p)) out += buf;
    pclose(p);
    return out;
}
#endif

bool installAutostart(const std::string& exePath, const std::string& extraArgs, std::string& err) {
#ifdef _WIN32
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &key) !=
        ERROR_SUCCESS) {
        err = "registry ochilmadi";
        return false;
    }
    std::wstring cmd = L"\"" + fs::u8path(exePath).wstring() + L"\" --background";
    if (!extraArgs.empty()) cmd += L" " + fs::u8path(extraArgs).wstring();
    LONG r = RegSetValueExW(key, L"SDriveCluster", 0, REG_SZ, (const BYTE*)cmd.c_str(),
                            (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (r != ERROR_SUCCESS) {
        err = "registryga yozilmadi";
        return false;
    }
    return true;
#elif defined(__APPLE__)
    std::string home = homeDir();
    if (home.empty()) { err = "HOME yo'q"; return false; }
    std::string dir = home + "/Library/LaunchAgents";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream f(dir + "/com.sdrive.daemon.plist", std::ios::trunc);
    if (!f) { err = "LaunchAgent yozilmadi"; return false; }
    f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      << "<plist version=\"1.0\"><dict>\n"
      << "  <key>Label</key><string>com.sdrive.daemon</string>\n"
      << "  <key>ProgramArguments</key><array>\n"
      << "    <string>" << xmlEscape(exePath) << "</string>\n"
      << "    <string>--background</string>\n";
    std::istringstream extra(extraArgs);
    std::string a;
    while (extra >> a) f << "    <string>" << xmlEscape(a) << "</string>\n";
    f << "  </array>\n"
      << "  <key>RunAtLoad</key><true/>\n"
      << "</dict></plist>\n";
    return f.good();
#else
    std::string home = homeDir();
    if (home.empty()) { err = "HOME yo'q"; return false; }
    const char* xdg = getenv("XDG_CONFIG_HOME");
    std::string dir = (xdg && *xdg ? std::string(xdg) : home + "/.config") + "/autostart";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream f(dir + "/sdrive.desktop", std::ios::trunc);
    if (!f) { err = "autostart fayli yozilmadi"; return false; }
    f << "[Desktop Entry]\n"
      << "Type=Application\n"
      << "Name=SDrive Cluster\n"
      << "Comment=SDrive Cluster fon xizmati\n"
      << "Exec=" << desktopQuote(exePath) << " --background" << (extraArgs.empty() ? "" : " " + extraArgs) << "\n"
      << "Terminal=false\n"
      << "X-GNOME-Autostart-enabled=true\n";
    return f.good();
#endif
}

void removeAutostart() {
#ifdef _WIN32
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &key) ==
        ERROR_SUCCESS) {
        RegDeleteValueW(key, L"SDriveCluster");
        RegCloseKey(key);
    }
#else
    std::string home = homeDir();
    if (home.empty()) return;
    std::error_code ec;
    const char* xdg = getenv("XDG_CONFIG_HOME");
    fs::remove((xdg && *xdg ? std::string(xdg) : home + "/.config") + "/autostart/sdrive.desktop", ec);
    fs::remove(home + "/Library/LaunchAgents/com.sdrive.daemon.plist", ec);
#endif
}

bool autostartInstalled() {
#ifdef _WIN32
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS)
        return false;
    bool ok = RegQueryValueExW(key, L"SDriveCluster", NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
#elif defined(__APPLE__)
    std::error_code ec;
    return fs::exists(homeDir() + "/Library/LaunchAgents/com.sdrive.daemon.plist", ec);
#else
    std::error_code ec;
    const char* xdg = getenv("XDG_CONFIG_HOME");
    return fs::exists((xdg && *xdg ? std::string(xdg) : homeDir() + "/.config") + "/autostart/sdrive.desktop", ec);
#endif
}

void openBrowser(const std::string& url) {
#ifdef _WIN32
    ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string c = "open '" + url + "' >/dev/null 2>&1 &";
    if (system(c.c_str()) != 0) {}
#else
    std::string c = "(xdg-open '" + url + "' || sensible-browser '" + url + "') >/dev/null 2>&1 &";
    if (system(c.c_str()) != 0) {}
#endif
}

bool diskFreeBytes(const std::string& path, int64_t& freeBytes) {
    std::error_code ec;
#ifdef _WIN32
    fs::path p = fs::u8path(path);
#else
    fs::path p = path;
#endif
    p = fs::absolute(p, ec);
    while (!p.empty() && !fs::exists(p, ec)) {
        fs::path parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    fs::space_info si = fs::space(p, ec);
    if (ec) return false;
    freeBytes = (int64_t)si.available;
    return true;
}

int64_t userIdleSeconds() {
#ifdef _WIN32
    LASTINPUTINFO lii;
    lii.cbSize = sizeof(lii);
    if (!GetLastInputInfo(&lii)) return -1;
    return (int64_t)((GetTickCount() - lii.dwTime) / 1000);
#elif defined(__APPLE__)
    std::string out = runCmd("ioreg -c IOHIDSystem -d 4 2>/dev/null | awk '/HIDIdleTime/ {print $NF; exit}'");
    if (out.empty()) return -1;
    return (int64_t)(strtoull(out.c_str(), nullptr, 10) / 1000000000ULL);
#else
    const char* disp = getenv("DISPLAY");
    if (!disp || !*disp) return -1;
    std::string out = runCmd("xprintidle 2>/dev/null");
    if (out.empty()) return -1;
    return (int64_t)(strtoull(out.c_str(), nullptr, 10) / 1000ULL);
#endif
}

std::string platformId() {
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

std::string osName() {
#ifdef _WIN32
    return "Windows";
#elif defined(__APPLE__)
    std::string v = runCmd("sw_vers -productVersion 2>/dev/null");
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
    return v.empty() ? "macOS" : "macOS " + v;
#else
    std::ifstream f("/etc/os-release");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            std::string v = line.substr(12);
            if (v.size() >= 2 && v.front() == '"') v = v.substr(1, v.size() - 2);
            return v;
        }
    }
    return "Linux";
#endif
}

std::string archName() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

std::string hostName() {
#ifdef _WIN32
    char buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = sizeof(buf);
    if (GetComputerNameA(buf, &n)) return std::string(buf, n);
    return "PC";
#else
    char buf[256] = {0};
    if (gethostname(buf, sizeof(buf) - 1) == 0 && buf[0]) return buf;
    return "PC";
#endif
}

int cpuCores() {
    unsigned n = std::thread::hardware_concurrency();
    return n ? (int)n : 1;
}

std::string freeDriveLetters() {
#ifdef _WIN32
    DWORD used = GetLogicalDrives();
    std::string out;
    for (char c = 'D'; c <= 'Z'; c++)
        if (!(used & (1u << (c - 'A')))) out += c;
    return out;
#else
    return "";
#endif
}
