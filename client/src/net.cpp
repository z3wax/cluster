#include "net.h"
#include <cstring>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
using sock_t = SOCKET;
#define CLOSE_SOCK(s) closesocket(s)
#define SOCK_ERR INVALID_SOCKET
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
using sock_t = int;
#define CLOSE_SOCK(s) ::close(s)
#define SOCK_ERR (-1)
#define SOCKET_ERROR (-1)
#endif

static int g_wsaReady = 0;

static int lastErr() {
#ifdef _WIN32
    return (int)WSAGetLastError();
#else
    return errno;
#endif
}

bool netInit() {
#ifdef _WIN32
    if (g_wsaReady) return true;
    WSADATA wsa;
    g_wsaReady = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    return g_wsaReady != 0;
#else
    g_wsaReady = 1;
    return true;
#endif
}

void netCleanup() {
#ifdef _WIN32
    if (g_wsaReady) { WSACleanup(); g_wsaReady = 0; }
#endif
}

static int resolveHost(const std::string& host, sockaddr_in& addr) {
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    struct hostent* he = gethostbyname(host.c_str());
    if (he && he->h_addr_list[0]) {
        memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
        return 0;
    }
    unsigned long ip = inet_addr(host.c_str());
    if (ip != INADDR_NONE) {
        addr.sin_addr.s_addr = ip;
        return 0;
    }
    return -1;
}

HttpResp http_request(const std::string& method, const std::string& url,
                      const std::string& body, const std::string& contentType) {
    HttpResp resp;
    if (!netInit()) { resp.error = "net init failed"; return resp; }

    // url parse: scheme://host[:port]/path
    std::string rest = url;
    size_t p = rest.find("://");
    if (p != std::string::npos) rest = rest.substr(p + 3);
    else { fprintf(stderr, "[net] url xato\n"); return resp; }
    std::string host, path = "/";
    p = rest.find('/');
    if (p != std::string::npos) { host = rest.substr(0, p); path = rest.substr(p); }
    else host = rest;
    if (host.empty()) { fprintf(stderr, "[net] host bo'sh\n"); return resp; }

    int port = 80;
    p = host.find(':');
    if (p != std::string::npos) {
        port = std::atoi(host.substr(p + 1).c_str());
        host = host.substr(0, p);
    }
    fprintf(stderr, "[net] ulanish: host=%s port=%d path=%s\n", host.c_str(), port, path.c_str());

    sock_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == SOCK_ERR) { fprintf(stderr, "[net] socket fail: %d\n", lastErr()); resp.error = "socket fail"; return resp; }
#ifdef _WIN32
    DWORD to = 15000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof(to));
#else
    timeval tv; tv.tv_sec = 15; tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    sockaddr_in addr;
    if (resolveHost(host, addr) != 0) { fprintf(stderr, "[net] dns fail\n"); CLOSE_SOCK(s); resp.error = "dns fail: " + host; return resp; }
    addr.sin_port = htons((unsigned short)port);

    fprintf(stderr, "[net] connect...\n");
    if (::connect(s, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "[net] connect fail: %d\n", lastErr());
        CLOSE_SOCK(s);
        resp.error = "connect fail";
        return resp;
    }
    fprintf(stderr, "[net] connected, send...\n");

    std::ostringstream req;
    req << method << " " << path << " HTTP/1.1\r\n"
        << "Host: " << host << ":" << port << "\r\n"
        << "User-Agent: SDriveClient/1.0\r\n"
        << "Connection: close\r\n";
    if (!body.empty() || method == "POST" || method == "PUT") {
        req << "Content-Type: " << contentType << "\r\n"
            << "Content-Length: " << body.size() << "\r\n";
    }
    req << "\r\n";
    std::string head = req.str();
    fprintf(stderr, "[net] head %d bayt, body %d bayt\n", (int)head.size(), (int)body.size());
    int sr = ::send(s, head.c_str(), (int)head.size(), 0);
    fprintf(stderr, "[net] send head = %d (err %d)\n", sr, sr == SOCKET_ERROR ? lastErr() : 0);
    if (sr == SOCKET_ERROR) { CLOSE_SOCK(s); resp.error = "send fail"; return resp; }
    if (!body.empty()) {
        int br = ::send(s, body.c_str(), (int)body.size(), 0);
        fprintf(stderr, "[net] send body = %d (err %d)\n", br, br == SOCKET_ERROR ? lastErr() : 0);
        if (br == SOCKET_ERROR) { CLOSE_SOCK(s); resp.error = "send fail"; return resp; }
    }

    fprintf(stderr, "[net] recv...\n");
    std::string raw;
    char buf[65536];
    int n;
    while ((n = ::recv(s, buf, sizeof(buf), 0)) > 0) raw.append(buf, (size_t)n);
    fprintf(stderr, "[net] recv done n=%d (err %d) total=%d\n", n, n == SOCKET_ERROR ? lastErr() : 0, (int)raw.size());
    CLOSE_SOCK(s);
    fprintf(stderr, "[net] socket yopildi\n");
    if (raw.empty()) { resp.error = "no data"; return resp; }

    // status qator + headerlar
    size_t hEnd = raw.find("\r\n\r\n");
    size_t headerDelimLen = 4;
    if (hEnd == std::string::npos) {
        hEnd = raw.find("\n\n");
        headerDelimLen = 2;
    }
    if (hEnd == std::string::npos) {
        resp.body = raw;
        resp.status = 200;
        return resp;
    }

    std::string headBlock = raw.substr(0, hEnd);
    resp.body = raw.substr(hEnd + headerDelimLen);

    size_t sp1 = headBlock.find(' ');
    if (sp1 != std::string::npos) {
        resp.status = std::atoi(headBlock.c_str() + sp1 + 1);
    }
    size_t pos = 0;
    while (pos < headBlock.size()) {
        size_t eol = headBlock.find("\r\n", pos);
        size_t lineLen = 2;
        if (eol == std::string::npos) {
            eol = headBlock.find('\n', pos);
            lineLen = 1;
        }
        if (eol == std::string::npos) break;
        std::string line = headBlock.substr(pos, eol - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string k = line.substr(0, colon);
            std::string v = line.substr(colon + 1);
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
            resp.headers[k] = v;
        }
        pos = eol + lineLen;
    }
    fprintf(stderr, "[net] qaytmoqda status=%d\n", resp.status);
    return resp;
}
