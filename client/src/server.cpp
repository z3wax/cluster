// Lokal boshqaruv paneli: faqat 127.0.0.1 da tinglaydi.
// Himoya: Host sarlavhasi tekshiruvi (DNS rebinding), POST uchun X-SDrive-UI sarlavhasi va
// Origin tekshiruvi (boshqa saytlar CSRF orqali buyruq yubora olmaydi).
#include "app.h"
#include "crypto.h"
#include "platform.h"
#include "ui.h"
#include "json.hpp"
#include <atomic>
#include <thread>
#include <cstring>
#include <cctype>
#include <cerrno>
#include <ctime>
#include <map>
#include <sstream>
#include <filesystem>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
#define CLOSE_SOCK(s) closesocket(s)
#define SOCK_ERR INVALID_SOCKET
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using sock_t = int;
#define CLOSE_SOCK(s) ::close(s)
#define SOCK_ERR (-1)
#endif

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

namespace fs = std::filesystem;

static std::atomic<bool> g_run{true};
static std::atomic<int> g_active{0}; // ishlov berilayotgan ulanishlar
static sock_t g_listen = SOCK_ERR;
static int g_port = 0;
static std::thread g_acceptThread;

struct Request {
    std::string method, path, body;
    std::map<std::string, std::string> headers; // kichik harfli kalitlar
    std::string header(const std::string& k) const {
        auto it = headers.find(k);
        return it == headers.end() ? "" : it->second;
    }
};

static const char* reason(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 502: return "Bad Gateway";
        default: return "Error";
    }
}

static void sendAll(sock_t fd, const std::string& data) {
    const char* p = data.data();
    size_t n = data.size();
    while (n > 0) {
        int w = (int)::send(fd, p, (int)n, SEND_FLAGS);
        if (w <= 0) return;
        p += w;
        n -= (size_t)w;
    }
}

static void respond(sock_t fd, int code, const std::string& ct, const std::string& body) {
    std::string head = "HTTP/1.1 " + std::to_string(code) + " " + reason(code) + "\r\n" +
                       "Content-Type: " + ct + "\r\n" +
                       "Content-Length: " + std::to_string(body.size()) + "\r\n" +
                       "Cache-Control: no-store\r\n"
                       "X-Content-Type-Options: nosniff\r\n"
                       "X-Frame-Options: DENY\r\n"
                       "Referrer-Policy: no-referrer\r\n"
                       "Connection: close\r\n\r\n";
    sendAll(fd, head + body);
}

static void respondJSON(sock_t fd, int code, const Json& j) { respond(fd, code, "application/json; charset=utf-8", j.dump()); }

static Json errJson(const std::string& msg) {
    Json o;
    o["ok"] = false;
    o["error"] = msg;
    return o;
}

static Json okJson() {
    Json o;
    o["ok"] = true;
    return o;
}

// Javob tanasini serverdan (Json yoki xato) ajratib olish
static Json serverJson(const HttpResp& r) {
    bool ok = false;
    Json j = Json::parse(r.body, &ok);
    if (ok && j.isObj()) return j;
    return errJson(r.status ? "server javobi: HTTP " + std::to_string(r.status) : (r.error.empty() ? "server bilan aloqa yo'q" : r.error));
}

// ---------------- API ----------------

