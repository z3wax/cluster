#pragma once
// Umumiy holat — daemon va lokal server o'rtasida bo'lishiladi
#include "config.h"
#include <mutex>
#include <string>
#include <cstdint>

struct AppState {
    std::mutex m;
    Config cfg;
    bool online = false;
    int tasksDue = 0;
    int64_t uptimeSec = 0;
    double earningsCents = 0;
    std::string lastError;
};

void startLocalServer(AppState& st);
void stopLocalServer();
