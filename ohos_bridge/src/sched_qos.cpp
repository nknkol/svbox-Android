/* SPDX-License-Identifier: MIT */
#include "ohos_bridge/sched_qos.h"
#include "ohos_bridge/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <fstream>
#include <mutex>
#include <sched.h>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace SchedQos {

#ifdef OHOS_BRIDGE_HEADLESS
#define QOS_FMT_S "%s"
#define QOS_FMT_D "%d"
#else
#define QOS_FMT_S "%{public}s"
#define QOS_FMT_D "%{public}d"
#endif

namespace {

using SetThreadQoSFn = int (*)(int);

SetThreadQoSFn GetSetThreadQoSFn() {
    static SetThreadQoSFn fn = []() -> SetThreadQoSFn {
        void* h = dlopen("libqos.so", RTLD_NOW | RTLD_LOCAL);
        if (!h) {
            OB_LOG_WARN("SvboxQos", "dlopen(libqos.so) unavailable");
            return nullptr;
        }
        return reinterpret_cast<SetThreadQoSFn>(dlsym(h, "OH_QoS_SetThreadQoS"));
    }();
    return fn;
}

struct HiDebugNativeMemInfoCompat {
    uint32_t pss;
    uint32_t vss;
    uint32_t rss;
    uint32_t sharedDirty;
    uint32_t privateDirty;
    uint32_t sharedClean;
    uint32_t privateClean;
};

struct HiDebugApi {
    double (*getSysCpu)(void) = nullptr;
    double (*getAppCpu)(void) = nullptr;
    void (*getNativeMem)(HiDebugNativeMemInfoCompat*, bool) = nullptr;
    int (*getGfxMem)(uint32_t*) = nullptr;
};

const HiDebugApi& GetHiDebugApi() {
    static HiDebugApi api = []() {
        HiDebugApi a;
        void* h = dlopen("libohhidebug.so", RTLD_NOW | RTLD_LOCAL);
        if (h) {
            a.getSysCpu = reinterpret_cast<double (*)(void)>(dlsym(h, "OH_HiDebug_GetSystemCpuUsage"));
            a.getAppCpu = reinterpret_cast<double (*)(void)>(dlsym(h, "OH_HiDebug_GetAppCpuUsage"));
            a.getNativeMem = reinterpret_cast<void (*)(HiDebugNativeMemInfoCompat*, bool)>(
                dlsym(h, "OH_HiDebug_GetAppNativeMemInfoWithCache"));
            a.getGfxMem = reinterpret_cast<int (*)(uint32_t*)>(dlsym(h, "OH_HiDebug_GetGraphicsMemory"));
        }
        return a;
    }();
    return api;
}

std::mutex g_topoMu;
std::condition_variable g_topoCv;
CoreTopology g_topo;
bool g_probeStarted = false;
bool g_probeDone = false;

std::string FormatCpuList(const std::vector<int32_t>& cpus) {
    if (cpus.empty()) return "";
    std::vector<int32_t> sorted = cpus;
    std::sort(sorted.begin(), sorted.end());
    std::ostringstream oss;
    size_t i = 0;
    while (i < sorted.size()) {
        if (i > 0) oss << ",";
        int32_t start = sorted[i];
        int32_t end = start;
        while (i + 1 < sorted.size() && sorted[i + 1] == end + 1) {
            end = sorted[++i];
        }
        if (end > start) oss << start << "-" << end;
        else oss << start;
        ++i;
    }
    return oss.str();
}

double MeasureCoreThroughput(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        return 0.0;
    }
    volatile uint64_t x = 0x123456789abcdef0ull + static_cast<uint64_t>(cpu);
    for (int i = 0; i < 60000; ++i) {
        x = (x * 6364136223846793005ull) + 1442695040888963407ull;
    }
    auto t0 = std::chrono::steady_clock::now();
    constexpr int kIters = 250000;
    for (int i = 0; i < kIters; ++i) {
        x ^= (x << 13);
        x ^= (x >> 7);
        x ^= (x << 17);
        x += 0x9e3779b97f4a7c15ull;
    }
    auto t1 = std::chrono::steady_clock::now();
    double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    (void)x;
    return ns > 1.0 ? (1e9 / ns) : 0.0;
}