static Json statusJson(AppState& st) {
    Json o;
    std::string dir;
    {
        std::lock_guard<std::mutex> lk(st.m);
        const Config& c = st.cfg;
        o["nodeId"] = c.nodeId;
        o["serverUrl"] = c.serverUrl;
        o["state"] = c.state;
        o["setupDone"] = c.setupDone;
        o["agreementAccepted"] = c.agreementAccepted;
        o["agreementVersion"] = c.agreementVersion;
        o["sdriveDir"] = c.sdriveDir;
        o["driveLetter"] = c.driveLetter;
        o["quotaBytes"] = (double)c.quotaBytes;
        o["registered"] = st.registered;
        o["online"] = st.online;
        o["tasksDue"] = st.tasksDue;
        o["uptimeSec"] = (double)st.uptimeSec;
        o["earningsCents"] = st.earningsCents;
        o["storedBytes"] = (double)st.serverStoredBytes;
        o["storedFiles"] = st.serverStoredFiles;
        o["payoutOpen"] = st.payoutOpen;
        o["payoutOpensAt"] = (double)st.payoutOpensAt;
        o["lastHeartbeat"] = (double)st.lastHeartbeat;
        o["lastError"] = st.lastError;
        o["activity"] = st.activity;
        dir = c.sdriveDir;
    }
    int64_t freeB = 0;
    o["diskFreeBytes"] = diskFreeBytes(dir, freeB) ? (double)freeB : -1.0;
    o["usedBytes"] = (double)st.usedBytes.load();
    o["localFiles"] = (double)st.storage.fileCount();
    o["userActive"] = st.userActive.load();
    o["computeEnabled"] = st.cfg.computeEnabled;
    o["computeActive"] = st.computeActive;
    o["computeEngine"] = st.computeEngineName;
    o["platform"] = platformId();
    o["version"] = SDRIVE_VERSION;
    o["defaultDir"] = defaultSdriveDir();
    o["freeLetters"] = freeDriveLetters();
    o["autostart"] = autostartInstalled();
    o["tls"] = tlsAvailable();
    o["serverTime"] = (double)time(nullptr);
    return o;
}

static void handleSetup(AppState& st, const Request& rq, sock_t fd) {
    bool ok = false;
    Json req = Json::parse(rq.body, &ok);
    if (!ok) return respondJSON(fd, 400, errJson("noto'g'ri so'rov"));
    double quotaGb = req["quotaGb"].asNum(0);
    std::string dir = req["sdriveDir"].asStr();
    std::string letter = req["driveLetter"].asStr("S");
    std::string server = req["serverUrl"].asStr();
    while (!server.empty() && (server.back() == '/' || server.back() == ' ')) server.pop_back();
    while (!server.empty() && server.front() == ' ') server.erase(server.begin());

    if (quotaGb < 1 || quotaGb > 1000000) return respondJSON(fd, 400, errJson("hajm kamida 1 GB bo'lishi kerak"));
    if (dir.empty()) dir = defaultSdriveDir();
    if (!fs::path(dir).is_absolute()) return respondJSON(fd, 400, errJson("papka to'liq yo'l bilan ko'rsatilishi kerak"));
    if (letter.size() != 1 || !std::isalpha((unsigned char)letter[0])) letter = "S";
    letter[0] = (char)std::toupper((unsigned char)letter[0]);
    if (!server.empty()) {
        ParsedUrl u;
        if (!parseUrl(server, u)) return respondJSON(fd, 400, errJson("server manzili noto'g'ri (masalan: http://server:8080)"));
        if (u.scheme == "https" && !tlsAvailable())
            return respondJSON(fd, 400, errJson("bu build HTTPS ni qo'llamaydi — OpenSSL bilan build qiling yoki http:// ishlating"));
    }
    int64_t quota = (int64_t)(quotaGb * (double)(1LL << 30));

    Config cur;
    {
        std::lock_guard<std::mutex> lk(st.m);
        cur = st.cfg;
    }
    bool sameDir = cur.setupDone && fs::path(cur.sdriveDir) == fs::path(dir);
    int64_t freeB = 0;
    if (diskFreeBytes(dir, freeB)) {
        int64_t avail = freeB + (sameDir ? st.usedBytes.load() : 0);
        if (quota > avail) {
            char msg[160];
            snprintf(msg, sizeof(msg), "diskda faqat %.1f GB bo'sh joy bor", (double)avail / (double)(1LL << 30));
            return respondJSON(fd, 400, errJson(msg));
        }
    }
    std::string err;
    if (cur.setupDone && (!sameDir || cur.driveLetter != letter)) unmountSdrive(cur.driveLetter, cur.sdriveDir);
    if (!ensureSdrive(dir, platformId() == "windows" ? letter : "", err)) return respondJSON(fd, 400, errJson(err));

    bool serverChanged = !server.empty() && server != cur.serverUrl;
    if (cur.setupDone && (!sameDir || serverChanged)) {
        // eski papka/serverdagi klaster ma'lumotlari endi kerak emas — server ularni boshqa tugunlarga ko'chiradi
        st.storage.wipe();
    }
    if (!st.storage.open(dir, cur.storageKey, err)) return respondJSON(fd, 400, errJson(err));
    st.usedBytes = st.storage.usedBytes();
    {
        std::lock_guard<std::mutex> lk(st.m);
        st.cfg.driveLetter = letter;
        st.cfg.sdriveDir = dir;
        st.cfg.quotaBytes = quota;
        if (serverChanged) {
            st.cfg.serverUrl = server;
            st.cfg.token.clear();
        }
        st.cfg.setupDone = true;
        st.cfg.state = st.cfg.agreementAccepted ? "running" : "agreement";
        st.registered = false; // yangi hajm serverga qayta yuboriladi
        saveConfig(st.cfg);
    }
    logf("[ui] sozlandi: %s, %.1f GB", dir.c_str(), quotaGb);
    wakeAgent(st);
    respondJSON(fd, 200, okJson());
}

