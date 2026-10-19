// Daemon mantiqi: server bilan aloqa, replikatsiya vazifalari, saqlash isboti (audit)
#include "app.h"
#include "crypto.h"
#include "platform.h"
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <algorithm>

using namespace std::chrono_literals;

static int64_t nowSec() { return (int64_t)time(nullptr); }

static Config snapshot(AppState& st) {
    std::lock_guard<std::mutex> lk(st.m);
    return st.cfg;
}

static bool agentActive(const Config& c) {
    return c.state == "running" && c.setupDone && c.agreementAccepted;
}

// ishni to'xtatish kerakmi (dastur yopilmoqda yoki o'chirilmoqda)
static bool stopping(AppState& st) { return !st.running || st.uninstalling; }

static void setActivity(AppState& st, const std::string& a) {
    std::lock_guard<std::mutex> lk(st.m);
    st.activity = a;
}

static std::string describe(const HttpResp& r) {
    if (r.status == 0) return r.error.empty() ? "server bilan aloqa yo'q" : r.error;
    bool ok = false;
    Json j = Json::parse(r.body, &ok);
    std::string msg = ok ? j["error"].asStr() : "";
    return "HTTP " + std::to_string(r.status) + (msg.empty() ? "" : ": " + msg);
}

HttpResp serverCall(AppState& st, const std::string& method, const std::string& path, const std::string& body,
                    int timeoutSec) {
    Config c = snapshot(st);
    HttpReq r;
    r.method = method;
    r.url = c.serverUrl + path;
    r.body = body;
    r.timeoutSec = timeoutSec;
    if (!c.token.empty()) r.headers.push_back({"Authorization", "Bearer " + c.token});
    return http_do(r);
}

void wakeAgent(AppState& st) {
    {
        std::lock_guard<std::mutex> lk(st.m);
        st.wakeHb = true;
        st.wakeWorker = true;
    }
    st.cv.notify_all();
}

static void wakeWorker(AppState& st) {
    {
        std::lock_guard<std::mutex> lk(st.m);
        st.wakeWorker = true;
    }
    st.cv.notify_all();
}

// ---------------- Ro'yxatdan o'tish ----------------

static bool ensureRegistered(AppState& st) {
    Config c = snapshot(st);
    Json req;
    req["id"] = c.nodeId;
    req["name"] = hostName();
    req["os"] = osName();
    req["platform"] = archName();
    req["version"] = SDRIVE_VERSION;
    req["cpuCores"] = cpuCores();
    req["spaceTotal"] = (double)c.quotaBytes;
    req["agreementVersion"] = c.agreementVersion;
    req["agreementAccepted"] = c.agreementAccepted;
    HttpResp r = serverCall(st, "POST", "/api/nodes/register", req.dump());
    bool ok = false;
    Json j = Json::parse(r.body, &ok);

    if (r.ok()) {
        std::lock_guard<std::mutex> lk(st.m);
        std::string tok = j["token"].asStr();
        if (!tok.empty()) {
            st.cfg.token = tok;
            saveConfig(st.cfg);
        }
        st.registered = true;
        st.online = true;
        st.lastError.clear();
        st.heartbeatIntervalMs = (int)j["heartbeatIntervalMs"].asInt(30000);
        if (st.heartbeatIntervalMs < 1000) st.heartbeatIntervalMs = 1000;
        logf("[agent] serverda ro'yxatdan o'tildi: %s (%s)", c.nodeId.c_str(), c.serverUrl.c_str());
        return true;
    }
    if (r.status == 409 || r.status == 410) {
        // ID boshqa kompyuterga tegishli yoki serverda o'chirilgan — yangi identifikator bilan qo'shilamiz.
        // Eski ma'lumotlar eski identifikatorga tayinlangan edi, ular tozalanadi.
        logf("[agent] tugun ID qabul qilinmadi (%s) — yangi ID yaratiladi", describe(r).c_str());
        st.storage.wipe();
        st.usedBytes = 0;
        std::lock_guard<std::mutex> lk(st.m);
        st.cfg.nodeId = newNodeId();
        st.cfg.token.clear();
        saveConfig(st.cfg);
        st.lastError = "Tugun qayta ro'yxatdan o'tkazilmoqda";
        return false;
    }
    if (r.status == 412) {
        std::lock_guard<std::mutex> lk(st.m);
        st.cfg.agreementAccepted = false;
        st.cfg.state = "agreement";
        saveConfig(st.cfg);
        st.lastError = "Shartnomaning yangi versiyasi chiqdi — qayta o'qib, rozilik bildiring";
        logf("[agent] %s", st.lastError.c_str());
        return false;
    }
    std::lock_guard<std::mutex> lk(st.m);
    st.online = false;
    st.lastError = "Ro'yxatdan o'tib bo'lmadi: " + describe(r);
    return false;
}

