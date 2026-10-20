#include "net.h"
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <cctype>
#include <sstream>
#include <mutex>
#include <algorithm>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#endif
using sock_t = SOCKET;
#define CLOSE_SOCK(s) closesocket(s)
#define SOCK_ERR INVALID_SOCKET
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>
using sock_t = int;
#define CLOSE_SOCK(s) ::close(s)
#define SOCK_ERR (-1)
#endif

#ifdef SDRIVE_HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#endif

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

static std::once_flag g_netOnce;
static bool g_netOk = false;

bool netInit() {
    std::call_once(g_netOnce, [] {
#ifdef _WIN32
        WSADATA wsa;
        g_netOk = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
        signal(SIGPIPE, SIG_IGN); // yopilgan soketga yozish jarayonni o'ldirmasin
        g_netOk = true;
#endif
    });
    return g_netOk;
}

void netCleanup() {
#ifdef _WIN32
    if (g_netOk) WSACleanup();
#endif
}

bool tlsAvailable() {
#if defined(SDRIVE_HAVE_OPENSSL) || defined(_WIN32)
    return true; // Windows: WinHTTP (tizim sertifikat ombori)
#else
    return false;
#endif
}

// ---------------- URL ----------------

bool parseUrl(const std::string& url, ParsedUrl& out) {
    size_t p = url.find("://");
    if (p == std::string::npos) return false;
    out.scheme = url.substr(0, p);
    std::transform(out.scheme.begin(), out.scheme.end(), out.scheme.begin(), ::tolower);
    if (out.scheme != "http" && out.scheme != "https") return false;
    std::string rest = url.substr(p + 3);
    size_t slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);
    out.port = out.scheme == "https" ? 443 : 80;
    if (!hostport.empty() && hostport[0] == '[') { // IPv6: [::1]:8080
        size_t rb = hostport.find(']');
        if (rb == std::string::npos) return false;
        out.host = hostport.substr(1, rb - 1);
        if (rb + 1 < hostport.size()) {
            if (hostport[rb + 1] != ':') return false;
            out.port = std::atoi(hostport.c_str() + rb + 2);
        }
    } else {
        size_t colon = hostport.rfind(':');
        if (colon != std::string::npos) {
            out.host = hostport.substr(0, colon);
            out.port = std::atoi(hostport.c_str() + colon + 1);
        } else {
            out.host = hostport;
        }
    }
    return !out.host.empty() && out.port > 0 && out.port < 65536;
}

// ---------------- Javobni tahlil qilish ----------------

static std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static bool decodeChunked(const std::string& in, std::string& out) {
    out.clear();
    size_t pos = 0;
    while (true) {
        size_t eol = in.find("\r\n", pos);
        if (eol == std::string::npos) return false;
        std::string szLine = in.substr(pos, eol - pos);
        size_t semi = szLine.find(';');
        if (semi != std::string::npos) szLine = szLine.substr(0, semi);
        char* end = nullptr;
        unsigned long long sz = std::strtoull(szLine.c_str(), &end, 16);
        if (szLine.empty() || end == szLine.c_str()) return false;
        pos = eol + 2;
        if (sz == 0) return true; // trailer'lar e'tiborsiz
        if (pos + sz + 2 > in.size()) return false;
        out.append(in, pos, (size_t)sz);
        pos += (size_t)sz + 2;
    }
}

bool parseHttpResponse(const std::string& raw, HttpResp& resp, bool headRequest) {
    size_t hEnd = raw.find("\r\n\r\n");
    if (hEnd == std::string::npos) return false;
    std::string head = raw.substr(0, hEnd);
    std::string rest = raw.substr(hEnd + 4);

    size_t eol = head.find("\r\n");
    std::string statusLine = head.substr(0, eol);
    if (statusLine.compare(0, 5, "HTTP/") != 0) return false;
    size_t sp = statusLine.find(' ');
    if (sp == std::string::npos) return false;
    resp.status = std::atoi(statusLine.c_str() + sp + 1);
    resp.headers.clear();
    size_t pos = eol == std::string::npos ? head.size() : eol + 2;
    while (pos < head.size()) {
        size_t e = head.find("\r\n", pos);
        if (e == std::string::npos) e = head.size();
        std::string line = head.substr(pos, e - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string k = lower(line.substr(0, colon));
            std::string v = line.substr(colon + 1);
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
            resp.headers[k] = v;
        }
        pos = e + 2;
    }

    if (headRequest || resp.status == 204 || resp.status == 304 || resp.status / 100 == 1) {
        resp.body.clear();
        return true;
    }
    if (lower(resp.header("transfer-encoding")).find("chunked") != std::string::npos) {
        return decodeChunked(rest, resp.body);
    }
    std::string cl = resp.header("content-length");
    if (!cl.empty()) {
        size_t n = (size_t)std::strtoull(cl.c_str(), nullptr, 10);
        if (rest.size() < n) return false;
        resp.body = rest.substr(0, n);
        return true;
    }
    resp.body = rest;
    return true;
}