static void handleAccept(AppState& st, const Request& rq, sock_t fd) {
    bool ok = false;
    Json req = Json::parse(rq.body, &ok);
    std::string version = ok ? req["version"].asStr() : "";
    if (version.empty()) return respondJSON(fd, 400, errJson("shartnoma versiyasi kerak"));
    std::string extra;
    {
        std::lock_guard<std::mutex> lk(st.m);
        st.cfg.agreementAccepted = true;
        st.cfg.agreementVersion = version;
        st.cfg.state = st.cfg.setupDone ? "running" : "setup";
        st.registered = false;
        saveConfig(st.cfg);
    }
    if (configDirOverridden()) extra = "--config-dir \"" + configDir() + "\"";
    std::string err;
    if (!installAutostart(currentExePath(), extra, err)) logf("[ui] avtostart o'rnatilmadi: %s", err.c_str());
    logf("[ui] shartnoma v%s qabul qilindi", version.c_str());
    wakeAgent(st);
    respondJSON(fd, 200, okJson());
}

static void handleRoute(AppState& st, const Request& rq, sock_t fd) {
    std::string path = rq.path.substr(0, rq.path.find('?'));
    if (rq.method == "GET" && (path == "/" || path == "/index.html")) {
        return respond(fd, 200, "text/html; charset=utf-8", UI_HTML);
    }
    if (rq.method == "GET" && path == "/api/status") return respondJSON(fd, 200, statusJson(st));
    if (rq.method == "GET" && path == "/api/agreement") {
        std::string server;
        {
            std::lock_guard<std::mutex> lk(st.m);
            server = st.cfg.serverUrl;
        }
        HttpReq r;
        r.url = server + "/api/agreement";
        r.timeoutSec = 15;
        HttpResp resp = http_do(r);
        if (resp.ok()) return respond(fd, 200, "application/json; charset=utf-8", resp.body);
        return respondJSON(fd, 502, errJson("server bilan bog'lanib bo'lmadi (" + server + "): " +
                                                (resp.error.empty() ? "HTTP " + std::to_string(resp.status) : resp.error)));
    }
    if (rq.method != "POST") return respondJSON(fd, 404, errJson("topilmadi"));

    if (path == "/api/setup") return handleSetup(st, rq, fd);
    if (path == "/api/accept") return handleAccept(st, rq, fd);
    if (path == "/api/claim") {
        HttpResp r = claimPayout(st);
        Json j = serverJson(r);
        return respondJSON(fd, r.ok() ? 200 : (r.status ? r.status : 502), j);
    }
    if (path == "/api/uninstall") {
        double forfeited = 0;
        bool serverOk = false;
        std::string err = uninstallNode(st, forfeited, serverOk);
        Json o = okJson();
        o["forfeitedCents"] = forfeited;
        o["serverOk"] = serverOk;
        if (!serverOk) o["warning"] = "server bilan bog'lanib bo'lmadi: " + err;
        respondJSON(fd, 200, o);
        st.exitAt = (int64_t)time(nullptr) + 2; // javob yetib borgach dastur yopiladi
        return;
    }
    if (path == "/api/sync") {
        wakeAgent(st);
        return respondJSON(fd, 200, okJson());
    }
    respondJSON(fd, 404, errJson("topilmadi"));
}

