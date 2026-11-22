/* SPDX-License-Identifier: MIT */
#pragma once

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

    bool IsRunning() const { return running_.load(); }
    std::string StatusText() const;

private:
    AndroidRuntime() = default;
    void BootWorker(std::string mode, int32_t width, int32_t height);
    void LmkdLoop(std::string sockPath);
    void LogdwLoop(std::string sockPath, std::string outPath);
    bool SleepCancelableMs(int totalMs) const;
    void SetStatus(const std::string& s);
    pid_t Spawn(const std::vector<std::string>& argv,
                bool nullStdio,
                const std::string& logPath = "",
                const std::vector<int>& keepFds = {});
    std::vector<std::string> BaseSvboxArgs() const;

    mutable std::mutex mu_;
    std::mutex procMu_;
    std::atomic<bool> running_{false};
    std::string status_ = "Idle";
    std::vector<pid_t> pids_;
    std::vector<int> ownedFds_;
    std::thread bootThread_;
    std::thread lmkdThread_;
    std::thread logdwThread_;

    std::string runBase_;
    std::string svboxPath_;
    std::string androidDir_;
    std::string rootfs_;
};
