/* SPDX-License-Identifier: MIT */
/* svbox-linux: generic ARM64 Linux distro runner built on Layer 1 (svbox +
 * svbox-net) + Layer 2 (ohos_bridge). Proves that ohos_bridge hosts standard
 * Linux distributions (Alpine, Debian, Ubuntu, etc.) with virtual network,
 * evdev touch/keyboard/gamepad, OHAudio PCM, Linux usbfs/ttyUSB0, RGBA
 * framebuffer, and SchedQos without any Android dependency. */
#include "ohos_bridge/audio_bridge.h"
#include "ohos_bridge/container_runner.h"
#include "ohos_bridge/input_bridge.h"
#include "ohos_bridge/sched_qos.h"
#include "ohos_bridge/surface_bridge.h"
#include "ohos_bridge/usb_bridge.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <linux/input.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

void PrintUsage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s --rootfs <dir> [options] -- <cmd> [args...]\n"
        "Options:\n"
        "  --rootfs <dir>          Read-only Linux rootfs layer (repeatable)\n"
        "  --upper <dir>           Writable overlay upper dir\n"
        "  --workdir <dir>         Unix-socket capable runtime dir\n"
        "  --svbox <path>          Path to svbox binary\n"
        "  --svbox-net <path>      Path to svbox-net binary\n"
        "  --share <host:guest>    Shared host directory mount\n"
        "  --profile <p>           CPU profile: eco|balanced|perf|all\n"
        "  --hostname <name>       Container hostname (default: svbox-linux)\n"
        "  --no-net                Disable svbox-net virtual network\n"
        "  --fb                    Enable generic RGBA framebuffer server (/ipc/fb.sock)\n"
        "  --fb-dump <path>        Dump last received RGBA frame (raw W,H,RGBA) on exit\n"
        "  --inject-test-gamepad   Inject test gamepad/kbd events once guest opens evdev\n"
        "  --print-stats           Print ohos_bridge telemetry summary on exit\n",
        prog);
}

} // namespace