// ---------------- HTTP so'rovni o'qish ----------------

static bool readRequest(sock_t fd, Request& rq) {
#ifdef _WIN32
    DWORD to = 10000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
#else
    timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    std::string buf;
    char tmp[8192];
    size_t hEnd = std::string::npos;
    while (buf.size() < 64 * 1024) {
        int n = (int)::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, (size_t)n);
        hEnd = buf.find("\r\n\r\n");
        if (hEnd != std::string::npos) break;
    }
    if (hEnd == std::string::npos) return false;

    std::istringstream hs(buf.substr(0, hEnd));
    std::string line;
    std::getline(hs, line);
    std::istringstream rl(line);
    rl >> rq.method >> rq.path;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string k = line.substr(0, c), v = line.substr(c + 1);
        for (auto& ch : k) ch = (char)std::tolower((unsigned char)ch);
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        rq.headers[k] = v;
    }
    size_t len = (size_t)std::strtoull(rq.header("content-length").c_str(), nullptr, 10);
    if (len > (1u << 20)) return false;
    rq.body = buf.substr(hEnd + 4);
    while (rq.body.size() < len) {
        int n = (int)::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        rq.body.append(tmp, (size_t)n);
    }
    rq.body.resize(len);
    return !rq.method.empty() && !rq.path.empty();
}

static bool allowedHost(const std::string& host) {
    std::string p = std::to_string(g_port);
    return host == "127.0.0.1:" + p || host == "localhost:" + p;
}

static void handleConn(AppState* st, sock_t c) {
    struct Guard {
        ~Guard() { g_active--; }
    } guard;
    Request rq;
    if (readRequest(c, rq)) {
        if (!allowedHost(rq.header("host"))) {
            respondJSON(c, 403, errJson("ruxsat yo'q (Host)"));
        } else if (rq.method == "POST" &&
                   (rq.header("x-sdrive-ui") != "1" ||
                    (!rq.header("origin").empty() && !allowedHost(rq.header("origin").substr(rq.header("origin").find("//") + 2))))) {
            respondJSON(c, 403, errJson("ruxsat yo'q (CSRF himoyasi)"));
        } else {
            handleRoute(*st, rq, c);
        }
    }
    CLOSE_SOCK(c);
}

static int lastSockErr() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool startLocalServer(AppState& st, int port, std::string& err) {
    netInit();
    g_listen = ::socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen == SOCK_ERR) {
        err = "socket() xato: " + std::to_string(lastSockErr());
        return false;
    }
#ifdef _WIN32
    int one = 1; // boshqa jarayon shu portni egallay olmasin (bitta nusxa kafolati)
    setsockopt(g_listen, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&one, sizeof(one));
#else
    int one = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
#endif
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7F000001); // faqat 127.0.0.1
    addr.sin_port = htons((unsigned short)port);
    if (::bind(g_listen, (sockaddr*)&addr, sizeof(addr)) != 0 || ::listen(g_listen, 16) != 0) {
        err = "127.0.0.1:" + std::to_string(port) + " band (dastur allaqachon ishlayotgan bo'lishi mumkin)";
        CLOSE_SOCK(g_listen);
        g_listen = SOCK_ERR;
        return false;
    }
    g_port = port;
    g_run = true;
    g_acceptThread = std::thread([&st] {
        while (g_run.load()) {
            sock_t c = ::accept(g_listen, nullptr, nullptr);
            if (c == SOCK_ERR) {
                if (!g_run.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            g_active++;
            std::thread(handleConn, &st, c).detach();
        }
    });
    return true;
}

void stopLocalServer() {
    if (g_listen == SOCK_ERR) return;
    g_run = false;
    // accept() ni uyg'otish uchun o'zimizga ulanamiz
    sock_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s != SOCK_ERR) {
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(0x7F000001);
        addr.sin_port = htons((unsigned short)g_port);
        ::connect(s, (sockaddr*)&addr, sizeof(addr));
        CLOSE_SOCK(s);
    }
    if (g_acceptThread.joinable()) g_acceptThread.join();
    for (int i = 0; i < 50 && g_active.load() > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CLOSE_SOCK(g_listen);
    g_listen = SOCK_ERR;
}
