// SDrive mijozi unit testlari: kriptografiya (rasmiy test vektorlari), JSON, HTTP, shifrlangan xotira
#include "config.h"
#include "crypto.h"
#include "json.hpp"
#include "net.h"
#include "storage.h"
#include "compute.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
#define CLOSE_SOCK(s) closesocket(s)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using sock_t = int;
#define CLOSE_SOCK(s) ::close(s)
#endif

namespace fs = std::filesystem;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failed++;                                                          \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                        \
    } while (0)
#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        g_checks++;                                                                                 \
        auto va = (a);                                                                              \
        auto vb = (b);                                                                              \
        if (!(va == vb)) {                                                                          \
            g_failed++;                                                                             \
            fprintf(stderr, "  FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b);                \
        }                                                                                           \
    } while (0)

static std::string unhex(const std::string& h) {
    std::string out;
    fromHex(h, out);
    return out;
}

static void testSha256() {
    CHECK_EQ(sha256Hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(sha256Hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    // million 'a' — bo'laklab (64 baytga karrali bo'lmagan qismlar bilan)
    Sha256 h;
    std::string block(997, 'a');
    size_t left = 1000000;
    while (left > 0) {
        size_t n = left < block.size() ? left : block.size();
        h.update(block.data(), n);
        left -= n;
    }
    CHECK_EQ(h.hexDigest(), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

static void testHmac() {
    CHECK_EQ(toHex(hmacSha256(std::string(20, '\x0b'), "Hi There")),
             std::string("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    CHECK_EQ(toHex(hmacSha256("Jefe", "what do ya want for nothing?")),
             std::string("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    CHECK_EQ(toHex(hmacSha256(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First")),
             std::string("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
}

static void testChaCha20() {
    // RFC 8439, 2.4.2
    std::string key = unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    std::string nonce = unhex("000000000000004a00000000");
    std::string pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    std::string data = pt;
    chacha20Xor((const uint8_t*)key.data(), (const uint8_t*)nonce.data(), 1, (uint8_t*)&data[0], data.size());
    CHECK_EQ(toHex(data), std::string("6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0bf91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d807ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab77937365af90bbf74a35be6b40b8eedf2785e42874d"));
    chacha20Xor((const uint8_t*)key.data(), (const uint8_t*)nonce.data(), 1, (uint8_t*)&data[0], data.size());
    CHECK_EQ(data, pt);
}

static void testHexRandom() {
    std::string raw;
    CHECK(fromHex("00ff10Ab", raw) && raw == std::string("\x00\xff\x10\xab", 4));
    CHECK(!fromHex("abc", raw));
    CHECK(!fromHex("zz", raw));
    std::string a = randomBytes(32), b = randomBytes(32);
    CHECK(a.size() == 32 && a != b);
    CHECK(constEq("abc", "abc") && !constEq("abc", "abd") && !constEq("abc", "ab"));
}

static void testJson() {
    bool ok = false;
    Json j = Json::parse(R"({"a":"b","n":5368709120,"f":0.05,"arr":[1,true,null,"x"],"o":{"k":"v"}})", &ok);
    CHECK(ok);
    CHECK_EQ(j["a"].asStr(), std::string("b"));
    CHECK_EQ(j["n"].asInt(), (int64_t)5368709120LL);
    CHECK(j["f"].asNum() == 0.05);
    CHECK_EQ(j["arr"].asArr().size(), (size_t)4);
    CHECK_EQ(j["o"]["k"].asStr(), std::string("v"));
    CHECK_EQ(j["missing"].asStr("def"), std::string("def"));
    CHECK_EQ(Json::parse(j.dump())["n"].asInt(), (int64_t)5368709120LL);

    Json u = Json::parse(R"("café 😀 \"q\" \\ \/")", &ok);
    CHECK(ok);
    CHECK_EQ(u.asStr(), std::string("caf\xc3\xa9 \xf0\x9f\x98\x80 \"q\" \\ /"));

    Json s(std::string("a\x01" "b\n\"c\""));
    CHECK_EQ(s.dump(), std::string("\"a\\u0001b\\n\\\"c\\\"\""));
    CHECK_EQ(Json::parse(s.dump()).asStr(), s.asStr());

    Json::parse("{\"a\":", &ok);
    CHECK(!ok);
    Json::parse("[1,2", &ok);
    CHECK(!ok);
    Json::parse("{\"a\":1} x", &ok);
    CHECK(!ok);
    Json::parse("-", &ok);
    CHECK(!ok);
    Json::parse("[1e3,-2.5]", &ok);
    CHECK(ok);

    Json d;
    d["x"] = 0.1;
    CHECK(Json::parse(d.dump())["x"].asNum() == 0.1);
}

static void testUrl() {
    ParsedUrl u;
    CHECK(parseUrl("http://example.com:8080/api/x?y=1", u));
    CHECK(u.scheme == "http" && u.host == "example.com" && u.port == 8080 && u.path == "/api/x?y=1");
    CHECK(parseUrl("https://cluster.uz", u));
    CHECK(u.scheme == "https" && u.port == 443 && u.path == "/");
    CHECK(parseUrl("http://[::1]:9000/p", u));
    CHECK(u.host == "::1" && u.port == 9000);
    CHECK(!parseUrl("ftp://x", u));
    CHECK(!parseUrl("example.com", u));
}

static void testHttpParse() {
    HttpResp r;
    CHECK(parseHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nX-A: b \r\n\r\nhello extra", r));
    CHECK(r.status == 200 && r.body == "hello" && r.header("x-a") == "b");
    CHECK(parseHttpResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5;ext=1\r\nhello\r\n6\r\n world\r\n0\r\n\r\n", r));
    CHECK_EQ(r.body, std::string("hello world"));
    CHECK(!parseHttpResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel", r));
    CHECK(!parseHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort", r));
    CHECK(parseHttpResponse("HTTP/1.1 404 Not Found\r\n\r\nbody-until-close", r));
    CHECK(r.status == 404 && r.body == "body-until-close");
    CHECK(parseHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n", r, true));
    CHECK(r.body.empty());
}

// Haqiqiy soket orqali: chunked va Content-Length javoblar, so'rov tanasi
static void testHttpSocket() {
    netInit();
    sock_t ls = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7F000001);
    addr.sin_port = 0;
    CHECK(::bind(ls, (sockaddr*)&addr, sizeof(addr)) == 0);
    CHECK(::listen(ls, 4) == 0);
    socklen_t len = sizeof(addr);
    getsockname(ls, (sockaddr*)&addr, &len);
    int port = ntohs(addr.sin_port);
    std::string bigBody(300000, 'z');
    std::atomic<int> served{0};
    std::string gotBody, gotAuth;
    std::thread srv([&] {
        for (int i = 0; i < 2; i++) {
            sock_t c = ::accept(ls, nullptr, nullptr);
            std::string req;
            char buf[65536];
            while (true) {
                int n = (int)::recv(c, buf, sizeof(buf), 0);
                if (n <= 0) break;
                req.append(buf, (size_t)n);
                size_t he = req.find("\r\n\r\n");
                if (he != std::string::npos) {
                    size_t cl = 0;
                    size_t p = req.find("Content-Length: ");
                    if (p != std::string::npos) cl = (size_t)atoi(req.c_str() + p + 16);
                    if (req.size() >= he + 4 + cl) break;
                }
            }
            std::string resp;
            if (i == 0) {
                gotBody = req.substr(req.find("\r\n\r\n") + 4);
                size_t a = req.find("Authorization: ");
                if (a != std::string::npos) gotAuth = req.substr(a + 15, req.find("\r\n", a) - a - 15);
                resp = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
                for (size_t off = 0; off < bigBody.size(); off += 70000) {
                    size_t n = std::min<size_t>(70000, bigBody.size() - off);
                    char hx[16];
                    snprintf(hx, sizeof(hx), "%zx\r\n", n);
                    resp += hx + bigBody.substr(off, n) + "\r\n";
                }
                resp += "0\r\n\r\n";
            } else {
                resp = "HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok";
            }
            size_t off = 0;
            while (off < resp.size()) {
                int w = (int)::send(c, resp.data() + off, (int)(resp.size() - off), 0);
                if (w <= 0) break;
                off += (size_t)w;
            }
            CLOSE_SOCK(c);
            served++;
        }
    });
    std::string base = "http://127.0.0.1:" + std::to_string(port);
    HttpResp r = http_request("POST", base + "/x", "{\"a\":1}", "application/json", "tok123");
    CHECK(r.status == 200);
    CHECK(r.body == bigBody);
    CHECK_EQ(gotBody, std::string("{\"a\":1}"));
    CHECK_EQ(gotAuth, std::string("Bearer tok123"));
#ifdef _WIN32
    HttpReq wr;
    wr.url = base + "/y";
    HttpResp r2 = http_do_winhttp(wr); // Windows HTTPS yo'li (WinHTTP) — shu yerda http bilan tekshiriladi
#else
    HttpResp r2 = http_request("GET", base + "/y");
#endif
    CHECK(r2.status == 201 && r2.body == "ok");
    srv.join();
    CLOSE_SOCK(ls);
    HttpResp bad = http_request("GET", "http://127.0.0.1:1/");
    CHECK(bad.status == 0 && !bad.error.empty());
}

static void testStorage() {
    fs::path dir = fs::temp_directory_path() / ("sdrive_test_" + toHex(randomBytes(4)));
    std::string key = toHex(randomBytes(32));
    Storage s;
    std::string err;
    CHECK(s.open(dir.string(), key, err));
    std::string plain(5000, '\0');
    for (size_t i = 0; i < plain.size(); i++) plain[i] = (char)(i * 31 + 7);
    ChunkInfo c0{0, (int64_t)plain.size(), sha256Hex(plain)};
    CHECK(s.storeChunk("f_abc123", 0, plain, err));
    std::string back;
    CHECK(s.loadChunk("f_abc123", 0, back) && back == plain);
    CHECK(s.chunkValid("f_abc123", c0));
    CHECK(s.chunkPresent("f_abc123", c0));

    // diskdagi fayl ochiq ma'lumotni o'z ichiga olmaydi
    std::ifstream f(dir / "chunks" / "f_abc123" / "0.bin", std::ios::binary);
    std::string onDisk((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    CHECK((int64_t)onDisk.size() == (int64_t)plain.size() + Storage::kOverhead);
    CHECK(onDisk.find(plain.substr(100, 64)) == std::string::npos);

    // boshqa kalit bilan ochib bo'lmaydi
    Storage other;
    CHECK(other.open(dir.string(), toHex(randomBytes(32)), err));
    CHECK(!other.loadChunk("f_abc123", 0, back));

    // bir bayt o'zgartirilsa — MAC aniqlaydi
    onDisk[50] ^= 1;
    {
        std::ofstream w(dir / "chunks" / "f_abc123" / "0.bin", std::ios::binary | std::ios::trunc);
        w.write(onDisk.data(), (std::streamsize)onDisk.size());
    }
    CHECK(!s.loadChunk("f_abc123", 0, back));
    CHECK(!s.chunkValid("f_abc123", c0));
    CHECK(s.chunkPresent("f_abc123", c0)); // hajm to'g'ri, lekin mazmun buzilgan

    // indeks, to'liq tekshiruv va tiklash
    CHECK(s.storeChunk("f_abc123", 0, plain, err));
    CHECK(s.storeChunk("f_abc123", 1, "tail", err));
    ChunkInfo c1{1, 4, sha256Hex("tail")};
    s.addFile("f_abc123", {c0, c1});
    CHECK(s.hasFile("f_abc123") && s.fileCount() == 1);
    CHECK(s.quickMissing().empty());
    CHECK(s.verifyBudget(1 << 30).empty());
    {
        std::ofstream w(dir / "chunks" / "f_abc123" / "1.bin", std::ios::binary | std::ios::trunc);
        w << std::string(4 + Storage::kOverhead, 'x');
    }
    auto bad = s.verifyBudget(1 << 30);
    CHECK(bad.size() == 1 && bad[0] == "f_abc123");
    auto missing = s.quickMissing();
    CHECK(missing.size() == 1 && missing[0] == "f_abc123");

    // indeks qayta ochilganda saqlanadi
    Storage again;
    CHECK(again.open(dir.string(), key, err));
    CHECK(again.hasFile("f_abc123") && again.chunksOf("f_abc123").size() == 2);
    CHECK(again.chunksOf("f_abc123")[1].sha256 == c1.sha256);

    // tugallanmagan papkalar tozalanadi, xavfli ID rad etiladi
    CHECK(s.storeChunk("f_orphan", 0, "x", err));
    CHECK(s.removeOrphans() == 1);
    CHECK(!fs::exists(dir / "chunks" / "f_orphan"));
    CHECK(!s.storeChunk("../evil", 0, "x", err));
    CHECK(!Storage::validFileId("a/b") && !Storage::validFileId("") && Storage::validFileId("f_0aF-9"));

    CHECK(s.usedBytes() > 0);
    s.removeFile("f_abc123");
    CHECK(!s.hasFile("f_abc123") && s.usedBytes() == 0);
    s.wipe();
    CHECK(s.fileCount() == 0);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Oddiy HTTP server: bitta so'rovga belgilangan javob qaytaradi (streaming testi uchun)
static int serveOnce(const std::string& response, int& port) {
    netInit();
    sock_t ls = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7F000001);
    addr.sin_port = 0;
    ::bind(ls, (sockaddr*)&addr, sizeof(addr));
    ::listen(ls, 4);
    socklen_t len = sizeof(addr);
    getsockname(ls, (sockaddr*)&addr, &len);
    port = ntohs(addr.sin_port);
    std::thread([ls, response] {
        sock_t c = ::accept(ls, nullptr, nullptr);
        char buf[8192];
        ::recv(c, buf, sizeof(buf), 0); // so'rovni o'qib tashlaymiz
        size_t off = 0;
        while (off < response.size()) {
            int w = (int)::send(c, response.data() + off, (int)(response.size() - off), 0);
            if (w <= 0) break;
            off += (size_t)w;
        }
        CLOSE_SOCK(c);
        CLOSE_SOCK(ls);
    }).detach();
    return 0;
}

static void testHttpStream() {
    // chunked javob — bo'laklab kelgan tokenlar to'g'ri yig'ilishi kerak
    int port = 0;
    std::string resp =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nHello\r\n"
        "1\r\n \r\n"
        "6\r\nworld!\r\n"
        "0\r\n\r\n";
    serveOnce(resp, port);
    HttpReq r;
    r.url = "http://127.0.0.1:" + std::to_string(port) + "/";
    std::string got;
    int parts = 0;
    HttpResp hr = http_stream(r, [&](const char* d, size_t n) {
        got.append(d, n);
        parts++;
        return true;
    });
    CHECK(hr.status == 200);
    CHECK_EQ(got, std::string("Hello world!"));
    CHECK(parts >= 3); // bo'laklab kelgan

    // content-length javob
    int port2 = 0;
    serveOnce("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nSalom dunyo", port2);
    HttpReq r2;
    r2.url = "http://127.0.0.1:" + std::to_string(port2) + "/";
    std::string got2;
    http_stream(r2, [&](const char* d, size_t n) { got2.append(d, n); return true; });
    CHECK_EQ(got2, std::string("Salom dunyo"));

    // onBody false qaytarsa oqim to'xtaydi — birinchi bo'lakdan keyin to'xtaymiz
    int port3 = 0;
    std::string chunked = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                          "4\r\nAAAA\r\n4\r\nBBBB\r\n4\r\nCCCC\r\n0\r\n\r\n";
    serveOnce(chunked, port3);
    HttpReq r3;
    r3.url = "http://127.0.0.1:" + std::to_string(port3) + "/";
    std::string collected;
    http_stream(r3, [&](const char* d, size_t n) {
        collected.append(d, n);
        return false; // birinchi bo'lakdan keyin to'xtaymiz
    });
    CHECK_EQ(collected, std::string("AAAA")); // qolgan bo'laklar kelmaydi
}

static void testEchoEngine() {
    Config cfg;
    cfg.computeModels = "gemma:2b,llama3";
    std::string name;
    auto e = makeEngine("echo", cfg);
    CHECK(e && e->available() && e->name() == "echo");
    CHECK(e->models().size() == 2);

    ComputeJob job;
    job.kind = "chat";
    job.model = "gemma:2b";
    job.messages = {{"system", "yordamchi"}, {"user", "bir ikki uch"}};
    std::string tokens, final, err;
    std::vector<double> emb;
    bool ok = e->run(
        job, [&](const std::string& t) {
            tokens += t;
            return true;
        },
        final, emb, err);
    CHECK(ok && final == "bir ikki uch" && tokens == "bir ikki uch");

    // bekor qilish: onToken false -> to'xtaydi
    int count = 0;
    e->run(
        job, [&](const std::string&) {
            count++;
            return false;
        },
        final, emb, err);
    CHECK(count == 1);

    // embedding
    ComputeJob ej;
    ej.kind = "embedding";
    ej.prompt = "matn";
    emb.clear();
    e->run(ej, [](const std::string&) { return true; }, final, emb, err);
    CHECK(emb.size() == 8);

    // auto rejimida echo tanlanmaydi (faqat haqiqiy dvigatellar)
    cfg.computeEngine = "auto";
    std::string chosen;
    auto autoE = selectEngine(cfg, chosen); // ollama/llama.cpp yo'q -> nullptr
    CHECK(!autoE);

    // buildComputeInfo — GPU/RAM/model maydonlari
    cfg.computeEnabled = true;
    Json info = buildComputeInfo(cfg, e.get(), "echo");
    CHECK(info["enabled"].asBool());
    CHECK(info["models"].asArr().size() == 2);
    CHECK(info["cpuCores"].asInt() >= 1);
    CHECK(info["engines"].asArr().size() == 1 && info["engines"].asArr()[0].asStr() == "echo");
}

int main() {
    setConfigDirOverride((fs::temp_directory_path() / "sdrive_test_cfg").string());
    struct T {
        const char* name;
        std::function<void()> fn;
    } tests[] = {
        {"sha256", testSha256},         {"hmac", testHmac},
        {"chacha20", testChaCha20},     {"hex/random", testHexRandom},
        {"json", testJson},             {"url", testUrl},
        {"http-parse", testHttpParse},  {"http-socket", testHttpSocket},
        {"http-stream", testHttpStream}, {"echo-engine", testEchoEngine},
        {"storage", testStorage},
    };
    for (auto& t : tests) {
        int before = g_failed;
        t.fn();
        printf("%-12s %s\n", t.name, g_failed == before ? "OK" : "FAIL");
    }
    printf("\n%d ta tekshiruv, %d ta xato\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
