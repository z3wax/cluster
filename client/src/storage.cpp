#include "storage.h"
#include "crypto.h"
#include "config.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <ctime>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

static const char kMagic[4] = {'S', 'D', 'C', '1'};

static fs::path u8path(const std::string& s) {
#if defined(_WIN32) && __cplusplus <= 201703L
    return fs::u8path(s);
#else
    return fs::path(s);
#endif
}

// Mavjud faylni atomik almashtirish (Windowsda ham)
static bool replaceFile(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
    return MoveFileExW(from.wstring().c_str(), to.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code ec;
    fs::rename(from, to, ec);
    return !ec;
#endif
}

static bool writeAtomic(const fs::path& p, const std::string& data) {
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), (std::streamsize)data.size());
        f.flush();
        if (!f.good()) return false;
    }
    if (!replaceFile(tmp, p)) {
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

static bool readAll(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool Storage::validFileId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id)
        if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-')) return false;
    return true;
}

bool Storage::open(const std::string& dir, const std::string& masterKeyHex, std::string& err) {
    std::string master;
    if (!fromHex(masterKeyHex, master) || master.size() != 32) {
        err = "shifrlash kaliti noto'g'ri";
        return false;
    }
    std::error_code ec;
    fs::create_directories(u8path(dir) / "chunks", ec);
    if (ec) {
        err = "papka yaratilmadi: " + ec.message();
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    dir_ = dir;
    encKey_ = hmacSha256(master, "sdrive-chunk-enc-v1");
    macKey_ = hmacSha256(master, "sdrive-chunk-mac-v1");
    files_ = Json(Json::Object());
    std::string raw;
    if (readAll(u8path(dir) / "index.json", raw)) {
        bool ok = false;
        Json j = Json::parse(raw, &ok);
        if (ok && j["files"].isObj()) files_ = j["files"];
    }
    fs::path readme = u8path(dir) / "README.txt";
    if (!fs::exists(readme, ec)) {
        std::ofstream f(readme);
        f << "SDrive Cluster\r\n\r\n"
             "Bu papkada SDrive Cluster tarmog'ining shifrlangan fayl nusxalari saqlanadi.\r\n"
             "Fayllarni dasturisiz o'qib bo'lmaydi. O'chirilgan fayllar avtomatik qayta yuklab olinadi\r\n"
             "(shartnoma 3.2-band), qayta yuklanguncha ular uchun to'lov to'xtaydi (3.3-band).\r\n";
    }
    return true;
}

std::string Storage::dir() const {
    std::lock_guard<std::mutex> lk(mu_);
    return dir_;
}

fs::path Storage::chunkPath(const std::string& fileId, int idx) const {
    std::string d;
    {
        std::lock_guard<std::mutex> lk(mu_);
        d = dir_;
    }
    return u8path(d) / "chunks" / fileId / (std::to_string(idx) + ".bin");
}

std::string Storage::tag(const std::string& fileId, int idx, const std::string& nonce, const std::string& ct) const {
    std::string aad;
    aad.reserve(4 + nonce.size() + fileId.size() + 16 + ct.size());
    aad.append(kMagic, 4);
    aad += nonce;
    aad += fileId;
    aad += "|" + std::to_string(idx) + "|";
    aad += ct;
    return hmacSha256(macKey_, aad);
}

bool Storage::storeChunk(const std::string& fileId, int idx, const std::string& plain, std::string& err) {
    if (!validFileId(fileId)) { err = "fayl ID noto'g'ri"; return false; }
    std::string nonce = randomBytes(12);
    std::string ct = plain;
    chacha20Xor((const uint8_t*)encKey_.data(), (const uint8_t*)nonce.data(), 1, (uint8_t*)&ct[0], ct.size());
    std::string out;
    out.reserve(plain.size() + kOverhead);
    out.append(kMagic, 4);
    out += nonce;
    out += ct;
    out += tag(fileId, idx, nonce, ct);
    fs::path p = chunkPath(fileId, idx);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (!writeAtomic(p, out)) {
        err = "diskka yozib bo'lmadi (disk to'lgan yoki ruxsat yo'q)";
        return false;
    }
    return true;
}

bool Storage::loadChunk(const std::string& fileId, int idx, std::string& plain) const {
    if (!validFileId(fileId)) return false;
    std::string raw;
    if (!readAll(chunkPath(fileId, idx), raw)) return false;
    if ((int64_t)raw.size() < kOverhead || raw.compare(0, 4, kMagic, 4) != 0) return false;
    std::string nonce = raw.substr(4, 12);
    std::string ct = raw.substr(16, raw.size() - kOverhead);
    std::string tg = raw.substr(raw.size() - 32);
    if (!constEq(tg, tag(fileId, idx, nonce, ct))) return false;
    plain = ct;
    chacha20Xor((const uint8_t*)encKey_.data(), (const uint8_t*)nonce.data(), 1, (uint8_t*)&plain[0], plain.size());
    return true;
}

bool Storage::chunkValid(const std::string& fileId, const ChunkInfo& c) const {
    std::string plain;
    if (c.sha256.empty() || !loadChunk(fileId, c.index, plain)) return false;
    return (int64_t)plain.size() == c.size && sha256Hex(plain) == c.sha256;
}

bool Storage::chunkPresent(const std::string& fileId, const ChunkInfo& c) const {
    std::error_code ec;
    auto sz = fs::file_size(chunkPath(fileId, c.index), ec);
    return !ec && (int64_t)sz == c.size + kOverhead;
}

void Storage::removeChunk(const std::string& fileId, int idx) {
    if (!validFileId(fileId)) return;
    std::error_code ec;
    fs::remove(chunkPath(fileId, idx), ec);
}

void Storage::removeFile(const std::string& fileId) {
    if (!validFileId(fileId)) return;
    std::lock_guard<std::mutex> lk(mu_);
    std::error_code ec;
    fs::remove_all(u8path(dir_) / "chunks" / fileId, ec);
    if (files_.has(fileId)) {
        files_.erase(fileId);
        saveIndexLocked();
    }
}

int64_t Storage::usedBytes() const {
    fs::path base;
    {
        std::lock_guard<std::mutex> lk(mu_);
        base = u8path(dir_) / "chunks";
    }
    int64_t total = 0;
    std::error_code ec;
    if (!fs::exists(base, ec)) return 0;
    for (auto it = fs::recursive_directory_iterator(base, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) total += (int64_t)it->file_size(e2);
    }
    return total;
}

void Storage::saveIndexLocked() {
    Json o;
    o["version"] = 2;
    o["files"] = files_;
    writeAtomic(u8path(dir_) / "index.json", o.dump());
}

void Storage::addFile(const std::string& fileId, const std::vector<ChunkInfo>& chunks) {
    if (!validFileId(fileId)) return;
    Json f;
    Json arr(Json::Array{});
    for (const auto& c : chunks) {
        Json cj;
        cj["index"] = c.index;
        cj["size"] = (double)c.size;
        cj["sha256"] = c.sha256;
        arr.push(cj);
    }
    f["chunks"] = arr;
    f["added"] = (double)time(nullptr);
    std::lock_guard<std::mutex> lk(mu_);
    files_[fileId] = f;
    saveIndexLocked();
}

bool Storage::hasFile(const std::string& fileId) const {
    std::lock_guard<std::mutex> lk(mu_);
    return files_.has(fileId);
}

std::vector<std::string> Storage::fileIds() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    for (const auto& kv : files_.asObj()) out.push_back(kv.first);
    return out;
}

