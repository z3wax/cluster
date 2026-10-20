#pragma once
// Umumiy holat — daemon oqimlari va lokal UI server o'rtasida bo'lishiladi
#include "config.h"
#include "storage.h"
#include "net.h"
#include "compute.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <cstdint>

struct AppState {
    std::mutex m;               // quyidagi maydonlar uchun (storage o'z mutex'iga ega)
    std::condition_variable cv; // oqimlarni uyg'otish
    Config cfg;
    Storage storage;
    std::atomic<bool> running{true};
    std::atomic<int64_t> usedBytes{0};
    std::atomic<bool> userActive{false};
    std::atomic<int64_t> exitAt{0}; // >0 bo'lsa — shu vaqtda (unix) dastur yopiladi
    std::atomic<bool> uninstalling{false}; // uninstall boshlandi — yangi ish boshlanmaydi
    std::atomic<bool> workerBusy{false};

    bool background = false;
    bool wakeHb = false;
    bool wakeWorker = false;

    // holat (server javoblaridan)
    bool registered = false;
    bool online = false;
    int tasksDue = 0;
    int64_t uptimeSec = 0;
    double earningsCents = 0;
    int64_t serverStoredBytes = 0;
    int serverStoredFiles = 0;
    bool payoutOpen = false;
    int64_t payoutOpensAt = 0;
    int64_t lastHeartbeat = 0;
    int heartbeatIntervalMs = 30000;
    std::string lastError;
    std::string activity;

    std::thread hbThread, workerThread;

    // hisoblash (GPU/CPU) ishchisi
    ComputeAgent computeAgent;
    std::string computeEngineName;
    bool computeActive = false;
};

// Daemon: ro'yxatdan o'tish, heartbeat, auditlar, vazifalar, fayllarni tekshirish
void startAgent(AppState& st);
void stopAgent(AppState& st);
void wakeAgent(AppState& st);

// Serverga tugun tokeni bilan so'rov
HttpResp serverCall(AppState& st, const std::string& method, const std::string& path, const std::string& body = "",
                    int timeoutSec = 30);

// Shartnoma 6: payout va dasturni o'chirish
HttpResp claimPayout(AppState& st);
std::string uninstallNode(AppState& st, double& forfeitedCents, bool& serverOk);

// Lokal boshqaruv paneli (127.0.0.1)
bool startLocalServer(AppState& st, int port, std::string& err);
void stopLocalServer();
