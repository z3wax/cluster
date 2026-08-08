#include "json.hpp"
#include <cstdio>
int main() {
    // XUDDI test_json.cpp dagi kabi: const char* massiv + loop
    const char* tests[] = {
        R"({"a":"b"})",
        R"([])",
    };
    for (auto t : tests) {
        bool ok = false;
        Json j = Json::parse(t, &ok);
        printf("[constchar*] ok=%d type=%d dump=%s\n", ok, (int)j.type(), j.dump().c_str());
    }
    // string bilan
    std::string s = R"({"a":"b"})";
    bool ok2 = false;
    Json j2 = Json::parse(s, &ok2);
    printf("[string]     ok=%d type=%d dump=%s\n", ok2, (int)j2.type(), j2.dump().c_str());
    return 0;
}