size_t Storage::fileCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return files_.asObj().size();
}

std::vector<ChunkInfo> Storage::chunksOf(const std::string& fileId) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<ChunkInfo> out;
    for (const auto& c : files_[fileId]["chunks"].asArr())
        out.push_back({(int)c["index"].asInt(), c["size"].asInt(), c["sha256"].asStr()});
    return out;
}

std::vector<std::string> Storage::quickMissing() const {
    std::vector<std::string> missing;
    for (const auto& fid : fileIds()) {
        for (const auto& c : chunksOf(fid)) {
            if (!chunkPresent(fid, c)) {
                missing.push_back(fid);
                break;
            }
        }
    }
    return missing;
}

std::vector<std::string> Storage::verifyBudget(int64_t budgetBytes) {
    std::vector<std::pair<std::string, ChunkInfo>> all;
    for (const auto& fid : fileIds())
        for (const auto& c : chunksOf(fid)) all.push_back({fid, c});
    std::vector<std::string> bad;
    if (all.empty()) return bad;
    size_t start;
    {
        std::lock_guard<std::mutex> lk(mu_);
        start = verifyCursor_ % all.size();
    }
    int64_t spent = 0;
    size_t i = 0;
    for (; i < all.size() && spent < budgetBytes; i++) {
        const auto& item = all[(start + i) % all.size()];
        spent += item.second.size;
        if (!chunkPresent(item.first, item.second)) continue; // quickMissing aniqlaydi
        if (!chunkValid(item.first, item.second)) {
            logf("[storage] buzilgan chunk: %s/%d — qayta yuklanadi", item.first.c_str(), item.second.index);
            removeChunk(item.first, item.second.index);
            bad.push_back(item.first);
        }
    }
    std::lock_guard<std::mutex> lk(mu_);
    verifyCursor_ = start + i;
    return bad;
}