bool TryReadSysCapacity(int cpu, int& outCap) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", cpu);
    std::ifstream f(path);
    if (f) {
        int val = 0;
        if (f >> val && val > 0) {
            outCap = val;
            return true;
        }
    }
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
    std::ifstream ff(path);
    if (ff) {
        int khz = 0;
        if (ff >> khz && khz > 0) {
            outCap = khz;
            return true;
        }
    }
    return false;
}

std::vector<int32_t> QueryAllowedCpus() {
    std::vector<int32_t> allowed;
    cpu_set_t orig;
    CPU_ZERO(&orig);
    if (sched_getaffinity(0, sizeof(orig), &orig) == 0) {
        for (int c = 0; c < CPU_SETSIZE && c < 64; ++c) {
            if (CPU_ISSET(c, &orig)) allowed.push_back(c);
        }
    }
    if (allowed.empty()) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        if (n <= 0) n = 4;
        if (n > 64) n = 64;
        for (int c = 0; c < n; ++c) allowed.push_back(c);
    }
    return allowed;
}

void RunProbeWorker() {
    SetCurrentThreadQoS(Level::UserInteractive, "topo-probe");
    cpu_set_t orig;
    CPU_ZERO(&orig);
    bool hasOrig = (sched_getaffinity(0, sizeof(orig), &orig) == 0);
    std::vector<int32_t> allowed = QueryAllowedCpus();
    if (allowed.empty()) {
        std::lock_guard<std::mutex> lk(g_topoMu);
        g_probeDone = true;
        g_topoCv.notify_all();
        return;
    }

    struct CoreScore {
        int32_t cpu;
        double score;
    };
    std::vector<CoreScore> scores;
    scores.reserve(allowed.size());

    bool sysOk = true;
    for (int32_t c : allowed) {
        int cap = 0;
        if (TryReadSysCapacity(c, cap)) {
            scores.push_back({ c, static_cast<double>(cap) });
        } else {
            sysOk = false;
            break;
        }
    }
    if (!sysOk) {
        scores.clear();
        for (int32_t c : allowed) {
            scores.push_back({ c, MeasureCoreThroughput(c) });
        }
        if (hasOrig) {
            sched_setaffinity(0, sizeof(orig), &orig);
        }
    }

    double maxScore = 0.0, minScore = 1e18;
    for (const auto& s : scores) {
        if (s.score > maxScore) maxScore = s.score;
        if (s.score > 0.0 && s.score < minScore) minScore = s.score;
    }

    CoreTopology topo;
    topo.probed = true;
    topo.totalCores = static_cast<int32_t>(allowed.size());

    if (maxScore <= 0.0 || (maxScore - minScore) / maxScore < 0.08) {
        size_t n = allowed.size();
        for (size_t i = 0; i < n; ++i) {
            if (i < n / 4) topo.littleCores.push_back(allowed[i]);
            else if (i < (n * 3) / 5) topo.mediumCores.push_back(allowed[i]);
            else topo.bigCores.push_back(allowed[i]);
        }
    } else {
        // Sort scores descending so highest-capacity cores are picked first inside each tier.
        std::vector<CoreScore> byDesc = scores;
        std::sort(byDesc.begin(), byDesc.end(), [](const CoreScore& a, const CoreScore& b) {
            if (a.score != b.score) return a.score > b.score;
            return a.cpu > b.cpu;
        });

        // Cluster into distinct capacity tiers (within 5% tolerance) so 3-tier SoCs
        // like Kirin X90 (688 / 888 / 1024) cleanly separate Little / Medium / Big
        // even when little-core capacity exceeds 0.65 * maxScore.
        std::vector<double> tiers;
        for (const auto& s : byDesc) {
            if (tiers.empty() || (tiers.back() - s.score) / std::max(1.0, tiers.back()) >= 0.05) {
                tiers.push_back(s.score);
            }
        }

        if (sysOk && tiers.size() >= 3) {
            const double highCut = tiers.front() * 0.95;
            const double lowCut = tiers.back() * 1.05;
            for (const auto& s : byDesc) {
                if (s.score >= highCut) topo.bigCores.push_back(s.cpu);
                else if (s.score <= lowCut) topo.littleCores.push_back(s.cpu);
                else topo.mediumCores.push_back(s.cpu);
            }
        } else if (sysOk && tiers.size() == 2) {
            const double highCut = tiers.front() * 0.95;
            for (const auto& s : byDesc) {
                if (s.score >= highCut) topo.bigCores.push_back(s.cpu);
                else topo.littleCores.push_back(s.cpu);
            }
        } else {
            for (const auto& s : byDesc) {
                double ratio = s.score / maxScore;
                if (ratio >= 0.90) topo.bigCores.push_back(s.cpu);
                else if (ratio >= 0.65) topo.mediumCores.push_back(s.cpu);
                else topo.littleCores.push_back(s.cpu);
            }
        }
        // Keep littleCores ascending (lowest-power first) and mediumCores descending (stronger mid first).
        std::reverse(topo.littleCores.begin(), topo.littleCores.end());
        if (topo.bigCores.empty()) {
            topo.bigCores = allowed;
        }
    }

    std::ostringstream sum;
    sum << topo.totalCores << "c ("
        << (sysOk ? "sys" : "alu") << ": "
        << "L=" << topo.littleCores.size()
        << " M=" << topo.mediumCores.size()
        << " B=" << topo.bigCores.size() << ")";
    topo.summary = sum.str();

    OB_LOG_INFO("SvboxQos", "Core topology probed: " QOS_FMT_S " [L=" QOS_FMT_S " M=" QOS_FMT_S " B=" QOS_FMT_S "]",
                topo.summary.c_str(),
                FormatCpuList(topo.littleCores).c_str(),
                FormatCpuList(topo.mediumCores).c_str(),
                FormatCpuList(topo.bigCores).c_str());
    {
        std::lock_guard<std::mutex> lk(g_topoMu);
        g_topo = std::move(topo);
        g_probeDone = true;
    }
    g_topoCv.notify_all();
}

