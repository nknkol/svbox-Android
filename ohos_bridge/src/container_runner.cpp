/* SPDX-License-Identifier: MIT */
#include "ohos_bridge/container_runner.h"
#include "ohos_bridge/audio_bridge.h"
#include "ohos_bridge/input_bridge.h"
#include "ohos_bridge/log.h"
#include "ohos_bridge/surface_bridge.h"
#include "ohos_bridge/usb_bridge.h"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <fstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace OhosBridge {

namespace {

bool MkdirP(const std::string& path, mode_t mode = 0755) {
    if (path.empty()) return false;
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur.push_back(path[i]);
        if (path[i] == '/' && i > 0) {
            mkdir(cur.c_str(), mode);
        }
    }
    return mkdir(path.c_str(), mode) == 0 || errno == EEXIST;
}

void TouchFile(const std::string& path) {
    int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0666);
    if (fd >= 0) close(fd);
}

bool WaitForFile(const std::string& path, int timeoutMs) {
    int waited = 0;
    while (waited < timeoutMs) {
        struct stat st = {};
        if (stat(path.c_str(), &st) == 0) return true;
        usleep(20000);
        waited += 20;
    }
    return false;
}

pid_t SpawnProcess(const std::vector<std::string>& args) {
    if (args.empty()) return -1;
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid == 0) {
        execv(argv[0], argv.data());
        _exit(127);
    }
    return pid;
}

} // namespace

ContainerRunner::~ContainerRunner() {
    Stop();
}

