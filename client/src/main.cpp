#include "app.h"
#include "net.h"
#include "storage.h"
#include "platform.h"
#include "json.hpp"
#include <chrono>
#include <thread>
#include <iostream>
#include <cstring>
#include <atomic>

using namespace std::chrono_literals;

#ifdef _WIN32
#include <windows.h>
#endif

static std::atomic<bool> g_running{true};

// ---------- server bilan aloqa ----------
static HttpResp apiCall(const std::string& server, const std::string& method,
                        const std::string& path, const std::string& body = "") {
    return http_request(method, server + path, body);
}

static bool g_registered = false;

// ro'yxatdan o'tish (register) — bir marta yoki server node'ni unitganda
static bool ensureRegistered(const Config& cfg) {
    if (g_registered) return true;
    Json req;
    req["id"] = cfg.nodeId;
    req["name"] = "PC-" + (cfg.nodeId.size() >= 4 ? cfg.nodeId.substr(cfg.nodeId.size() - 4) : cfg.nodeId);
    req["os"] =
#ifdef _WIN32
        "Windows";
#elif defined(__APPLE__)
        "macOS";
#else
        "Linux";
#endif
    req["platform"] = "x64";
    req["spaceTotal"] = (double)cfg.quotaBytes;
    req["agreementVersion"] = "1.0";
    req["agreementAccepted"] = cfg.agreementAccepted;
    HttpResp r = apiCall(cfg.serverUrl, "POST", "/api/nodes/register", req.dump());
    if (r.ok()) {
        g_registered = true;
        return true;
    }
    return false;
}

// heartbeat — har 30 soniyada
static void heartbeat(const Config& cfg, AppState& st) {
    Json req;
    req["id"] = cfg.nodeId;
    req["spaceUsed"] = (double)sdriveUsedBytes(cfg.sdriveDir);
    HttpResp r = apiCall(cfg.serverUrl, "POST", "/api/nodes/heartbeat", req.dump());
    std::lock_guard<std::mutex> lk(st.m);
    if (r.ok()) {
        st.online = true;
        st.lastError.clear();
        Json j = Json::parse(r.body);
        st.tasksDue = (int)j["tasksDue"].asInt(0);
        st.earningsCents = j["earningsCents"].asNum(0);
        st.uptimeSec = j["verifiedUptime"].asInt(0);
    } else {
        st.online = false;
        st.lastError = r.error;
        if (r.status == 404) {
            g_registered = false;
        }
    }
}

// navbatdagi vazifalarni bajarish (chunk yuklab olish + saqlash)
static void processTasks(const Config& cfg) {
    std::string server = cfg.serverUrl;
    HttpResp r = apiCall(server, "GET", "/api/nodes/" + cfg.nodeId + "/tasks");
    if (!r.ok()) return;
    Json j = Json::parse(r.body);
    const auto& tasks = j["tasks"].asArr();
    if (tasks.empty()) return;

    LocalIndex idx;
    idx.load(cfg.sdriveDir);

    for (const auto& t : tasks) {
        std::string taskId = t["id"].asStr();
        std::string fileId = t["fileId"].asStr();
        std::string fname = t["fileId"].asStr();
        const auto& chunks = t["chunks"].asArr();

        bool allOk = true;
        for (const auto& c : chunks) {
            int ci = (int)c["index"].asInt();
            int64_t csize = c["size"].asInt();
            // allaqachon mavjud bo'lsa o'tkazib yuboramiz
            if (chunkOk(cfg.sdriveDir, fileId, ci, csize, cfg.nodeId)) continue;
            HttpResp cr = apiCall(server, "GET", "/api/files/" + fileId + "/chunks/" + std::to_string(ci));
            if (!cr.ok() || cr.body.size() != (size_t)csize) {
                allOk = false;
                continue;
            }
            if (!storeChunk(cfg.sdriveDir, fileId, ci, cr.body, cfg.nodeId)) {
                allOk = false;
                continue;
            }
            // SDrive hajmi cheklovi
            if (sdriveUsedBytes(cfg.sdriveDir) > cfg.quotaBytes) {
                allOk = false;
                break;
            }
        }
        if (allOk) {
            std::vector<ChunkInfo> infos;
            for (const auto& c : chunks) infos.push_back({(int)c["index"].asInt(), c["size"].asInt()});
            idx.addFile(cfg.sdriveDir, fileId, fname, infos);
            apiCall(server, "POST", "/api/nodes/" + cfg.nodeId + "/tasks/" + taskId + "/done");
        }
    }
}

// foydalanuvchi o'chirib tashlagan fayllarni qayta yuklab olish
static void repairScan(const Config& cfg) {
    LocalIndex idx;
    idx.load(cfg.sdriveDir);
    auto missing = findMissingFiles(cfg.sdriveDir, idx, cfg.nodeId);
    if (missing.empty()) return;
    for (const auto& fid : missing) {
        Json req;
        req["fileId"] = fid;
        apiCall(cfg.serverUrl, "POST", "/api/nodes/" + cfg.nodeId + "/repair", req.dump());
    }
}

// ---------- main ----------
int main(int argc, char** argv) {
    netInit();

    AppState st;
    loadConfig(st.cfg);
    saveConfig(st.cfg);

    // Windowsda konsol oynasini yashirish (--background)
#ifdef _WIN32
    if (argc > 1 && std::string(argv[1]) == "--background") {
        HWND h = GetConsoleWindow();
        if (h) ShowWindow(h, SW_HIDE);
    }
#endif

    startLocalServer(st);

    // birinchi marta: brauzerni ochamiz (sozlash oynasi)
    if (!st.cfg.setupDone || !st.cfg.agreementAccepted) {
        std::thread([] { std::this_thread::sleep_for(700ms); openBrowser("http://127.0.0.1:48673/"); }).detach();
    }

    std::cout << "SDrive Cluster mijoz ishga tushdi" << std::endl;
    std::cout << "Boshqaruv paneli: http://127.0.0.1:48673/" << std::endl;
    std::cout << "Node ID: " << st.cfg.nodeId << std::endl;
    std::cout << "Server: " << st.cfg.serverUrl << std::endl;

    // avtostart (faqat --background rejimida)
    if (argc > 1 && std::string(argv[1]) == "--background") {
        std::string err;
        installAutostart(currentExePath(), err);
    }

    int scanCounter = 0;
    while (g_running.load()) {
        fprintf(stderr, "[sdrive] loop iteration\n");
        // Eslatma: mutexni uzoq ushlab turmang — UI server ham st.m ishlatadi
        Config cur;
        {
            std::lock_guard<std::mutex> lk(st.m);
            cur = st.cfg;
        }
        if (cur.state != "running" || !cur.setupDone || !cur.agreementAccepted) {
            fprintf(stderr, "[sdrive] state=%s kutish\n", cur.state.c_str());
            std::this_thread::sleep_for(2s);
            continue;
        }
        fprintf(stderr, "[sdrive] register boshlanmoqda\n");
        ensureRegistered(cur);
        fprintf(stderr, "[sdrive] heartbeat boshlanmoqda\n");
        heartbeat(cur, st);
        fprintf(stderr, "[sdrive] heartbeat tugadi online=%d\n", (int)st.online);
        processTasks(cur);
        fprintf(stderr, "[sdrive] tasks tugadi\n");
        if (++scanCounter % 10 == 0) repairScan(cur); // har ~5 daqiqada
        std::this_thread::sleep_for(30s);
    }
    stopLocalServer();
    netCleanup();
    return 0;
}