uint64_t ReadProcPssKb(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/smaps_rollup", static_cast<int>(pid));
    std::ifstream rf(path);
    if (rf) {
        std::string line;
        while (std::getline(rf, line)) {
            if (line.rfind("Pss:", 0) == 0) {
                uint64_t kb = 0;
                if (sscanf(line.c_str() + 4, "%llu", reinterpret_cast<unsigned long long*>(&kb)) == 1) {
                    return kb;
                }
            }
        }
    }
    snprintf(path, sizeof(path), "/proc/%d/statm", static_cast<int>(pid));
    std::ifstream f(path);
    if (!f) return 0;
    uint64_t sizePages = 0, rssPages = 0;
    if (!(f >> sizePages >> rssPages)) return 0;
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) pg = 4096;
    return (rssPages * static_cast<uint64_t>(pg)) / 1024ull;
}

} // namespace

bool SetCurrentThreadQoS(Level level, const char* label) {
    auto fn = GetSetThreadQoSFn();
    if (!fn) return false;
    int rc = fn(static_cast<int>(level));
    if (rc == 0) {
        if (level != Level::Background) {
            OB_LOG_INFO("SvboxQos", "SetThreadQoS(" QOS_FMT_S ") = " QOS_FMT_D " OK",
                        label ? label : "?", static_cast<int>(level));
        }
        return true;
    }
    return false;
}

void StartCoreTopologyProbeAsync() {
    std::lock_guard<std::mutex> lk(g_topoMu);
    if (g_probeStarted) return;
    g_probeStarted = true;
    std::thread(RunProbeWorker).detach();
}

void EnsureCoreTopologyProbed(int maxWaitMs) {
    StartCoreTopologyProbeAsync();
    std::unique_lock<std::mutex> lk(g_topoMu);
    if (g_probeDone) return;
    g_topoCv.wait_for(lk, std::chrono::milliseconds(maxWaitMs > 0 ? maxWaitMs : 600),
                      []() { return g_probeDone; });
}

CoreTopology GetCoreTopology() {
    std::lock_guard<std::mutex> lk(g_topoMu);
    return g_topo;
}

