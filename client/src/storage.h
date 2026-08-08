#pragma once
// SDrive xotira boshqaruvi: disk yaratish (Windows subst), shifrlash, chunk saqlash
#include "json.hpp"
#include <string>
#include <vector>
#include <cstdint>

struct ChunkInfo {
    int index = 0;
    int64_t size = 0;
};

// SDrive papkasini yaratadi; Windowsda subst bilan drive harfini bog'laydi
// driveLetter masalan "S". Har safar kompyuter yoqilganda chaqiriladi.
bool ensureSdrive(const std::string& dir, const std::string& driveLetter, std::string& err);

// drive bog'langanmi tekshirish
bool sdriveMounted(const std::string& driveLetter);

// Drive bog'lanishni olib tashlash (uninstall)
void unmountSdrive(const std::string& driveLetter);

// XOR-shifrlash bilan chunk saqlash. chunk: serverdan olingan xom baytlar.
// key: nodeId asosida hosil qilinadi (MVP; prod: AES-256-GCM)
bool storeChunk(const std::string& dir, const std::string& fileId, int chunkIndex,
                const std::string& rawData, const std::string& nodeId);

// Chunk shifrni ochish va qaytarish (verifikatsiya uchun)
std::string loadChunk(const std::string& dir, const std::string& fileId, int chunkIndex, const std::string& nodeId);

// Chunk bor va hajmi to'g'rimi
bool chunkOk(const std::string& dir, const std::string& fileId, int chunkIndex, int64_t expectedSize, const std::string& nodeId);

// sdrive dagi barcha chunk'lar bo'yicha band joy
int64_t sdriveUsedBytes(const std::string& dir);

// Lokal index (sdrive/index.json) — qaysi fayllar saqlanmoqda
struct LocalIndex {
    Json files; // { fileId: {name, chunks:[{index,size}]} }
    void load(const std::string& dir);
    void save(const std::string& dir);
    void addFile(const std::string& dir, const std::string& fileId, const std::string& name,
                 const std::vector<ChunkInfo>& chunks);
    std::vector<std::string> fileIds() const;
    std::vector<ChunkInfo> chunksOf(const std::string& fileId) const;
};

// Foydalanuvchi o'chirib tashlagan chunk'larni topadi
std::vector<std::string> findMissingFiles(const std::string& dir, const LocalIndex& idx, const std::string& nodeId);

std::string xorStream(const std::string& data, uint64_t seed);
