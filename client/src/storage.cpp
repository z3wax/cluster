#include "storage.h"
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

// ---- kriptografik asos: seed dan xorshift64 keystream ----
uint64_t hashSeed(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (char c : s) { h ^= (unsigned char)c; h *= 1099511628211ULL; }
    return h;
}

std::string xorStream(const std::string& data, uint64_t seed) {
    std::string out(data.size(), 0);
    uint64_t x = seed ? seed : 0x9E3779B97F4A7C15ULL;
    for (size_t i = 0; i < data.size(); i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        out[i] = (char)((unsigned char)data[i] ^ (unsigned char)(x & 0xFF));
    }
    return out;
}

static uint64_t chunkSeed(const std::string& nodeId, const std::string& fileId, int idx) {
    uint64_t a = hashSeed(nodeId + "|" + fileId);
    return a ^ (uint64_t)(idx + 1) * 0x9E3779B97F4A7C15ULL;
}

// ---- SDrive disk ----
bool ensureSdrive(const std::string& dir, const std::string& driveLetter, std::string& err) {
    try {
        fs::create_directories(dir);
        fs::create_directories(dir + "/chunks");
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
#ifdef _WIN32
    // subst bilan drive harfi bog'lash (faqat Windows)
    if (!driveLetter.empty()) {
        char letter = (char)toupper((unsigned char)driveLetter[0]);
        if (letter >= 'A' && letter <= 'Z') {
            bool used = (GetLogicalDrives() & (1u << (letter - 'A'))) != 0;
            if (!used) {
                std::string c = "subst " + std::string(1, letter) + ": \"" + dir + "\"";
                if (system(c.c_str()) != 0) {
                    err = "subst amalga oshmadi: " + c;
                    return false;
                }
            }
        }
    }
#endif
    (void)driveLetter;
    return true;
}

bool sdriveMounted(const std::string& driveLetter) {
#ifdef _WIN32
    if (driveLetter.empty()) return true;
    char root[4] = {driveLetter[0], ':', '\\', 0};
    return GetDriveTypeA(root) != DRIVE_NO_ROOT_DIR;
#else
    return true;
#endif
}

void unmountSdrive(const std::string& driveLetter) {
#ifdef _WIN32
    if (driveLetter.empty()) return;
    std::string c = "subst " + driveLetter + ": /D";
    system(c.c_str());
#endif
}

// ---- Chunk saqlash ----
bool storeChunk(const std::string& dir, const std::string& fileId, int chunkIndex,
                const std::string& rawData, const std::string& nodeId) {
    try {
        fs::path d = fs::path(dir) / "chunks" / fileId;
        fs::create_directories(d);
        std::string enc = xorStream(rawData, chunkSeed(nodeId, fileId, chunkIndex));
        fs::path p = d / (std::to_string(chunkIndex) + ".bin");
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f.write(enc.data(), (std::streamsize)enc.size());
        return true;
    } catch (...) {
        return false;
    }
}

std::string loadChunk(const std::string& dir, const std::string& fileId, int chunkIndex, const std::string& nodeId) {
    std::ifstream f(fs::path(dir) / "chunks" / fileId / (std::to_string(chunkIndex) + ".bin"), std::ios::binary);
    if (!f) return "";
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return xorStream(data, chunkSeed(nodeId, fileId, chunkIndex));
}

bool chunkOk(const std::string& dir, const std::string& fileId, int chunkIndex, int64_t expectedSize, const std::string& nodeId) {
    fs::path p = fs::path(dir) / "chunks" / fileId / (std::to_string(chunkIndex) + ".bin");
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    if (ec || sz != (uint64_t)expectedSize) return false;
    // shifrni ochib tekshirish
    std::string raw = loadChunk(dir, fileId, chunkIndex, nodeId);
    return raw.size() == (size_t)expectedSize;
}

int64_t sdriveUsedBytes(const std::string& dir) {
    int64_t total = 0;
    fs::path base = fs::path(dir) / "chunks";
    if (!fs::exists(base)) return 0;
    for (auto& d : fs::recursive_directory_iterator(base, fs::directory_options::skip_permission_denied)) {
        if (d.is_regular_file()) total += (int64_t)d.file_size();
    }
    return total;
}

// ---- Lokal index ----
void LocalIndex::load(const std::string& dir) {
    files = Json();
    std::ifstream f(fs::path(dir) / "index.json");
    if (f) {
        std::stringstream ss;
        ss << f.rdbuf();
        bool ok = false;
        Json j = Json::parse(ss.str(), &ok);
        if (ok && j.isObj()) files = j["files"];
    }
    if (files.isNull()) files = Json(Json::Object());
}

void LocalIndex::save(const std::string& dir) {
    Json o;
    o["files"] = files;
    std::ofstream f(fs::path(dir) / "index.json", std::ios::trunc);
    f << o.dump();
}

void LocalIndex::addFile(const std::string& dir, const std::string& fileId, const std::string& name,
                         const std::vector<ChunkInfo>& chunks) {
    Json f;
    f["name"] = name;
    Json arr(Json::Array{});
    for (const auto& c : chunks) {
        Json cj;
        cj["index"] = c.index;
        cj["size"] = (double)c.size;
        arr.push(cj);
    }
    f["chunks"] = arr;
    files[fileId] = f;
    save(dir);
}

std::vector<std::string> LocalIndex::fileIds() const {
    std::vector<std::string> out;
    if (!files.isObj()) return out;
    for (const auto& kv : files.asObj()) out.push_back(kv.first);
    return out;
}

std::vector<ChunkInfo> LocalIndex::chunksOf(const std::string& fileId) const {
    std::vector<ChunkInfo> out;
    const Json& f = files[fileId];
    if (!f.isObj()) return out;
    const auto& arr = f["chunks"].asArr();
    for (const auto& c : arr) out.push_back({(int)c["index"].asInt(), c["size"].asInt()});
    return out;
}

std::vector<std::string> findMissingFiles(const std::string& dir, const LocalIndex& idx, const std::string& nodeId) {
    std::vector<std::string> missing;
    for (const auto& fid : idx.fileIds()) {
        for (const auto& c : idx.chunksOf(fid)) {
            if (!chunkOk(dir, fid, c.index, c.size, nodeId)) {
                missing.push_back(fid);
                break;
            }
        }
    }
    return missing;
}
