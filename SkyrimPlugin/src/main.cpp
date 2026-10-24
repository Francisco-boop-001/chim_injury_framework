// CHIM Injury Framework - SKSE plugin
//
// Tracks wounds of the player and the followers: body region, wound type and severity.
// Wounds heal with game time (light and medium), with healing magic or potions (light and
// medium heal, severe ones get treated) and by sleeping (treated severe wounds).
// Wounds are stored in the save (SKSE co-save), shown on the body through RaceMenu body
// overlays when RaceMenu is installed, reported to CHIM's event log and mirrored to the
// chim_injury_framework server plugin, which describes them to the LLM.
//
// No ESP/ESL is required. Needs SKSE, Address Library and CHIM. RaceMenu is optional (visuals).

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/ringbuffer_sink.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <condition_variable>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

namespace logger = SKSE::log;
using namespace std::chrono_literals;

namespace
{
    constexpr const char* kPluginVersion = "0.1.1";
    constexpr const char* kPluginId = "chim_injury_framework";
    constexpr const char* kNotifyPrefix = "CHIM-injury: ";
    constexpr const char* kChimScript = "AIAgentFunctions";
    constexpr const char* kNio = "NiOverride";
    constexpr const char* kIni = "Data\\SKSE\\Plugins\\CHIMInjury.ini";
    constexpr const char* kSkeeIni = "Data\\SKSE\\Plugins\\skee64.ini";
    constexpr const char* kTextureRoot = "Actors\\Character\\CHIMInjury\\";
    constexpr const char* kDefaultOverlay = "actors\\character\\overlays\\default.dds";
    constexpr RE::FormID kCurrentFollowerFaction = 0x0005C84E;
    constexpr std::uint32_t kSerializationId = 'CHIJ';
    constexpr std::uint32_t kRecordWounds = 'WNDS';
    constexpr std::uint32_t kRecordVersion = 1;

    // ------------------------------------------------------------------------------------
    // Settings: INI defaults, overridden by the server plugin page when reachable.
    // Only read and written on the game thread.
    // ------------------------------------------------------------------------------------
    struct Settings
    {
        bool enabled{true};
        bool debug{false};
        bool trackPlayer{true};
        bool trackFollowers{true};
        bool showOverlays{true};
        bool showOnPlayer{true};
        bool notifications{true};
        int notifyMinSeverity{2};
        bool logToChim{true};
        int logMinSeverity{2};
        float minDamagePct{5.0f};
        float mediumDamagePct{12.0f};
        float severeDamagePct{25.0f};
        int maxWounds{6};
        float lightHealDays{1.0f};
        float mediumHealDays{3.0f};
        float healMinAmount{15.0f};
        float minSleepHours{6.0f};
        int overlayStrength{100};
        int logLevel{2};  // 0 error, 1 warn, 2 info, 3 debug
    };

    struct ServerSettings
    {
        std::string host{"127.0.0.1"};
        int port{8081};
        std::string basePath{"/HerikaServer/ext/chim_injury_framework"};
        bool fromIni{false};
    };

    Settings g_settings;
    ServerSettings g_server;
    std::atomic<bool> g_serverReachable{false};

    std::string Trim(std::string s)
    {
        const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
        return s;
    }

    std::string ToLower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    bool ReadBool(const char* section, const char* key, bool def)
    {
        return GetPrivateProfileIntA(section, key, def ? 1 : 0, kIni) != 0;
    }

    int ReadInt(const char* section, const char* key, int def)
    {
        return static_cast<int>(GetPrivateProfileIntA(section, key, def, kIni));
    }

    float ReadFloat(const char* section, const char* key, float def)
    {
        char buffer[64]{};
        GetPrivateProfileStringA(section, key, "", buffer, sizeof(buffer), kIni);
        try {
            return buffer[0] ? std::stof(buffer) : def;
        } catch (...) {
            return def;
        }
    }

    std::string ReadString(const char* section, const char* key)
    {
        char buffer[256]{};
        GetPrivateProfileStringA(section, key, "", buffer, sizeof(buffer), kIni);
        return Trim(buffer);
    }

    void ClampSettings(Settings& s)
    {
        s.logLevel = std::clamp(s.logLevel, 0, 3);
        s.notifyMinSeverity = std::clamp(s.notifyMinSeverity, 1, 3);
        s.logMinSeverity = std::clamp(s.logMinSeverity, 1, 3);
        s.minDamagePct = std::clamp(s.minDamagePct, 0.0f, 100.0f);
        s.mediumDamagePct = std::clamp(s.mediumDamagePct, s.minDamagePct, 100.0f);
        s.severeDamagePct = std::clamp(s.severeDamagePct, s.mediumDamagePct, 100.0f);
        s.maxWounds = std::clamp(s.maxWounds, 1, 12);
        s.lightHealDays = std::clamp(s.lightHealDays, 0.05f, 60.0f);
        s.mediumHealDays = std::clamp(s.mediumHealDays, 0.05f, 60.0f);
        s.healMinAmount = std::clamp(s.healMinAmount, 0.0f, 1000.0f);
        s.minSleepHours = std::clamp(s.minSleepHours, 0.0f, 24.0f);
        s.overlayStrength = std::clamp(s.overlayStrength, 10, 100);
    }

    void LoadIniSettings()
    {
        auto& s = g_settings;
        s.enabled = ReadBool("General", "bEnabled", s.enabled);
        s.debug = ReadBool("General", "bDebug", s.debug);
        s.notifications = ReadBool("General", "bNotifications", s.notifications);
        s.logToChim = ReadBool("General", "bLogToChim", s.logToChim);
        s.logLevel = ReadInt("General", "iLogLevel", s.logLevel);
        s.trackPlayer = ReadBool("Tracking", "bTrackPlayer", s.trackPlayer);
        s.trackFollowers = ReadBool("Tracking", "bTrackFollowers", s.trackFollowers);
        s.showOverlays = ReadBool("Visuals", "bShowOverlays", s.showOverlays);
        s.showOnPlayer = ReadBool("Visuals", "bShowOnPlayer", s.showOnPlayer);
        s.overlayStrength = ReadInt("Visuals", "iOverlayStrength", s.overlayStrength);
        s.minDamagePct = ReadFloat("Wounds", "fMinDamagePercent", s.minDamagePct);
        s.mediumDamagePct = ReadFloat("Wounds", "fMediumDamagePercent", s.mediumDamagePct);
        s.severeDamagePct = ReadFloat("Wounds", "fSevereDamagePercent", s.severeDamagePct);
        s.maxWounds = ReadInt("Wounds", "iMaxWounds", s.maxWounds);
        s.lightHealDays = ReadFloat("Healing", "fLightHealDays", s.lightHealDays);
        s.mediumHealDays = ReadFloat("Healing", "fMediumHealDays", s.mediumHealDays);
        s.healMinAmount = ReadFloat("Healing", "fHealMinAmount", s.healMinAmount);
        s.minSleepHours = ReadFloat("Healing", "fMinSleepHours", s.minSleepHours);
        ClampSettings(s);

        const auto host = ReadString("Server", "sServer");
        const int port = ReadInt("Server", "iPort", 0);
        if (!host.empty()) {
            g_server.host = host;
            g_server.fromIni = true;
        }
        if (port > 0 && port <= 65535) {
            g_server.port = port;
        }
    }

    // ------------------------------------------------------------------------------------
    // Logging: Documents/My Games/Skyrim Special Edition/SKSE/chim-injury.log, plus an in-memory
    // tail that is sent to the server for the diagnostics page.
    // ------------------------------------------------------------------------------------
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> g_logTail;
    std::mutex g_logSentMutex;
    std::set<std::string> g_logSent;

    void SetupLog()
    {
        auto dist = std::make_shared<spdlog::sinks::dist_sink_mt>();
        if (auto dir = logger::log_directory()) {
            dist->add_sink(std::make_shared<spdlog::sinks::basic_file_sink_mt>((*dir / "chim-injury.log").string(), true));
        }
        g_logTail = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(300);
        dist->add_sink(g_logTail);
        auto log = std::make_shared<spdlog::logger>("global log", std::move(dist));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    }

    void ApplyLogLevel()
    {
        static constexpr spdlog::level::level_enum levels[] = {
            spdlog::level::err, spdlog::level::warn, spdlog::level::info, spdlog::level::debug
        };
        const auto level = g_settings.debug ? spdlog::level::debug : levels[g_settings.logLevel];
        spdlog::set_level(level);
        spdlog::flush_on(level);
    }

    // On-screen notification (where the HUD shows them). `always` ignores debug mode.
    void Notify(const std::string& text, bool always = false)
    {
        auto show = [text, always]() {
            if (always || g_settings.debug) {
                RE::DebugNotification((std::string(kNotifyPrefix) + text).c_str());
            }
        };
        if (auto tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask(show);
        }
    }

    void RunOnGameThread(std::function<void()> fn)
    {
        if (auto tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask([fn = std::move(fn)]() {
                try {
                    fn();
                } catch (const std::exception& e) {
                    logger::error("Game thread task failed: {}", e.what());
                }
            });
        }
    }

    // ------------------------------------------------------------------------------------
    // Tiny scheduler: runs callbacks on the game thread after a delay.
    // ------------------------------------------------------------------------------------
    class Scheduler
    {
    public:
        static Scheduler& Get()
        {
            static Scheduler instance;
            return instance;
        }

        void After(std::chrono::milliseconds delay, std::function<void()> fn)
        {
            {
                std::lock_guard lock(_mutex);
                _jobs.push_back({ std::chrono::steady_clock::now() + delay, std::move(fn) });
            }
            _cv.notify_one();
        }

        void Start()
        {
            if (_running.exchange(true)) {
                return;
            }
            std::thread([this]() { Loop(); }).detach();
        }

    private:
        struct Job
        {
            std::chrono::steady_clock::time_point due;
            std::function<void()> fn;
        };

