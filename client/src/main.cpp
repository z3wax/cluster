#include "app.h"
#include "net.h"
#include "platform.h"
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace std::chrono_literals;

static AppState* g_state = nullptr;

static void onSignal(int) {
    if (g_state) g_state->running = false;
}

static void usage() {
    std::cout << "SDrive Cluster mijozi " << SDRIVE_VERSION << "\n\n"
              << "Foydalanish: sdrive [parametrlar]\n"
              << "  --server URL        klaster serveri (masalan http://server:8080)\n"
              << "  --ui-port N         lokal boshqaruv paneli porti (standart 48673)\n"
              << "  --config-dir DIR    sozlamalar papkasi\n"
              << "  --background        fon rejimi (avtostart): brauzer ochilmaydi\n"
              << "  --no-browser        brauzerni avtomatik ochmaslik\n"
              << "  --compute           GPU/CPU hisoblash tuguni sifatida qo'shilish\n"
              << "  --compute-engine E  inference dvigateli: auto | ollama | llama.cpp | echo\n"
              << "  --compute-models M  modellar ro'yxati (vergul bilan; echo yoki cheklash uchun)\n"
              << "  --compute-parallel N  bir vaqtda nechta inference ishi\n"
              << "  --no-compute        hisoblashni o'chirish\n"
              << "  --version           versiyani ko'rsatish\n";
}

int main(int argc, char** argv) {
    bool background = false, noBrowser = false;
    std::string serverArg;
    int uiPortArg = 0;
    bool computeFlag = false, computeOff = false;
    std::string computeEngineArg, computeModelsArg;
    int computeParArg = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](std::string& out) {
            if (i + 1 >= argc) {
                std::cerr << a << " uchun qiymat kerak\n";
                exit(2);
            }
            out = argv[++i];
        };
        if (a == "--background") background = true;
        else if (a == "--no-browser") noBrowser = true;
        else if (a == "--server") next(serverArg);
        else if (a == "--compute") computeFlag = true;
        else if (a == "--no-compute") computeOff = true;
        else if (a == "--compute-engine") next(computeEngineArg);
        else if (a == "--compute-models") next(computeModelsArg);
        else if (a == "--compute-parallel") {
            std::string v;
            next(v);
            computeParArg = atoi(v.c_str());
        } else if (a == "--ui-port") {
            std::string v;
            next(v);
            uiPortArg = atoi(v.c_str());
        } else if (a == "--config-dir") {
            std::string v;
            next(v);
            setConfigDirOverride(v);
        } else if (a == "--version") {
            std::cout << SDRIVE_VERSION << "\n";
            return 0;
        } else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            std::cerr << "noma'lum parametr: " << a << "\n";
            usage();
            return 2;
        }
    }

#ifdef _WIN32
    if (background) {
        HWND h = GetConsoleWindow();
        if (h) ShowWindow(h, SW_HIDE);
    }
#endif
    if (!netInit()) {
        std::cerr << "tarmoq ishga tushmadi\n";
        return 1;
    }

    static AppState st;
    g_state = &st;
    st.background = background;
    loadConfig(st.cfg);
    if (st.cfg.state == "uninstalled") {
        // Dastur o'chirilgandan keyin qayta ishga tushirildi — yangi o'rnatish
        resetForReinstall(st.cfg);
        logf("[sdrive] avvalgi o'rnatish o'chirilgan edi — yangi sozlash boshlanadi");
    }
    if (st.cfg.agreementAccepted && st.cfg.agreementVersion.empty()) st.cfg.agreementVersion = "1.0"; // 1.x config
    if (!serverArg.empty()) {
        while (!serverArg.empty() && serverArg.back() == '/') serverArg.pop_back();
        if (serverArg != st.cfg.serverUrl) {
            st.cfg.serverUrl = serverArg;
            st.cfg.token.clear();
        }
    }
    if (uiPortArg > 0 && uiPortArg < 65536) st.cfg.uiPort = uiPortArg;

    // Hisoblash (GPU/CPU) klasteri — flag yoki muhit o'zgaruvchilari
    if (const char* v = getenv("SDRIVE_COMPUTE")) st.cfg.computeEnabled = std::string(v) == "1" || std::string(v) == "true";
    if (const char* v = getenv("SDRIVE_COMPUTE_ENGINE")) st.cfg.computeEngine = v;
    if (const char* v = getenv("SDRIVE_COMPUTE_MODELS")) st.cfg.computeModels = v;
    if (const char* v = getenv("SDRIVE_COMPUTE_PARALLEL")) st.cfg.computeMaxParallel = atoi(v);
    if (const char* v = getenv("SDRIVE_OLLAMA_URL")) st.cfg.ollamaUrl = v;
    if (const char* v = getenv("SDRIVE_LLAMA_URL")) st.cfg.llamaUrl = v;
    if (computeFlag) st.cfg.computeEnabled = true;
    if (computeOff) st.cfg.computeEnabled = false;
    if (!computeEngineArg.empty()) st.cfg.computeEngine = computeEngineArg;
    if (!computeModelsArg.empty()) st.cfg.computeModels = computeModelsArg;
    if (computeParArg > 0) st.cfg.computeMaxParallel = computeParArg;
    saveConfig(st.cfg);

    std::string url = "http://127.0.0.1:" + std::to_string(st.cfg.uiPort) + "/";
    std::string err;
    if (!startLocalServer(st, st.cfg.uiPort, err)) {
        // Bitta kompyuterda faqat bitta nusxa ishlaydi
        logf("[sdrive] %s", err.c_str());
        if (!background && !noBrowser) openBrowser(url);
        return 0;
    }

    if (st.cfg.setupDone) {
        if (!ensureSdrive(st.cfg.sdriveDir, platformId() == "windows" ? st.cfg.driveLetter : "", err))
            logf("[sdrive] SDrive diski: %s", err.c_str());
        if (!st.storage.open(st.cfg.sdriveDir, st.cfg.storageKey, err)) logf("[sdrive] xotira: %s", err.c_str());
        st.usedBytes = st.storage.usedBytes();
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    logf("[sdrive] SDrive Cluster %s ishga tushdi", SDRIVE_VERSION);
    logf("[sdrive] Boshqaruv paneli: %s", url.c_str());
    logf("[sdrive] Tugun: %s | Server: %s", st.cfg.nodeId.c_str(), st.cfg.serverUrl.c_str());

    if ((!st.cfg.setupDone || !st.cfg.agreementAccepted) && !background && !noBrowser) {
        std::thread([url] {
            std::this_thread::sleep_for(500ms);
            openBrowser(url);
        }).detach();
    }

    startAgent(st);
    while (st.running) {
        std::this_thread::sleep_for(200ms);
        int64_t ex = st.exitAt.load();
        if (ex > 0 && (int64_t)time(nullptr) >= ex) st.running = false;
    }
    logf("[sdrive] to'xtatilmoqda...");
    stopAgent(st);
    stopLocalServer();
    if (st.uninstalling) st.storage.wipe(); // oqimlar to'xtagach — kech yozilgan chunk ham qolmasin
    {
        std::lock_guard<std::mutex> lk(st.m);
        saveConfig(st.cfg);
    }
    netCleanup();
    return 0;
}