std::string ResolveCpuset(const std::string& profile,
                          const std::string& customCpuset,
                          int32_t targetCores,
                          int32_t* outNice,
                          std::string* outSched) {
    if (outNice) *outNice = 0x7fffffff;
    if (outSched) outSched->clear();
    if (!customCpuset.empty()) {
        return customCpuset;
    }

    EnsureCoreTopologyProbed(600);

    std::string p;
    p.reserve(profile.size());
    for (unsigned char ch : profile) {
        p.push_back(static_cast<char>(ch >= 'A' && ch <= 'Z' ? (ch - 'A' + 'a') : ch));
    }

    CoreTopology topo = GetCoreTopology();
    std::vector<int32_t> allowed = QueryAllowedCpus();
    int32_t total = topo.probed && topo.totalCores > 0
                        ? topo.totalCores
                        : static_cast<int32_t>(allowed.size());
    if (total <= 0) total = 4;

    // Always reserve headroom for host UI/GPU compositor on multi-core devices.
    int32_t hostReserve = (total >= 12) ? 2 : ((total >= 6) ? 1 : 0);
    int32_t maxGuest = std::max<int32_t>(1, total - hostReserve);

    int32_t want = targetCores;
    if (want <= 0) {
        if (p == "eco") {
            want = std::min<int32_t>(maxGuest, std::max<int32_t>(2, total / 4));
        } else if (p == "balanced") {
            want = std::min<int32_t>(maxGuest, (total >= 16) ? 6 : std::max<int32_t>(2, total / 2));
        } else {
            want = std::min<int32_t>(maxGuest, (total >= 16) ? 8 : ((total >= 10) ? 6 : maxGuest));
        }
    }
    want = std::clamp<int32_t>(want, 1, maxGuest);

    if (p == "eco") {
        if (outNice) *outNice = 10;
        if (outSched) *outSched = "batch";
    }

    std::vector<int32_t> ranked;
    ranked.reserve(static_cast<size_t>(total));
    auto appendUnique = [&](const std::vector<int32_t>& src) {
        for (int32_t c : src) {
            if (std::find(ranked.begin(), ranked.end(), c) == ranked.end()) {
                ranked.push_back(c);
            }
        }
    };

    if (topo.probed && (!topo.bigCores.empty() || !topo.mediumCores.empty() || !topo.littleCores.empty())) {
        if (p == "eco") {
            appendUnique(topo.littleCores);
            appendUnique(topo.mediumCores);
            appendUnique(topo.bigCores);
        } else if (p == "balanced") {
            appendUnique(topo.mediumCores);
            appendUnique(topo.bigCores);
            appendUnique(topo.littleCores);
        } else {
            appendUnique(topo.bigCores);
            appendUnique(topo.mediumCores);
            appendUnique(topo.littleCores);
        }
    } else {
        // Dynamic fallback based on real allowed CPUs (high-index cores are typically big cores on ARM64).
        if (p == "eco") {
            appendUnique(allowed);
        } else {
            std::vector<int32_t> rev(allowed.rbegin(), allowed.rend());
            appendUnique(rev);
        }
    }
    appendUnique(allowed);

    if (static_cast<int32_t>(ranked.size()) > want) {
        ranked.resize(static_cast<size_t>(want));
    }
    std::string result = FormatCpuList(ranked);
    OB_LOG_INFO("SvboxQos", "ResolveCpuset(profile=" QOS_FMT_S ", target=" QOS_FMT_D ", host=" QOS_FMT_D "c) -> " QOS_FMT_S,
                p.c_str(), targetCores, total, result.c_str());
    return result;
}