int Storage::removeOrphans() {
    fs::path base;
    {
        std::lock_guard<std::mutex> lk(mu_);
        base = u8path(dir_) / "chunks";
    }
    int removed = 0;
    std::error_code ec;
    std::vector<fs::path> victims;
    for (auto it = fs::directory_iterator(base, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (!hasFile(name)) victims.push_back(it->path());
    }
    for (const auto& p : victims) {
        std::error_code e2;
        fs::remove_all(p, e2);
        removed++;
    }
    return removed;
}

void Storage::wipe() {
    std::lock_guard<std::mutex> lk(mu_);
    std::error_code ec;
    fs::remove_all(u8path(dir_) / "chunks", ec);
    fs::remove(u8path(dir_) / "index.json", ec);
    files_ = Json(Json::Object());
}

// ---------------- SDrive disk harfi ----------------

bool ensureSdrive(const std::string& dir, const std::string& driveLetter, std::string& err) {
    std::error_code ec;
    fs::create_directories(u8path(dir) / "chunks", ec);
    if (ec) {
        err = "papka yaratilmadi: " + ec.message();
        return false;
    }
#ifdef _WIN32
    // subst bilan bir xil: S: -> papka (administrator huquqi talab qilinmaydi)
    if (!driveLetter.empty()) {
        char letter = (char)toupper((unsigned char)driveLetter[0]);
        if (letter < 'A' || letter > 'Z') {
            err = "disk harfi noto'g'ri";
            return false;
        }
        std::string dev = std::string(1, letter) + ":";
        std::wstring wdir = fs::absolute(u8path(dir)).wstring();
        std::wstring wdev(dev.begin(), dev.end());
        wchar_t target[1024] = {0};
        if (QueryDosDeviceW(wdev.c_str(), target, 1024)) {
            std::wstring t(target);
            std::wstring want = L"\\??\\" + wdir;
            if (_wcsicmp(t.c_str(), want.c_str()) != 0) {
                err = dev + " disk harfi band — boshqa harf tanlang";
                return false;
            }
            return true; // allaqachon bog'langan
        }
        if (!DefineDosDeviceW(0, wdev.c_str(), wdir.c_str())) {
            err = dev + " diskini yaratib bo'lmadi (DefineDosDevice)";
            return false;
        }
    }
#else
    (void)driveLetter;
#endif
    return true;
}

void unmountSdrive(const std::string& driveLetter, const std::string& dir) {
#ifdef _WIN32
    if (driveLetter.empty()) return;
    char letter = (char)toupper((unsigned char)driveLetter[0]);
    std::wstring wdev = {(wchar_t)letter, L':'};
    std::wstring wdir = fs::absolute(u8path(dir)).wstring();
    DefineDosDeviceW(DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE, wdev.c_str(), wdir.c_str());
#else
    (void)driveLetter;
    (void)dir;
#endif
}