        void Loop()
        {
            std::unique_lock lock(_mutex);
            while (_running) {
                _cv.wait_for(lock, 100ms);
                const auto now = std::chrono::steady_clock::now();
                std::vector<std::function<void()>> ready;
                for (auto it = _jobs.begin(); it != _jobs.end();) {
                    if (it->due <= now) {
                        ready.push_back(std::move(it->fn));
                        it = _jobs.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (ready.empty()) {
                    continue;
                }
                lock.unlock();
                for (auto& fn : ready) {
                    RunOnGameThread(std::move(fn));
                }
                lock.lock();
            }
        }

        std::mutex _mutex;
        std::condition_variable _cv;
        std::vector<Job> _jobs;
        std::atomic<bool> _running{false};
    };

    // ------------------------------------------------------------------------------------
    // Papyrus static calls with an optional result callback.
    // ------------------------------------------------------------------------------------
    class ResultCallback : public RE::BSScript::IStackCallbackFunctor
    {
    public:
        explicit ResultCallback(std::function<void(const RE::BSScript::Variable&)> fn) :
            _fn(std::move(fn))
        {}

        void operator()(RE::BSScript::Variable a_result) override
        {
            if (_fn) {
                _fn(a_result);
            }
        }

        bool CanSave() const override { return false; }
        void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

    private:
        std::function<void(const RE::BSScript::Variable&)> _fn;
    };

    template <class... Args>
    bool CallStatic(const char* scriptName, const char* functionName,
        std::function<void(const RE::BSScript::Variable&)> onResult, Args&&... args)
    {
        auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            return false;
        }
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        if (onResult) {
            callback.reset(new ResultCallback(std::move(onResult)));
        }
        // Arguments are passed by value (MakeFunctionArguments rejects lvalue references).
        auto arguments = RE::MakeFunctionArguments(static_cast<std::decay_t<Args>>(std::forward<Args>(args))...);
        const bool ok = vm->DispatchStaticCall(scriptName, functionName, arguments, callback);
        if (!ok) {
            logger::warn("Papyrus call {}.{} could not be dispatched", scriptName, functionName);
        }
        return ok;
    }

    std::string FileVersion(const char* path)
    {
        DWORD handle = 0;
        const DWORD size = GetFileVersionInfoSizeA(path, &handle);
        if (size == 0) {
            return {};
        }
        std::vector<char> data(size);
        if (!GetFileVersionInfoA(path, 0, size, data.data())) {
            return {};
        }
        VS_FIXEDFILEINFO* info = nullptr;
        UINT len = 0;
        if (!VerQueryValueA(data.data(), "\\", reinterpret_cast<void**>(&info), &len) || !info) {
            return {};
        }
        return std::format("{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS));
    }

    // ------------------------------------------------------------------------------------
    // HTTP to the server plugin (settings and commands in, status, wounds and log tail out).
    // ------------------------------------------------------------------------------------
    std::string JsonEscape(const std::string& in)
    {
        std::string out;
        out.reserve(in.size() + 8);
        for (unsigned char c : in) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += std::format("\\u{:04x}", c);
                } else {
                    out += static_cast<char>(c);
                }
            }
        }
        return out;
    }

    std::wstring Widen(const std::string& s)
    {
        if (s.empty()) {
            return {};
        }
        const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
        return out;
    }

    bool HttpRequest(const std::string& host, int port, const std::string& method, const std::string& path,
        const std::string& body, std::string* response, int timeoutMs = 3000)
    {
        bool ok = false;
        HINTERNET session = WinHttpOpen(L"CHIMInjury/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) {
            return false;
        }
        WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
        HINTERNET connect = WinHttpConnect(session, Widen(host).c_str(), static_cast<INTERNET_PORT>(port), 0);
        HINTERNET request = connect ? WinHttpOpenRequest(connect, Widen(method).c_str(), Widen(path).c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0)
                                    : nullptr;
        if (request) {
            const wchar_t* headers = L"Content-Type: application/json\r\n";
            const BOOL sent = WinHttpSendRequest(request, body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers,
                body.empty() ? 0 : static_cast<DWORD>(-1L), body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
            if (sent && WinHttpReceiveResponse(request, nullptr)) {
                DWORD status = 0;
                DWORD statusSize = sizeof(status);
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                    &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
                std::string data;
                DWORD available = 0;
                while (WinHttpQueryDataAvailable(request, &available) && available > 0 && data.size() < 262144) {
                    std::string chunk(available, '\0');
                    DWORD read = 0;
                    if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) {
                        break;
                    }
                    data.append(chunk.data(), read);
                }
                ok = status >= 200 && status < 300;
                if (response) {
                    *response = std::move(data);
                }
            }
            WinHttpCloseHandle(request);
        }
        if (connect) {
            WinHttpCloseHandle(connect);
        }
        WinHttpCloseHandle(session);
        return ok;
    }

    // Same discovery order as CHIM: AIAgent.ini, then the CHIM.exe proxy on 7135, then 127.0.0.1:8081.
    void DiscoverServer()
    {
        if (g_server.fromIni) {
            logger::info("Server from CHIMInjury.ini: {}:{}", g_server.host, g_server.port);
            return;
        }
        std::ifstream aiagent("Data/SKSE/Plugins/AIAgent.ini");
        if (aiagent.is_open()) {
            std::string line;
            while (std::getline(aiagent, line)) {
                const auto eq = line.find('=');
                if (line.empty() || line[0] == '#' || eq == std::string::npos) {
                    continue;
                }
                const auto key = Trim(line.substr(0, eq));
                const auto value = Trim(line.substr(eq + 1));
                if (key == "SERVER" && !value.empty()) {
                    g_server.host = value;
                } else if (key == "PORT") {
                    try {
                        g_server.port = std::stoi(value);
                    } catch (...) {
                    }
                }
            }
            logger::info("Server from AIAgent.ini: {}:{}", g_server.host, g_server.port);
            return;
        }
        std::string body;
        if (HttpRequest("127.0.0.1", 7135, "GET", "/discover", {}, &body, 1500)) {
            body = Trim(body);
            const auto colon = body.find(':');
            if (colon != std::string::npos) {
                g_server.host = body.substr(0, colon);
                try {
                    g_server.port = std::stoi(body.substr(colon + 1));
                } catch (...) {
                }
                logger::info("Server from CHIM launcher discovery: {}:{}", g_server.host, g_server.port);
                return;
            }
        }
        logger::info("Server: default {}:{}", g_server.host, g_server.port);
    }

    // ------------------------------------------------------------------------------------
    // Counters reported to the server
    // ------------------------------------------------------------------------------------
    std::atomic<int> g_statHits{0};
    std::atomic<int> g_statWounds{0};
    std::atomic<int> g_statHealedMagic{0};
    std::atomic<int> g_statHealedRest{0};
    std::atomic<int> g_statHealedTime{0};
    std::atomic<int> g_statLogged{0};
    std::atomic<int> g_statOverlayApplied{0};
    std::atomic<int> g_statFailures{0};
    std::mutex g_lastErrorMutex;
    std::string g_lastError;

    void SetLastError(const std::string& error)
    {
        std::lock_guard lock(g_lastErrorMutex);
        g_lastError = error;
        ++g_statFailures;
    }

    std::mt19937& Rng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    // Some NPCs return an empty display name; fall back to the reference and base names.
    std::string ActorName(RE::Actor* actor)
    {
        if (!actor) {
            return "someone";
        }
        if (const char* n = actor->GetDisplayFullName(); n && n[0] != '\0') {
            return n;
        }
        if (const char* n = actor->GetName(); n && n[0] != '\0') {
            return n;
        }
        if (auto base = actor->GetActorBase()) {
            if (const char* n = base->GetName(); n && n[0] != '\0') {
                return n;
            }
        }
        return actor->IsPlayerRef() ? "the player" : "someone";
    }

    bool IsFemale(RE::Actor* actor)
    {
        auto base = actor ? actor->GetActorBase() : nullptr;
        return base && base->GetSex() == RE::SEX::kFemale;
    }

    float GameDays()
    {
        auto calendar = RE::Calendar::GetSingleton();
        return calendar ? calendar->GetCurrentGameTime() : 0.0f;
    }

    // Summoned or raised creatures (atronachs, spectral helpers, thralls) are never tracked.
    bool IsSummon(RE::Actor* actor)
    {
        if (!actor || actor->IsPlayerRef()) {
            return false;
        }
        if (actor->GetCommandingActor().get()) {
            return true;
        }
        auto base = actor->GetActorBase();
        return base && base->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kSummonable);
    }

    bool HasActorKeyword(RE::Actor* actor, std::string_view keyword)
    {
        if (!actor) {
            return false;
        }
        if (auto race = actor->GetRace(); race && race->HasKeywordString(keyword)) {
            return true;
        }
        auto base = actor->GetActorBase();
        return base && base->HasKeywordString(keyword);
    }

    bool IsFollower(RE::Actor* actor)
    {
        if (!actor || actor->IsPlayerRef() || IsSummon(actor) || actor->IsDead()) {
            return false;
        }
        if (actor->IsPlayerTeammate()) {
            return true;
        }
        static RE::TESFaction* faction = RE::TESForm::LookupByID<RE::TESFaction>(kCurrentFollowerFaction);
        return faction && actor->IsInFaction(faction);
    }
}

// ========================================================================================
// Wound model
// ========================================================================================
namespace
{
    enum class Region : std::uint8_t
    {
        kHead,
        kChest,
        kBack,
        kLeftArm,
        kRightArm,
        kLeftLeg,
        kRightLeg,
        kTotal
    };

    enum class WType : std::uint8_t
    {
        kSlash,
        kPierce,
        kBlunt,
        kMaul,
        kBurn,
        kFrost,
        kShock,
        kTotal
    };

    // Keys shared with the server plugin (lib/chim_injury.php). Keep both sides in sync.
    constexpr std::array<const char*, 7> kRegionKeys = { "head", "chest", "back", "left_arm", "right_arm", "left_leg", "right_leg" };
    constexpr std::array<const char*, 7> kRegionLabels = { "head", "chest", "back", "left arm", "right arm", "left leg", "right leg" };
    constexpr std::array<const char*, 7> kTypeKeys = { "slash", "pierce", "blunt", "maul", "burn", "frost", "shock" };

    const char* RegionKey(Region r) { return kRegionKeys[static_cast<size_t>(r)]; }
    const char* RegionLabel(Region r) { return kRegionLabels[static_cast<size_t>(r)]; }
    const char* TypeKey(WType t) { return kTypeKeys[static_cast<size_t>(t)]; }

    bool IsCutType(WType t) { return t == WType::kSlash || t == WType::kPierce || t == WType::kMaul; }

    struct Wound
    {
        Region region{Region::kChest};
        WType type{WType::kSlash};
        std::uint8_t severity{1};  // 1 light, 2 medium, 3 severe
        bool treated{false};       // severe wound after healing magic: heals fully after sleeping
        bool arrow{false};
        float created{0.0f};       // game days
        float due{0.0f};           // game day when it heals by itself (0 = never)
        std::string by;            // attacker name
        std::string with;          // weapon or spell name
    };

    std::string WoundKey(const Wound& w) { return std::format("{}:{}", RegionKey(w.region), TypeKey(w.type)); }

