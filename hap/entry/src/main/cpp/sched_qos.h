/* SPDX-License-Identifier: MIT */
/* HarmonyOS PC thread QoS (libqos.so), sandbox-safe Big/Medium/Little CPU
 * cluster discovery via ~2ms/core ALU micro-probe (when /sys cpu_capacity is
 * blocked by HAP sandbox), and HiDebug + /proc container telemetry. */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SchedQos {

enum class Level : int32_t {
    Background = 0,
    Utility = 1,
    Default = 2,
    UserInitiated = 3,
    DeadlineRequest = 4,
    UserInteractive = 5,
};

struct CoreTopology {
    bool probed = false;
    int32_t totalCores = 0;
    std::vector<int32_t> littleCores;
    std::vector<int32_t> mediumCores;
    std::vector<int32_t> bigCores;
    std::string summary;
};

struct Telemetry {
    double systemCpuPercent = 0.0;
    double appCpuPercent = 0.0;
    uint32_t containerRssMb = 0;
    uint32_t hostNativeRssMb = 0;
    uint32_t graphicsMemMb = 0;
    uint32_t activeProcs = 0;
};

bool SetCurrentThreadQoS(Level level, const char* label);
void StartCoreTopologyProbeAsync();
CoreTopology GetCoreTopology();

// Converts a profile ("eco"|"balanced"|"perf"|"all") + optional custom cpuset
// into a concrete Linux cpuset string (e.g. "0-3", "4-9", "12-19") and nice/sched.
std::string ResolveCpuset(const std::string& profile,
                          const std::string& customCpuset,
                          int32_t* outNice,
                          std::string* outSched);

// Samples HiDebug CPU/graphics metrics + container process tree RSS from /proc.
Telemetry SampleTelemetry(const std::vector<pid_t>& rootPids);

} // namespace SchedQos
