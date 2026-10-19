#include "crypto.h"
#include <cstring>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#endif
#endif

// ---------------- SHA-256 (FIPS 180-4) ----------------

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

Sha256::Sha256() {
    static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(h_, init, sizeof(h_));
}

void Sha256::block(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}

void Sha256::update(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    total_ += len;
    if (bufLen_ > 0) {
        size_t take = 64 - bufLen_ < len ? 64 - bufLen_ : len;
        memcpy(buf_ + bufLen_, p, take);
        bufLen_ += take; p += take; len -= take;
        if (bufLen_ == 64) { block(buf_); bufLen_ = 0; }
    }
    while (len >= 64) { block(p); p += 64; len -= 64; }
    if (len > 0) { memcpy(buf_, p, len); bufLen_ = len; }
}

std::string Sha256::digest() {
    uint64_t bits = total_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (bufLen_ != 56) update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(len, 8);
    std::string out(32, '\0');
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (char)(h_[i] >> 24);
        out[i * 4 + 1] = (char)(h_[i] >> 16);
        out[i * 4 + 2] = (char)(h_[i] >> 8);
        out[i * 4 + 3] = (char)h_[i];
    }
    return out;
}

std::string Sha256::hexDigest() { return toHex(digest()); }

std::string sha256(const std::string& data) {
    Sha256 h;
    h.update(data);
    return h.digest();
}

std::string sha256Hex(const std::string& data) { return toHex(sha256(data)); }

// ---------------- HMAC-SHA256 (RFC 2104) ----------------

std::string hmacSha256(const std::string& key, const std::string& data) {
    std::string k = key.size() > 64 ? sha256(key) : key;
    k.resize(64, '\0');
    std::string ipad(64, '\0'), opad(64, '\0');
    for (int i = 0; i < 64; i++) {
        ipad[i] = (char)(k[i] ^ 0x36);
        opad[i] = (char)(k[i] ^ 0x5c);
    }
    Sha256 inner;
    inner.update(ipad);
    inner.update(data);
    Sha256 outer;
    outer.update(opad);
    outer.update(inner.digest());
    return outer.digest();
}

// ---------------- ChaCha20 (RFC 8439) ----------------

static inline uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static inline uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

#define QR(a, b, c, d)                \
    a += b; d ^= a; d = rotl(d, 16);  \
    c += d; b ^= c; b = rotl(b, 12);  \
    a += b; d ^= a; d = rotl(d, 8);   \
    c += d; b ^= c; b = rotl(b, 7);

static void chachaBlock(const uint32_t in[16], uint8_t out[64]) {
    uint32_t x[16];
    memcpy(x, in, sizeof(x));
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) {
        uint32_t v = x[i] + in[i];
        out[i * 4] = (uint8_t)v;
        out[i * 4 + 1] = (uint8_t)(v >> 8);
        out[i * 4 + 2] = (uint8_t)(v >> 16);
        out[i * 4 + 3] = (uint8_t)(v >> 24);
    }
}

void chacha20Xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, uint8_t* data, size_t len) {
    uint32_t st[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; i++) st[4 + i] = le32(key + i * 4);
    st[12] = counter;
    st[13] = le32(nonce);
    st[14] = le32(nonce + 4);
    st[15] = le32(nonce + 8);
    uint8_t ks[64];
    size_t off = 0;
    while (off < len) {
        chachaBlock(st, ks);
        size_t n = len - off < 64 ? len - off : 64;
        for (size_t i = 0; i < n; i++) data[off + i] ^= ks[i];
        off += n;
        st[12]++;
    }
}

// ---------------- Yordamchilar ----------------

std::string toHex(const std::string& raw) {
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char c : raw) {
        out += d[c >> 4];
        out += d[c & 15];
    }
    return out;
}

static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool fromHex(const std::string& hex, std::string& out) {
    if (hex.size() % 2) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = hexVal(hex[i]), lo = hexVal(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out += (char)(hi << 4 | lo);
    }
    return true;
}

bool secureRandom(void* buf, size_t len) {
#ifdef _WIN32
    return BCRYPT_SUCCESS(BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG));
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, len, f);
    fclose(f);
    return n == len;
#endif
}

std::string randomBytes(size_t len) {
    std::string s(len, '\0');
    if (!secureRandom(&s[0], len)) {
        fprintf(stderr, "[sdrive] FATAL: tasodifiy generator ishlamadi\n");
        abort();
    }
    return s;
}

bool constEq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char r = 0;
    for (size_t i = 0; i < a.size(); i++) r |= (unsigned char)(a[i] ^ b[i]);
    return r == 0;
}
