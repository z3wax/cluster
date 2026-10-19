#pragma once
// SDrive xotirasi: shifrlangan chunk'lar (ChaCha20 + HMAC-SHA256), lokal indeks, disk harfi.
//
// Chunk fayli formati:  "SDC1" | nonce(12) | shifrlangan ma'lumot | tag(32)
//   tag = HMAC-SHA256(macKey, "SDC1" | nonce | fileId | "|" | index | "|" | shifrlangan ma'lumot)
// Kalitlar 256-bitli asosiy kalitdan HMAC orqali hosil qilinadi va faqat shu kompyuterda saqlanadi.
#include "json.hpp"
#include <string>
#include <vector>
#include <mutex>
#include <cstdint>
#include <filesystem>

struct ChunkInfo {
    int index = 0;
    int64_t size = 0;
    std::string sha256; // ochiq ma'lumotning SHA-256 (hex)
};

class Storage {
public:
    static constexpr int64_t kOverhead = 4 + 12 + 32;

    bool open(const std::string& dir, const std::string& masterKeyHex, std::string& err);
    std::string dir() const;

    bool storeChunk(const std::string& fileId, int idx, const std::string& plain, std::string& err);
    // MAC tekshirilib, ochilgan ma'lumot qaytariladi
    bool loadChunk(const std::string& fileId, int idx, std::string& plain) const;
    // to'liq tekshiruv: MAC + hajm + SHA-256
    bool chunkValid(const std::string& fileId, const ChunkInfo& c) const;
    // tezkor tekshiruv: fayl bor va hajmi to'g'ri
    bool chunkPresent(const std::string& fileId, const ChunkInfo& c) const;
    void removeChunk(const std::string& fileId, int idx);
    void removeFile(const std::string& fileId);
    int64_t usedBytes() const;

    void addFile(const std::string& fileId, const std::vector<ChunkInfo>& chunks);
    bool hasFile(const std::string& fileId) const;
    std::vector<std::string> fileIds() const;
    std::vector<ChunkInfo> chunksOf(const std::string& fileId) const;
    size_t fileCount() const;

    // indeksdagi, lekin diskda yo'q/hajmi buzilgan fayllar (foydalanuvchi o'chirgan)
    std::vector<std::string> quickMissing() const;
    // navbat bilan budget baytgacha to'liq tekshiradi; buzilgan chunk'lar o'chiriladi
    std::vector<std::string> verifyBudget(int64_t budgetBytes);
    // indeksda yo'q (tugallanmagan) papkalarni o'chiradi
    int removeOrphans();
    // barcha klaster ma'lumotlarini o'chiradi (uninstall)
    void wipe();

    static bool validFileId(const std::string& id);

private:
    std::string dir_;
    std::string encKey_, macKey_;
    mutable std::mutex mu_;
    Json files_ = Json(Json::Object());
    size_t verifyCursor_ = 0;

    std::filesystem::path chunkPath(const std::string& fileId, int idx) const;
    std::string tag(const std::string& fileId, int idx, const std::string& nonce, const std::string& ct) const;
    void saveIndexLocked();
};

// SDrive papkasini yaratadi; Windowsda disk harfini (masalan S:) papkaga bog'laydi
bool ensureSdrive(const std::string& dir, const std::string& driveLetter, std::string& err);
void unmountSdrive(const std::string& driveLetter, const std::string& dir);