// ---------------- Ulanish ----------------

static int lastErr() {
#ifdef _WIN32
    return (int)WSAGetLastError();
#else
    return errno;
#endif
}

static void setNonBlocking(sock_t s, bool on) {
#ifdef _WIN32
    u_long mode = on ? 1 : 0;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
}

static void setTimeouts(sock_t s, int sec) {
#ifdef _WIN32
    DWORD to = (DWORD)sec * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof(to));
#else
    timeval tv;
    tv.tv_sec = sec;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#endif
}

static sock_t connectTo(const std::string& host, int port, int timeoutSec, std::string& err) {
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        err = "DNS xatosi: " + host;
        return SOCK_ERR;
    }
    err = "ulanib bo'lmadi: " + host + ":" + std::to_string(port);
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        sock_t s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == SOCK_ERR) continue;
        setNonBlocking(s, true);
        int rc = ::connect(s, ai->ai_addr, (int)ai->ai_addrlen);
        if (rc != 0) {
            int e = lastErr();
#ifdef _WIN32
            bool pending = e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
            bool pending = e == EINPROGRESS;
#endif
            if (!pending) { CLOSE_SOCK(s); continue; }
            fd_set wf, ef;
            FD_ZERO(&wf);
            FD_ZERO(&ef);
            FD_SET(s, &wf);
            FD_SET(s, &ef);
            timeval tv;
            tv.tv_sec = timeoutSec < 10 ? timeoutSec : 10;
            tv.tv_usec = 0;
            if (::select((int)s + 1, nullptr, &wf, &ef, &tv) <= 0) { CLOSE_SOCK(s); err = "ulanish vaqti tugadi"; continue; }
            int soerr = 0;
            socklen_t len = sizeof(soerr);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soerr, &len);
            if (soerr != 0) { CLOSE_SOCK(s); continue; }
        }
        setNonBlocking(s, false);
        setTimeouts(s, timeoutSec);
        freeaddrinfo(res);
        err.clear();
        return s;
    }
    freeaddrinfo(res);
    return SOCK_ERR;
}

struct Conn {
    sock_t s = SOCK_ERR;
#ifdef SDRIVE_HAVE_OPENSSL
    SSL* ssl = nullptr;
#endif

    int read(char* buf, int n) {
#ifdef SDRIVE_HAVE_OPENSSL
        if (ssl) return SSL_read(ssl, buf, n);
#endif
        return (int)::recv(s, buf, n, 0);
    }

    bool writeAll(const char* buf, size_t n) {
        while (n > 0) {
            int chunk = n > (1u << 20) ? (1 << 20) : (int)n;
            int w;
#ifdef SDRIVE_HAVE_OPENSSL
            if (ssl) w = SSL_write(ssl, buf, chunk);
            else
#endif
                w = (int)::send(s, buf, chunk, SEND_FLAGS);
            if (w <= 0) return false;
            buf += w;
            n -= (size_t)w;
        }
        return true;
    }

    void close() {
#ifdef SDRIVE_HAVE_OPENSSL
        if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ssl = nullptr;
        }
#endif
        if (s != SOCK_ERR) CLOSE_SOCK(s);
        s = SOCK_ERR;
    }
};

#ifdef SDRIVE_HAVE_OPENSSL
static SSL_CTX* tlsCtx() {
    static SSL_CTX* ctx = [] {
        SSL_CTX* c = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
        SSL_CTX_set_verify(c, SSL_VERIFY_PEER, nullptr);
        const char* ca = getenv("SDRIVE_CA_FILE");
        if (ca && *ca) SSL_CTX_load_verify_locations(c, ca, nullptr);
        else SSL_CTX_set_default_verify_paths(c);
        return c;
    }();
    return ctx;
}

static bool isIpLiteral(const std::string& h) {
    unsigned char buf[16];
    return inet_pton(AF_INET, h.c_str(), buf) == 1 || inet_pton(AF_INET6, h.c_str(), buf) == 1;
}
#endif

