#include "net.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using test_socket = SOCKET;
#define TEST_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using test_socket = int;
#define TEST_CLOSE ::close
#endif

#define REQUIRE(condition) do { if (!(condition)) { std::cerr << "check failed: " #condition " at line " << __LINE__ << "\n"; std::abort(); } } while (false)

static HttpResp exchange(const std::string& response, const std::string& payload = "", bool reset = false) {
    test_socket listener = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
#ifdef _WIN32
    int length = sizeof(addr);
#else
    socklen_t length = sizeof(addr);
#endif
    REQUIRE(getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &length) == 0);
    REQUIRE(listen(listener, 1) == 0);
    bool bodyMatched = false;
    std::thread server([&] {
        test_socket connection = accept(listener, nullptr, nullptr);
        std::string request;
        char buffer[8192];
        size_t headerEnd = std::string::npos;
        size_t expected = 0;
        while (true) {
            int received = recv(connection, buffer, sizeof(buffer), 0);
            if (received <= 0) break;
            request.append(buffer, static_cast<size_t>(received));
            if (headerEnd == std::string::npos) {
                headerEnd = request.find("\r\n\r\n");
                if (headerEnd != std::string::npos) {
                    expected = headerEnd + 4 + payload.size();
                }
            }
            if (headerEnd != std::string::npos && request.size() >= expected) break;
        }
        bodyMatched = headerEnd != std::string::npos && request.substr(headerEnd + 4) == payload;
        // Fragment headers and chunk framing as a real TCP connection may do.
        size_t offset = 0;
        while (offset < response.size()) {
            int count = static_cast<int>(std::min(static_cast<size_t>(37), response.size() - offset));
            int flags = 0;
#ifdef MSG_NOSIGNAL
            flags = MSG_NOSIGNAL;
#endif
            int sent = send(connection, response.data() + offset, count, flags);
            if (sent <= 0) break;
            offset += static_cast<size_t>(sent);
        }
        if (reset) {
            linger immediate{};
            immediate.l_onoff = 1;
            immediate.l_linger = 0;
#ifdef _WIN32
            setsockopt(connection, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&immediate), sizeof(immediate));
#else
            setsockopt(connection, SOL_SOCKET, SO_LINGER, &immediate, sizeof(immediate));
#endif
        }
        TEST_CLOSE(connection);
    });
    HttpResp result = http_request(payload.empty() ? "GET" : "POST",
        "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port)) + "/tasks", payload);
    server.join();
    TEST_CLOSE(listener);
    REQUIRE(bodyMatched);
    return result;
}

int main() {
    REQUIRE(netInit());
    std::string json = "{\"tasks\":[\"" + std::string(6000, 'x') + "\"]}";
    std::ostringstream chunk;
    chunk << std::hex << json.size();
    HttpResp response = exchange("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: Chunked\r\n\r\n" +
        chunk.str() + ";test=value\r\n" + json + "\r\n0\r\nX-End: yes\r\n\r\n");
    REQUIRE(response.ok() && response.error.empty() && response.body == json);
    REQUIRE(response.headers.at("Transfer-Encoding") == "Chunked");
    const std::string binary("a\0b", 3);
    response = exchange("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\n" + binary + "\r\n0\r\n\r\n");
    REQUIRE(response.ok() && response.body == binary);
    response = exchange("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc");
    REQUIRE(response.ok() && response.body == "abc" && response.headers.at("Content-Length") == "3");
    for (const auto& invalid : {
        "not an HTTP response",
        "HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nabc",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nabc\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\ncontent-length: 4\r\n\r\nabc",
    }) {
        response = exchange(invalid);
        REQUIRE(!response.ok() && !response.error.empty() && response.body.empty());
    }
    response = exchange("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\npartial", "", true);
    REQUIRE(!response.ok() && !response.error.empty());
    // A multi-MiB POST must reach the server completely even if send writes partially.
    response = exchange("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK", std::string(12 * 1024 * 1024, 'p'));
    REQUIRE(response.ok() && response.body == "OK");
    for (const auto& invalidUrl : {"https://127.0.0.1:1/", "http://127.0.0.1:0/", "http://127.0.0.1:65536/", "http://127.0.0.1:abc/"}) {
        response = http_request("GET", invalidUrl);
        REQUIRE(!response.ok() && !response.error.empty());
    }
    netCleanup();
    std::cout << "HTTP regressions passed\n";
}
