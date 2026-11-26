/* SPDX-License-Identifier: MIT */
#pragma once

#include "ohos_bridge/gpu_bridge.h"
#include "ohos_bridge/sched_qos.h"

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
                           uint32_t memLimitMb,
                           uint32_t diskLimitGb = 0,
                           uint32_t cpuCores = 0);
    void SetInstanceConfig(const std::string& instanceId,
                           const std::string& dataSubdir,
                           int32_t customDpi,
                           bool enableAudio,
                           bool enableRoot,
                           const std::string& brand,
                           const std::string& model,
                           int32_t adbPort,
                           bool enableGms = false);
    std::string ActiveInstanceId() const;
    std::string DataSubdir() const;
    std::string ShareHostDir() const;
    void SetGpuMode(const std::string& gpuMode);
    std::string GpuMode() const;
    void SetSyscallMode(const std::string& syscallMode);
    std::string SyscallMode() const;
    bool UseGfxstream() const;
    void SetRefreshRate(int32_t hz);
    int32_t RefreshRate() const { return refreshRate_.load(); }
    int32_t DisplayWidth() const { return displayW_.load(); }
    int32_t DisplayHeight() const { return displayH_.load(); }
    void OnSurfaceChanged(OHNativeWindow* window, int32_t w, int32_t h);
    void OnSurfaceDestroyed(OHNativeWindow* window = nullptr);
    void OnDisplaySurfaceChanged(int32_t displaySlot, OHNativeWindow* window, int32_t w, int32_t h);
    void OnDisplaySurfaceDestroyed(int32_t displaySlot, OHNativeWindow* window = nullptr);
    void SetDisplayVisible(int32_t displaySlot, bool visible);
    std::string SetPaused(bool paused);
    void SetVisible(bool visible);
    bool RunGuestCmd(const std::string& shCmd, const std::string& outPath);
    std::string InstallApkSync(const std::string& guestApkPath);
    bool DumpProcs(const std::string& outPath);

    // Multi-window / WM Agent RPC & slot management
    bool IsWmAgentReady() const { return wmAgentReady_.load(); }
    std::string IconsHostDir() const;
    std::string WmRpc(const std::string& cmd, int timeoutMs = 15000);
    int32_t AllocDisplaySlotForPkg(const std::string& pkg);
    int32_t FindDisplaySlotForPkg(const std::string& pkg) const;
    void ReleaseDisplaySlot(int32_t slot);
    std::string PollWmEventsJson();
    SvboxGfxDisplayStats GetDisplayStats(int32_t displaySlot) const;

    bool IsRunning() const { return running_.load(); }
    bool IsPaused() const { return paused_.load(); }
    std::string StatusText() const;
    std::string Profile() const;
    std::string ActiveCpuset() const;
    uint32_t MemLimitMb() const { return memLimitMb_.load(); }
    uint32_t DiskLimitGb() const { return diskLimitGb_.load(); }
    bool IsGmsEnabled() const { return enableGms_.load(); }
    bool IsGmsAvailable() const;
    SchedQos::Telemetry GetTelemetry();

private:
    AndroidRuntime() = default;
    void FlushGuestStateBeforeStop();
    void BootWorker(std::string mode, int32_t width, int32_t height);
    void LmkdLoop(std::string sockPath);
    void LogdwLoop(std::string sockPath, std::string outPath);
    void DnsProxyLoop(int listenFd);
    void WmEventLoop(std::string sockPath);
    bool SleepCancelableMs(int totalMs) const;
    bool WaitForConditionMs(int maxMs, const std::function<bool()>& pred) const;
    void SetStatus(const std::string& s);
    pid_t Spawn(const std::vector<std::string>& argv,
                bool nullStdio,
                const std::string& logPath = "",
                const std::vector<int>& keepFds = {});
    std::vector<std::string> BaseSvboxArgs(bool includeStorageSubmounts = true) const;

    mutable std::mutex mu_;
    std::mutex procMu_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::string status_ = "Idle";
    std::string profile_ = "perf";
    std::string gpuMode_ = "vulkan";
    std::string activeGpuMode_ = "vulkan";
    std::string syscallMode_ = "adaptive";
    std::string activeSyscallMode_ = "adaptive";
    std::string activeInstanceId_ = "inst_0";
    std::string dataSubdir_ = "userdata14";
    std::string deviceBrand_;
    std::string deviceModel_;
    std::atomic<int32_t> customDpi_{0};
    std::atomic<bool> enableAudio_{true};
    std::atomic<bool> enableRoot_{true};
    std::atomic<bool> enableGms_{false};
    std::atomic<int32_t> adbPort_{5555};
    std::string customCpuset_;
    std::string activeCpuset_;
    int32_t activeNice_ = 0x7fffffff;
    std::string activeSched_;
    std::atomic<uint32_t> memLimitMb_{4096};
    std::atomic<uint32_t> targetCpuCores_{0};
    std::atomic<uint32_t> diskLimitGb_{64};
    std::atomic<uint32_t> diskLimitMb_{59100};
    std::atomic<uint32_t> rootDiskMb_{1900};
    std::atomic<uint32_t> diskUsedMb_{0};
    std::atomic<int32_t> refreshRate_{60};
    std::atomic<int32_t> displayW_{720};
    std::atomic<int32_t> displayH_{1280};

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

    std::string sandboxFiles_;
    std::string runBase_;
    std::string dataBase_;
    std::string rootUpper_;
    std::string injectLayer_;
    std::string gmsLayerDir_;
    std::string shareHostDir_;
    std::string svboxPath_;
    std::string netSockPath_;
    std::string procCmdline_;
    std::string audioHalPath_;
    std::string audioSvcPath_;
    std::string cameraSvcPath_;
    std::string sensorsSvcPath_;
    std::string gpuGuestDir_;
    std::string androidDir_;
    std::string rootfs_;

    std::atomic<bool> wmAgentReady_{false};
    std::string wmSockPath_;
    std::string iconsHostDir_;
    std::thread wmEventThread_;
    std::mutex wmEventMu_;
    std::vector<std::string> wmEventQueue_;
    mutable std::mutex slotMapMu_;
    std::string slotPkg_[9];
};
