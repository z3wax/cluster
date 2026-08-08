#include "platform.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/statvfs.h>
#endif

namespace fs = std::filesystem;

std::string currentExePath() {
    char buf[4096];
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, buf, sizeof(buf));
    if (n > 0) return std::string(buf, n);
#elif defined(__APPLE__)
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0) n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0) { return "sdrive"; }
    buf[n] = 0;
    return std::string(buf);
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0) return "sdrive";
    buf[n] = 0;
    return std::string(buf);
#endif
    return "sdrive";
}

bool installAutostart(const std::string& exePath, std::string& err) {
#ifdef _WIN32
    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        err = "registry ochilmadi";
        return false;
    }
    std::string cmd = "\"" + exePath + "\" --background";
    LONG r = RegSetValueExA(key, "SDriveCluster", 0, REG_SZ,
                            (const BYTE*)cmd.c_str(), (DWORD)cmd.size() + 1);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS) { err = "registryga yozilmadi"; return false; }
    return true;
#elif defined(__APPLE__)
    // LaunchAgent plist
    const char* home = getenv("HOME");
    if (!home) { err = "HOME yo'q"; return false; }
    std::string dir = std::string(home) + "/Library/LaunchAgents";
    fs::create_directories(dir);
    std::ofstream f(dir + "/com.sdrive.daemon.plist");
    f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      << "<plist version=\"1.0\"><dict>\n"
      << "  <key>Label</key><string>com.sdrive.daemon</string>\n"
      << "  <key>ProgramArguments</key><array>\n"
      << "    <string>" << exePath << "</string>\n"
      << "    <string>--background</string>\n"
      << "  </array>\n"
      << "  <key>RunAtLoad</key><true/>\n"
      << "</dict></plist>\n";
    f.close();
    system("launchctl load -w ~/Library/LaunchAgents/com.sdrive.daemon.plist 2>/dev/null");
    return true;
#else
    const char* home = getenv("HOME");
    if (!home) { err = "HOME yo'q"; return false; }
    std::string dir = std::string(home) + "/.config/autostart";
    fs::create_directories(dir);
    std::ofstream f(dir + "/sdrive.desktop");
    f << "[Desktop Entry]\n"
      << "Type=Application\n"
      << "Name=SDrive Cluster\n"
      << "Exec=" << exePath << " --background\n"
      << "X-GNOME-Autostart-enabled=true\n";
    f.close();
    return true;
#endif
}

void removeAutostart() {
#ifdef _WIN32
    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueA(key, "SDriveCluster");
        RegCloseKey(key);
    }
#else
    const char* home = getenv("HOME");
    if (home) {
        std::error_code ec;
        fs::remove(std::string(home) + "/.config/autostart/sdrive.desktop", ec);
        fs::remove(std::string(home) + "/Library/LaunchAgents/com.sdrive.daemon.plist", ec);
    }
#endif
}

void openBrowser(const std::string& url) {
#ifdef _WIN32
    std::string c = "start \"\" \"" + url + "\"";
    system(c.c_str());
#elif defined(__APPLE__)
    std::string c = "open \"" + url + "\"";
    system(c.c_str());
#else
    std::string c = "xdg-open \"" + url + "\" 2>/dev/null || sensible-browser \"" + url + "\"";
    system(c.c_str());
#endif
}

bool diskFreeBytes(const std::string& path, int64_t& freeBytes) {
#ifdef _WIN32
    ULARGE_INTEGER freeAvail, total, totalFree;
    if (!GetDiskFreeSpaceExA(path.c_str(), &freeAvail, &total, &totalFree)) return false;
    freeBytes = (int64_t)freeAvail.QuadPart;
    return true;
#else
    struct statvfs st;
    if (statvfs(path.c_str(), &st) != 0) return false;
    freeBytes = (int64_t)st.f_bavail * (int64_t)st.f_frsize;
    return true;
#endif
}