#ifdef _WIN32
static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// Windows HTTPS: WinHTTP (TLS, sertifikat tekshiruvi va proksi — operatsion tizim tomonidan)
HttpResp http_do_winhttp(const HttpReq& req) {
    HttpResp resp;
    ParsedUrl u;
    if (!parseUrl(req.url, u)) { resp.error = "URL noto'g'ri: " + req.url; return resp; }
    HINTERNET hs = WinHttpOpen(L"SDriveClient/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) { resp.error = "WinHTTP ishga tushmadi"; return resp; }
    int ms = req.timeoutSec * 1000;
    WinHttpSetTimeouts(hs, ms, ms < 10000 ? ms : 10000, ms, ms);
    HINTERNET hc = WinHttpConnect(hs, widen(u.host).c_str(), (INTERNET_PORT)u.port, 0);
    HINTERNET hr = hc ? WinHttpOpenRequest(hc, widen(req.method).c_str(), widen(u.path).c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           u.scheme == "https" ? WINHTTP_FLAG_SECURE : 0)
                      : nullptr;
    auto done = [&](const std::string& err) {
        if (!err.empty()) resp.error = err + " (WinHTTP " + std::to_string(GetLastError()) + ")";
        if (hr) WinHttpCloseHandle(hr);
        if (hc) WinHttpCloseHandle(hc);
        WinHttpCloseHandle(hs);
        return resp;
    };
    if (!hr) return done("ulanib bo'lmadi: " + u.host);
    std::string hdrs;
    for (const auto& kv : req.headers) hdrs += kv.first + ": " + kv.second + "\r\n";
    if (!req.body.empty() || req.method == "POST" || req.method == "PUT") hdrs += "Content-Type: " + req.contentType + "\r\n";
    std::wstring wh = widen(hdrs);
    if (!WinHttpSendRequest(hr, wh.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wh.c_str(), wh.empty() ? 0 : (DWORD)-1L,
                            req.body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)req.body.data(), (DWORD)req.body.size(),
                            (DWORD)req.body.size(), 0) ||
        !WinHttpReceiveResponse(hr, nullptr))
        return done("so'rov yuborilmadi yoki TLS xatosi");
    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                        WINHTTP_NO_HEADER_INDEX);
    DWORD hlen = 0;
    WinHttpQueryHeaders(hr, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &hlen,
                        WINHTTP_NO_HEADER_INDEX);
    if (hlen > 0) {
        std::wstring raw(hlen / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(hr, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, &raw[0], &hlen,
                                WINHTTP_NO_HEADER_INDEX)) {
            HttpResp tmp;
            std::string h = narrow(raw.c_str());
            if (h.find("\r\n\r\n") == std::string::npos) h += "\r\n";
            if (parseHttpResponse(h, tmp, true)) resp.headers = tmp.headers;
        }
    }
    std::vector<char> buf(64 * 1024);
    while (true) {
        DWORD got = 0;
        if (!WinHttpReadData(hr, buf.data(), (DWORD)buf.size(), &got)) return done("javob o'qilmadi");
        if (got == 0) break;
        resp.body.append(buf.data(), got);
        if (resp.body.size() > req.maxBody) return done("javob juda katta");
    }
    resp.status = (int)status;
    return done("");
}
#endif

HttpResp http_do(const HttpReq& req) {
    HttpResp resp;
    if (!netInit()) { resp.error = "tarmoq ishga tushmadi"; return resp; }
    ParsedUrl u;
    if (!parseUrl(req.url, u)) { resp.error = "URL noto'g'ri: " + req.url; return resp; }
#ifdef _WIN32
    if (u.scheme == "https") return http_do_winhttp(req);
#endif
    if (u.scheme == "https" && !tlsAvailable()) {
        resp.error = "HTTPS uchun dastur OpenSSL bilan build qilinishi kerak";
        return resp;
    }

    Conn c;
    c.s = connectTo(u.host, u.port, req.timeoutSec, resp.error);
    if (c.s == SOCK_ERR) return resp;

#ifdef SDRIVE_HAVE_OPENSSL
    if (u.scheme == "https") {
        c.ssl = SSL_new(tlsCtx());
        SSL_set_fd(c.ssl, (int)c.s);
        if (isIpLiteral(u.host)) {
            X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(c.ssl), u.host.c_str());
        } else {
            SSL_set_tlsext_host_name(c.ssl, u.host.c_str());
            SSL_set1_host(c.ssl, u.host.c_str());
        }
        if (SSL_connect(c.ssl) != 1) {
            char ebuf[256];
            ERR_error_string_n(ERR_get_error(), ebuf, sizeof(ebuf));
            resp.error = std::string("TLS xatosi: ") + ebuf;
            c.close();
            return resp;
        }
    }
