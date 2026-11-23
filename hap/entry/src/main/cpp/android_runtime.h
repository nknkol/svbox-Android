/* SPDX-License-Identifier: MIT */
#pragma once

#include "sched_qos.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <native_window/external_window.h>
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
    void SetGpuMode(const std::string& gpuMode);
    std::string GpuMode() const;
    bool UseGfxstream() const;
    void OnSurfaceChanged(OHNativeWindow* window, int32_t w, int32_t h);
    void OnSurfaceDestroyed(OHNativeWindow* window = nullptr);
    std::string SetPaused(bool paused);
    void SetVisible(bool visible);
    bool RunGuestCmd(const std::string& shCmd, const std::string& outPath);
    bool DumpProcs(const std::string& outPath);

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
    std::string gpuMode_ = "gles";
    std::string activeGpuMode_ = "gles";
    std::string customCpuset_;
    std::string activeCpuset_;
    int32_t activeNice_ = 0x7fffffff;
    std::string activeSched_;
    std::atomic<uint32_t> memLimitMb_{4096};

    OHNativeWindow* surfaceWindow_ = nullptr;
    int32_t surfaceW_ = 0;
    int32_t surfaceH_ = 0;

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
    std::string cameraSvcPath_;
    std::string gpuGuestDir_;
    std::string androidDir_;
    std::string rootfs_;
};
