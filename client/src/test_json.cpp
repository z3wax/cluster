#include "json.hpp"
#include <cstdio>
int main() {
    const char* tests[] = {
        R"({"driveLetter":"S","quotaGb":5,"sdriveDir":"C:/sdrive_test"})",
        R"({"a":"b"})",
        R"({"n":5,"arr":[1,2,3],"s":"hello"})",
        R"([])",
        R"({"x":{"y":[true,false,null,1.5]}})",
    };
    for (auto t : tests) {
        bool ok = false;
        Json j = Json::parse(t, &ok);
        printf("ok=%d type=%d  %s\n", ok, (int)j.type(), t);
    }
    return 0;
}