// ---------------- Audit (saqlash isboti) ----------------

static void answerAudits(AppState& st, const Json& audits) {
    Json results(Json::Array{});
    for (const auto& a : audits.asArr()) {
        Json res;
        res["id"] = a["id"].asStr();
        std::string plain, nonce;
        if (st.storage.loadChunk(a["fileId"].asStr(), (int)a["index"].asInt(), plain) && fromHex(a["nonce"].asStr(), nonce)) {
            Sha256 h;
            h.update(nonce);
            h.update(plain);
            res["proof"] = h.hexDigest();
        } else {
            res["error"] = "chunk topilmadi yoki buzilgan";
        }
        results.push(res);
    }
    Json req;
    req["results"] = results;
    Config c = snapshot(st);
    HttpResp r = serverCall(st, "POST", "/api/nodes/" + c.nodeId + "/audits", req.dump());
    if (!r.ok()) logf("[agent] audit javobi yuborilmadi: %s", describe(r).c_str());
}

// ---------------- Heartbeat ----------------

static void heartbeatOnce(AppState& st) {
    Config c = snapshot(st);
    int64_t idle = userIdleSeconds();
    bool active = idle >= 0 && idle < 60; // foydalanuvchi kompyuterdan foydalanmoqda (shartnoma 2.4)
    st.userActive = active;
    Json req;
    req["spaceUsed"] = (double)st.usedBytes.load();
    req["userActive"] = active;
    HttpResp r = serverCall(st, "POST", "/api/nodes/" + c.nodeId + "/heartbeat", req.dump());
    bool ok = false;
    Json j = Json::parse(r.body, &ok);
    if (r.ok() && ok) {
        int due;
        {
            std::lock_guard<std::mutex> lk(st.m);
            st.online = true;
            st.lastError.clear();
            st.lastHeartbeat = nowSec();
            st.tasksDue = due = (int)j["tasksDue"].asInt(0);
            st.earningsCents = j["earningsCents"].asNum(0);
            st.uptimeSec = j["verifiedUptime"].asInt(0);
            st.serverStoredBytes = j["storedBytes"].asInt(0);
            st.serverStoredFiles = (int)j["storedFiles"].asInt(0);
            st.payoutOpen = j["payoutOpen"].asBool(false);
            st.payoutOpensAt = j["payoutOpensAt"].asInt(0);
            int iv = (int)j["heartbeatIntervalMs"].asInt(st.heartbeatIntervalMs);
            if (iv >= 1000) st.heartbeatIntervalMs = iv;
        }
        if (!j["audits"].asArr().empty()) answerAudits(st, j["audits"]);
        if (due > 0) wakeWorker(st);
        return;
    }
    std::lock_guard<std::mutex> lk(st.m);
    st.online = false;
    st.lastError = describe(r);
    if (r.status == 401 || r.status == 404 || r.status == 410) st.registered = false; // qayta ro'yxatdan o'tamiz
}

static void heartbeatLoop(AppState& st) {
    while (st.running) {
        Config c = snapshot(st);
        int waitMs = 1000;
        if (agentActive(c) && !st.uninstalling) {
            bool reg;
            {
                std::lock_guard<std::mutex> lk(st.m);
                reg = st.registered;
            }
            if (!reg && ensureRegistered(st)) {
                reg = true;
                wakeWorker(st);
            }
            if (reg) heartbeatOnce(st);
            std::lock_guard<std::mutex> lk(st.m);
            waitMs = st.registered ? st.heartbeatIntervalMs : 5000;
        }
        std::unique_lock<std::mutex> lk(st.m);
        st.cv.wait_for(lk, std::chrono::milliseconds(waitMs), [&] { return !st.running || st.wakeHb; });
        st.wakeHb = false;
    }
}

// ---------------- Vazifalar ----------------

// Foydalanuvchi faol bo'lsa yuklab olishni sekinlashtiramiz (shartnoma 2.4)
static bool throttle(AppState& st) {
    if (!st.userActive) return !stopping(st);
    for (int i = 0; i < 30 && !stopping(st); i++) std::this_thread::sleep_for(100ms);
    return !stopping(st);
}

enum TaskResult { TaskOk, TaskFail, TaskAborted };

static std::vector<ChunkInfo> chunkInfos(const Json& t) {
    std::vector<ChunkInfo> out;
    for (const auto& c : t["chunks"].asArr()) out.push_back({(int)c["index"].asInt(), c["size"].asInt(), c["sha256"].asStr()});
    return out;
}

