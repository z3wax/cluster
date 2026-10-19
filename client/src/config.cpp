#include "config.h"
#include "crypto.h"
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

static std::string g_configOverride;

void setConfigDirOverride(const std::string& dir) { g_configOverride = dir; }

bool configDirOverridden() {
    const char* ov = getenv("SDRIVE_CONFIG_DIR");
    return !g_configOverride.empty() || (ov && *ov);
}

std::string configDir() {
    if (!g_configOverride.empty()) return g_configOverride;
    const char* ov = getenv("SDRIVE_CONFIG_DIR");
    if (ov && *ov) return ov;
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, buf))) return std::string(buf) + "\\SDrive";
    return "SDrive";
#else
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/sdrive";
    const char* home = getenv("HOME");
    if (!home) return "sdrive";
#ifdef __APPLE__
    return std::string(home) + "/Library/Application Support/SDrive";
#else
    return std::string(home) + "/.config/sdrive";
#endif
#endif
}

std::string configPath() { return (fs::path(configDir()) / "config.json").string(); }

std::string defaultSdriveDir() {
#ifdef _WIN32
    const char* ud = getenv("USERPROFILE");
    return std::string(ud ? ud : "C:") + "\\SDriveData";
#else
    const char* home = getenv("HOME");
    return std::string(home ? home : ".") + "/SDriveData";
#endif
}

std::string newNodeId() { return "node_" + toHex(randomBytes(12)); }

static void ensureSecrets(Config& cfg) {
    if (cfg.nodeId.empty()) cfg.nodeId = newNodeId();
    std::string raw;
    if (!fromHex(cfg.storageKey, raw) || raw.size() != 32) cfg.storageKey = toHex(randomBytes(32));
}

void resetForReinstall(Config& cfg) {
    Config fresh;
    fresh.serverUrl = cfg.serverUrl;
    fresh.uiPort = cfg.uiPort;
    fresh.driveLetter = cfg.driveLetter;
    fresh.sdriveDir = cfg.sdriveDir;
    cfg = fresh;
    ensureSecrets(cfg);
}

void loadConfig(Config& cfg) {
    std::ifstream f(configPath(), std::ios::binary);
    if (f) {
        std::stringstream ss;
        ss << f.rdbuf();
        bool ok = false;
        Json j = Json::parse(ss.str(), &ok);
        if (ok) cfg.fromJson(j);
        else logf("[config] %s o'qib bo'lmadi — standart sozlamalar", configPath().c_str());
    }
    ensureSecrets(cfg);
    if (cfg.sdriveDir.empty()) cfg.sdriveDir = defaultSdriveDir();
    if (cfg.driveLetter.empty()) cfg.driveLetter = "S";
    if (cfg.serverUrl.empty()) cfg.serverUrl = "http://127.0.0.1:8080";
    while (!cfg.serverUrl.empty() && cfg.serverUrl.back() == '/') cfg.serverUrl.pop_back();
    if (cfg.uiPort <= 0 || cfg.uiPort > 65535) cfg.uiPort = 48673;
}

bool saveConfig(const Config& cfg) {
    std::error_code ec;
    fs::create_directories(configDir(), ec);
    std::string path = configPath();
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << cfg.toJson().dump();
        if (!f.good()) return false;
    }
#ifndef _WIN32
    chmod(tmp.c_str(), 0600); // token va kalit faqat foydalanuvchiga ko'rinadi
#endif
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
    }
    return !ec;
}

// ---------------- Jurnal ----------------

static std::mutex g_logMu;

void logf(const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char ts[32];
    time_t t = time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

    std::lock_guard<std::mutex> lk(g_logMu);
    fprintf(stderr, "%s %s\n", ts, msg);
    std::error_code ec;
    fs::path p = fs::path(configDir()) / "sdrive.log";
    if (fs::exists(p, ec) && fs::file_size(p, ec) > (5u << 20)) {
        fs::rename(p, fs::path(configDir()) / "sdrive.log.1", ec);
    }
    std::ofstream f(p, std::ios::app);
    if (f) f << ts << " " << msg << "\n";
}
