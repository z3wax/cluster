#pragma once
// Config: SDrive mijoz sozlamalari (config.json)
#include "json.hpp"
#include <string>
#include <cstdint>

#define SDRIVE_VERSION "2.0.0"

struct Config {
    std::string nodeId;
    std::string token;            // server bergan tugun tokeni
    std::string storageKey;       // 256-bit shifrlash kaliti (hex) — faqat shu kompyuterda
    std::string serverUrl = "http://127.0.0.1:8080";
    std::string driveLetter = "S";
    std::string sdriveDir;        // sdrive papkasi
    int64_t quotaBytes = 0;       // foydalanuvchi bergan joy
    bool agreementAccepted = false;
    std::string agreementVersion; // rozilik berilgan shartnoma versiyasi
    bool setupDone = false;       // disk+joy tanlangan
    std::string state = "setup";  // setup | agreement | running | uninstalled
    int uiPort = 48673;

    Json toJson() const {
        Json o;
        o["nodeId"] = nodeId;
        o["token"] = token;
        o["storageKey"] = storageKey;
        o["serverUrl"] = serverUrl;
        o["driveLetter"] = driveLetter;
        o["sdriveDir"] = sdriveDir;
        o["quotaBytes"] = (double)quotaBytes;
        o["agreementAccepted"] = agreementAccepted;
        o["agreementVersion"] = agreementVersion;
        o["setupDone"] = setupDone;
        o["state"] = state;
        o["uiPort"] = uiPort;
        return o;
    }

    void fromJson(const Json& o) {
        nodeId = o["nodeId"].asStr(nodeId);
        token = o["token"].asStr(token);
        storageKey = o["storageKey"].asStr(storageKey);
        serverUrl = o["serverUrl"].asStr(serverUrl);
        driveLetter = o["driveLetter"].asStr(driveLetter);
        sdriveDir = o["sdriveDir"].asStr(sdriveDir);
        quotaBytes = o["quotaBytes"].asInt(quotaBytes);
        agreementAccepted = o["agreementAccepted"].asBool(agreementAccepted);
        agreementVersion = o["agreementVersion"].asStr(agreementVersion);
        setupDone = o["setupDone"].asBool(setupDone);
        state = o["state"].asStr(state);
        uiPort = (int)o["uiPort"].asInt(uiPort);
    }
};

// Config papkasi: --config-dir > SDRIVE_CONFIG_DIR > OS standarti
void setConfigDirOverride(const std::string& dir);
bool configDirOverridden();
std::string configDir();
std::string configPath();
std::string defaultSdriveDir();
void loadConfig(Config& cfg);
bool saveConfig(const Config& cfg);
std::string newNodeId();
// Yangi o'rnatish uchun holatni tozalaydi (yangi ID, token va kalit)
void resetForReinstall(Config& cfg);

// Jurnal: stderr + <config>/sdrive.log
void logf(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