    // Short English label (notifications and fallback event text). The server has its own longer text.
    std::string WoundLabel(const Wound& w)
    {
        const int s = std::clamp<int>(w.severity, 1, 3) - 1;
        const bool limb = w.region == Region::kLeftArm || w.region == Region::kRightArm || w.region == Region::kLeftLeg || w.region == Region::kRightLeg;
        switch (w.type) {
        case WType::kSlash:
            return std::array{ "shallow cut", "cut", "deep gash" }[s];
        case WType::kPierce:
            return w.arrow ? std::array{ "arrow graze", "arrow wound", "deep arrow wound" }[s]
                           : std::array{ "small stab wound", "stab wound", "deep stab wound" }[s];
        case WType::kBlunt:
            if (s == 2) {
                if (w.region == Region::kHead) return "concussion";
                if (w.region == Region::kChest || w.region == Region::kBack) return "cracked ribs";
                if (limb) return "possible fracture";
            }
            return std::array{ "bruise", "heavy bruising", "bad bruising" }[s];
        case WType::kMaul:
            return std::array{ "claw scratches", "bite wound", "deep mauling" }[s];
        case WType::kBurn:
            return std::array{ "minor burn", "burn", "severe burn" }[s];
        case WType::kFrost:
            return std::array{ "frostnip", "frostbite", "severe frostbite" }[s];
        case WType::kShock:
            return std::array{ "singed skin", "shock burn", "severe shock burn" }[s];
        default:
            return "wound";
        }
    }

    struct ActorWounds
    {
        std::string name;
        bool female{false};
        bool isPlayer{false};
        std::vector<Wound> wounds;
    };

    // All wound state lives on the game thread (event sinks forward to it with SKSE tasks).
    std::map<RE::FormID, ActorWounds> g_wounds;
    std::set<RE::FormID> g_tracked;      // player + current followers (refreshed every 2 s)
    bool g_gameReady{false};             // a save is loaded
    std::atomic<bool> g_stateDirty{true};
    std::uint64_t g_lastCommand{0};      // last executed page command (this session)

    RE::Actor* LookupActor(RE::FormID id)
    {
        return RE::TESForm::LookupByID<RE::Actor>(id);
    }

    bool IsTracked(RE::Actor* actor)
    {
        if (!actor || !g_settings.enabled) {
            return false;
        }
        if (actor->IsPlayerRef()) {
            return g_settings.trackPlayer;
        }
        return g_settings.trackFollowers && IsFollower(actor);
    }

    void RefreshTracked()
    {
        std::set<RE::FormID> tracked;
        if (auto player = RE::PlayerCharacter::GetSingleton(); player && g_settings.trackPlayer) {
            tracked.insert(player->GetFormID());
        }
        if (g_settings.trackFollowers) {
            if (auto lists = RE::ProcessLists::GetSingleton()) {
                for (auto& handle : lists->highActorHandles) {
                    auto actor = handle.get();
                    if (actor && IsFollower(actor.get())) {
                        tracked.insert(actor->GetFormID());
                    }
                }
            }
        }
        if (tracked != g_tracked) {
            g_tracked = std::move(tracked);
            g_stateDirty = true;
        }
    }

    ActorWounds& Entry(RE::Actor* actor)
    {
        auto& entry = g_wounds[actor->GetFormID()];
        entry.name = ActorName(actor);
        entry.female = IsFemale(actor);
        entry.isPlayer = actor->IsPlayerRef();
        return entry;
    }

    float HealDays(int severity)
    {
        return severity <= 1 ? g_settings.lightHealDays : severity == 2 ? g_settings.mediumHealDays : 0.0f;
    }
}

// ========================================================================================
// CHIM event log
// ========================================================================================
namespace
{
    // "info_*" events are stored in CHIM's event log. The server plugin (prerequest.php) removes the
    // marker and makes the event visible to everyone nearby.
    void LogToChim(const std::string& actorName, const std::string& event, const std::string& sentence)
    {
        if (!g_settings.logToChim) {
            return;
        }
        const std::string text = std::format("[injury actor=\"{}\" event={}] {}", actorName, event, sentence);
        const bool dispatched = CallStatic(kChimScript, "logMessage",
            [actorName](const RE::BSScript::Variable& result) {
                if (result.IsInt() && result.GetSInt() == 0) {
                    ++g_statLogged;
                    logger::debug("CHIM accepted the injury event for {}", actorName);
                } else {
                    SetLastError("CHIM logMessage returned an unexpected result");
                    logger::warn("CHIM logMessage returned an unexpected result for {}", actorName);
                }
            },
            text, std::string("info_injury"));
        if (!dispatched) {
            SetLastError("CHIM not reachable (AIAgentFunctions.logMessage)");
        }
    }
}

// ========================================================================================
// Body overlays (RaceMenu / NiOverride). Optional: without RaceMenu nothing is drawn.
// ========================================================================================
namespace
{
    struct OverlayInfo
    {
        bool skeeLoaded{false};
        std::string skeeVersion;
        int numOverlays{6};
        bool playerOnly{true};
    };

    OverlayInfo g_overlay;

    void DetectOverlays()
    {
        g_overlay.skeeLoaded = GetModuleHandleA("skee64.dll") != nullptr;
        if (g_overlay.skeeLoaded) {
            g_overlay.skeeVersion = FileVersion("Data\\SKSE\\Plugins\\skee64.dll");
        }
        g_overlay.numOverlays = std::clamp(static_cast<int>(GetPrivateProfileIntA("Overlays/Body", "iNumOverlays", 6, kSkeeIni)), 0, 32);
        g_overlay.playerOnly = GetPrivateProfileIntA("Overlays", "bPlayerOnly", 1, kSkeeIni) != 0;
        logger::info("RaceMenu (skee64.dll): {} {}, body overlay slots: {}, bPlayerOnly: {}", g_overlay.skeeLoaded ? "found" : "not found",
            g_overlay.skeeVersion, g_overlay.numOverlays, g_overlay.playerOnly);
    }

    struct OverlayWant
    {
        Region region{Region::kChest};
        std::string texture;
        float alpha{1.0f};
        std::int32_t color{0xFFFFFF};
        int priority{0};
    };

    std::string TextureFor(Region region, const Wound& w, bool female)
    {
        const bool cut = IsCutType(w.type);
        std::string name;
        switch (region) {
        case Region::kChest:
            name = cut ? "Wounds_Chest" : "Wounds_Torso_Bruises";
            break;
        case Region::kBack:
            // Wounds_Torso_Bruises sits on the front and side of the torso, so back bruises and burns
            // use the small back texture, tinted (ColorFor).
            name = cut && w.severity > 1 ? "Wounds_Back_Long" : "Wounds_Smaller_Back";
            break;
        case Region::kLeftArm:
            name = cut ? (w.severity <= 1 ? "Wounds_Arm_Left_Small" : "Wounds_Arm_Left_Long") : "Wounds_Arm_Left_Bruise";
            break;
        case Region::kRightArm:
            name = cut ? "Wounds_Arm_Right" : "Wounds_Arm_Right_Bruise";
            break;
        case Region::kLeftLeg:
            name = cut ? "Wounds_Legs_Left" : "Wounds_Legs_Left_Bruises";
            break;
        case Region::kRightLeg:
            name = cut ? "Wounds_Legs_Right" : "Wounds_Legs_Right_Bruises";
            break;
        default:
            return {};  // no head overlays yet
        }
        return std::format("{}{}\\{}.dds", kTextureRoot, female ? "Female" : "Male", name);
    }

    std::int32_t ColorFor(Region region, WType type)
    {
        switch (type) {
        case WType::kBlunt: return region == Region::kBack ? 0xA07890 : 0xFFFFFF;  // back: tinted abrasion texture
        case WType::kBurn: return 0xD9773F;
        case WType::kFrost: return 0x8FB8FF;
        case WType::kShock: return 0xC8A8FF;
        default: return 0xFFFFFF;
        }
    }

    float AlphaFor(const Wound& w)
    {
        float alpha = w.severity <= 1 ? 0.5f : w.severity == 2 ? 0.8f : (w.treated ? 0.65f : 1.0f);
        return std::clamp(alpha * static_cast<float>(g_settings.overlayStrength) / 100.0f, 0.05f, 1.0f);
    }