#endif

    bool defPort = (u.scheme == "http" && u.port == 80) || (u.scheme == "https" && u.port == 443);
    std::string hostHdr = u.host.find(':') != std::string::npos ? "[" + u.host + "]" : u.host;
    if (!defPort) hostHdr += ":" + std::to_string(u.port);

    std::ostringstream h;
    h << req.method << " " << u.path << " HTTP/1.1\r\n"
      << "Host: " << hostHdr << "\r\n"
      << "User-Agent: SDriveClient/2.0\r\n"
      << "Accept: */*\r\n"
      << "Connection: close\r\n";
    for (const auto& kv : req.headers) h << kv.first << ": " << kv.second << "\r\n";
    if (!req.body.empty() || req.method == "POST" || req.method == "PUT") {
        h << "Content-Type: " << req.contentType << "\r\n"
          << "Content-Length: " << req.body.size() << "\r\n";
    }
    h << "\r\n";
    std::string head = h.str();
    bool sent;
    if (req.body.size() <= 64 * 1024) {
        head += req.body; // kichik so'rov sarlavha bilan bitta paketda yuboriladi
        sent = c.writeAll(head.data(), head.size());
    } else {
        sent = c.writeAll(head.data(), head.size()) && c.writeAll(req.body.data(), req.body.size());
    }
    if (!sent) {
        resp.error = "so'rov yuborilmadi";
        c.close();
        return resp;
    }

    std::string raw;
    std::vector<char> buf(64 * 1024);
    size_t want = std::string::npos; // Content-Length ma'lum bo'lsa — to'liq javob hajmi
    int readErr = 0;
    while (true) {
        int n = c.read(buf.data(), (int)buf.size());
        if (n < 0) readErr = lastErr();
        if (n <= 0) break;
        raw.append(buf.data(), (size_t)n);
        if (raw.size() > req.maxBody + 64 * 1024) {
            resp.error = "javob juda katta";
            c.close();
            return resp;
        }
        if (want == std::string::npos) {
            size_t hEnd = raw.find("\r\n\r\n");
            if (hEnd != std::string::npos) {
                HttpResp tmp;
                std::string hdrOnly = raw.substr(0, hEnd + 4);
                parseHttpResponse(hdrOnly, tmp, true);
                std::string cl = tmp.header("content-length");
                if (req.method == "HEAD" || tmp.status == 204 || tmp.status == 304) want = hEnd + 4;
                else if (!cl.empty() && tmp.header("transfer-encoding").empty())
                    want = hEnd + 4 + (size_t)std::strtoull(cl.c_str(), nullptr, 10);
            }
        }
        if (want != std::string::npos && raw.size() >= want) break;
    }
    c.close();
    if (raw.empty()) {
        resp.error = "javob kelmadi (vaqt tugadi yoki ulanish uzildi" + (readErr ? ", kod " + std::to_string(readErr) : std::string()) + ")";
        return resp;
    }
    if (!parseHttpResponse(raw, resp, req.method == "HEAD")) {
        resp.error = "javob to'liq kelmadi";
        resp.status = 0;
        return resp;
    }
    return resp;
}

HttpResp http_request(const std::string& method, const std::string& url, const std::string& body,
                      const std::string& contentType, const std::string& bearerToken) {
    HttpReq r;
    r.method = method;
    r.url = url;
    r.body = body;
    r.contentType = contentType;
    if (!bearerToken.empty()) r.headers.push_back({"Authorization", "Bearer " + bearerToken});
    return http_do(r);
}

// --- incremental chunked-transfer dekoder ---
namespace {
struct ChunkDecoder {
    std::string buf;
    bool inSize = true;
    long long remaining = 0;
    bool done = false;

    // feed: kelgan baytlarni qo'shadi, dekodlangan payloadni emit ga uzatadi
    bool feed(const char* data, size_t n, const std::function<bool(const char*, size_t)>& emit) {
        buf.append(data, n);
        while (!done) {
            if (inSize) {
                size_t eol = buf.find("\r\n");
                if (eol == std::string::npos) return true; // ko'proq kerak
                std::string line = buf.substr(0, eol);
                size_t semi = line.find(';');
                if (semi != std::string::npos) line = line.substr(0, semi);
                remaining = strtoll(line.c_str(), nullptr, 16);
                buf.erase(0, eol + 2);
                inSize = false;
                if (remaining == 0) {
                    done = true;
                    return true;
                }
            }
            if (buf.empty()) return true;
            size_t take = buf.size() < (size_t)remaining ? buf.size() : (size_t)remaining;
            if (take > 0 && !emit(buf.data(), take)) return false;
            buf.erase(0, take);
            remaining -= (long long)take;
            if (remaining == 0) {
                // bo'lak oxiridagi CRLF
                if (buf.size() < 2) return true;
                buf.erase(0, 2);
                inSize = true;
            }
        }
        return true;
    }
};
} // namespace

