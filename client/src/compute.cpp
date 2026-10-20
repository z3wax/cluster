#include "compute.h"
#include "net.h"
#include "platform.h"
#include "config.h"
#include <sstream>

// ---------------- Yordamchilar ----------------

static void applyParams(Json& dst, const Json& params, bool ollamaStyle) {
    if (!params.isObj()) return;
    auto num = [&](const char* k) -> const Json& { return params[k]; };
    double temp = num("temperature").asNum(-1);
    double topP = num("top_p").asNum(-1);
    int64_t maxTok = num("max_tokens").asInt(0);
    int64_t seed = num("seed").asInt(-1);
    if (ollamaStyle) {
        Json opt;
        if (temp >= 0) opt["temperature"] = temp;
        if (topP >= 0) opt["top_p"] = topP;
        if (maxTok > 0) opt["num_predict"] = (double)maxTok;
        if (seed >= 0) opt["seed"] = (double)seed;
        if (opt.isObj()) dst["options"] = opt;
    } else {
        if (temp >= 0) dst["temperature"] = temp;
        if (topP >= 0) dst["top_p"] = topP;
        if (maxTok > 0) dst["max_tokens"] = (double)maxTok;
        if (seed >= 0) dst["seed"] = (double)seed;
    }
}

static std::vector<double> embedOf(const Json& j, const char* key) {
    std::vector<double> out;
    for (const auto& v : j[key].asArr()) out.push_back(v.asNum(0));
    return out;
}

// ================= Echo dvigateli (test / dvigatelsiz mashinalar) =================

class EchoEngine : public Engine {
public:
    explicit EchoEngine(std::vector<std::string> models) : models_(std::move(models)) {
        if (models_.empty()) models_.push_back("echo");
    }
    std::string name() const override { return "echo"; }
    bool available() override { return true; }
    std::vector<std::string> models() override { return models_; }

    bool run(const ComputeJob& job, const std::function<bool(const std::string&)>& onToken,
             std::string& finalText, std::vector<double>& embedding, std::string& err) override {
        (void)err;
        std::string text = job.kind == "chat" ? job.lastUser() : job.prompt;
        if (job.kind == "embedding") {
            embedding.assign(8, 0.0);
            int i = 0;
            for (unsigned char ch : text) embedding[(i++) % 8] += (ch % 17) / 17.0;
            return true;
        }
        // so'zma-so'z oqim (bo'sh joyni saqlab)
        size_t start = 0;
        for (size_t i = 0; i <= text.size(); i++) {
            if (i == text.size() || text[i] == ' ') {
                std::string tok = text.substr(start, i - start + (i < text.size() ? 1 : 0));
                if (!tok.empty() && !onToken(tok)) return true; // bekor qilingan
                start = i + 1;
            }
        }
        finalText = text;
        return true;
    }

private:
    std::vector<std::string> models_;
};

// ================= Ollama dvigateli =================

class OllamaEngine : public Engine {
public:
    explicit OllamaEngine(std::string url) : url_(std::move(url)) {}
    std::string name() const override { return "ollama"; }

    bool available() override {
        HttpResp r = http_request("GET", url_ + "/api/tags", "", "application/json");
        return r.ok();
    }

    std::vector<std::string> models() override {
        std::vector<std::string> out;
        HttpResp r = http_request("GET", url_ + "/api/tags");
        if (!r.ok()) return out;
        bool ok = false;
        Json j = Json::parse(r.body, &ok);
        if (ok) {
            for (const auto& m : j["models"].asArr()) {
                std::string nm = m["name"].asStr();
                if (!nm.empty()) out.push_back(nm);
            }
        }
        return out;
    }