    // Worst wound per body region, most important first; one overlay slot per texture.
    std::vector<OverlayWant> DesiredOverlays(RE::FormID id, bool isPlayer)
    {
        std::vector<OverlayWant> out;
        if (!g_settings.enabled || !g_settings.showOverlays || (isPlayer && !g_settings.showOnPlayer)) {
            return out;
        }
        auto it = g_wounds.find(id);
        if (it == g_wounds.end()) {
            return out;
        }
        std::map<Region, const Wound*> worst;
        auto rank = [](const Wound& w) { return w.severity * 4 + (w.treated ? 0 : 2) + (IsCutType(w.type) ? 1 : 0); };
        for (auto& w : it->second.wounds) {
            if (w.region == Region::kHead) {
                continue;
            }
            auto& slot = worst[w.region];
            if (!slot || rank(w) > rank(*slot)) {
                slot = &w;
            }
        }
        std::set<std::string> textures;
        for (auto& [region, w] : worst) {
            OverlayWant want;
            want.region = region;
            want.texture = TextureFor(region, *w, it->second.female);
            want.alpha = AlphaFor(*w);
            want.color = ColorFor(region, w->type);
            want.priority = rank(*w);
            if (!want.texture.empty() && textures.insert(ToLower(want.texture)).second) {
                out.push_back(std::move(want));
            }
        }
        std::stable_sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.priority > b.priority; });
        return out;
    }

    std::string Signature(const std::vector<OverlayWant>& wants)
    {
        std::string sig;
        for (auto& w : wants) {
            sig += std::format("{}|{:.2f}|{:06X};", w.texture, w.alpha, w.color);
        }
        return sig;
    }

    std::string SlotNode(int index) { return std::format("Body [Ovl{}]", index); }

    struct OverlayState
    {
        bool busy{false};
        bool again{false};
        bool applied{false};
        std::string sig;
        RE::NiAVObject* root{nullptr};  // 3D the overlays were applied to (reloaded 3D needs a new pass)
        int shown{0};
        int wanted{0};
    };

    std::unordered_map<RE::FormID, OverlayState> g_overlayState;
    std::atomic<int> g_overlaySlotsShort{0};

    struct SlotQuery
    {
        std::mutex mutex;
        RE::FormID id{0};
        bool female{false};
        std::vector<std::string> textures;
        int remaining{0};
        bool finished{false};
    };

    void RequestOverlays(RE::Actor* actor);

    void ApplySlots(const std::shared_ptr<SlotQuery>& query)
    {
        auto& state = g_overlayState[query->id];
        state.busy = false;
        auto actor = LookupActor(query->id);
        if (!actor || !actor->Is3DLoaded()) {
            state.applied = false;
            return;
        }
        RE::TESObjectREFR* ref = actor;
        const bool female = query->female;
        auto wants = DesiredOverlays(query->id, actor->IsPlayerRef());

        std::vector<int> owned;
        std::vector<int> freeSlots;
        std::vector<std::string> current;
        {
            std::lock_guard lock(query->mutex);
            current = query->textures;
        }
        for (int i = static_cast<int>(current.size()) - 1; i >= 0; --i) {
            const auto tex = ToLower(current[static_cast<size_t>(i)]);
            if (tex.find("chiminjury") != std::string::npos) {
                owned.push_back(i);
            } else if (tex.empty() || tex.find("default.dds") != std::string::npos) {
                freeSlots.push_back(i);  // highest index first: RaceMenu body paints usually start at slot 0
            }
        }

        // Keep slots that already show the wanted texture, then reuse our other slots, then free ones.
        std::map<int, const OverlayWant*> assigned;
        std::vector<const OverlayWant*> pending;
        for (auto& want : wants) {
            const auto tex = ToLower(want.texture);
            auto it = std::find_if(owned.begin(), owned.end(), [&](int slot) {
                return !assigned.contains(slot) && ToLower(current[static_cast<size_t>(slot)]) == tex;
            });
            if (it != owned.end()) {
                assigned[*it] = &want;
            } else {
                pending.push_back(&want);
            }
        }
        std::vector<int> available;
        for (int slot : owned) {
            if (!assigned.contains(slot)) {
                available.push_back(slot);
            }
        }
        available.insert(available.end(), freeSlots.begin(), freeSlots.end());
        size_t next = 0;
        int dropped = 0;
        for (auto* want : pending) {
            if (next < available.size()) {
                assigned[available[next++]] = want;
            } else {
                ++dropped;
            }
        }
        if (dropped > 0) {
            ++g_overlaySlotsShort;
            logger::info("{}: {} wound overlay(s) not shown, no free RaceMenu body overlay slot (iNumOverlays={})",
                ActorName(actor), dropped, g_overlay.numOverlays);
        }

        if (!actor->IsPlayerRef() && !assigned.empty()) {
            CallStatic(kNio, "AddOverlays", nullptr, ref);
        }
        for (auto& [slot, want] : assigned) {
            const auto node = SlotNode(slot);
            CallStatic(kNio, "AddNodeOverrideString", nullptr, ref, female, node, static_cast<std::int32_t>(9), static_cast<std::int32_t>(0), want->texture, true);
            CallStatic(kNio, "AddNodeOverrideInt", nullptr, ref, female, node, static_cast<std::int32_t>(7), static_cast<std::int32_t>(-1), want->color, true);
            CallStatic(kNio, "AddNodeOverrideFloat", nullptr, ref, female, node, static_cast<std::int32_t>(8), static_cast<std::int32_t>(-1), want->alpha, true);
            ++g_statOverlayApplied;
        }
        for (int slot : owned) {
            if (assigned.contains(slot)) {
                continue;
            }
            const auto node = SlotNode(slot);
            CallStatic(kNio, "RemoveAllNodeNameOverrides", nullptr, ref, female, node);
            CallStatic(kNio, "AddNodeOverrideString", nullptr, ref, female, node, static_cast<std::int32_t>(9), static_cast<std::int32_t>(0), std::string(kDefaultOverlay), false);
            CallStatic(kNio, "AddNodeOverrideFloat", nullptr, ref, female, node, static_cast<std::int32_t>(8), static_cast<std::int32_t>(-1), 0.0f, false);
        }
        state.applied = true;
        state.sig = Signature(wants) + (female ? "F" : "M");
        state.root = actor->Get3D();
        state.shown = static_cast<int>(assigned.size());
        state.wanted = static_cast<int>(wants.size());
        logger::debug("{}: {} wound overlay(s) shown, {} slot(s) ours before, {} free", ActorName(actor), assigned.size(), owned.size(), freeSlots.size());
        if (state.again) {
            state.again = false;
            RequestOverlays(actor);
        }
    }

    void FinishQuery(const std::shared_ptr<SlotQuery>& query)
    {
        {
            std::lock_guard lock(query->mutex);
            if (query->finished) {
                return;
            }
            query->finished = true;
        }
        RunOnGameThread([query]() { ApplySlots(query); });
    }

    // Reads which body overlay slots are free or ours, then applies the wanted overlays.
    void RequestOverlays(RE::Actor* actor)
    {
        if (!actor || !g_overlay.skeeLoaded || g_overlay.numOverlays <= 0 || !g_gameReady) {
            return;
        }
        auto& state = g_overlayState[actor->GetFormID()];
        if (!actor->Is3DLoaded()) {
            state.applied = false;
            return;
        }
        const bool female = IsFemale(actor);
        const auto sig = Signature(DesiredOverlays(actor->GetFormID(), actor->IsPlayerRef())) + (female ? "F" : "M");
        if (state.busy) {
            state.again = true;
            return;
        }
        if (state.applied && state.sig == sig && state.root == actor->Get3D()) {
            return;
        }
        state.busy = true;
        auto query = std::make_shared<SlotQuery>();
        query->id = actor->GetFormID();
        query->female = female;
        query->textures.resize(static_cast<size_t>(g_overlay.numOverlays));
        query->remaining = g_overlay.numOverlays;
        RE::TESObjectREFR* ref = actor;
        for (int i = 0; i < g_overlay.numOverlays; ++i) {
            const bool ok = CallStatic(kNio, "GetNodeOverrideString",
                [query, i](const RE::BSScript::Variable& result) {
                    bool done = false;
                    {
                        std::lock_guard lock(query->mutex);
                        if (result.IsString()) {
                            query->textures[static_cast<size_t>(i)] = std::string(result.GetString());
                        }
                        done = --query->remaining == 0;
                    }
                    if (done) {
                        FinishQuery(query);
                    }
                },
                ref, female, SlotNode(i), static_cast<std::int32_t>(9), static_cast<std::int32_t>(0));
            if (!ok) {
                state.busy = false;
                SetLastError("NiOverride (RaceMenu) Papyrus functions not available");
                return;
            }
        }
        // Watchdog: if Papyrus never answers, carry on with what we have.
        Scheduler::Get().After(5000ms, [query]() {
            bool late = false;
            {
                std::lock_guard lock(query->mutex);
                late = !query->finished && query->remaining > 0;
                if (late) {
                    // Unknown slots must not be treated as free.
                    for (auto& t : query->textures) {
                        if (t.empty()) {
                            t = "unknown";
                        }
                    }
                }
            }
            if (late) {
                logger::warn("RaceMenu did not answer the overlay slot query in time");
                FinishQuery(query);
            }
        });
    }
}

// ========================================================================================
// Wounds: creation, healing, sleeping
// ========================================================================================
namespace
{
    float Health(RE::Actor* actor)
    {
        return actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
    }

