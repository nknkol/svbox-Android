/* SPDX-License-Identifier: MIT */
/* Generic HarmonyOS Linux container orchestrator built on top of svbox +
 * svbox-net + ohos_bridge (InputBridge, AudioBridge, UsbBridge, SurfaceBridge,
 * SchedQos). Zero guest-OS assumptions: hosts Alpine, Debian, Ubuntu, or
 * specialized stacks identically. */
#pragma once

#include <cstdint>
#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace OhosBridge {

struct VdevMount {
    std::string guestPath;
    std::string hostSock;
    std::string type; // "chr", "tun", "binder"
};

struct ContainerSpec {
    std::string svboxBin;
    std::string svboxNetBin;
    std::string workDir;        // must allow AF_UNIX sockets (e.g. cache/temp)
    std::string upperDir;       // writable union upper dir
    std::vector<std::string> lowerDirs; // read-only rootfs layers (top to bottom)

    std::string hostname = "svbox-linux";
    std::string kernelRelease = "6.1.0-svbox";
    std::string uidGid = "0:0";
    std::string cpuset;
    uint32_t memMb = 2048;
    uint32_t diskMb = 16384;

    // Virtual network (svbox-net)
    bool enableNet = true;
    std::string ethSpec = "eth0:10.0.2.15/24:10.0.2.2:02:00:00:00:00:01";
    std::vector<std::string> dnsServers = { "223.5.5.5", "8.8.8.8" };

    // Bridge services
    bool enableInput = true;
    bool enableAudio = true;
    bool enableUsb = true;
    bool enableFramebuffer = false;
    int32_t displayW = 1280;
    int32_t displayH = 720;
    bool mapEscToBack = false;

    // Extra binds ("host:guest"), vdevs, and env ("KEY=VAL")
    std::vector<std::pair<std::string, std::string>> binds;
    std::vector<VdevMount> extraVdevs;
    std::vector<std::string> env;
    std::string sharedHostDir;
    std::string sharedGuestDir = "/mnt/share";

    // Guest command line (e.g. {"/bin/sh", "-c", "..."})
    std::vector<std::string> guestArgv;
};

class ContainerRunner {
public:
    ContainerRunner() = default;
    ~ContainerRunner();

    ContainerRunner(const ContainerRunner&) = delete;
    ContainerRunner& operator=(const ContainerRunner&) = delete;

    // Prepares staging dirs, starts ohos_bridge services + svbox-net, and launches svbox.
    bool Start(const ContainerSpec& spec, std::string* errOut = nullptr);
    // Waits for the guest process to exit and returns its exit code (or -1 on error).
    int Wait();
    // Stops guest process, svbox-net, and all started ohos_bridge services cleanly.
    void Stop();

    pid_t GuestPid() const { return guestPid_; }
    pid_t NetPid() const { return netPid_; }
    std::string SocketsDir() const { return sockDir_; }
    std::string IpcDir() const { return ipcDir_; }

private:
    bool started_ = false;
    ContainerSpec spec_;
    std::string sockDir_;
    std::string ipcDir_;
    std::string devInputDir_;
    std::string devBusUsbDir_;
    std::string resolvPath_;
    std::string tmpfsDir_;
    pid_t netPid_ = -1;
    pid_t guestPid_ = -1;
};

} // namespace OhosBridge
