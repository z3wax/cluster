#include "compute.h"
#include "app.h"
#include "net.h"
#include "config.h"
#include <chrono>

using namespace std::chrono_literals;

// ================= Ishchi oqimi =================

// forward (agent.cpp dan)
HttpResp serverCall(AppState& st, const std::string& method, const std::string& path, const std::string& body,
                    int timeoutSec);

static ComputeJob parseJob(const Json& j) {
    ComputeJob job;
    job.id = j["id"].asStr();
    job.model = j["model"].asStr();
    job.kind = j["kind"].asStr();
    job.prompt = j["prompt"].asStr();
    job.params = j["params"];
    for (const auto& m : j["messages"].asArr()) {
        job.messages.push_back({m["role"].asStr(), m["content"].asStr()});
    }
    return job;
}

void ComputeAgent::start(AppState& st) {
    engine_ = selectEngine(st.cfg, engineName_);
    enabled_ = st.cfg.computeEnabled && engine_ != nullptr;
    if (!enabled_) {
        if (st.cfg.computeEnabled && !engine_)
            logf("[compute] dvigatel topilmadi (%s) — hisoblash o'chirilgan", st.cfg.computeEngine.c_str());
        return;
    }
    int n = st.cfg.computeMaxParallel;
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    logf("[compute] dvigatel: %s, %d parallel ish, modellar: %d", engineName_.c_str(), n,
         (int)engine_->models().size());
    run_ = true;
    for (int i = 0; i < n; i++) threads_.emplace_back([this, &st] { workerLoop(st); });
}

void ComputeAgent::stop() {
    run_ = false;
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
}

void ComputeAgent::workerLoop(AppState& st) {
    while (run_.load() && st.running.load()) {
        std::string nodeId, token;
        bool active;
        {
            std::lock_guard<std::mutex> lk(st.m);
            nodeId = st.cfg.nodeId;
            token = st.cfg.token;
            active = st.cfg.state == "running" && st.cfg.agreementAccepted && !st.uninstalling.load();
        }
        if (!active || token.empty()) {
            std::this_thread::sleep_for(1s);
            continue;
        }

        Json poll;
        Json engines(Json::Array{});
        engines.push(engineName_);
        poll["engines"] = engines;
        poll["allowPull"] = st.cfg.computeAllowPull && engineName_ != "echo";
        Json models(Json::Array{});
        for (const auto& m : engine_->models()) models.push(m);
        poll["models"] = models;

        HttpResp r = serverCall(st, "POST", "/api/nodes/" + nodeId + "/compute/poll", poll.dump(), 40);
        if (!r.ok()) {
            std::this_thread::sleep_for(2s);
            continue;
        }
        bool ok = false;
        Json j = Json::parse(r.body, &ok);
        if (!ok || j["job"].isNull()) continue;
        runJob(st, parseJob(j["job"]));
    }
}

void ComputeAgent::runJob(AppState& st, const ComputeJob& job) {
    std::string base = "/api/compute/jobs/" + job.id;
    std::atomic<bool> canceled{false};
    auto onToken = [&](const std::string& tok) -> bool {
        Json c;
        c["token"] = tok;
        HttpResp r = serverCall(st, "POST", base + "/chunk", c.dump(), 30);
        if (r.status == 409) {
            canceled = true;
            return false;
        }
        return run_.load() && st.running.load();
    };

    std::string finalText, err;
    std::vector<double> embedding;
    bool okRun = engine_->run(job, onToken, finalText, embedding, err);

    if (canceled.load()) return;
    if (!okRun) {
        Json f;
        f["error"] = err.empty() ? std::string("inference xatosi") : err;
        f["requeue"] = true;
        serverCall(st, "POST", base + "/fail", f.dump(), 30);
        logf("[compute] ish bajarilmadi (%s): %s", job.model.c_str(), err.c_str());
        return;
    }
    Json d;
    d["result"] = finalText;
    d["finishReason"] = "stop";
    d["promptTokens"] = (double)(job.prompt.size() + job.messages.size());
    if (!embedding.empty()) {
        Json e(Json::Array{});
        for (double v : embedding) e.push(v);
        d["embedding"] = e;
    }
    serverCall(st, "POST", base + "/done", d.dump(), 30);
}
