#pragma once
// Hisoblash (GPU/CPU) ishchi tuguni: koordinatordan {model, prompt} turidagi ishlarni oladi va
// lokal inference dvigateli (ollama / llama.cpp / test "echo") orqali bajaradi.
// Koordinator ixtiyoriy BUYRUQ yubora olmaydi — faqat model nomi, matn va parametrlar.
#include "json.hpp"
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>
#include <thread>

struct Config;
struct AppState;

struct ComputeMessage {
    std::string role, content;
};

struct ComputeJob {
    std::string id, model, kind, prompt;
    std::vector<ComputeMessage> messages;
    Json params;
    std::string lastUser() const {
        for (auto it = messages.rbegin(); it != messages.rend(); ++it)
            if (it->role == "user") return it->content;
        return messages.empty() ? prompt : messages.back().content;
    }
};

// Inference dvigateli (adapter)
class Engine {
public:
    virtual ~Engine() = default;
    virtual std::string name() const = 0;
    virtual bool available() = 0;
    virtual std::vector<std::string> models() = 0;
    // run: tokenlarni onToken orqali uzatadi (false qaytarsa — bekor qilingan, to'xtaydi).
    // Muvaffaqiyatda finalText (yoki embedding) to'ldiriladi va true qaytadi.
    virtual bool run(const ComputeJob& job, const std::function<bool(const std::string&)>& onToken,
                     std::string& finalText, std::vector<double>& embedding, std::string& err) = 0;
};

std::unique_ptr<Engine> makeEngine(const std::string& kind, const Config& cfg);

// Konfiguratsiyaga mos dvigatelni tanlaydi (auto: ollama -> llama.cpp). Topilmasa nullptr.
std::unique_ptr<Engine> selectEngine(const Config& cfg, std::string& engineName);

// Register/heartbeat uchun imkoniyatlar JSON'i (modellar har safar yangilanadi)
Json buildComputeInfo(const Config& cfg, Engine* engine, const std::string& engineName);

// Hisoblash ishchisi: poll -> run -> stream/done oqimini maxParallel oqimda yuritadi
class ComputeAgent {
public:
    void start(AppState& st);
    void stop();
    bool enabled() const { return enabled_; }
    std::string engineName() const { return engineName_; }
    Engine* engine() const { return engine_.get(); }

private:
    void workerLoop(AppState& st);
    void runJob(AppState& st, const ComputeJob& job);
    std::unique_ptr<Engine> engine_;
    std::string engineName_;
    std::atomic<bool> run_{false};
    std::atomic<bool> enabled_{false};
    std::vector<std::thread> threads_;
};
