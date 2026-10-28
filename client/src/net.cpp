#include "net.h"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cstring>
#include <limits>
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

static bool interrupted() {
#ifdef _WIN32
    return lastErr() == WSAEINTR;
#else
    return lastErr() == EINTR;
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

static bool resolveHost(const std::string& host, sockaddr_in& addr) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0) return false;
    bool found = result && result->ai_addrlen >= sizeof(addr);
    if (found) std::memcpy(&addr, result->ai_addr, sizeof(addr));
    if (result) freeaddrinfo(result);
    return found;
}

static bool sendAll(sock_t socket, const std::string& data) {
    size_t offset = 0;
    while (offset < data.size()) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        int count = static_cast<int>(std::min(data.size() - offset, static_cast<size_t>(INT_MAX)));
        int sent = ::send(socket, data.data() + offset, count, flags);
        if (sent == SOCKET_ERROR && interrupted()) continue;
        if (sent <= 0) return false;
        offset += static_cast<size_t>(sent);
    }
    return true;
}

static std::string trim(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? "" : value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

static std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

static bool parseSize(const std::string& text, size_t& result, unsigned base = 10) {
    if (text.empty()) return false;
    result = 0;
    for (unsigned char ch : text) {
        unsigned digit;
        if (ch >= '0' && ch <= '9') digit = ch - '0';
        else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
        else return false;
        if (digit >= base || result > (std::numeric_limits<size_t>::max() - digit) / base) return false;
        result = result * base + digit;
    }
    return true;
}

static bool decodeChunked(const std::string& input, std::string& output) {
    size_t offset = 0;
    output.clear();
    while (true) {
        const size_t lineEnd = input.find("\r\n", offset);
        if (lineEnd == std::string::npos) return false;
        std::string length = input.substr(offset, lineEnd - offset);
        const size_t extension = length.find(';');
        if (extension != std::string::npos) length.resize(extension);
        size_t count;
        if (!parseSize(trim(length), count, 16)) return false;
        offset = lineEnd + 2;
        if (count == 0) {
            // Consume trailers, including the mandatory final empty line.
            while (true) {
                const size_t end = input.find("\r\n", offset);
                if (end == std::string::npos) return false;
                if (end == offset) return end + 2 == input.size();
                if (input.find(':', offset) >= end) return false;
                offset = end + 2;
            }
        }
        if (count > input.size() - offset) return false;
        output.append(input, offset, count);
        offset += count;
        if (input.size() - offset < 2 || input.compare(offset, 2, "\r\n") != 0) return false;
        offset += 2;
    }
}

static bool parseResponse(const std::string& raw, const std::string& method, HttpResp& resp) {
    const size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return false;
    std::istringstream lines(raw.substr(0, headerEnd));
    std::string line;
    if (!std::getline(lines, line)) return false;
    std::istringstream statusLine(line);
    std::string version, status;
    if (!(statusLine >> version >> status) || (version != "HTTP/1.0" && version != "HTTP/1.1")) return false;
    size_t code;
    if (status.size() != 3 || !parseSize(status, code) || code < 100 || code > 599) return false;
    std::map<std::string, std::string> headers;
    while (std::getline(lines, line)) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) return false;
        std::string key = trim(line.substr(0, colon));
        std::string value = trim(line.substr(colon + 1));
        std::string normalized = lower(key);
        auto previous = headers.find(normalized);
        if (previous != headers.end()) {
            if (normalized == "content-length") {
                if (previous->second != value) return false;
            } else {
                value = previous->second + ", " + value;
            }
        }
        headers[normalized] = value;
        resp.headers[key] = value;
    }
    std::string encoded = raw.substr(headerEnd + 4);
    if (method == "HEAD" || code == 204 || code == 304) {
        if (!encoded.empty()) return false;
        resp.body.clear();
    } else if (headers.count("transfer-encoding")) {
        // Only chunked is implemented: never hand encoded framing to JSON/storage.
        if (lower(headers["transfer-encoding"]) != "chunked" || headers.count("content-length")) return false;
        if (!decodeChunked(encoded, resp.body)) return false;
    } else if (headers.count("content-length")) {
        size_t expected;
        if (!parseSize(headers["content-length"], expected) || encoded.size() != expected) return false;
        resp.body = std::move(encoded);
    } else {
        resp.body = std::move(encoded); // HTTP close-delimited response
    }
    resp.status = static_cast<int>(code);
    return true;
}