    float MaxHealth(RE::Actor* actor)
    {
        return std::max(1.0f, actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kHealth));
    }

    std::string DisplayName(RE::Actor* actor)
    {
        return actor && actor->IsPlayerRef() ? "You" : ActorName(actor);
    }

    std::string Possessive(const std::string& name)
    {
        if (!name.empty() && (name.back() == 's' || name.back() == 'S')) {
            return name + "'";
        }
        return name + "'s";
    }

    void OverlaysChanged(RE::FormID id)
    {
        g_stateDirty = true;
        if (auto actor = LookupActor(id)) {
            RequestOverlays(actor);
        }
    }

    std::string SourceText(const Wound& w)
    {
        if (!w.by.empty() && !w.with.empty()) {
            return std::format(", from {} {}", Possessive(w.by), w.with);
        }
        if (!w.by.empty()) {
            return std::format(", from {}", w.by);
        }
        if (!w.with.empty()) {
            return std::format(", from {}", w.with);
        }
        return {};
    }

    void AnnounceWound(RE::Actor* actor, const Wound& w, bool worse)
    {
        ++g_statWounds;
        const auto label = WoundLabel(w);
        logger::info("{} {}: {} on the {} (severity {}, type {}){}", ActorName(actor), worse ? "wound worse" : "wounded", label,
            RegionLabel(w.region), w.severity, TypeKey(w.type), SourceText(w));
        if (g_settings.notifications && w.severity >= g_settings.notifyMinSeverity) {
            Notify(std::format("{}: {} ({})", DisplayName(actor), label, RegionLabel(w.region)), true);
        }
        if (w.severity >= g_settings.logMinSeverity) {
            const auto name = ActorName(actor);
            LogToChim(name, worse ? "worse" : "new",
                std::format("{} {} {} on the {}{}.", name, worse ? "now has a worse wound:" : "was wounded:", label, RegionLabel(w.region), SourceText(w)));
        }
    }

    // Adds a wound (or makes an existing one worse). Max `maxWounds` per actor.
    void AddWound(RE::Actor* actor, Wound w)
    {
        auto& entry = Entry(actor);
        const float now = GameDays();
        w.created = now;
        w.due = w.severity < 3 ? now + HealDays(w.severity) : 0.0f;
        auto same = std::find_if(entry.wounds.begin(), entry.wounds.end(), [&](auto& x) { return x.region == w.region && x.type == w.type; });
        if (same != entry.wounds.end()) {
            if (w.severity > same->severity) {
                *same = w;
                AnnounceWound(actor, *same, true);
                OverlaysChanged(actor->GetFormID());
            } else if (same->due > 0.0f) {
                same->due = std::max(same->due, now + HealDays(same->severity));  // fresh hit on the same wound: heals later
                g_stateDirty = true;
            }
            return;
        }
        if (static_cast<int>(entry.wounds.size()) >= g_settings.maxWounds) {
            // Full: a heavier wound replaces one in the same region, otherwise the lightest one.
            auto victimIt = std::find_if(entry.wounds.begin(), entry.wounds.end(), [&](auto& x) { return x.region == w.region; });
            if (victimIt == entry.wounds.end() || victimIt->severity > w.severity) {
                victimIt = std::min_element(entry.wounds.begin(), entry.wounds.end(), [](auto& a, auto& b) {
                    return a.severity != b.severity ? a.severity < b.severity : a.created < b.created;
                });
            }
            if (victimIt == entry.wounds.end() || victimIt->severity > w.severity) {
                logger::debug("{}: wound limit reached, new {} ignored", ActorName(actor), WoundLabel(w));
                return;
            }
            *victimIt = w;
            AnnounceWound(actor, *victimIt, false);
            OverlaysChanged(actor->GetFormID());
            return;
        }
        entry.wounds.push_back(w);
        AnnounceWound(actor, entry.wounds.back(), false);
        OverlaysChanged(actor->GetFormID());
    }

    // --- classification -------------------------------------------------------------------
    struct Classified
    {
        WType type{WType::kBlunt};
        bool arrow{false};
        std::string with;
    };

    std::optional<WType> ElementOf(RE::MagicItem* item)
    {
        if (!item) {
            return std::nullopt;
        }
        for (auto* effect : item->effects) {
            auto* mgef = effect ? effect->baseEffect : nullptr;
            if (!mgef || !(mgef->IsDetrimental() || mgef->IsHostile())) {
                continue;
            }
            switch (mgef->data.resistVariable) {
            case RE::ActorValue::kResistFire: return WType::kBurn;
            case RE::ActorValue::kResistFrost: return WType::kFrost;
            case RE::ActorValue::kResistShock: return WType::kShock;
            default: break;
            }
        }
        return std::nullopt;
    }

    RE::MagicItem* AsMagicItem(RE::TESForm* form)
    {
        if (!form) {
            return nullptr;
        }
        if (auto s = form->As<RE::SpellItem>()) return s;
        if (auto s = form->As<RE::ScrollItem>()) return s;
        if (auto s = form->As<RE::EnchantmentItem>()) return s;
        return nullptr;
    }

    std::optional<Classified> Classify(RE::FormID sourceId, RE::Actor* attacker, bool bash)
    {
        Classified c;
        const bool creature = attacker && !HasActorKeyword(attacker, "ActorTypeNPC");
        std::string raceId;
        if (attacker && attacker->GetRace()) {
            raceId = ToLower(attacker->GetRace()->GetFormEditorID());
        }
        const bool heavyCreature = raceId.find("giant") != std::string::npos || raceId.find("mammoth") != std::string::npos ||
                                   raceId.find("dwarven") != std::string::npos || raceId.find("centurion") != std::string::npos;
        auto* form = sourceId ? RE::TESForm::LookupByID(sourceId) : nullptr;
        if (bash) {
            c.type = WType::kBlunt;
            return c;
        }
        if (auto weap = form ? form->As<RE::TESObjectWEAP>() : nullptr) {
            if (const char* n = weap->GetName(); n && n[0] != '\0' && !weap->IsHandToHandMelee()) {
                c.with = n;
            }
            switch (weap->GetWeaponType()) {
            case RE::WEAPON_TYPE::kHandToHandMelee:
                c.type = creature && !heavyCreature ? WType::kMaul : WType::kBlunt;
                break;
            case RE::WEAPON_TYPE::kOneHandSword:
            case RE::WEAPON_TYPE::kTwoHandSword:
                c.type = weap->HasKeywordString("WeapTypeSpear") || weap->HasKeywordString("WeapTypePike") || weap->HasKeywordString("WeapTypeRapier")
                             ? WType::kPierce
                             : WType::kSlash;
                break;
            case RE::WEAPON_TYPE::kOneHandDagger:
                c.type = WType::kPierce;
                break;
            case RE::WEAPON_TYPE::kOneHandAxe:
                c.type = WType::kSlash;
                break;
            case RE::WEAPON_TYPE::kTwoHandAxe:
                c.type = weap->HasKeywordString("WeapTypeWarhammer") ? WType::kBlunt : WType::kSlash;
                break;
            case RE::WEAPON_TYPE::kOneHandMace:
                c.type = WType::kBlunt;
                break;
            case RE::WEAPON_TYPE::kStaff:
                c.type = ElementOf(weap->formEnchanting).value_or(WType::kBlunt);
                break;
            case RE::WEAPON_TYPE::kBow:
            case RE::WEAPON_TYPE::kCrossbow:
                c.type = WType::kPierce;
                c.arrow = true;
                break;
            default:
                c.type = WType::kBlunt;
                break;
            }
            if (weap->HasKeywordString("WeapTypeWhip") || weap->HasKeywordString("WeapTypeClaw")) {
                c.type = WType::kSlash;
            }
            return c;
        }
        if (auto item = AsMagicItem(form)) {
            auto element = ElementOf(item);
            if (!element) {
                return std::nullopt;  // other magic (poison, paralysis, ...) leaves no wound
            }
            c.type = *element;
            if (const char* n = item->GetName(); n && n[0] != '\0') {
                c.with = n;
            }
            return c;
        }
        if (form) {
            return std::nullopt;  // explosions, traps without a weapon, ...
        }
        if (attacker) {
            c.type = creature && !heavyCreature ? WType::kMaul : WType::kBlunt;  // natural attack without a weapon form
            return c;
        }
        return std::nullopt;
    }

    Region PickRegion(RE::Actor* victim, RE::Actor* attacker)
    {
        // head 8, torso 34, arms 14 + 14, legs 15 + 15
        static constexpr std::array<int, 6> weights = { 8, 34, 14, 14, 15, 15 };
        std::discrete_distribution<int> dist(weights.begin(), weights.end());
        switch (dist(Rng())) {
        case 0: return Region::kHead;
        case 1:
            if (attacker && victim) {
                const float angle = victim->GetHeadingAngle(attacker->GetPosition(), true);
                if (angle > 110.0f) {
                    return Region::kBack;
                }
            }
            return Region::kChest;
        case 2: return Region::kLeftArm;
        case 3: return Region::kRightArm;
        case 4: return Region::kLeftLeg;
        default: return Region::kRightLeg;
        }
    }

    // --- hits -------------------------------------------------------------------------------
    // The hit event has no damage amount: health is compared just before and shortly after the hit.
    struct HitInfo
    {
        RE::FormID victim{0};
        RE::ActorHandle attacker;
        RE::FormID source{0};
        bool power{false};
        bool sneak{false};
        bool bash{false};
        bool blocked{false};
    };

    struct PendingHit
    {
        HitInfo hit;
        float baseline{0.0f};
    };

    using Clock = std::chrono::steady_clock;
    std::unordered_map<RE::FormID, PendingHit> g_pending;
    std::unordered_map<RE::FormID, std::pair<float, Clock::time_point>> g_healthSample;
    std::unordered_map<RE::FormID, Clock::time_point> g_lastWoundAt;

    void EvaluateHit(RE::FormID id)
    {
        auto it = g_pending.find(id);
        if (it == g_pending.end()) {
            return;
        }
        const PendingHit pending = it->second;
        g_pending.erase(it);
        auto victim = LookupActor(id);
        if (!victim || victim->IsDead() || !g_gameReady) {
            return;
        }
        const auto now = Clock::now();
        const float health = Health(victim);
        const float maxHealth = std::max(MaxHealth(victim), pending.baseline);
        g_healthSample[id] = { health, now };
        const float drop = pending.baseline - health;
        const float pct = drop / maxHealth * 100.0f;
        const bool special = pending.hit.power || pending.hit.sneak;
        logger::debug("Hit on {}: health {:.0f} -> {:.0f} ({:.1f}% of {:.0f}), power={} sneak={} bash={} blocked={}", ActorName(victim),
            pending.baseline, health, pct, maxHealth, pending.hit.power, pending.hit.sneak, pending.hit.bash, pending.hit.blocked);
        if (drop < 0.5f || (pct < g_settings.minDamagePct && (!special || pending.hit.blocked))) {
            return;
        }
        if (auto last = g_lastWoundAt.find(id); last != g_lastWoundAt.end() && now - last->second < 1000ms) {
            return;
        }
        auto attackerPtr = pending.hit.attacker.get();
        RE::Actor* attacker = attackerPtr.get();
        auto classified = Classify(pending.hit.source, attacker, pending.hit.bash);
        if (!classified) {
            logger::debug("Hit on {}: source {:08X} leaves no wound", ActorName(victim), pending.hit.source);
            return;
        }
        int severity = pct >= g_settings.severeDamagePct ? 3 : pct >= g_settings.mediumDamagePct ? 2 : 1;
        if (special && !pending.hit.blocked) {
            ++severity;
        }
        if (health / maxHealth < 0.2f || victim->AsActorState()->IsBleedingOut()) {
            ++severity;
        }
        Wound w;
        w.region = PickRegion(victim, attacker);
        w.type = classified->type;
        w.arrow = classified->arrow;
        w.severity = static_cast<std::uint8_t>(std::clamp(severity, 1, 3));
        w.with = classified->with;
        if (attacker && attacker != victim) {
            w.by = ActorName(attacker);
        }
        g_lastWoundAt[id] = now;
        AddWound(victim, std::move(w));
    }

    void OnHit(HitInfo hit)
    {
        if (!g_gameReady || !g_settings.enabled) {
            return;
        }
        auto victim = LookupActor(hit.victim);
        if (!victim || victim->IsDead() || !IsTracked(victim)) {
            return;
        }
        ++g_statHits;
        if (auto it = g_pending.find(hit.victim); it != g_pending.end()) {
            auto& p = it->second.hit;
            p.power = p.power || hit.power;
            p.sneak = p.sneak || hit.sneak;
            p.blocked = p.blocked && hit.blocked;
            const bool oldWeapon = p.source && RE::TESForm::LookupByID<RE::TESObjectWEAP>(p.source);
            const bool newWeapon = hit.source && RE::TESForm::LookupByID<RE::TESObjectWEAP>(hit.source);
            if (!oldWeapon && newWeapon) {
                p.source = hit.source;
                p.attacker = hit.attacker;
                p.bash = hit.bash;
            }
            return;
        }
        float baseline = Health(victim);
        if (auto s = g_healthSample.find(hit.victim); s != g_healthSample.end() && Clock::now() - s->second.second < 1500ms) {
            baseline = std::max(baseline, s->second.first);
        }
        const auto id = hit.victim;
        g_pending[id] = { std::move(hit), baseline };
        Scheduler::Get().After(350ms, [id]() { EvaluateHit(id); });
    }

    // --- healing ----------------------------------------------------------------------------
    std::unordered_map<RE::FormID, Clock::time_point> g_lastHealAt;

    // Healing magic or a potion: light and medium wounds close, severe ones get treated.
    void HealByMagic(RE::Actor* actor, const std::string& how)
    {
        auto it = g_wounds.find(actor->GetFormID());
        if (it == g_wounds.end() || it->second.wounds.empty()) {
            return;
        }
        auto& wounds = it->second.wounds;
        int closed = 0;
        int closedMedium = 0;
        std::vector<std::string> treated;
        for (auto w = wounds.begin(); w != wounds.end();) {
            if (w->severity < 3) {
                closedMedium += w->severity == 2 ? 1 : 0;
                ++closed;
                w = wounds.erase(w);
                continue;
            }
            if (!w->treated) {
                w->treated = true;
                treated.push_back(std::format("{} on the {}", WoundLabel(*w), RegionLabel(w->region)));
            }
            ++w;
        }
        if (closed == 0 && treated.empty()) {
            return;
        }
        g_statHealedMagic += closed + static_cast<int>(treated.size());
        logger::info("{} healed by {}: {} wound(s) closed, {} severe wound(s) treated", ActorName(actor), how, closed, treated.size());
        if (g_settings.notifications) {
            Notify(treated.empty() ? std::format("{}: wounds healed", DisplayName(actor))
                                   : std::format("{}: wounds treated, rest in a bed to heal fully", DisplayName(actor)),
                true);
        }
        if (closedMedium > 0 || !treated.empty()) {
            const auto name = ActorName(actor);
            std::string sentence = std::format("{} wounds were healed with {}.", Possessive(name), how);
            if (!treated.empty()) {
                std::string list;
                for (size_t i = 0; i < treated.size(); ++i) {
                    list += (i == 0 ? "" : (i + 1 == treated.size() ? " and " : ", ")) + treated[i];
                }
                sentence += std::format(" The {} closed only partly and need a night's rest in a bed.", list);
            }
            LogToChim(name, "treated", sentence);
        }
        OverlaysChanged(actor->GetFormID());
    }

    bool IsRestoreHealth(const RE::EffectSetting* mgef)
    {
        return mgef && mgef->GetArchetype() == RE::EffectArchetypes::ArchetypeID::kValueModifier &&
               mgef->data.primaryAV == RE::ActorValue::kHealth && !mgef->IsDetrimental() && !mgef->IsHostile();
    }

    float RestoreAmount(RE::MagicItem* item, const RE::EffectSetting* only = nullptr)
    {
        float amount = 0.0f;
        if (!item) {
            return amount;
        }
        for (auto* effect : item->effects) {
            if (effect && IsRestoreHealth(effect->baseEffect) && (!only || effect->baseEffect == only)) {
                amount = std::max(amount, effect->effectItem.magnitude * std::max(1.0f, static_cast<float>(effect->effectItem.duration)));
            }
        }
        return amount;
    }

    // A restore-health effect landed on a tracked actor (spells; potions are handled by OnPotion).
    void OnHealEffect(RE::FormID targetId, RE::FormID mgefId, RE::ActorHandle casterHandle)
    {
        auto actor = LookupActor(targetId);
        auto mgef = RE::TESForm::LookupByID<RE::EffectSetting>(mgefId);
        if (!actor || !IsRestoreHealth(mgef) || !g_wounds.contains(targetId) || g_wounds[targetId].wounds.empty()) {
            return;
        }
        bool accept = false;
        bool found = false;
        if (auto* list = actor->GetMagicTarget() ? actor->GetMagicTarget()->GetActiveEffectList() : nullptr) {
            for (auto* ae : *list) {
                if (!ae || ae->GetBaseObject() != mgef || !ae->spell) {
                    continue;
                }
                found = true;
                auto* spell = ae->spell;
                if (spell->As<RE::AlchemyItem>() || spell->IsFood()) {
                    return;  // potions and food: OnPotion decides
                }
                const auto casting = spell->GetCastingType();
                if (casting == RE::MagicSystem::CastingType::kConstantEffect) {
                    continue;  // regeneration enchantments and abilities never heal wounds
                }
                if (casting == RE::MagicSystem::CastingType::kConcentration ||
                    ae->magnitude * std::max(1.0f, ae->duration) >= g_settings.healMinAmount) {
                    accept = true;
                }
            }
        }
        if (!found) {
            // Instant spell: its effect is already gone; look at what the caster has equipped.
            auto casterPtr = casterHandle.get();
            if (auto caster = casterPtr.get()) {
                for (bool left : { false, true }) {
                    auto spell = caster->GetEquippedObject(left) ? caster->GetEquippedObject(left)->As<RE::SpellItem>() : nullptr;
                    if (spell && RestoreAmount(spell, mgef) >= g_settings.healMinAmount) {
                        accept = true;
                    }
                }
            }
        }
        if (!accept) {
            return;
        }
        const auto now = Clock::now();
        if (auto last = g_lastHealAt.find(targetId); last != g_lastHealAt.end() && now - last->second < 2s) {
            return;
        }
        g_lastHealAt[targetId] = now;
        HealByMagic(actor, "healing magic");
    }

    void OnPotion(RE::FormID actorId, RE::FormID itemId)
    {
        auto actor = LookupActor(actorId);
        auto potion = RE::TESForm::LookupByID<RE::AlchemyItem>(itemId);
        if (!actor || !potion || potion->IsFood() || potion->IsPoison() || !IsTracked(actor)) {
            return;
        }
        if (RestoreAmount(potion) >= g_settings.healMinAmount) {
            HealByMagic(actor, "a healing potion");
        }
    }

    // --- time and sleep ---------------------------------------------------------------------
    void HealByTime()
    {
        const float now = GameDays();
        for (auto& [id, entry] : g_wounds) {
            const auto before = entry.wounds.size();
            std::erase_if(entry.wounds, [&](const Wound& w) { return w.due > 0.0f && now >= w.due; });
            const auto healed = before - entry.wounds.size();
            if (healed > 0) {
                g_statHealedTime += static_cast<int>(healed);
                logger::info("{}: {} wound(s) healed with time", entry.name, healed);
                OverlaysChanged(id);
            }
        }
    }

    float g_sleepStartDays{-1.0f};

    void OnSleepStart()
    {
        g_sleepStartDays = GameDays();
    }

    void OnSleepStop()
    {
        if (g_sleepStartDays < 0.0f) {
            return;
        }
        const float hours = (GameDays() - g_sleepStartDays) * 24.0f;
        g_sleepStartDays = -1.0f;
        HealByTime();
        if (hours + 0.01f < g_settings.minSleepHours) {
            logger::info("Slept {:.1f} h, less than {:.0f} h: no rest healing", hours, g_settings.minSleepHours);
            return;
        }
        auto player = RE::PlayerCharacter::GetSingleton();
        int total = 0;
        for (auto& [id, entry] : g_wounds) {
            // The party rests together: the player and followers who are here.
            if (!(player && id == player->GetFormID()) && !g_tracked.contains(id)) {
                continue;
            }
            std::vector<std::string> healed;
            std::erase_if(entry.wounds, [&](const Wound& w) {
                if (w.severity >= 3 && w.treated) {
                    healed.push_back(std::format("{} on the {}", WoundLabel(w), RegionLabel(w.region)));
                    return true;
                }
                return false;
            });
            if (healed.empty()) {
                continue;
            }
            total += static_cast<int>(healed.size());
            g_statHealedRest += static_cast<int>(healed.size());
            std::string list;
            for (size_t i = 0; i < healed.size(); ++i) {
                list += (i == 0 ? "" : (i + 1 == healed.size() ? " and " : ", ")) + healed[i];
            }
            logger::info("{} rested: {} healed", entry.name, list);
            LogToChim(entry.name, "rested", std::format("After a good rest in a bed, {} {} has healed.", Possessive(entry.name), list));
            OverlaysChanged(id);
        }
        if (total > 0 && g_settings.notifications) {
            Notify(std::format("rested, {} treated wound(s) healed", total), true);
        }
    }

    void OnActorDeath(RE::FormID id)
    {
        auto actor = LookupActor(id);
        if (!actor || actor->IsPlayerRef()) {
            return;
        }
        if (auto it = g_wounds.find(id); it != g_wounds.end()) {
            g_wounds.erase(it);
            g_stateDirty = true;
        }
    }

    // --- page commands ------------------------------------------------------------------------
    void FullHeal(RE::FormID id)
    {
        if (auto it = g_wounds.find(id); it != g_wounds.end() && !it->second.wounds.empty()) {
            it->second.wounds.clear();
            OverlaysChanged(id);
        }
    }

    void ExecuteCommand(const std::string& action, const std::string& actorHex, const std::string& key)
    {
        RE::FormID id = 0;
        try {
            id = actorHex.empty() ? 0 : static_cast<RE::FormID>(std::stoul(actorHex, nullptr, 16));
        } catch (...) {
            id = 0;
        }
        logger::info("Command from the plugin page: {} {:08X} {}", action, id, key);
        if (action == "heal") {
            FullHeal(id);
        } else if (action == "treat") {
            if (auto actor = LookupActor(id)) {
                HealByMagic(actor, "healing magic");
            }
        } else if (action == "remove") {
            if (auto it = g_wounds.find(id); it != g_wounds.end()) {
                std::erase_if(it->second.wounds, [&](const Wound& w) { return WoundKey(w) == key; });
                OverlaysChanged(id);
            }
        } else if (action == "heal_all" || action == "reset") {
            std::vector<RE::FormID> ids;
            for (auto& [wid, entry] : g_wounds) {
                ids.push_back(wid);
            }
            for (auto wid : ids) {
                FullHeal(wid);
            }
            if (action == "reset") {
                g_wounds.clear();
                g_stateDirty = true;
                Notify("all wounds and wound overlays removed", true);
            }
        }
    }
}

