#pragma once
// Config: SDrive mijoz sozlamalari
#include "json.hpp"
#include <string>
#include <mutex>

struct Config {
    std::string nodeId;
    std::string serverUrl = "http://127.0.0.1:8080";
    std::string driveLetter = "S";
    std::string sdriveDir;      // sdrive papkasi
    int64_t quotaBytes = 0;     // foydalanuvchi bergan joy
    bool agreementAccepted = false;
    bool setupDone = false;     // disk+joy tanlangan
    std::string state = "setup"; // setup | agreement | running

    Json toJson() const {
        Json o;
        o["nodeId"] = nodeId;
        o["serverUrl"] = serverUrl;
        o["driveLetter"] = driveLetter;
        o["sdriveDir"] = sdriveDir;
        o["quotaBytes"] = (double)quotaBytes;
        o["agreementAccepted"] = agreementAccepted;
        o["setupDone"] = setupDone;
        o["state"] = state;
        return o;
    }

    void fromJson(const Json& o) {
        nodeId = o["nodeId"].asStr(nodeId);
        serverUrl = o["serverUrl"].asStr(serverUrl);
        driveLetter = o["driveLetter"].asStr(driveLetter);
        sdriveDir = o["sdriveDir"].asStr(sdriveDir);
        quotaBytes = o["quotaBytes"].asInt(quotaBytes);
        agreementAccepted = o["agreementAccepted"].asBool(agreementAccepted);
        setupDone = o["setupDone"].asBool(setupDone);
        state = o["state"].asStr(state);
    }
};

// O/S ga qarab config papkasi
std::string configDir();
std::string configPath();
std::string defaultSdriveDir();
void loadConfig(Config& cfg);
void saveConfig(const Config& cfg);