HttpResp http_request(const std::string& method, const std::string& url,
                      const std::string& body, const std::string& contentType) {
    HttpResp resp;
    if (!netInit()) { resp.error = "net init failed"; return resp; }
    // This raw socket client has no TLS implementation. Fail explicitly for HTTPS.
    if (url.compare(0, 7, "http://") != 0) { resp.error = "only http:// URLs are supported"; return resp; }
    if (method.empty() || method.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos ||
        contentType.find_first_of("\r\n") != std::string::npos || url.find_first_of("\r\n") != std::string::npos) {
        resp.error = "invalid HTTP request";
        return resp;
    }
    std::string rest = url.substr(7);
    const size_t fragment = rest.find('#');
    if (fragment != std::string::npos) rest.resize(fragment);
    const size_t pathStart = rest.find_first_of("/?");
    std::string authority = rest.substr(0, pathStart);
    std::string path = pathStart == std::string::npos ? "/" : rest.substr(pathStart);
    if (path.front() == '?') path.insert(0, "/");
    std::string host = authority;
    size_t port = 80;
    const size_t colon = authority.find(':');
    if (colon != std::string::npos) {
        host = authority.substr(0, colon);
        if (!parseSize(authority.substr(colon + 1), port) || port < 1 || port > 65535) {
            resp.error = "invalid HTTP port";
            return resp;
        }
    }
    if (host.empty() || host.find_first_of(" @[]\t") != std::string::npos || path.find(' ') != std::string::npos) {
        resp.error = "invalid HTTP host or path";
        return resp;
    }

    sock_t socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket == SOCK_ERR) { resp.error = "socket fail"; return resp; }
#ifdef _WIN32
    DWORD timeout = 15000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout{};
    timeout.tv_sec = 15;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
    int noSigpipe = 1;
    setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe, sizeof(noSigpipe));
#endif
#endif
    sockaddr_in address{};
    if (!resolveHost(host, address)) { CLOSE_SOCK(socket); resp.error = "dns fail: " + host; return resp; }
    address.sin_port = htons(static_cast<unsigned short>(port));
    if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        CLOSE_SOCK(socket);
        resp.error = "connect fail";
        return resp;
    }
    std::ostringstream request;
    request << method << " " << path << " HTTP/1.1\r\n"
            << "Host: " << host << ":" << port << "\r\n"
            << "User-Agent: SDriveClient/1.0\r\nConnection: close\r\n";
    if (!body.empty() || method == "POST" || method == "PUT") {
        request << "Content-Type: " << contentType << "\r\nContent-Length: " << body.size() << "\r\n";
    }
    request << "\r\n";
    if (!sendAll(socket, request.str()) || !sendAll(socket, body)) {
        CLOSE_SOCK(socket);
        resp.error = "send fail";
        return resp;
    }
    std::string raw;
    char buffer[65536];
    int received;
    do {
        received = ::recv(socket, buffer, sizeof(buffer), 0);
        if (received > 0) raw.append(buffer, static_cast<size_t>(received));
    } while (received > 0 || (received == SOCKET_ERROR && interrupted()));
    CLOSE_SOCK(socket);
    if (received == SOCKET_ERROR) { resp.error = "receive failed or timed out"; return resp; }
    if (!parseResponse(raw, method, resp)) {
        resp.status = 0;
        resp.body.clear();
        resp.error = "invalid or incomplete HTTP response";
    }
    return resp;
}