static TaskResult runTask(AppState& st, const Json& t, std::string& err) {
    Config c = snapshot(st);
    std::string kind = t["kind"].asStr();
    std::string fid = t["fileId"].asStr();
    std::string taskId = t["id"].asStr();
    if (!Storage::validFileId(fid)) {
        err = "fayl ID noto'g'ri";
        return TaskFail;
    }
    std::vector<ChunkInfo> chunks = chunkInfos(t);

    if (kind == "replica" || kind == "repair") {
        int64_t need = 0;
        for (const auto& ch : chunks)
            if (!st.storage.chunkValid(fid, ch)) need += ch.size + Storage::kOverhead;
        if (st.usedBytes.load() + need > c.quotaBytes) {
            err = "SDrive uchun ajratilgan joy yetarli emas";
            return TaskFail;
        }
        for (size_t i = 0; i < chunks.size(); i++) {
            const ChunkInfo& ch = chunks[i];
            if (st.storage.chunkValid(fid, ch)) continue;
            if (!throttle(st)) return TaskAborted;
            setActivity(st, "Yuklab olinmoqda: " + fid + " (" + std::to_string(i + 1) + "/" + std::to_string(chunks.size()) + ")" +
                                (st.userActive ? " — sekinlashtirilgan" : ""));
            HttpReq rq;
            rq.url = c.serverUrl + "/api/nodes/" + c.nodeId + "/files/" + fid + "/chunks/" + std::to_string(ch.index);
            rq.headers.push_back({"Authorization", "Bearer " + c.token});
            rq.timeoutSec = 120;
            rq.maxBody = (size_t)ch.size + 4096;
            HttpResp r = http_do(rq);
            if (!r.ok()) {
                err = "chunk " + std::to_string(ch.index) + ": " + describe(r);
                return TaskFail;
            }
            if ((int64_t)r.body.size() != ch.size) {
                err = "chunk " + std::to_string(ch.index) + ": hajm mos emas";
                return TaskFail;
            }
            if (sha256Hex(r.body) != ch.sha256) {
                err = "chunk " + std::to_string(ch.index) + ": sha256 mos emas";
                return TaskFail;
            }
            if (!st.storage.storeChunk(fid, ch.index, r.body, err)) return TaskFail;
            st.usedBytes += ch.size + Storage::kOverhead;
            if (stopping(st)) return TaskAborted;
        }
        st.storage.addFile(fid, chunks);
        return TaskOk;
    }
    if (kind == "delete") {
        st.storage.removeFile(fid);
        return TaskOk;
    }
    if (kind == "restore") {
        for (const auto& ch : chunks) {
            if (stopping(st)) return TaskAborted;
            std::string plain;
            if (!st.storage.loadChunk(fid, ch.index, plain) || sha256Hex(plain) != ch.sha256) {
                err = "chunk " + std::to_string(ch.index) + " lokal nusxasi yo'q yoki buzilgan";
                return TaskFail;
            }
            setActivity(st, "Server nusxasi tiklanmoqda: " + fid);
            HttpReq rq;
            rq.method = "PUT";
            rq.url = c.serverUrl + "/api/nodes/" + c.nodeId + "/tasks/" + taskId + "/chunks/" + std::to_string(ch.index);
            rq.body = plain;
            rq.contentType = "application/octet-stream";
            rq.headers.push_back({"Authorization", "Bearer " + c.token});
            rq.timeoutSec = 120;
            HttpResp r = http_do(rq);
            if (!r.ok()) {
                err = "restore chunk " + std::to_string(ch.index) + ": " + describe(r);
                return TaskFail;
            }
        }
        return TaskOk;
    }
    err = "noma'lum vazifa turi: " + kind;
    return TaskFail;
}

static void processTasks(AppState& st) {
    Config c = snapshot(st);
    HttpResp r = serverCall(st, "GET", "/api/nodes/" + c.nodeId + "/tasks");
    bool ok = false;
    Json j = Json::parse(r.body, &ok);
    if (!r.ok() || !ok) return;
    for (const auto& t : j["tasks"].asArr()) {
        if (stopping(st)) break;
        std::string err;
        std::string taskId = t["id"].asStr();
        std::string fid = t["fileId"].asStr();
        TaskResult res = runTask(st, t, err);
        if (res == TaskAborted) break;
        if (res == TaskOk) {
            HttpResp d = serverCall(st, "POST", "/api/nodes/" + c.nodeId + "/tasks/" + taskId + "/done", "{}");
            if (d.status == 410) st.storage.removeFile(fid); // vazifa bekor qilingan / fayl o'chirilgan
            logf("[agent] vazifa bajarildi: %s %s", t["kind"].asStr().c_str(), fid.c_str());
        } else {
            Json fj;
            fj["error"] = err;
            serverCall(st, "POST", "/api/nodes/" + c.nodeId + "/tasks/" + taskId + "/fail", fj.dump());
            logf("[agent] vazifa bajarilmadi: %s %s — %s", t["kind"].asStr().c_str(), fid.c_str(), err.c_str());
        }
        st.usedBytes = st.storage.usedBytes();
    }
    setActivity(st, "");
}

