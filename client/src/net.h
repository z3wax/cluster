#pragma once
// HTTP client — raw socket, cross-platform (Winsock / POSIX)
#include <string>
#include <map>

struct HttpResp {
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string error; // agar ulanish muvaffaqiyatsiz bo'lsa
    bool ok() const { return status >= 200 && status < 300; }
};

// GET/POST so'rov. body bo'sh bo'lmasa POST, aks holda GET.
HttpResp http_request(const std::string& method, const std::string& url,
                      const std::string& body = "", const std::string& contentType = "application/json");

bool netInit();
void netCleanup();
