#include "net.h"
#include "json.hpp"
#include <cstdio>
#include <ctime>

int main() {
    fprintf(stderr, "[t] start\n");
    netInit();
    fprintf(stderr, "[t] netInit ok\n");
    Json req;
    req["id"] = "net_test_1";
    req["name"] = "t";
    req["os"] = "Windows";
    req["platform"] = "x64";
    req["spaceTotal"] = 1000000.0;
    req["agreementVersion"] = "1.0";
    req["agreementAccepted"] = true;
    std::string body = req.dump();
    fprintf(stderr, "[t] dump ok: %s\n", body.c_str());
    time_t t0 = time(nullptr);
    fprintf(stderr, "[t] before http_request\n");
    HttpResp r = http_request("POST", "http://127.0.0.1:8080/api/nodes/register", body);
    fprintf(stderr, "[t] after http_request status=%d err=[%s]\n", r.status, r.error.c_str());
    time_t t1 = time(nullptr);
    fprintf(stderr, "[t] after time, %lds\n", (long)(t1 - t0));
    printf("RESULT status=%d error=[%s] body=%s\n", r.status, r.error.c_str(), r.body.substr(0, 120).c_str());
    fprintf(stderr, "[t] before return\n");
    fflush(stdout);
    return 0;
}