// Foydalanuvchi o'chirgan yoki buzilgan fayllarni topib, qayta yuklashni so'raydi (shartnoma 3.2)
static void repairScan(AppState& st) {
    setActivity(st, "Saqlangan fayllar tekshirilmoqda");
    std::vector<std::string> missing = st.storage.quickMissing();
    for (const auto& fid : st.storage.verifyBudget(256LL << 20))
        if (std::find(missing.begin(), missing.end(), fid) == missing.end()) missing.push_back(fid);
    if (!missing.empty()) {
        Config c = snapshot(st);
        Json req;
        Json arr(Json::Array{});
        for (const auto& f : missing) arr.push(f);
        req["fileIds"] = arr;
        HttpResp r = serverCall(st, "POST", "/api/nodes/" + c.nodeId + "/repair", req.dump());
        bool ok = false;
        Json j = Json::parse(r.body, &ok);
        if (r.ok() && ok) {
            logf("[agent] %d ta fayl yo'qolgan — qayta yuklash so'raldi", (int)missing.size());
            for (const auto& u : j["unknown"].asArr()) st.storage.removeFile(u.asStr());
            wakeWorker(st);
        }
    }
    int orphans = st.storage.removeOrphans();
    if (orphans > 0) logf("[agent] %d ta tugallanmagan papka tozalandi", orphans);
    st.usedBytes = st.storage.usedBytes();
    setActivity(st, "");
}

static void workerLoop(AppState& st) {
    int64_t lastScan = 0;
    int64_t scanEvery = 300; // har 5 daqiqada lokal fayllarni tekshirish
    if (const char* v = getenv("SDRIVE_SCAN_INTERVAL")) scanEvery = atoll(v) > 0 ? atoll(v) : scanEvery;
    auto waitFor = std::chrono::seconds(scanEvery < 60 ? scanEvery : 60);
    while (st.running) {
        {
            std::unique_lock<std::mutex> lk(st.m);
            st.cv.wait_for(lk, waitFor, [&] { return !st.running || st.wakeWorker; });
            st.wakeWorker = false;
        }
        if (!st.running) break;
        Config c = snapshot(st);
        bool reg;
        {
            std::lock_guard<std::mutex> lk(st.m);
            reg = st.registered;
        }
        if (!agentActive(c) || !reg || st.uninstalling) continue;
        st.workerBusy = true;
        processTasks(st);
        if (!stopping(st) && nowSec() - lastScan >= scanEvery) {
            repairScan(st);
            lastScan = nowSec();
        }
        st.workerBusy = false;
    }
}

void startAgent(AppState& st) {
    st.hbThread = std::thread(heartbeatLoop, std::ref(st));
    st.workerThread = std::thread(workerLoop, std::ref(st));
}

void stopAgent(AppState& st) {
    st.running = false;
    st.cv.notify_all();
    if (st.hbThread.joinable()) st.hbThread.join();
    if (st.workerThread.joinable()) st.workerThread.join();
}

// ---------------- Payout va uninstall ----------------

HttpResp claimPayout(AppState& st) {
    Config c = snapshot(st);
    return serverCall(st, "POST", "/api/nodes/" + c.nodeId + "/payout", "{}");
}

std::string uninstallNode(AppState& st, double& forfeitedCents, bool& serverOk) {
    // avval fon ishini to'xtatamiz: yangi chunk yozilmasin
    st.uninstalling = true;
    st.cv.notify_all();
    for (int i = 0; i < 150 && st.workerBusy; i++) std::this_thread::sleep_for(100ms);
    Config c = snapshot(st);
    forfeitedCents = 0;
    HttpResp r = serverCall(st, "POST", "/api/nodes/" + c.nodeId + "/uninstall", "{}", 15);
    serverOk = r.ok();
    if (serverOk) {
        bool ok = false;
        Json j = Json::parse(r.body, &ok);
        forfeitedCents = j["forfeitedCents"].asNum(0);
    }
    removeAutostart();
    unmountSdrive(c.driveLetter, c.sdriveDir);
    st.storage.wipe(); // klaster ma'lumotlari (shifrlangan nusxalar) diskdan o'chiriladi
    st.usedBytes = 0;
    {
        std::lock_guard<std::mutex> lk(st.m);
        st.cfg.state = "uninstalled";
        st.cfg.token.clear();
        st.registered = false;
        st.online = false;
        saveConfig(st.cfg);
    }
    logf("[agent] dastur o'chirildi (server: %s)", serverOk ? "ok" : describe(r).c_str());
    return serverOk ? "" : describe(r);
}