// ========================================================================================
// Periodic work (game thread)
// ========================================================================================
namespace
{
    std::atomic<bool> g_tickStarted{false};
    int g_tickCount{0};

    void RefreshAllOverlays(bool force)
    {
        std::set<RE::FormID> ids(g_tracked.begin(), g_tracked.end());
        for (auto& [id, entry] : g_wounds) {
            ids.insert(id);
        }
        for (auto id : ids) {
            if (force) {
                g_overlayState[id].applied = false;
            }
            if (auto actor = LookupActor(id)) {
                RequestOverlays(actor);
            }
        }
    }

    void Tick()
    {
        Scheduler::Get().After(250ms, []() { Tick(); });
        if (!g_gameReady) {
            return;
        }
        const auto now = Clock::now();
        for (auto id : g_tracked) {
            if (g_pending.contains(id)) {
                continue;
            }
            if (auto actor = LookupActor(id); actor && actor->Is3DLoaded() && !actor->IsDead()) {
                g_healthSample[id] = { Health(actor), now };
            }
        }
        if (++g_tickCount % 8 != 0) {
            return;
        }
        RefreshTracked();
        HealByTime();
        RefreshAllOverlays(false);  // cheap unless an actor's 3D was reloaded or its wounds changed
    }
}

// ========================================================================================
// Event sinks: forward to the game thread
// ========================================================================================
namespace
{
    class HitSink : public RE::BSTEventSink<RE::TESHitEvent>
    {
    public:
        static HitSink* Get()
        {
            static HitSink instance;
            return &instance;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
        {
            if (!a_event || !a_event->target || !g_settings.enabled) {
                return RE::BSEventNotifyControl::kContinue;
            }
            auto target = a_event->target->As<RE::Actor>();
            if (!target) {
                return RE::BSEventNotifyControl::kContinue;
            }
            if (!target->IsPlayerRef() && !IsFollower(target)) {
                return RE::BSEventNotifyControl::kContinue;  // only the player and followers get wounds
            }
            auto cause = a_event->cause ? a_event->cause->As<RE::Actor>() : nullptr;
            if (cause == target) {
                return RE::BSEventNotifyControl::kContinue;
            }
            HitInfo hit;
            hit.victim = target->GetFormID();
            if (cause) {
                hit.attacker = cause->GetHandle();
            }
            hit.source = a_event->source;
            hit.power = a_event->flags.any(RE::TESHitEvent::Flag::kPowerAttack);
            hit.sneak = a_event->flags.any(RE::TESHitEvent::Flag::kSneakAttack);
            hit.bash = a_event->flags.any(RE::TESHitEvent::Flag::kBashAttack);
            hit.blocked = a_event->flags.any(RE::TESHitEvent::Flag::kHitBlocked);
            RunOnGameThread([hit]() { OnHit(hit); });
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class MagicSink : public RE::BSTEventSink<RE::TESMagicEffectApplyEvent>
    {
    public:
        static MagicSink* Get()
        {
            static MagicSink instance;
            return &instance;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESMagicEffectApplyEvent* a_event, RE::BSTEventSource<RE::TESMagicEffectApplyEvent>*) override
        {
            if (!a_event || !a_event->target || !g_settings.enabled) {
                return RE::BSEventNotifyControl::kContinue;
            }
            auto target = a_event->target->As<RE::Actor>();
            if (!target) {
                return RE::BSEventNotifyControl::kContinue;
            }
            const auto mgefId = a_event->magicEffect;
            if (!IsRestoreHealth(RE::TESForm::LookupByID<RE::EffectSetting>(mgefId))) {
                return RE::BSEventNotifyControl::kContinue;
            }
            const auto targetId = target->GetFormID();
            RE::ActorHandle caster;
            if (auto c = a_event->caster ? a_event->caster->As<RE::Actor>() : nullptr) {
                caster = c->GetHandle();
            }
            Scheduler::Get().After(100ms, [targetId, mgefId, caster]() { OnHealEffect(targetId, mgefId, caster); });
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class EquipSink : public RE::BSTEventSink<RE::TESEquipEvent>
    {
    public:
        static EquipSink* Get()
        {
            static EquipSink instance;
            return &instance;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_event, RE::BSTEventSource<RE::TESEquipEvent>*) override
        {
            if (!a_event || !a_event->actor || !a_event->equipped || !g_settings.enabled) {
                return RE::BSEventNotifyControl::kContinue;
            }
            auto actor = a_event->actor->As<RE::Actor>();
            if (!actor) {
                return RE::BSEventNotifyControl::kContinue;
            }
            const auto itemId = a_event->baseObject;
            if (!RE::TESForm::LookupByID<RE::AlchemyItem>(itemId)) {
                return RE::BSEventNotifyControl::kContinue;
            }
            const auto actorId = actor->GetFormID();
            RunOnGameThread([actorId, itemId]() { OnPotion(actorId, itemId); });
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class SleepStartSink : public RE::BSTEventSink<RE::TESSleepStartEvent>
    {
    public:
        static SleepStartSink* Get()
        {
            static SleepStartSink instance;
            return &instance;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESSleepStartEvent*, RE::BSTEventSource<RE::TESSleepStartEvent>*) override
        {
            RunOnGameThread([]() { OnSleepStart(); });
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class SleepStopSink : public RE::BSTEventSink<RE::TESSleepStopEvent>
    {
    public:
        static SleepStopSink* Get()
        {
            static SleepStopSink instance;
            return &instance;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESSleepStopEvent*, RE::BSTEventSource<RE::TESSleepStopEvent>*) override
        {
            RunOnGameThread([]() { OnSleepStop(); });
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class DeathSink : public RE::BSTEventSink<RE::TESDeathEvent>
    {
    public:
        static DeathSink* Get()
        {
            static DeathSink instance;
            return &instance;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* a_event, RE::BSTEventSource<RE::TESDeathEvent>*) override
        {
            if (!a_event || !a_event->actorDying || !a_event->dead) {
                return RE::BSEventNotifyControl::kContinue;
            }
            const auto id = a_event->actorDying->GetFormID();
            RunOnGameThread([id]() { OnActorDeath(id); });
            return RE::BSEventNotifyControl::kContinue;
        }
    };
}

// ========================================================================================
// Server sync: settings + page commands in, status + wounds + log tail out
// ========================================================================================
namespace
{
    void ApplyServerConfig(const std::string& text)
    {
        std::map<std::string, std::string> values;
        std::vector<std::string> commands;
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            auto key = Trim(line.substr(0, eq));
            auto value = Trim(line.substr(eq + 1));
            if (key == "cmd") {
                commands.push_back(value);
            } else {
                values[key] = value;
            }
        }
        if (values["plugin"] != kPluginId) {
            return;
        }
        auto b = [&](const char* key, bool& target) {
            if (values.contains(key)) target = values[key] == "1";
        };
        auto f = [&](const char* key, float& target) {
            try {
                if (values.contains(key)) target = std::stof(values[key]);
            } catch (...) {
            }
        };
        auto i = [&](const char* key, int& target) {
            try {
                if (values.contains(key)) target = std::stoi(values[key]);
            } catch (...) {
            }
        };
        Settings s = g_settings;
        b("enabled", s.enabled);
        b("debug", s.debug);
        b("track_player", s.trackPlayer);
        b("track_followers", s.trackFollowers);
        b("show_overlays", s.showOverlays);
        b("show_on_player", s.showOnPlayer);
        b("notifications", s.notifications);
        i("notify_min_severity", s.notifyMinSeverity);
        b("log_to_chim", s.logToChim);
        i("log_min_severity", s.logMinSeverity);
        f("min_damage_pct", s.minDamagePct);
        f("medium_damage_pct", s.mediumDamagePct);
        f("severe_damage_pct", s.severeDamagePct);
        i("max_wounds", s.maxWounds);
        f("light_heal_days", s.lightHealDays);
        f("medium_heal_days", s.mediumHealDays);
        f("heal_min_amount", s.healMinAmount);
        f("min_sleep_hours", s.minSleepHours);
        i("overlay_strength", s.overlayStrength);
        ClampSettings(s);
        const bool debugChanged = s.debug != g_settings.debug;
        const bool visualsChanged = s.showOverlays != g_settings.showOverlays || s.showOnPlayer != g_settings.showOnPlayer ||
                                    s.overlayStrength != g_settings.overlayStrength || s.enabled != g_settings.enabled;
        g_settings = s;
        if (debugChanged) {
            ApplyLogLevel();
            logger::info("Debug mode {}", s.debug ? "on" : "off");
        }
        if (visualsChanged && g_gameReady) {
            RefreshAllOverlays(false);
        }
        // cmd=<id>|<action>|<actor form id hex>|<wound key>, executed once each, oldest first.
        std::vector<std::pair<std::uint64_t, std::vector<std::string>>> parsed;
        for (auto& c : commands) {
            std::vector<std::string> parts;
            std::stringstream ss(c);
            std::string part;
            while (std::getline(ss, part, '|')) {
                parts.push_back(Trim(part));
            }
            if (parts.size() < 2) {
                continue;
            }
            try {
                parsed.emplace_back(std::stoull(parts[0]), parts);
            } catch (...) {
            }
        }
        std::sort(parsed.begin(), parsed.end(), [](auto& a, auto& b) { return a.first < b.first; });
        for (auto& [id, parts] : parsed) {
            if (id <= g_lastCommand || !g_gameReady) {
                continue;
            }
            g_lastCommand = id;
            ExecuteCommand(parts[1], parts.size() > 2 ? parts[2] : "", parts.size() > 3 ? parts[3] : "");
        }
    }

    std::string BuildStateJson()
    {
        const float now = GameDays();
        auto player = RE::PlayerCharacter::GetSingleton();
        std::set<RE::FormID> ids(g_tracked.begin(), g_tracked.end());
        for (auto& [id, entry] : g_wounds) {
            if (!entry.wounds.empty()) {
                ids.insert(id);
            }
        }
        std::string actors;
        for (auto id : ids) {
            auto actor = LookupActor(id);
            auto it = g_wounds.find(id);
            const std::string name = it != g_wounds.end() ? it->second.name : ActorName(actor);
            const bool isPlayer = player && id == player->GetFormID();
            std::string wounds;
            if (it != g_wounds.end()) {
                for (auto& w : it->second.wounds) {
                    if (!wounds.empty()) {
                        wounds += ",";
                    }
                    wounds += std::format(
                        "{{\"key\":\"{}\",\"region\":\"{}\",\"type\":\"{}\",\"severity\":{},\"treated\":{},\"arrow\":{},"
                        "\"age_days\":{:.3f},\"heals_in_days\":{:.3f},\"label\":\"{}\",\"by\":\"{}\",\"with\":\"{}\"}}",
                        WoundKey(w), RegionKey(w.region), TypeKey(w.type), w.severity, w.treated, w.arrow, std::max(0.0f, now - w.created),
                        w.due > 0.0f ? std::max(0.0f, w.due - now) : -1.0f, JsonEscape(WoundLabel(w)), JsonEscape(w.by), JsonEscape(w.with));
                }
            }
            const auto& ov = g_overlayState[id];
            if (!actors.empty()) {
                actors += ",";
            }
            actors += std::format(
                "{{\"id\":\"{:08X}\",\"name\":\"{}\",\"player\":{},\"female\":{},\"follower\":{},\"loaded\":{},"
                "\"overlays_shown\":{},\"overlays_wanted\":{},\"wounds\":[{}]}}",
                id, JsonEscape(name), isPlayer, actor ? IsFemale(actor) : false, !isPlayer && g_tracked.contains(id),
                actor ? actor->Is3DLoaded() : false, ov.shown, ov.wanted, wounds);
        }
        return std::format("{{\"game_days\":{:.4f},\"player_name\":\"{}\",\"actors\":[{}]}}", now,
            JsonEscape(player ? ActorName(player) : std::string()), actors);
    }

    std::string BuildStatusJson()
    {
        std::vector<std::string> fresh;
        if (g_logTail) {
            std::lock_guard lock(g_logSentMutex);
            for (auto& line : g_logTail->last_formatted()) {
                auto trimmed = Trim(line);
                if (!trimmed.empty() && !g_logSent.contains(trimmed)) {
                    fresh.push_back(trimmed);
                }
            }
        }
        std::string lines;
        for (auto& line : fresh) {
            if (!lines.empty()) {
                lines += ",";
            }
            lines += "\"" + JsonEscape(line.substr(0, 500)) + "\"";
        }
        std::string lastError;
        {
            std::lock_guard lock(g_lastErrorMutex);
            lastError = g_lastError;
        }
        int woundCount = 0;
        for (auto& [id, entry] : g_wounds) {
            woundCount += static_cast<int>(entry.wounds.size());
        }
        const auto runtime = REL::Module::get().version().string(".");
        const std::string state = g_gameReady ? BuildStateJson() : std::string("null");
        g_stateDirty = false;
        return std::format(
            "{{\"plugin\":\"{}\",\"version\":\"{}\",\"game_version\":\"{}\",\"server_from\":\"{}\",\"game_loaded\":{},"
            "\"racemenu_loaded\":{},\"racemenu_version\":\"{}\",\"overlay_slots\":{},\"overlay_player_only\":{},\"overlay_slots_short\":{},"
            "\"enabled\":{},\"debug\":{},\"tracked\":{},\"wounds\":{},\"hits_seen\":{},\"wounds_made\":{},\"healed_magic\":{},"
            "\"healed_rest\":{},\"healed_time\":{},\"logged_to_chim\":{},\"overlays_applied\":{},\"failures\":{},\"cmd_done\":{},"
            "\"last_error\":\"{}\",\"state\":{},\"log_lines\":[{}]}}",
            kPluginId, kPluginVersion, JsonEscape(runtime), g_server.fromIni ? "CHIMInjury.ini" : "auto", g_gameReady,
            g_overlay.skeeLoaded, JsonEscape(g_overlay.skeeVersion), g_overlay.numOverlays, g_overlay.playerOnly, g_overlaySlotsShort.load(),
            g_settings.enabled, g_settings.debug, g_tracked.size(), woundCount, g_statHits.load(), g_statWounds.load(),
            g_statHealedMagic.load(), g_statHealedRest.load(), g_statHealedTime.load(), g_statLogged.load(), g_statOverlayApplied.load(),
            g_statFailures.load(), g_lastCommand, JsonEscape(lastError), state, lines);
    }

    void MarkLinesSent(const std::string& json)
    {
        if (!g_logTail) {
            return;
        }
        std::lock_guard lock(g_logSentMutex);
        for (auto& line : g_logTail->last_formatted()) {
            auto trimmed = Trim(line);
            if (json.find(JsonEscape(trimmed.substr(0, 500))) != std::string::npos) {
                g_logSent.insert(trimmed);
            }
        }
        if (g_logSent.size() > 2000) {
            g_logSent.clear();
            for (auto& line : g_logTail->last_formatted()) {
                g_logSent.insert(Trim(line));
            }
        }
    }

    class NetWorker
    {
    public:
        static NetWorker& Get()
        {
            static NetWorker instance;
            return instance;
        }

        void Start()
        {
            if (_running.exchange(true)) {
                return;
            }
            std::thread([this]() { Loop(); }).detach();
        }

        void Wake()
        {
            _wake = true;
            _cv.notify_one();
        }

    private:
        void Loop()
        {
            DiscoverServer();
            int idle = 0;
            while (_running) {
                SyncOnce();
                // Every 15 s, or within 2 s after the wounds changed.
                for (idle = 0; idle < 15 && _running; ++idle) {
                    std::unique_lock lock(_mutex);
                    if (_cv.wait_for(lock, 1s, [this]() { return _wake.load(); })) {
                        break;
                    }
                    if (g_stateDirty && idle >= 1) {
                        break;
                    }
                }
                _wake = false;
            }
        }

        void SyncOnce()
        {
            std::string config;
            const bool configOk = HttpRequest(g_server.host, g_server.port, "GET", g_server.basePath + "/api/config.php", {}, &config);
            if (configOk != g_serverReachable.exchange(configOk)) {
                if (configOk) {
                    logger::info("{} server plugin reachable at {}:{}", kPluginId, g_server.host, g_server.port);
                } else {
                    logger::info("{} server plugin not reachable at {}:{} (using CHIMInjury.ini settings)", kPluginId, g_server.host, g_server.port);
                }
            }
            if (!configOk) {
                return;
            }
            // Settings, commands and status are handled on the game thread.
            auto promise = std::make_shared<std::promise<std::string>>();
            auto future = promise->get_future();
            if (auto tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([promise, config]() {
                    try {
                        ApplyServerConfig(config);
                        promise->set_value(BuildStatusJson());
                    } catch (const std::exception& e) {
                        logger::error("Status failed: {}", e.what());
                        promise->set_value({});
                    }
                });
            } else {
                return;
            }
            if (future.wait_for(5s) != std::future_status::ready) {
                return;
            }
            const auto json = future.get();
            if (!json.empty() && HttpRequest(g_server.host, g_server.port, "POST", g_server.basePath + "/api/status.php", json, nullptr)) {
                MarkLinesSent(json);
            }
        }

        std::mutex _mutex;
        std::condition_variable _cv;
        std::atomic<bool> _running{false};
        std::atomic<bool> _wake{false};
    };
}

// ========================================================================================
// Save game (SKSE co-save): wounds belong to the save, so loading an older save restores them.
// ========================================================================================
namespace
{
    void WriteString(SKSE::SerializationInterface* intfc, const std::string& s)
    {
        const auto len = static_cast<std::uint16_t>(std::min<size_t>(s.size(), 200));
        intfc->WriteRecordData(len);
        if (len > 0) {
            intfc->WriteRecordData(s.data(), len);
        }
    }

    bool ReadString(SKSE::SerializationInterface* intfc, std::string& out)
    {
        std::uint16_t len = 0;
        if (intfc->ReadRecordData(len) != sizeof(len)) {
            return false;
        }
        out.assign(len, '\0');
        return len == 0 || intfc->ReadRecordData(out.data(), len) == len;
    }

    void OnSave(SKSE::SerializationInterface* intfc)
    {
        if (!intfc->OpenRecord(kRecordWounds, kRecordVersion)) {
            logger::error("Could not write wounds to the save");
            return;
        }
        std::uint32_t count = 0;
        for (auto& [id, entry] : g_wounds) {
            count += entry.wounds.empty() ? 0 : 1;
        }
        intfc->WriteRecordData(count);
        for (auto& [id, entry] : g_wounds) {
            if (entry.wounds.empty()) {
                continue;
            }
            intfc->WriteRecordData(id);
            const auto n = static_cast<std::uint8_t>(std::min<size_t>(entry.wounds.size(), 12));
            intfc->WriteRecordData(n);
            for (std::uint8_t k = 0; k < n; ++k) {
                const auto& w = entry.wounds[k];
                const std::uint8_t region = static_cast<std::uint8_t>(w.region);
                const std::uint8_t type = static_cast<std::uint8_t>(w.type);
                const std::uint8_t flags = (w.treated ? 1 : 0) | (w.arrow ? 2 : 0);
                intfc->WriteRecordData(region);
                intfc->WriteRecordData(type);
                intfc->WriteRecordData(w.severity);
                intfc->WriteRecordData(flags);
                intfc->WriteRecordData(w.created);
                intfc->WriteRecordData(w.due);
                WriteString(intfc, w.by);
                WriteString(intfc, w.with);
            }
        }
        logger::info("Saved wounds of {} actor(s)", count);
    }

    void OnRevert(SKSE::SerializationInterface*)
    {
        g_wounds.clear();
        g_pending.clear();
        g_healthSample.clear();
        g_lastWoundAt.clear();
        g_lastHealAt.clear();
        g_overlayState.clear();
        g_tracked.clear();
        g_sleepStartDays = -1.0f;
        g_stateDirty = true;
    }

    void OnLoad(SKSE::SerializationInterface* intfc)
    {
        std::uint32_t type = 0;
        std::uint32_t version = 0;
        std::uint32_t length = 0;
        while (intfc->GetNextRecordInfo(type, version, length)) {
            if (type != kRecordWounds || version != kRecordVersion) {
                logger::warn("Unknown save record {:08X} v{} skipped", type, version);
                continue;
            }
            std::uint32_t count = 0;
            if (intfc->ReadRecordData(count) != sizeof(count)) {
                return;
            }
            int loaded = 0;
            for (std::uint32_t a = 0; a < count; ++a) {
                RE::FormID oldId = 0;
                std::uint8_t n = 0;
                if (intfc->ReadRecordData(oldId) != sizeof(oldId) || intfc->ReadRecordData(n) != sizeof(n)) {
                    return;
                }
                std::vector<Wound> wounds;
                for (std::uint8_t k = 0; k < n; ++k) {
                    Wound w;
                    std::uint8_t region = 0, wtype = 0, flags = 0;
                    intfc->ReadRecordData(region);
                    intfc->ReadRecordData(wtype);
                    intfc->ReadRecordData(w.severity);
                    intfc->ReadRecordData(flags);
                    intfc->ReadRecordData(w.created);
                    intfc->ReadRecordData(w.due);
                    if (!ReadString(intfc, w.by) || !ReadString(intfc, w.with)) {
                        return;
                    }
                    if (region >= static_cast<std::uint8_t>(Region::kTotal) || wtype >= static_cast<std::uint8_t>(WType::kTotal)) {
                        continue;
                    }
                    w.region = static_cast<Region>(region);
                    w.type = static_cast<WType>(wtype);
                    w.severity = std::clamp<std::uint8_t>(w.severity, 1, 3);
                    w.treated = (flags & 1) != 0;
                    w.arrow = (flags & 2) != 0;
                    wounds.push_back(std::move(w));
                }
                RE::FormID newId = 0;
                if (!intfc->ResolveFormID(oldId, newId)) {
                    continue;
                }
                auto actor = LookupActor(newId);
                if (!actor || wounds.empty()) {
                    continue;
                }
                auto& entry = Entry(actor);
                entry.wounds = std::move(wounds);
                ++loaded;
            }
            logger::info("Loaded wounds of {} actor(s) from the save", loaded);
        }
    }
}

// ========================================================================================
// SKSE entry
// ========================================================================================
namespace
{
    void AnnounceActivation()
    {
        if (!g_settings.enabled) {
            Notify("disabled", true);
            return;
        }
        Notify(g_overlay.skeeLoaded ? "activated" : "activated (RaceMenu not found: wounds are not drawn on the body)", true);
        logger::info("Activated for this session (tracking player={}, followers={})", g_settings.trackPlayer, g_settings.trackFollowers);
        NetWorker::Get().Wake();
    }

    void OnGameReady()
    {
        g_gameReady = true;
        RefreshTracked();
        Scheduler::Get().After(3000ms, []() {
            AnnounceActivation();
            RefreshAllOverlays(true);
        });
    }

    void OnMessage(SKSE::MessagingInterface::Message* message)
    {
        switch (message->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            DetectOverlays();
            if (auto holder = RE::ScriptEventSourceHolder::GetSingleton()) {
                holder->AddEventSink<RE::TESHitEvent>(HitSink::Get());
                holder->AddEventSink<RE::TESMagicEffectApplyEvent>(MagicSink::Get());
                holder->AddEventSink<RE::TESEquipEvent>(EquipSink::Get());
                holder->AddEventSink<RE::TESSleepStartEvent>(SleepStartSink::Get());
                holder->AddEventSink<RE::TESSleepStopEvent>(SleepStopSink::Get());
                holder->AddEventSink<RE::TESDeathEvent>(DeathSink::Get());
            }
            Scheduler::Get().Start();
            NetWorker::Get().Start();
            if (!g_tickStarted.exchange(true)) {
                Scheduler::Get().After(1000ms, []() { Tick(); });
            }
            logger::info("Ready");
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            g_gameReady = false;
            break;
        case SKSE::MessagingInterface::kNewGame:
        case SKSE::MessagingInterface::kPostLoadGame:
            OnGameReady();
            break;
        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    SetupLog();
    LoadIniSettings();
    ApplyLogLevel();
    logger::info("CHIM Injury Framework {} loading (enabled={}, debug={})", kPluginVersion, g_settings.enabled, g_settings.debug);
    if (auto serialization = SKSE::GetSerializationInterface()) {
        serialization->SetUniqueID(kSerializationId);
        serialization->SetSaveCallback(OnSave);
        serialization->SetRevertCallback(OnRevert);
        serialization->SetLoadCallback(OnLoad);
    }
    if (auto messaging = SKSE::GetMessagingInterface()) {
        messaging->RegisterListener(OnMessage);
    }
    return true;
}
