#include "config.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <random>
#else
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <random>
#endif

namespace fs = std::filesystem;

std::string configDir() {
    // Test/xizmat rejimi uchun override
    const char* ov = getenv("SDRIVE_CONFIG_DIR");
    if (ov && *ov) return ov;
#ifdef _WIN32
    static std::string dir;
    if (dir.empty()) {
        char buf[MAX_PATH] = {0};
        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, buf))) {
            dir = std::string(buf) + "\\SDrive";
        } else {
            dir = "SDrive";
        }
    }
    return dir;
#else
    static std::string dir;
    if (dir.empty()) {
        const char* xdg = getenv("XDG_CONFIG_HOME");
        if (xdg && *xdg) dir = std::string(xdg) + "/sdrive";
        else {
#ifdef __APPLE__
            const char* home = getenv("HOME");
            if (home) dir = std::string(home) + "/Library/Application Support/SDrive";
#else
            const char* home = getenv("HOME");
            if (home) dir = std::string(home) + "/.config/sdrive";
#endif
        }
        if (dir.empty()) dir = "sdrive";
    }
    return dir;
#endif
}

std::string configPath() { return configDir() + (configDir().find('\\') != std::string::npos ? "\\config.json" : "/config.json"); }

std::string defaultSdriveDir() {
#ifdef _WIN32
    const char* ud = getenv("USERPROFILE");
    return std::string(ud ? ud : "C:\\") + "\\SDriveData";
#else
    const char* home = getenv("HOME");
    if (!home) home = ".";
    return std::string(home) + "/SDriveData";
#endif
}

void loadConfig(Config& cfg) {
    std::ifstream f(configPath());
    if (f) {
        std::stringstream ss;
        ss << f.rdbuf();
        bool ok = false;
        Json j = Json::parse(ss.str(), &ok);
        if (ok) cfg.fromJson(j);
    }
    if (cfg.nodeId.empty()) {
        std::mt19937_64 rng(std::random_device{}());
        std::ostringstream ss;
        ss << "node_" << std::hex << rng() << rng();
        cfg.nodeId = ss.str();
    }
    if (cfg.sdriveDir.empty()) cfg.sdriveDir = defaultSdriveDir();
    if (cfg.driveLetter.empty()) cfg.driveLetter = "S";
    if (cfg.serverUrl.empty()) cfg.serverUrl = "http://127.0.0.1:8080";
}

void saveConfig(const Config& cfg) {
    fs::create_directories(configDir());
    std::ofstream f(configPath(), std::ios::trunc);
    f << cfg.toJson().dump();
}
