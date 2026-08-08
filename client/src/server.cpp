#include "app.h"
#include "net.h"
#include "storage.h"
#include "platform.h"
#include "ui.h"
#include "json.hpp"
#include <atomic>
#include <thread>
#include <cstring>
#include <sstream>
#include <filesystem>

#ifdef _WIN32
#include <winsock2.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
using sock_t = SOCKET;
#define CLOSE_SOCK(s) closesocket(s)
#define SOCK_ERR INVALID_SOCKET
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
using sock_t = int;
#define CLOSE_SOCK(s) ::close(s)
#define SOCK_ERR (-1)
#endif

static std::atomic<bool> g_run{true};
static sock_t g_listen = SOCK_ERR;

namespace fs = std::filesystem;

// ---------- yordamchilar ----------
static void respond(int fd, int code, const std::string& ct, const std::string& body, bool keepAlive = false) {
    std::string head = "HTTP/1.1 " + std::to_string(code) +
        (code == 200 ? " OK\r\n" : " Error\r\n") +
        "Content-Type: " + ct + "\r\n" +
        "Content-Length: " + std::to_string(body.size()) + "\r\n" +
        "Connection: " + std::string(keepAlive ? "keep-alive" : "close") + "\r\n\r\n";
    std::string all = head + body;
    ::send(fd, all.c_str(), (int)all.size(), 0);
}

static void respondJSON(int fd, int code, const std::string& json) {
    respond(fd, code, "application/json", json);
}

// ---------- API logikasi ----------
static void handleRoute(AppState& st, const std::string& method, const std::string& path,
                        const std::string& body, int fd) {
    if (path == "/" || path == "/index.html") {
        respond(fd, 200, "text/html; charset=utf-8", UI_HTML);
        return;
    }
    if (path == "/api/status") {
        Json o;
        std::string sdriveDir;
        int64_t usedMB = 0;
        {
            std::lock_guard<std::mutex> lk(st.m);
            o["nodeId"] = st.cfg.nodeId;
            o["serverUrl"] = st.cfg.serverUrl;
            o["setupDone"] = st.cfg.setupDone;
            o["agreementAccepted"] = st.cfg.agreementAccepted;
            o["sdriveDir"] = st.cfg.sdriveDir;
            o["driveLetter"] = st.cfg.driveLetter;
            o["quotaGB"] = (double)(st.cfg.quotaBytes >> 30);
            o["online"] = st.online;
            o["tasksDue"] = st.tasksDue;
            o["uptimeSec"] = (double)st.uptimeSec;
            o["earningsCents"] = st.earningsCents;
            o["state"] = st.cfg.state;
            sdriveDir = st.cfg.sdriveDir;
        }
        o["spaceUsedMB"] = (double)(sdriveUsedBytes(sdriveDir) >> 20);
        respondJSON(fd, 200, o.dump());
        return;
    }
    if (path == "/api/agreement" && method == "GET") {
        std::string serverUrl;
        {
            std::lock_guard<std::mutex> lk(st.m);
            serverUrl = st.cfg.serverUrl;
        }
        HttpResp r = http_request("GET", serverUrl + "/api/agreement");
        if (r.ok()) {
            respondJSON(fd, 200, r.body);
        } else {
            Json o;
            o["error"] = "server bilan bog'lanib bo'lmadi: " + r.error;
            respondJSON(fd, 502, o.dump());
        }
        return;
    }
    if (path == "/api/setup" && method == "POST") {
        bool ok = false;
        Json req = Json::parse(body, &ok);
        std::string err;
        if (!ok) {
            respondJSON(fd, 400, "{\"ok\":false,\"error\":\"noto'g'ri so'rov\"}");
            return;
        }
        std::string driveLetter = req["driveLetter"].asStr("S");
        int64_t quotaGb = req["quotaGb"].asInt(0);
        std::string dir = req["sdriveDir"].asStr("");
        if (quotaGb <= 0) {
            respondJSON(fd, 400, "{\"ok\":false,\"error\":\"hajm noto'g'ri\"}");
            return;
        }
        if (dir.empty()) dir = defaultSdriveDir();
        if (driveLetter.size() != 1) driveLetter = "S";
        if (driveLetter[0] >= 'a' && driveLetter[0] <= 'z') driveLetter[0] -= 32;

        if (!ensureSdrive(dir, driveLetter, err)) {
            respondJSON(fd, 400, "{\"ok\":false,\"error\":\"" + err + "\"}");
            return;
        }
        {
            std::lock_guard<std::mutex> lk(st.m);
            st.cfg.driveLetter = driveLetter;
            st.cfg.sdriveDir = dir;
            st.cfg.quotaBytes = quotaGb << 30;
            st.cfg.setupDone = true;
            if (!st.cfg.agreementAccepted) st.cfg.state = "agreement";
            else st.cfg.state = "running";
            saveConfig(st.cfg);
        }
        respondJSON(fd, 200, "{\"ok\":true}");
        return;
    }
    if (path == "/api/accept" && method == "POST") {
        {
            std::lock_guard<std::mutex> lk(st.m);
            st.cfg.agreementAccepted = true;
            st.cfg.state = "running";
            saveConfig(st.cfg);
        }
        respondJSON(fd, 200, "{\"ok\":true}");
        return;
    }
    if (path == "/api/claim" && method == "POST") {
        std::string serverUrl, nodeId;
        {
            std::lock_guard<std::mutex> lk(st.m);
            serverUrl = st.cfg.serverUrl;
            nodeId = st.cfg.nodeId;
        }
        Json req;
        req["nodeId"] = nodeId;
        HttpResp r = http_request("POST", serverUrl + "/api/payouts/claim", req.dump());
        if (r.ok()) {
            Json res = Json::parse(r.body);
            Json o;
            o["ok"] = true;
            o["amountCents"] = res["payout"]["amountCents"].asNum(0);
            o["forfeited"] = res["payout"]["status"].asStr("") == "forfeited";
            respondJSON(fd, 200, o.dump());
        } else {
            respondJSON(fd, 502, "{\"ok\":false,\"error\":\"serverga ulanishmadi\"}");
        }
        return;
    }
    if (path == "/api/uninstall" && method == "POST") {
        std::string serverUrl, nodeId, driveLetter;
        {
            std::lock_guard<std::mutex> lk(st.m);
            serverUrl = st.cfg.serverUrl;
            nodeId = st.cfg.nodeId;
            driveLetter = st.cfg.driveLetter;
        }
        http_request("POST", serverUrl + "/api/nodes/" + nodeId + "/uninstall");
        removeAutostart();
        unmountSdrive(driveLetter);
        {
            std::lock_guard<std::mutex> lk(st.m);
            st.cfg.state = "uninstalled";
            saveConfig(st.cfg);
        }
        respondJSON(fd, 200, "{\"ok\":true}");
        return;
    }
    respondJSON(fd, 404, "{\"error\":\"not found\"}");
}