bool ContainerRunner::Start(const ContainerSpec& spec, std::string* errOut) {
    Stop();
    spec_ = spec;

    if (spec_.svboxBin.empty() || spec_.workDir.empty() || spec_.guestArgv.empty()) {
        if (errOut) *errOut = "ContainerSpec missing svboxBin, workDir, or guestArgv";
        return false;
    }
    if (spec_.upperDir.empty()) {
        spec_.upperDir = spec_.workDir + "/root_upper";
    }

    sockDir_ = spec_.workDir + "/sockets";
    ipcDir_ = spec_.workDir + "/ipc";
    devInputDir_ = spec_.workDir + "/dev_input";
    devBusUsbDir_ = spec_.workDir + "/dev_bus_usb";
    tmpfsDir_ = spec_.workDir + "/tmpfs";
    resolvPath_ = spec_.workDir + "/resolv.conf";

    MkdirP(spec_.workDir);
    MkdirP(spec_.upperDir);
    MkdirP(sockDir_);
    MkdirP(ipcDir_);
    MkdirP(devInputDir_);
    MkdirP(devBusUsbDir_ + "/001");
    MkdirP(tmpfsDir_);

    {
        std::ofstream rf(resolvPath_, std::ios::trunc);
        for (const auto& dns : spec_.dnsServers) {
            rf << "nameserver " << dns << "\n";
        }
    }

    std::string netSock = sockDir_ + "/net.sock";
    std::string touchSock = sockDir_ + "/input_touch.sock";
    std::string kbdSock = sockDir_ + "/input_kbd.sock";
    std::string gamepadSock = sockDir_ + "/input_gamepad.sock";
    std::string audioSock = ipcDir_ + "/audio.sock";
    std::string fbSock = ipcDir_ + "/fb.sock";
    std::string usbfsSock = sockDir_ + "/usb_001_002.sock";
    std::string serialSock = sockDir_ + "/ttyusb0.sock";

    // 1. Start svbox-net if enabled
    if (spec_.enableNet && !spec_.svboxNetBin.empty()) {
        unlink(netSock.c_str());
        std::vector<std::string> netArgs = {
            spec_.svboxNetBin,
            "--sock", netSock,
            "--iface", spec_.ethSpec,
        };
        for (const auto& d : spec_.dnsServers) {
            netArgs.push_back("--dns");
            netArgs.push_back(d);
        }
        netPid_ = SpawnProcess(netArgs);
        if (netPid_ < 0 || !WaitForFile(netSock, 3000)) {
            if (errOut) *errOut = "Failed to start svbox-net daemon";
            Stop();
            return false;
        }
    }

    // 2. Start ohos_bridge device services
    if (spec_.enableInput) {
        TouchFile(devInputDir_ + "/event0");
        TouchFile(devInputDir_ + "/event1");
        TouchFile(devInputDir_ + "/event2");
        InputBridge::Instance().Start(touchSock, kbdSock, spec_.displayW, spec_.displayH,
                                      gamepadSock, spec_.mapEscToBack);
        WaitForFile(touchSock, 2000);
        WaitForFile(kbdSock, 2000);
        WaitForFile(gamepadSock, 2000);
    }

    if (spec_.enableAudio) {
        AudioBridge::Instance().Start(audioSock);
        WaitForFile(audioSock, 2000);
    }

    if (spec_.enableUsb) {
        TouchFile(devBusUsbDir_ + "/001/002");
        UsbBridge::Instance().Start(usbfsSock, serialSock);
        WaitForFile(usbfsSock, 2000);
        WaitForFile(serialSock, 2000);
    }

    if (spec_.enableFramebuffer) {
        SurfaceBridge::Instance().StartFramebufferServer(fbSock, spec_.displayW, spec_.displayH);
        WaitForFile(fbSock, 2000);
    }

    // 3. Build svbox invocation
    std::vector<std::string> args;
    args.push_back(spec_.svboxBin);
    args.push_back("--root");
    args.push_back(spec_.upperDir);
    for (const auto& lower : spec_.lowerDirs) {
        args.push_back("--lower");
        args.push_back(lower);
    }
    args.push_back("-u");
    args.push_back(spec_.uidGid);
    args.push_back("--cap-add");
    args.push_back("ALL");
    args.push_back("--trap-sigsys");
    args.push_back("--kernel-sysname");
    args.push_back("Linux");
    args.push_back("--kernel-release");
    args.push_back(spec_.kernelRelease);
    args.push_back("--hostname");
    args.push_back(spec_.hostname);
    args.push_back("--emulate-mounts");
    args.push_back("--mount-scratch");
    args.push_back(tmpfsDir_);
    if (!spec_.cpuset.empty()) {
        args.push_back("--cpuset");
        args.push_back(spec_.cpuset);
    }
    if (spec_.memMb > 0) {
        args.push_back("--mem-mb");
        args.push_back(std::to_string(spec_.memMb));
    }
    if (spec_.diskMb > 0) {
        args.push_back("--disk-mb");
        args.push_back(std::to_string(spec_.diskMb));
    }

    // Standard Linux pseudo-devices & bridge binds
    auto addBind = [&args](const std::string& h, const std::string& g) {
        args.push_back("-v");
        args.push_back(h + ":" + g);
    };
    addBind("/proc", "/proc");
    addBind("/dev/null", "/dev/null");
    addBind("/dev/zero", "/dev/zero");
    addBind("/dev/urandom", "/dev/urandom");
    addBind("/dev/random", "/dev/random");
    addBind(resolvPath_, "/etc/resolv.conf");
    addBind(ipcDir_, "/ipc");
    if (spec_.enableInput) {
        addBind(devInputDir_, "/dev/input");
    }
    if (spec_.enableUsb) {
        addBind(devBusUsbDir_, "/dev/bus/usb");
    }
    if (!spec_.sharedHostDir.empty() && !spec_.sharedGuestDir.empty()) {
        MkdirP(spec_.sharedHostDir);
        addBind(spec_.sharedHostDir, spec_.sharedGuestDir);
    }
    for (const auto& kv : spec_.binds) {
        addBind(kv.first, kv.second);
    }

    auto addVdev = [&args](const std::string& g, const std::string& s, const std::string& t) {
        args.push_back("--vdev");
        args.push_back(g + ":" + s + ":" + t);
    };
    if (spec_.enableNet && !spec_.svboxNetBin.empty()) {
        args.push_back("--net-sock");
        args.push_back(netSock);
        addVdev("/dev/tun", netSock, "tun");
        addVdev("/dev/net/tun", netSock, "tun");
    }
    if (spec_.enableInput) {
        addVdev("/dev/input/event0", touchSock, "chr");
        addVdev("/dev/input/event1", kbdSock, "chr");
        addVdev("/dev/input/event2", gamepadSock, "chr");
    }
    if (spec_.enableUsb) {
        addVdev("/dev/bus/usb/001/002", usbfsSock, "chr");
        addVdev("/dev/ttyUSB0", serialSock, "chr");
    }
    for (const auto& vd : spec_.extraVdevs) {
        addVdev(vd.guestPath, vd.hostSock, vd.type);
    }

    for (const auto& e : spec_.env) {
        args.push_back("-e");
        args.push_back(e);
    }

    args.push_back("--");
    for (size_t gi = 0; gi < spec_.guestArgv.size(); ++gi) {
        std::string a = spec_.guestArgv[gi];
        // svbox expects guest argv[0] relative to --root (no leading slash)
        if (gi == 0 && !a.empty() && a[0] == '/') {
            while (!a.empty() && a[0] == '/') a.erase(a.begin());
        }
        args.push_back(a);
    }

    guestPid_ = SpawnProcess(args);
    if (guestPid_ < 0) {
        if (errOut) *errOut = "Failed to fork/exec svbox";
        Stop();
        return false;
    }

    started_ = true;
    return true;
}

int ContainerRunner::Wait() {
    if (guestPid_ <= 0) return -1;
    int status = 0;
    while (waitpid(guestPid_, &status, 0) < 0) {
        if (errno == EINTR) continue;
        guestPid_ = -1;
        return -1;
    }
    guestPid_ = -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

void ContainerRunner::Stop() {
    if (guestPid_ > 0) {
        kill(guestPid_, SIGTERM);
        int status = 0;
        waitpid(guestPid_, &status, 0);
        guestPid_ = -1;
    }
    if (netPid_ > 0) {
        kill(netPid_, SIGTERM);
        int status = 0;
        waitpid(netPid_, &status, 0);
        netPid_ = -1;
    }
    if (started_) {
        if (spec_.enableFramebuffer) SurfaceBridge::Instance().Stop();
        if (spec_.enableUsb) UsbBridge::Instance().Stop();
        if (spec_.enableAudio) AudioBridge::Instance().Stop();
        if (spec_.enableInput) InputBridge::Instance().Stop();
        started_ = false;
    }
}

} // namespace OhosBridge
