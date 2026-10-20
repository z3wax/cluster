#pragma once
// HTTP/1.1 mijoz — Winsock / POSIX soketlar, ixtiyoriy TLS (OpenSSL bilan build qilinganda).
// Content-Length, chunked va ulanish yopilishi bilan tugaydigan javoblarni to'g'ri o'qiydi.
#include <string>
#include <map>
#include <vector>
#include <utility>
#include <functional>

struct HttpResp {
    int status = 0;
    std::map<std::string, std::string> headers; // kalitlar kichik harflarda
    std::string body;
    std::string error; // ulanish muvaffaqiyatsiz bo'lsa
    bool ok() const { return status >= 200 && status < 300; }
    std::string header(const std::string& lowerName) const {
        auto it = headers.find(lowerName);
        return it == headers.end() ? "" : it->second;
    }
};

struct HttpReq {
    std::string method = "GET";
    std::string url;
    std::string body;
    std::string contentType = "application/json";
    std::vector<std::pair<std::string, std::string>> headers;
    int timeoutSec = 30;
    size_t maxBody = 64u << 20;
};

HttpResp http_do(const HttpReq& req);

// http_stream — javob tanasini (dekodlangan) kelgani sayin onBody ga uzatadi.
// onBody false qaytarsa oqim to'xtaydi. Lokal inference dvigatellaridan (ollama/llama.cpp)
// tokenlarni real vaqtda olish uchun. Faqat http:// (lokal dvigatellar); https bo'lsa to'liq
// o'qib bir martada uzatadi.
HttpResp http_stream(const HttpReq& req, const std::function<bool(const char*, size_t)>& onBody);

// Qisqa variant: body bo'sh bo'lmasa Content-Type bilan yuboriladi
HttpResp http_request(const std::string& method, const std::string& url,
                      const std::string& body = "", const std::string& contentType = "application/json",
                      const std::string& bearerToken = "");

bool netInit();
void netCleanup();
bool tlsAvailable();

// --- ichki (testlar uchun ochiq) ---
struct ParsedUrl {
    std::string scheme, host, path;
    int port = 0;
};
bool parseUrl(const std::string& url, ParsedUrl& out);
// to'liq xom javobni (sarlavha + tana) tahlil qiladi
bool parseHttpResponse(const std::string& raw, HttpResp& resp, bool headRequest = false);
#ifdef _WIN32
HttpResp http_do_winhttp(const HttpReq& req); // https uchun; http bilan ham ishlaydi (testlar)
#endif