    bool run(const ComputeJob& job, const std::function<bool(const std::string&)>& onToken,
             std::string& finalText, std::vector<double>& embedding, std::string& err) override {
        if (job.kind == "embedding") {
            Json req;
            req["model"] = job.model;
            req["prompt"] = job.prompt;
            HttpResp r = http_request("POST", url_ + "/api/embeddings", req.dump());
            if (!r.ok()) {
                err = "ollama embeddings: " + describeHttp(r);
                return false;
            }
            bool ok = false;
            Json j = Json::parse(r.body, &ok);
            embedding = embedOf(j, "embedding");
            return !embedding.empty();
        }

        std::string path;
        Json req;
        req["model"] = job.model;
        req["stream"] = true;
        applyParams(req, job.params, true);
        if (job.kind == "chat") {
            path = "/api/chat";
            Json msgs(Json::Array{});
            for (const auto& m : job.messages) {
                Json mj;
                mj["role"] = m.role;
                mj["content"] = m.content;
                msgs.push(mj);
            }
            req["messages"] = msgs;
        } else {
            path = "/api/generate";
            req["prompt"] = job.prompt;
        }

        HttpReq hr;
        hr.method = "POST";
        hr.url = url_ + path;
        hr.body = req.dump();
        hr.timeoutSec = 600;
        std::string lineBuf, errField;
        bool aborted = false, sawDone = false;
        HttpResp resp = http_stream(hr, [&](const char* data, size_t n) -> bool {
            lineBuf.append(data, n);
            size_t nl;
            while ((nl = lineBuf.find('\n')) != std::string::npos) {
                std::string line = lineBuf.substr(0, nl);
                lineBuf.erase(0, nl + 1);
                if (line.empty()) continue;
                bool ok = false;
                Json j = Json::parse(line, &ok);
                if (!ok) continue;
                if (j.has("error")) {
                    errField = j["error"].asStr();
                    return false;
                }
                std::string tok = job.kind == "chat" ? j["message"]["content"].asStr() : j["response"].asStr();
                if (!tok.empty()) {
                    finalText += tok;
                    if (!onToken(tok)) {
                        aborted = true;
                        return false;
                    }
                }
                if (j["done"].asBool(false)) {
                    sawDone = true;
                    return false;
                }
            }
            return true;
        });
        if (aborted) return true;
        if (!errField.empty()) {
            err = "ollama: " + errField;
            return false;
        }
        if (!resp.ok() && !sawDone && finalText.empty()) {
            err = "ollama " + path + ": " + describeHttp(resp);
            return false;
        }
        return true;
    }

private:
    static std::string describeHttp(const HttpResp& r) {
        if (r.status == 0) return r.error.empty() ? "ulanib bo'lmadi" : r.error;
        return "HTTP " + std::to_string(r.status);
    }
    std::string url_;
};

// ================= llama.cpp server (OpenAI-mos) =================

class LlamaEngine : public Engine {
public:
    explicit LlamaEngine(std::string url) : url_(std::move(url)) {}
    std::string name() const override { return "llama.cpp"; }

    bool available() override {
        HttpResp r = http_request("GET", url_ + "/v1/models");
        if (r.ok()) return true;
        r = http_request("GET", url_ + "/health");
        return r.ok();
    }

    std::vector<std::string> models() override {
        std::vector<std::string> out;
        HttpResp r = http_request("GET", url_ + "/v1/models");
        if (!r.ok()) return out;
        bool ok = false;
        Json j = Json::parse(r.body, &ok);
        if (ok) {
            for (const auto& m : j["data"].asArr()) {
                std::string id = m["id"].asStr();
                if (!id.empty()) out.push_back(id);
            }
        }
        return out;
    }

