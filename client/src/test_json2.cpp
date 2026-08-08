#include "json.hpp"
#include <cstdio>

// debug: parseValue ni kuzatish uchun qo'lda chaqirish
static void step(const std::string& s, size_t i) {
    printf("  step: pos=%zu char='%c'\n", i, i < s.size() ? s[i] : '?');
}

int main() {
    const std::string s = R"({"a":"b"})";
    bool ok = true;
    size_t i = 0;
    Json j = Json::parse(s, &ok);
    printf("final: ok=%d type=%d dump=%s\n", ok, (int)j.type(), j.dump().c_str());

    // Json xususiyatlarini tekshiramiz
    Json num(5);
    printf("Json(5): type=%d (Num kutilgan=2)\n", (int)num.type());
    Json b(true);
    printf("Json(true): type=%d (Bool kutilgan=1)\n", (int)b.type());
    Json str(std::string("x"));
    printf("Json(string): type=%d (Str kutilgan=3)\n", (int)str.type());
    Json i2(42LL);
    printf("Json(int64): type=%d (Num kutilgan=2)\n", (int)i2.type());
    return 0;
}