HttpResp http_stream(const HttpReq& req, const std::function<bool(const char*, size_t)>& onBody) {
    ParsedUrl u;
    if (!parseUrl(req.url, u)) {
        HttpResp r;
        r.error = "URL noto'g'ri: " + req.url;
        return r;
    }
    if (u.scheme != "http") {
        // https (kamdan-kam, lokal bo'lmagan): oddiy to'liq o'qish, so'ng bir martada uzatish
        HttpResp r = http_do(req);
        if (r.ok() && !r.body.empty()) onBody(r.body.data(), r.body.size());
        return r;
    }
    if (!netInit()) {
        HttpResp r;
        r.error = "tarmoq ishga tushmadi";
        return r;
    }
    HttpResp resp;
    Conn c;
    c.s = connectTo(u.host, u.port, req.timeoutSec, resp.error);
    if (c.s == SOCK_ERR) return resp;

    bool defPort = u.port == 80;
    std::string hostHdr = u.host.find(':') != std::string::npos ? "[" + u.host + "]" : u.host;
    if (!defPort) hostHdr += ":" + std::to_string(u.port);
    std::ostringstream h;
    h << req.method << " " << u.path << " HTTP/1.1\r\n"
      << "Host: " << hostHdr << "\r\n"
      << "User-Agent: SDriveClient/2.0\r\n"
      << "Accept: */*\r\n"
      << "Connection: close\r\n";
    for (const auto& kv : req.headers) h << kv.first << ": " << kv.second << "\r\n";
    if (!req.body.empty() || req.method == "POST" || req.method == "PUT") {
        h << "Content-Type: " << req.contentType << "\r\n"
          << "Content-Length: " << req.body.size() << "\r\n";
    }
    h << "\r\n";
    std::string head = h.str() + req.body;
    if (!c.writeAll(head.data(), head.size())) {
        resp.error = "so'rov yuborilmadi";
        c.close();
        return resp;
    }

    std::string raw;
    std::vector<char> buf(32 * 1024);
    bool haveHeaders = false, chunked = false, stopped = false;
    long long contentLen = -1, bodyGot = 0;
    ChunkDecoder dec;
    auto emit = [&](const char* d, size_t n) -> bool {
        if (stopped) return false;
        if (!onBody(d, n)) {
            stopped = true;
            return false;
        }
        return true;
    };
    while (true) {
        int n = c.read(buf.data(), (int)buf.size());
        if (n <= 0) break;
        if (!haveHeaders) {
            raw.append(buf.data(), (size_t)n);
            size_t hEnd = raw.find("\r\n\r\n");
            if (hEnd == std::string::npos) {
                if (raw.size() > 256 * 1024) {
                    resp.error = "sarlavha juda katta";
                    break;
                }
                continue;
            }
            parseHttpResponse(raw.substr(0, hEnd + 4), resp, true);
            haveHeaders = true;
            chunked = lower(resp.header("transfer-encoding")).find("chunked") != std::string::npos;
            std::string cl = resp.header("content-length");
            if (!cl.empty()) contentLen = strtoll(cl.c_str(), nullptr, 10);
            std::string rest = raw.substr(hEnd + 4);
            if (!rest.empty()) {
                if (chunked) {
                    if (!dec.feed(rest.data(), rest.size(), emit)) break;
                } else {
                    bodyGot += (long long)rest.size();
                    if (!emit(rest.data(), rest.size())) break;
                }
            }
        } else {
            if (chunked) {
                if (!dec.feed(buf.data(), (size_t)n, emit) || dec.done) break;
            } else {
                bodyGot += n;
                if (!emit(buf.data(), (size_t)n)) break;
            }
        }
        if (!chunked && contentLen >= 0 && bodyGot >= contentLen) break;
        if (chunked && dec.done) break;
    }
    c.close();
    if (!haveHeaders && resp.error.empty()) resp.error = "javob kelmadi";
    return resp;
}
