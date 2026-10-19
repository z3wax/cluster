#pragma once
// Kriptografiya: SHA-256, HMAC-SHA256, ChaCha20 (RFC 8439), xavfsiz tasodifiy baytlar.
// Tashqi kutubxonasiz — barcha platformalarda bir xil ishlaydi.
#include <cstdint>
#include <cstddef>
#include <string>

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    void update(const std::string& s) { update(s.data(), s.size()); }
    std::string digest();     // 32 bayt (xom)
    std::string hexDigest();  // 64 belgili hex
private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t bufLen_ = 0;
    uint64_t total_ = 0;
};

std::string sha256(const std::string& data);       // xom 32 bayt
std::string sha256Hex(const std::string& data);    // hex
std::string hmacSha256(const std::string& key, const std::string& data); // xom 32 bayt

// ChaCha20 oqim shifri: data ni joyida shifrlaydi/ochadi (bir xil amal)
void chacha20Xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, uint8_t* data, size_t len);

std::string toHex(const std::string& raw);
bool fromHex(const std::string& hex, std::string& out);

// OS kriptografik tasodifiy generatori (BCryptGenRandom / /dev/urandom)
bool secureRandom(void* buf, size_t len);
std::string randomBytes(size_t len);

// Vaqtga bog'liq bo'lmagan taqqoslash
bool constEq(const std::string& a, const std::string& b);