namespace {

bool ParseCpuListToMask(const std::string& spec, cpu_set_t& outMask) {
    CPU_ZERO(&outMask);
    bool any = false;
    size_t i = 0;
    while (i < spec.size()) {
        while (i < spec.size() && (spec[i] == ',' || spec[i] == ' ')) ++i;
        if (i >= spec.size()) break;
        int a = 0;
        if (spec[i] < '0' || spec[i] > '9') return false;
        while (i < spec.size() && spec[i] >= '0' && spec[i] <= '9') {
            a = a * 10 + (spec[i++] - '0');
        }
        int b = a;
        if (i < spec.size() && spec[i] == '-') {
            ++i;
            b = 0;
            if (i >= spec.size() || spec[i] < '0' || spec[i] > '9') return false;
            while (i < spec.size() && spec[i] >= '0' && spec[i] <= '9') {
                b = b * 10 + (spec[i++] - '0');
            }
        }
        if (a > b) std::swap(a, b);
        for (int c = a; c <= b && c < CPU_SETSIZE && c < 64; ++c) {
            if (c >= 0) {
                CPU_SET(c, &outMask);
                any = true;
            }
        }
    }
    return any;
}

bool HasAnyCpuOverlap(const cpu_set_t& a, const cpu_set_t& b) {
    for (int c = 0; c < CPU_SETSIZE && c < 64; ++c) {
        if (CPU_ISSET(c, &a) && CPU_ISSET(c, &b)) return true;
    }
    return false;
}

bool IsHostBackgroundThreadComm(const char* comm) {
    if (!comm || !comm[0]) return false;
    return std::strncmp(comm, "telemetry", 9) == 0 ||
           std::strncmp(comm, "lmkd", 4) == 0 ||
           std::strncmp(comm, "logdw", 5) == 0 ||
           std::strncmp(comm, "dns-proxy", 9) == 0 ||
           std::strncmp(comm, "topo-probe", 10) == 0;
}

struct FastPerfHintApi {
    int (*builderCreate)(void**) = nullptr;
    void (*builderDestroy)(void*) = nullptr;
    int (*setSceneType)(void*, int) = nullptr;
    int (*setSceneState)(void*, int) = nullptr;
    int (*setDurationType)(void*, int) = nullptr;
    int (*build)(void*, void**) = nullptr;
    void (*configDestroy)(void*) = nullptr;
    int (*perfHint)(const void*) = nullptr;
    bool loaded = false;
};

const FastPerfHintApi& GetFastPerfHintApi() {
    static FastPerfHintApi api = []() {
        FastPerfHintApi a;
        void* h = dlopen("libscheduling_optimization.z.so", RTLD_NOW | RTLD_LOCAL);
        if (!h) return a;
        a.builderCreate = reinterpret_cast<int (*)(void**)>(dlsym(h, "HMS_FAST_PerfHintConfigBuilder_Create"));
        a.builderDestroy = reinterpret_cast<void (*)(void*)>(dlsym(h, "HMS_FAST_PerfHintConfigBuilder_Destroy"));
        a.setSceneType = reinterpret_cast<int (*)(void*, int)>(dlsym(h, "HMS_FAST_PerfHintConfigBuilder_SetSceneType"));
        a.setSceneState = reinterpret_cast<int (*)(void*, int)>(dlsym(h, "HMS_FAST_PerfHintConfigBuilder_SetSceneState"));
        a.setDurationType = reinterpret_cast<int (*)(void*, int)>(dlsym(h, "HMS_FAST_PerfHintConfigBuilder_SetDurationType"));
        a.build = reinterpret_cast<int (*)(void*, void**)>(dlsym(h, "HMS_FAST_PerfHintConfigBuilder_Build"));
        a.configDestroy = reinterpret_cast<void (*)(void*)>(dlsym(h, "HMS_FAST_PerfHintConfig_Destroy"));
        a.perfHint = reinterpret_cast<int (*)(const void*)>(dlsym(h, "HMS_FAST_SchedulingOptimization_PerfHint"));
        a.loaded = (a.builderCreate && a.builderDestroy && a.setSceneType && a.setSceneState &&
                    a.setDurationType && a.build && a.configDestroy && a.perfHint);
        return a;
    }();
    return api;
}

bool EmitFastPerfHint(bool begin) {
    constexpr int kFastPageDrawing = 6;
    constexpr int kFastEnd = 0;
    constexpr int kFastBegin = 1;
    constexpr int kFastShort = 1;

    const auto& api = GetFastPerfHintApi();
    if (!api.loaded) return false;
    void* builder = nullptr;
    if (api.builderCreate(&builder) != 0 || !builder) return false;
    void* config = nullptr;
    const int state = begin ? kFastBegin : kFastEnd;
    bool ok = (api.setSceneType(builder, kFastPageDrawing) == 0) &&
              (api.setSceneState(builder, state) == 0) &&
              (!begin || api.setDurationType(builder, kFastShort) == 0) &&
              // Never pass SetTids so HMS_FAST_END cannot clear permanent QOS_USER_INTERACTIVE.
              (api.build(builder, &config) == 0) &&
              (config != nullptr);
    int rc = ok ? api.perfHint(config) : -1;
    if (config) api.configDestroy(config);
    api.builderDestroy(builder);
    return rc == 0;
}

std::atomic<bool> g_fastSceneActive{false};
std::atomic<int64_t> g_lastFastBeginMs{0};
std::atomic<bool> g_wasInBgCgroup{false};
std::atomic<int64_t> g_lastAffinitySweepMs{0};
std::atomic<bool> g_affinitySweepBusy{false};

int64_t SteadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

void NotifyForegroundBurst(bool active) {
    if (active) {
        const int64_t now = SteadyNowMs();
        const int64_t prev = g_lastFastBeginMs.load(std::memory_order_relaxed);
        if (g_fastSceneActive.load(std::memory_order_relaxed) && (now - prev) < 3200) {
            return;
        }
        if ((now - prev) < 3200) {
            return;
        }
        g_lastFastBeginMs.store(now, std::memory_order_relaxed);
        if (EmitFastPerfHint(true)) {
            g_fastSceneActive.store(true, std::memory_order_release);
        }
    } else {
        if (g_fastSceneActive.exchange(false, std::memory_order_acq_rel)) {
            EmitFastPerfHint(false);
        }
    }
}

int ReassertFastAffinity(const std::string& profile,
                         const std::string& customCpuset,
                         bool forceSweep,
                         bool* outWokeFromBg) {
    if (outWokeFromBg) *outWokeFromBg = false;
    std::string p;
    p.reserve(profile.size());
    for (unsigned char ch : profile) {
        p.push_back(static_cast<char>(ch >= 'A' && ch <= 'Z' ? (ch - 'A' + 'a') : ch));
    }
    if (p == "eco" && customCpuset.empty()) {
        return 0;
    }

    CoreTopology topo = GetCoreTopology();
    if (!topo.probed) {
        StartCoreTopologyProbeAsync();
        return 0;
    }

    cpu_set_t fastMask, bigMask, littleMask;
    CPU_ZERO(&fastMask);
    CPU_ZERO(&bigMask);
    CPU_ZERO(&littleMask);
    std::vector<int32_t> fastCpus;

    if (!customCpuset.empty()) {
        if (!ParseCpuListToMask(customCpuset, fastMask)) return 0;
        bigMask = fastMask;
        for (int c = 0; c < topo.totalCores && c < 64; ++c) {
            if (!CPU_ISSET(c, &fastMask)) CPU_SET(c, &littleMask);
            else fastCpus.push_back(c);
        }
    } else {
        if (topo.littleCores.empty() || topo.bigCores.empty()) {
            return 0; // homogeneous or tiny topology
        }
        for (int32_t c : topo.littleCores) {
            if (c >= 0 && c < 64) CPU_SET(c, &littleMask);
        }
        for (int32_t c : topo.bigCores) {
            if (c >= 0 && c < 64) {
                CPU_SET(c, &bigMask);
                CPU_SET(c, &fastMask);
                fastCpus.push_back(c);
            }
        }
        if (!topo.mediumCores.empty()) {
            // Include medium cores alongside big cores: on HarmonyOS Kirin kernels,
            // requesting big-only triggers silent kernel fallback, whereas big + mid
            // is honored verbatim while strictly excluding the 5-10x slower little cores.
            size_t takeMid = (p == "balanced")
                                 ? topo.mediumCores.size()
                                 : std::max<size_t>(std::min<size_t>(2, topo.mediumCores.size()),
                                                    topo.mediumCores.size() / 2);
            for (size_t i = 0; i < takeMid && i < topo.mediumCores.size(); ++i) {
                int32_t c = topo.mediumCores[i];
                if (c >= 0 && c < 64 && !CPU_ISSET(c, &fastMask)) {
                    CPU_SET(c, &fastMask);
                    fastCpus.push_back(c);
                }
            }
        }
    }
    if (fastCpus.empty()) return 0;
    for (int32_t c : fastCpus) {
        if (c >= 0 && c < 64) CPU_CLR(c, &littleMask);
    }
    if (!HasAnyCpuOverlap(littleMask, littleMask)) return 0;

    cpu_set_t curSelf;
    CPU_ZERO(&curSelf);
    if (sched_getaffinity(getpid(), sizeof(curSelf), &curSelf) != 0) {
        return 0;
    }
    // If HarmonyOS currently has us in /key-background (big cores stripped),
    // remember that we entered background and do not fight system throttling.
    if (!HasAnyCpuOverlap(curSelf, bigMask)) {
        g_wasInBgCgroup.store(true, std::memory_order_relaxed);
        return 0;
    }

    const bool wokeFromBg = g_wasInBgCgroup.exchange(false, std::memory_order_acq_rel);
    if (wokeFromBg && outWokeFromBg) {
        *outWokeFromBg = true;
    }
    const bool selfHasLittle = HasAnyCpuOverlap(curSelf, littleMask);
    const int64_t nowMs = SteadyNowMs();
    const int64_t lastMs = g_lastAffinitySweepMs.load(std::memory_order_relaxed);
    if (!forceSweep && !wokeFromBg && !selfHasLittle && (nowMs - lastMs) < 2000) {
        return 0;
    }

    bool expected = false;
    if (!g_affinitySweepBusy.compare_exchange_strong(expected, true)) {
        return 0;
    }
    g_lastAffinitySweepMs.store(nowMs, std::memory_order_relaxed);

    auto maybePinTid = [&](pid_t tid) -> bool {
        cpu_set_t cur;
        CPU_ZERO(&cur);
        if (sched_getaffinity(tid, sizeof(cur), &cur) != 0) return false;
        if (!HasAnyCpuOverlap(cur, bigMask)) return false;
        if (!HasAnyCpuOverlap(cur, littleMask)) return false;
        return sched_setaffinity(tid, sizeof(fastMask), &fastMask) == 0;
    };

    int repinned = 0;
    // 1. Host process threads (UI, RenderThread, gfx-epoll, gfx-present, audio/input)
    if (DIR* td = opendir("/proc/self/task")) {
        struct dirent* tent = nullptr;
        while ((tent = readdir(td)) != nullptr) {
            if (tent->d_name[0] < '1' || tent->d_name[0] > '9') continue;
            pid_t tid = static_cast<pid_t>(std::atoi(tent->d_name));
            if (tid <= 0) continue;
            char commPath[64];
            snprintf(commPath, sizeof(commPath), "/proc/self/task/%d/comm", static_cast<int>(tid));
            char comm[32] = {};
            if (FILE* f = fopen(commPath, "r")) {
                if (fgets(comm, sizeof(comm), f)) {
                    size_t n = std::strlen(comm);
                    while (n > 0 && (comm[n - 1] == '\n' || comm[n - 1] == '\r')) comm[--n] = '\0';
                }
                fclose(f);
            }
            if (IsHostBackgroundThreadComm(comm)) continue;
            if (maybePinTid(tid)) ++repinned;
        }
        closedir(td);
    }

    // 2. Same-UID child container processes (svbox / surfaceflinger / hwcomposer / games)
    const uid_t myUid = getuid();
    const pid_t myPid = getpid();
    if (DIR* pd = opendir("/proc")) {
        struct dirent* pent = nullptr;
        while ((pent = readdir(pd)) != nullptr) {
            if (pent->d_name[0] < '1' || pent->d_name[0] > '9') continue;
            pid_t pid = static_cast<pid_t>(std::atoi(pent->d_name));
            if (pid <= 0 || pid == myPid) continue;
            char ppath[64];
            snprintf(ppath, sizeof(ppath), "/proc/%d", static_cast<int>(pid));
            struct stat st = {};
            if (stat(ppath, &st) != 0 || st.st_uid != myUid) continue;
            char tdirPath[80];
            snprintf(tdirPath, sizeof(tdirPath), "/proc/%d/task", static_cast<int>(pid));
            if (DIR* td = opendir(tdirPath)) {
                struct dirent* tent = nullptr;
                while ((tent = readdir(td)) != nullptr) {
                    if (tent->d_name[0] < '1' || tent->d_name[0] > '9') continue;
                    pid_t tid = static_cast<pid_t>(std::atoi(tent->d_name));
                    if (tid > 0 && maybePinTid(tid)) ++repinned;
                }
                closedir(td);
            }
        }
        closedir(pd);
    }

    g_affinitySweepBusy.store(false, std::memory_order_release);

    if (repinned > 0 || wokeFromBg) {
        OB_LOG_INFO("SvboxQos",
                    "ReassertFastAffinity: repinned " QOS_FMT_D " threads -> " QOS_FMT_S
                    " (wokeFromBg=" QOS_FMT_D ", force=" QOS_FMT_D ")",
                    repinned, FormatCpuList(fastCpus).c_str(),
                    wokeFromBg ? 1 : 0, forceSweep ? 1 : 0);
        if (wokeFromBg || forceSweep) {
            NotifyForegroundBurst(true);
        }
    }
    return repinned;
}

static Telemetry ComputeTelemetryNow(const std::vector<pid_t>& rootPids) {
    Telemetry t;
    const auto& hd = GetHiDebugApi();
    if (hd.getSysCpu) t.systemCpuPercent = hd.getSysCpu() * 100.0;
    if (hd.getAppCpu) t.appCpuPercent = hd.getAppCpu() * 100.0;
    if (hd.getNativeMem) {
        HiDebugNativeMemInfoCompat nmem = {};
        // Use HarmonyOS cached native memory info so polling never walks our
        // 2+ GB host Vulkan address space and blocks mmap_lock.
        hd.getNativeMem(&nmem, true);
        uint32_t hostKb = nmem.pss > 0 ? nmem.pss : nmem.rss;
        t.hostNativeRssMb = static_cast<uint32_t>(hostKb / 1024u);
    }
    if (hd.getGfxMem) {
        uint32_t gfxKb = 0;
        if (hd.getGfxMem(&gfxKb) == 0) {
            t.graphicsMemMb = gfxKb / 1024u;
        }
    }

    uid_t myUid = getuid();
    pid_t myPid = getpid();
    uint64_t totalKb = 0;
    uint32_t count = 0;
    DIR* d = opendir("/proc");
    if (d) {
        struct dirent* ent = nullptr;
        int scanned = 0;
        while ((ent = readdir(d)) != nullptr) {
            if (ent->d_name[0] < '1' || ent->d_name[0] > '9') continue;
            pid_t p = static_cast<pid_t>(std::atoi(ent->d_name));
            if (p <= 0 || p == myPid) continue;
            char ppath[64];
            snprintf(ppath, sizeof(ppath), "/proc/%d", static_cast<int>(p));
            struct stat st = {};
            if (stat(ppath, &st) == 0 && st.st_uid == myUid) {
                uint64_t kb = ReadProcPssKb(p);
                if (kb > 0) {
                    totalKb += kb;
                    count++;
                }
                // Yield gently so reading smaps_rollup never creates kernel mmap_lock bursts
                if ((++scanned & 3) == 0) {
                    usleep(1000);
                }
            }
        }
        closedir(d);
    } else {
        for (pid_t p : rootPids) {
            if (p > 0) {
                totalKb += ReadProcPssKb(p);
                count++;
            }
        }
    }
    t.containerRssMb = static_cast<uint32_t>(totalKb / 1024ull);
    t.activeProcs = count;
    return t;
}

Telemetry SampleTelemetry(const std::vector<pid_t>& rootPids, bool sync, bool allowRefresh) {
    static std::mutex g_telMu;
    static Telemetry g_cachedTel;
    static int64_t g_lastSampleMs = 0;
    static std::atomic<bool> g_sampling{false};

    if (!sync && !allowRefresh) {
        std::lock_guard<std::mutex> lk(g_telMu);
        return g_cachedTel;
    }

    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count();

    if (sync) {
        Telemetry fresh = ComputeTelemetryNow(rootPids);
        std::lock_guard<std::mutex> lk(g_telMu);
        g_cachedTel = fresh;
        g_lastSampleMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
        return fresh;
    }

    Telemetry snap;
    bool needRefresh = false;
    {
        std::lock_guard<std::mutex> lk(g_telMu);
        snap = g_cachedTel;
        needRefresh = (g_lastSampleMs == 0 || (nowMs - g_lastSampleMs) >= 10000);
    }
    if (needRefresh) {
        bool expected = false;
        if (g_sampling.compare_exchange_strong(expected, true)) {
            std::vector<pid_t> pidsCopy = rootPids;
            std::thread([pidsCopy]() {
                SetCurrentThreadQoS(Level::Background, "telemetry");
                Telemetry fresh = ComputeTelemetryNow(pidsCopy);
                {
                    std::lock_guard<std::mutex> lk(g_telMu);
                    g_cachedTel = fresh;
                    g_lastSampleMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count();
                }
                g_sampling.store(false);
            }).detach();
        }
    }
    return snap;
}

} // namespace SchedQos