int main(int argc, char** argv) {
    OhosBridge::ContainerSpec spec;
    spec.svboxBin = "/storage/Users/currentUser/Workspace/svbox/build/svbox";
    spec.svboxNetBin = "/storage/Users/currentUser/Workspace/svbox/build/svbox-net";
    spec.workDir = "/data/storage/el2/base/cache/svbox-linux-run";
    spec.env = {
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "HOME=/root",
        "TERM=xterm-256color",
    };

    std::string profile = "all";
    std::string fbDumpPath;
    bool injectTestGamepad = false;
    bool printStats = false;

    int i = 1;
    for (; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--") {
            ++i;
            break;
        } else if (a == "--rootfs" && i + 1 < argc) {
            spec.lowerDirs.push_back(argv[++i]);
        } else if (a == "--upper" && i + 1 < argc) {
            spec.upperDir = argv[++i];
        } else if (a == "--workdir" && i + 1 < argc) {
            spec.workDir = argv[++i];
        } else if (a == "--svbox" && i + 1 < argc) {
            spec.svboxBin = argv[++i];
        } else if (a == "--svbox-net" && i + 1 < argc) {
            spec.svboxNetBin = argv[++i];
        } else if (a == "--share" && i + 1 < argc) {
            std::string s = argv[++i];
            auto pos = s.find(':');
            if (pos != std::string::npos) {
                spec.sharedHostDir = s.substr(0, pos);
                spec.sharedGuestDir = s.substr(pos + 1);
            }
        } else if (a == "--profile" && i + 1 < argc) {
            profile = argv[++i];
        } else if (a == "--hostname" && i + 1 < argc) {
            spec.hostname = argv[++i];
        } else if (a == "--no-net") {
            spec.enableNet = false;
        } else if (a == "--fb") {
            spec.enableFramebuffer = true;
        } else if (a == "--fb-dump" && i + 1 < argc) {
            spec.enableFramebuffer = true;
            fbDumpPath = argv[++i];
        } else if (a == "--inject-test-gamepad") {
            injectTestGamepad = true;
        } else if (a == "--print-stats") {
            printStats = true;
        } else if (a == "-h" || a == "--help") {
            PrintUsage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", a.c_str());
            PrintUsage(argv[0]);
            return 2;
        }
    }

    for (; i < argc; ++i) {
        spec.guestArgv.push_back(argv[i]);
    }
    if (spec.guestArgv.empty()) {
        spec.guestArgv = { "/bin/sh" };
    }
    if (spec.lowerDirs.empty()) {
        std::fprintf(stderr, "Error: at least one --rootfs <dir> is required\n");
        PrintUsage(argv[0]);
        return 2;
    }

    if (profile != "all") {
        SchedQos::StartCoreTopologyProbeAsync();
        usleep(120000);
        spec.cpuset = SchedQos::ResolveCpuset(profile, "", nullptr, nullptr);
    }

    OhosBridge::ContainerRunner runner;
    std::string err;
    if (!runner.Start(spec, &err)) {
        std::fprintf(stderr, "svbox-linux start failed: %s\n", err.c_str());
        return 1;
    }

    std::atomic<bool> stopInject{false};
    std::thread injectThread;
    if (injectTestGamepad) {
        injectThread = std::thread([&stopInject]() {
            auto& inp = InputBridge::Instance();
            bool sentGp = false;
            bool sentKbd = false;
            for (int tick = 0; tick < 200 && !stopInject.load(); ++tick) {
                if (!sentGp && inp.HasGamepadClient()) {
                    usleep(50000);
                    inp.InjectGamepadFrame(
                        { { ABS_X, 16384 }, { ABS_Y, -16384 }, { ABS_RZ, 200 }, { ABS_HAT0X, 1 } },
                        { { BTN_SOUTH, true } });
                    usleep(30000);
                    inp.InjectGamepadButton(BTN_SOUTH, false);
                    sentGp = true;
                }
                if (!sentKbd && inp.HasKbdClient()) {
                    usleep(50000);
                    inp.TapLinuxKey(KEY_A);
                    sentKbd = true;
                }
                if (sentGp && sentKbd) break;
                usleep(50000);
            }
        });
    }

    int rc = runner.Wait();

    stopInject.store(true);
    if (injectThread.joinable()) injectThread.join();

    if (!fbDumpPath.empty() && SurfaceBridge::Instance().FrameCount() > 0) {
        std::vector<uint8_t> rgba;
        int32_t fw = 0, fh = 0;
        if (SurfaceBridge::Instance().CopyLatestFrame(rgba, fw, fh)) {
            FILE* fp = std::fopen(fbDumpPath.c_str(), "wb");
            if (fp) {
                uint32_t hdr[2] = { static_cast<uint32_t>(fw), static_cast<uint32_t>(fh) };
                std::fwrite(hdr, sizeof(uint32_t), 2, fp);
                std::fwrite(rgba.data(), 1, rgba.size(), fp);
                std::fclose(fp);
            }
        }
    }

    if (printStats) {
        std::fprintf(stderr,
            "[ohos_bridge stats] input_events=%llu gck=%d audio_rx=%llu audio_played=%llu "
            "audio_backend=%d usb_xfers=%llu serial_bytes=%llu fb_frames=%llu\n",
            static_cast<unsigned long long>(InputBridge::Instance().EventCount()),
            InputBridge::Instance().GameControllerKitActive() ? 1 : 0,
            static_cast<unsigned long long>(AudioBridge::Instance().BytesReceived()),
            static_cast<unsigned long long>(AudioBridge::Instance().BytesPlayed()),
            AudioBridge::Instance().HasBackend() ? 1 : 0,
            static_cast<unsigned long long>(UsbBridge::Instance().UsbTransfers()),
            static_cast<unsigned long long>(UsbBridge::Instance().SerialBytes()),
            static_cast<unsigned long long>(SurfaceBridge::Instance().FrameCount()));
    }

    runner.Stop();
    return rc;
}