// ---------- HTTP so'rovni qadam-baqadam o'qish ----------
// "\\r\\n\\r\\n" gacha o'qiymiz, so'ng Content-Length bo'yicha body.
static bool readRequest(int fd, std::string& method, std::string& path, std::string& body) {
    std::string buf;
    char tmp[4096];
    size_t headEnd = std::string::npos;
    int contentLength = -1;
    size_t headerBytes = 0;

#ifdef _WIN32
    DWORD to = 8000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
#else
    timeval tv; tv.tv_sec = 8; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    // 1. Headerlarni o'qish
    while (buf.size() < (1 << 20)) {
        int n = (int)::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, n);
        headEnd = buf.find("\r\n\r\n");
        if (headEnd != std::string::npos) {
            headerBytes = headEnd + 4;
            size_t cl = buf.find("Content-Length:");
            if (cl != std::string::npos && cl < headerBytes) {
                size_t eol = buf.find("\r\n", cl);
                contentLength = std::atoi(buf.substr(cl + 15, eol - cl - 15).c_str());
                if (contentLength < 0) contentLength = 0;
            } else contentLength = 0;
            break;
        }
    }
    if (headEnd == std::string::npos) return false;

    // 2. Body ni o'qish
    while ((int)buf.size() - (int)headerBytes < contentLength) {
        int n = (int)::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, n);
    }

    std::string reqLine = buf.substr(0, buf.find("\r\n"));
    std::istringstream ss(reqLine);
    ss >> method >> path;
    if (contentLength > 0 && headerBytes < buf.size())
        body = buf.substr(headerBytes, (size_t)contentLength);
    return !method.empty() && !path.empty();
}

static int lastSockErr() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

// ---------- server thread ----------
static void serverLoop(AppState& st) {
    g_listen = ::socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen == SOCK_ERR) { fprintf(stderr, "[sdrive] socket() xato: %d\n", lastSockErr()); return; }
    int one = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7F000001); // 127.0.0.1
    addr.sin_port = htons(48673);
    if (::bind(g_listen, (sockaddr*)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "[sdrive] bind 48673 xato: %d\n", lastSockErr());
        CLOSE_SOCK(g_listen);
        return;
    }
    if (::listen(g_listen, 8) != 0) {
        fprintf(stderr, "[sdrive] listen xato: %d\n", lastSockErr());
        CLOSE_SOCK(g_listen);
        return;
    }

    while (g_run.load()) {
        sock_t c = ::accept(g_listen, nullptr, nullptr);
        if (c == SOCK_ERR) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
        std::string method, path, body;
        if (readRequest(c, method, path, body)) {
            handleRoute(st, method, path, body, c);
        }
        CLOSE_SOCK(c);
    }
}

void startLocalServer(AppState& st) {
    std::thread(serverLoop, std::ref(st)).detach();
}

void stopLocalServer() {
    g_run = false;
    if (g_listen != SOCK_ERR) CLOSE_SOCK(g_listen);
}
