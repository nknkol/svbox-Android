/* SPDX-License-Identifier: MIT */
#include "ohos_bridge/sched_qos.h"
#include "ohos_bridge/log.h"

#include <algorithm>
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
        for (const auto& s : byDesc) {
            double ratio = s.score / maxScore;
            if (ratio >= 0.90) topo.bigCores.push_back(s.cpu);
            else if (ratio >= 0.65) topo.mediumCores.push_back(s.cpu);
            else topo.littleCores.push_back(s.cpu);
        }
        // Keep littleCores ascending (lowest-power first) and mediumCores descending (stronger mid first).
        std::reverse(topo.littleCores.begin(), topo.littleCores.end());
        if (topo.littleCores.empty() && !topo.mediumCores.empty()) {
            topo.littleCores = topo.mediumCores;
        }
        if (topo.mediumCores.empty()) {
            topo.mediumCores = !topo.bigCores.empty() ? topo.bigCores : allowed;
        }
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

    OB_LOG_INFO("SvboxQos", "Core topology probed: %s [L=%s M=%s B=%s]",
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
        OB_LOG_INFO("SvboxQos", "SetThreadQoS(%s) = %d OK",
                    label ? label : "?", static_cast<int>(level));
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
    OB_LOG_INFO("SvboxQos", "ResolveCpuset(profile=%s, target=%d, host=%dc) -> %s",
                p.c_str(), targetCores, total, result.c_str());
    return result;
}

Telemetry SampleTelemetry(const std::vector<pid_t>& rootPids) {
    Telemetry t;
    const auto& hd = GetHiDebugApi();
    if (hd.getSysCpu) t.systemCpuPercent = hd.getSysCpu() * 100.0;
    if (hd.getAppCpu) t.appCpuPercent = hd.getAppCpu() * 100.0;
    if (hd.getNativeMem) {
        HiDebugNativeMemInfoCompat nmem = {};
        hd.getNativeMem(&nmem, false);
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

} // namespace SchedQos