    bool run(const ComputeJob& job, const std::function<bool(const std::string&)>& onToken,
             std::string& finalText, std::vector<double>& embedding, std::string& err) override {
        if (job.kind == "embedding") {
            Json req;
            req["model"] = job.model;
            req["input"] = job.prompt;
            HttpResp r = http_request("POST", url_ + "/v1/embeddings", req.dump());
            if (!r.ok()) {
                err = "llama embeddings: HTTP " + std::to_string(r.status);
                return false;
            }
            bool ok = false;
            Json j = Json::parse(r.body, &ok);
            embedding = embedOf(j["data"].asArr().empty() ? Json() : j["data"].asArr()[0], "embedding");
            return !embedding.empty();
        }

        Json req;
        req["model"] = job.model;
        req["stream"] = true;
        applyParams(req, job.params, false);
        Json msgs(Json::Array{});
        if (job.kind == "chat") {
            for (const auto& m : job.messages) {
                Json mj;
                mj["role"] = m.role;
                mj["content"] = m.content;
                msgs.push(mj);
            }
        } else {
            Json mj;
            mj["role"] = "user";
            mj["content"] = job.prompt;
            msgs.push(mj);
        }
        req["messages"] = msgs;

        HttpReq hr;
        hr.method = "POST";
        hr.url = url_ + "/v1/chat/completions";
        hr.body = req.dump();
        hr.timeoutSec = 600;
        std::string lineBuf;
        bool aborted = false, sawDone = false;
        HttpResp resp = http_stream(hr, [&](const char* data, size_t n) -> bool {
            lineBuf.append(data, n);
            size_t nl;
            while ((nl = lineBuf.find('\n')) != std::string::npos) {
                std::string line = lineBuf.substr(0, nl);
                lineBuf.erase(0, nl + 1);
                while (!line.empty() && (line.back() == '\r')) line.pop_back();
                if (line.rfind("data: ", 0) != 0) continue;
                std::string payload = line.substr(6);
                if (payload == "[DONE]") {
                    sawDone = true;
                    return false;
                }
                bool ok = false;
                Json j = Json::parse(payload, &ok);
                if (!ok) continue;
                const Json& choices = j["choices"];
                if (!choices.asArr().empty()) {
                    std::string tok = choices.asArr()[0]["delta"]["content"].asStr();
                    if (!tok.empty()) {
                        finalText += tok;
                        if (!onToken(tok)) {
                            aborted = true;
                            return false;
                        }
                    }
                }
            }
            return true;
        });
        if (aborted) return true;
        if (!resp.ok() && !sawDone && finalText.empty()) {
            err = "llama.cpp: HTTP " + std::to_string(resp.status) + " " + resp.error;
            return false;
        }
        return true;
    }

private:
    std::string url_;
};

// ================= Dvigatel tanlash + imkoniyatlar =================

static std::vector<std::string> splitCSV(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        while (!item.empty() && item.front() == ' ') item.erase(item.begin());
        while (!item.empty() && item.back() == ' ') item.pop_back();
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

std::unique_ptr<Engine> makeEngine(const std::string& kind, const Config& cfg) {
    if (kind == "echo") return std::make_unique<EchoEngine>(splitCSV(cfg.computeModels));
    if (kind == "ollama") return std::make_unique<OllamaEngine>(cfg.ollamaUrl);
    if (kind == "llama.cpp") return std::make_unique<LlamaEngine>(cfg.llamaUrl);
    return nullptr;
}

std::unique_ptr<Engine> selectEngine(const Config& cfg, std::string& engineName) {
    std::string want = cfg.computeEngine;
    if (want.empty() || want == "off") return nullptr;
    if (want != "auto") {
        auto e = makeEngine(want, cfg);
        if (e && (want == "echo" || e->available())) {
            engineName = want;
            return e;
        }
        return nullptr;
    }
    for (const char* k : {"ollama", "llama.cpp"}) {
        auto e = makeEngine(k, cfg);
        if (e && e->available()) {
            engineName = k;
            return e;
        }
    }
    return nullptr;
}

Json buildComputeInfo(const Config& cfg, Engine* engine, const std::string& engineName) {
    Json o;
    if (!cfg.computeEnabled || !engine) {
        o["enabled"] = false;
        return o;
    }
    o["enabled"] = true;
    Json engines(Json::Array{});
    engines.push(engineName);
    o["engines"] = engines;

    std::vector<std::string> ms = engine->models();
    auto restrict = splitCSV(cfg.computeModels);
    if (!restrict.empty() && engineName != "echo") {
        std::vector<std::string> filtered;
        for (const auto& m : ms)
            for (const auto& r : restrict)
                if (m == r) filtered.push_back(m);
        ms = filtered;
    }
    Json models(Json::Array{});
    for (const auto& m : ms) models.push(m);
    o["models"] = models;
    o["allowPull"] = cfg.computeAllowPull && engineName != "echo";
    o["maxParallel"] = cfg.computeMaxParallel;
    o["cpuCores"] = cpuCores();
    o["ramBytes"] = (double)totalRAMBytes();

    Json gpu;
    std::string gname, gbackend;
    int64_t vram = 0;
    if (detectGPU(gname, vram, gbackend)) {
        gpu["present"] = true;
        gpu["name"] = gname;
        gpu["vramBytes"] = (double)vram;
        gpu["backend"] = gbackend;
    } else {
        gpu["present"] = false;
        gpu["backend"] = "none";
    }
    o["gpu"] = gpu;
    return o;
}
