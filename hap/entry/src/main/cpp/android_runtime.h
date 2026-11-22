/* SPDX-License-Identifier: MIT */
#pragma once

#include "sched_qos.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

class AndroidRuntime {
public:
    static AndroidRuntime& Instance();

    std::string Start(const std::string& sandboxTempDir,
                      const std::string& svboxPath,
                      const std::string& androidDir,
                      const std::string& mode,
                      int32_t width,
                      int32_t height);

    std::string Stop();

    void SetResourceConfig(const std::string& profile,
                           const std::string& customCpuset,
                           uint32_t memLimitMb);
    std::string SetPaused(bool paused);
    void SetVisible(bool visible);

    bool IsRunning() const { return running_.load(); }
    bool IsPaused() const { return paused_.load(); }
    std::string StatusText() const;
    std::string Profile() const;
    std::string ActiveCpuset() const;
    uint32_t MemLimitMb() const { return memLimitMb_.load(); }
    SchedQos::Telemetry GetTelemetry();

private:
    AndroidRuntime() = default;
    void BootWorker(std::string mode, int32_t width, int32_t height);
    void LmkdLoop(std::string sockPath);
    void LogdwLoop(std::string sockPath, std::string outPath);
    void DnsProxyLoop(int listenFd);
    bool SleepCancelableMs(int totalMs) const;
    bool WaitForConditionMs(int maxMs, const std::function<bool()>& pred) const;
    void SetStatus(const std::string& s);
    pid_t Spawn(const std::vector<std::string>& argv,
                bool nullStdio,
                const std::string& logPath = "",
                const std::vector<int>& keepFds = {});
    std::vector<std::string> BaseSvboxArgs() const;

    mutable std::mutex mu_;
    std::mutex procMu_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::string status_ = "Idle";
    std::string profile_ = "perf";
    std::string customCpuset_;
    std::string activeCpuset_;
    int32_t activeNice_ = 0x7fffffff;
    std::string activeSched_;
    std::atomic<uint32_t> memLimitMb_{4096};

    std::vector<pid_t> pids_;
    std::vector<int> ownedFds_;
    std::thread bootThread_;
    std::thread lmkdThread_;
    std::thread logdwThread_;
    std::thread dnsThread_;
    int dnsListenFd_ = -1;

    std::string runBase_;
    std::string svboxPath_;
    std::string audioHalPath_;
    std::string audioSvcPath_;
    std::string androidDir_;
    std::string rootfs_;
};
