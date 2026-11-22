/* SPDX-License-Identifier: MIT */
/* In-HAP Android 11 container supervisor:
 * Launches svbox-propd, svbox-binderd, servicemanager, hwservicemanager,
 * graphics HALs, SurfaceFlinger, and either BootAnimation demo or full
 * Android 11 (Zygote64 -> system_server) using svbox. */
#include "android_runtime.h"
#include "hwc_renderer.h"

#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <hilog/log.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#undef LOG_TAG
#define LOG_TAG "SvboxRuntime"

namespace {

constexpr const char* kBootClassPath =
    "/apex/com.android.art/javalib/core-oj.jar:"
    "/apex/com.android.art/javalib/core-libart.jar:"
    "/apex/com.android.art/javalib/core-icu4j.jar:"
    "/apex/com.android.art/javalib/okhttp.jar:"
    "/apex/com.android.art/javalib/bouncycastle.jar:"
    "/apex/com.android.art/javalib/apache-xml.jar:"
    "/system/framework/framework.jar:"
    "/system/framework/ext.jar:"
    "/system/framework/telephony-common.jar:"
    "/system/framework/voip-common.jar:"
    "/system/framework/ims-common.jar:"
    "/system/framework/framework-atb-backward-compatibility.jar:"
    "/apex/com.android.conscrypt/javalib/conscrypt.jar:"
    "/apex/com.android.media/javalib/updatable-media.jar:"
    "/apex/com.android.mediaprovider/javalib/framework-mediaprovider.jar:"
    "/apex/com.android.os.statsd/javalib/framework-statsd.jar:"
    "/apex/com.android.permission/javalib/framework-permission.jar:"
    "/apex/com.android.sdkext/javalib/framework-sdkextensions.jar:"
    "/apex/com.android.wifi/javalib/framework-wifi.jar:"
    "/apex/com.android.tethering/javalib/framework-tethering.jar";

constexpr const char* kDex2OatBootClassPath =
    "/apex/com.android.art/javalib/core-oj.jar:"
    "/apex/com.android.art/javalib/core-libart.jar:"
    "/apex/com.android.art/javalib/core-icu4j.jar:"
    "/apex/com.android.art/javalib/okhttp.jar:"
    "/apex/com.android.art/javalib/bouncycastle.jar:"
    "/apex/com.android.art/javalib/apache-xml.jar:"
    "/system/framework/framework.jar:"
    "/system/framework/ext.jar:"
    "/system/framework/telephony-common.jar:"
    "/system/framework/voip-common.jar:"
    "/system/framework/ims-common.jar:"
    "/system/framework/framework-atb-backward-compatibility.jar";

constexpr const char* kSystemServerClassPath =
    "/system/framework/com.android.location.provider.jar:"
    "/system/framework/services.jar:"
    "/system/framework/ethernet-service.jar:"
    "/apex/com.android.permission/javalib/service-permission.jar:"
    "/apex/com.android.ipsec/javalib/android.net.ipsec.ike.jar";

bool MkdirP(const std::string& path) {
    if (path.empty()) return false;
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur.push_back(path[i]);
        if (path[i] == '/' && cur.size() > 1) {
            mkdir(cur.c_str(), 0755);
        }
    }
    return mkdir(cur.c_str(), 0755) == 0 || errno == EEXIST;
}

bool FileExists(const std::string& path) {
    struct stat st = {};
    return stat(path.c_str(), &st) == 0;
}

void WriteFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (f) {
        f << content;
    }
}

int MakeListenUnix(const std::string& path) {
    unlink(path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM, 0); // NOT SOCK_CLOEXEC so child inherits it
    if (fd < 0) return -1;
    struct sockaddr_un sa = {};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, path.c_str(), sizeof(sa.sun_path) - 1);
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) < 0 || listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool WaitForFile(const std::string& path, int maxMs) {
    for (int elapsed = 0; elapsed < maxMs; elapsed += 25) {
        if (FileExists(path)) return true;
        usleep(25000);
    }
    return FileExists(path);
}

} // namespace

AndroidRuntime& AndroidRuntime::Instance() {
    static AndroidRuntime inst;
    return inst;
}

void AndroidRuntime::SetStatus(const std::string& s) {
    std::lock_guard<std::mutex> lk(mu_);
    status_ = s;
}

std::string AndroidRuntime::StatusText() const {
    std::lock_guard<std::mutex> lk(mu_);
    return status_;
}

std::vector<std::string> AndroidRuntime::BaseSvboxArgs() const {
    const std::string sockDir = runBase_ + "/sockets";
    return {
        svboxPath_,
        "--root", rootfs_,
        "-u", "0:0",
        "--cap-add", "ALL",
        "--owner-db", runBase_ + "/owners.db",
        "--kernel-sysname", "Linux",
        "--kernel-release", "5.10.0-android11",
        "--hostname", "localhost",
        "--emulate-mounts",
        "--mount-scratch", runBase_ + "/tmpfs",
        "-v", "/proc:/proc",
        "-v", "/dev/null:/dev/null",
        "-v", "/dev/zero:/dev/zero",
        "-v", "/dev/urandom:/dev/urandom",
        "-v", "/dev/random:/dev/random",
        "-v", "/dev/ashmem:/dev/ashmem",
        "-v", rootfs_ + "/system/apex:/apex",
        "-v", rootfs_ + "/system/apex/com.android.art.debug:/apex/com.android.art",
        "-v", rootfs_ + "/system/apex/com.android.vndk.current:/apex/com.android.vndk.v30",
        "-v", runBase_ + "/__properties__:/dev/__properties__",
        "-v", sockDir + ":/dev/socket",
        "-v", runBase_ + "/dev_input:/dev/input",
        "-v", runBase_ + "/ipc:/ipc",
        "-v", runBase_ + "/data:/data",
        "-v", runBase_ + "/cache:/cache",
        "-v", runBase_ + "/acct:/acct",
        "-v", runBase_ + "/cgroup:/sys/fs/cgroup",
        "-v", runBase_ + "/sys_power:/sys/power",
        "-v", runBase_ + "/cpuset:/dev/cpuset",
        "-v", runBase_ + "/stune:/dev/stune",
        "-v", runBase_ + "/mnt:/mnt",
        "-v", runBase_ + "/storage:/storage",
        "--vdev", "/dev/binder:" + sockDir + "/binder.sock:binder",
        "--vdev", "/dev/hwbinder:" + sockDir + "/hwbinder.sock:binder",
        "--vdev", "/dev/vndbinder:" + sockDir + "/vndbinder.sock:binder",
        "-e", "ANDROID_BOOTLOGO=1",
        "-e", "ANDROID_ROOT=/system",
        "-e", "ANDROID_ASSETS=/system/app",
        "-e", "ANDROID_DATA=/data",
        "-e", "ANDROID_STORAGE=/storage",
        "-e", "ANDROID_ART_ROOT=/apex/com.android.art",
        "-e", "ANDROID_I18N_ROOT=/apex/com.android.i18n",
        "-e", "ANDROID_TZDATA_ROOT=/apex/com.android.tzdata",
        "-e", "ANDROID_RUNTIME_ROOT=/apex/com.android.runtime",
        "-e", "EXTERNAL_STORAGE=/sdcard",
        "-e", "ASEC_MOUNTPOINT=/mnt/asec",
        "-e", std::string("BOOTCLASSPATH=") + kBootClassPath,
        "-e", std::string("DEX2OATBOOTCLASSPATH=") + kDex2OatBootClassPath,
        "-e", std::string("SYSTEMSERVERCLASSPATH=") + kSystemServerClassPath,
        "-e", "PATH=/product/bin:/apex/com.android.runtime/bin:/apex/com.android.art/bin:/system_ext/bin:/system/bin:/system/xbin:/vendor/bin",
    };
}

pid_t AndroidRuntime::Spawn(const std::vector<std::string>& argv,
                            bool nullStdio,
                            const std::vector<int>& keepFds) {
    if (argv.empty()) return -1;
    std::vector<char*> cargs;
    cargs.reserve(argv.size() + 1);
    for (const auto& s : argv) {
        cargs.push_back(const_cast<char*>(s.c_str()));
    }
    cargs.push_back(nullptr);

    pid_t pid = fork();
    if (pid == 0) {
        if (nullStdio) {
            int dn = open("/dev/null", O_RDWR);
            if (dn >= 0) {
                dup2(dn, 0);
                dup2(dn, 1);
                dup2(dn, 2);
                if (dn > 2) close(dn);
            }
        }
        // Close extra FDs (except 0..2 and keepFds) so Android Zygote FD whitelist passes.
        long maxFd = sysconf(_SC_OPEN_MAX);
        if (maxFd < 0 || maxFd > 1024) maxFd = 1024;
        for (int fd = 3; fd < static_cast<int>(maxFd); ++fd) {
            bool keep = false;
            for (int k : keepFds) {
                if (k == fd) { keep = true; break; }
            }
            if (!keep) close(fd);
        }
        execv(cargs[0], cargs.data());
        _exit(127);
    }
    if (pid > 0) {
        std::lock_guard<std::mutex> lk(procMu_);
        pids_.push_back(pid);
    }
    return pid;
}

bool AndroidRuntime::SleepCancelableMs(int totalMs) const {
    for (int elapsed = 0; elapsed < totalMs; elapsed += 25) {
        if (!running_.load()) return false;
        usleep(25000);
    }
    return running_.load();
}

std::string AndroidRuntime::Start(const std::string& sandboxTempDir,
                                  const std::string& svboxPath,
                                  const std::string& androidDir,
                                  const std::string& mode,
                                  int32_t width,
                                  int32_t height) {
    if (running_.load()) {
        return "Container already running";
    }
    if (bootThread_.joinable()) {
        bootThread_.join();
    }

    const std::string binderd = androidDir + "/binderd/svbox-binderd";
    const std::string propd = androidDir + "/propd/svbox-propd";
    const std::string rootfs = androidDir + "/rootfs";

    if (!FileExists(svboxPath)) return "Missing svbox binary: " + svboxPath;
    if (!FileExists(binderd)) return "Missing svbox-binderd: " + binderd;
    if (!FileExists(propd)) return "Missing svbox-propd: " + propd;
    if (!FileExists(rootfs + "/system/bin/surfaceflinger")) {
        return "Missing Android 11 rootfs at: " + rootfs;
    }

    runBase_ = sandboxTempDir + "/svbox-android-hap";
    svboxPath_ = svboxPath;
    androidDir_ = androidDir;
    rootfs_ = rootfs;

    running_.store(true);
    SetStatus("Booting Android 11 (" + mode + ")...");
    bootThread_ = std::thread(&AndroidRuntime::BootWorker, this, mode, width, height);
    return StatusText();
}

void AndroidRuntime::BootWorker(std::string mode, int32_t width, int32_t height) {
    const std::string binderd = androidDir_ + "/binderd/svbox-binderd";
    const std::string propd = androidDir_ + "/propd/svbox-propd";
    const std::string sockDir = runBase_ + "/sockets";
    const std::string propDir = runBase_ + "/__properties__";
    const std::string ipcDir = runBase_ + "/ipc";

    const char* dirs[] = {
        "", "/sockets", "/__properties__", "/ipc", "/cache", "/acct", "/cgroup",
        "/sys_power", "/dev_input", "/cpuset", "/stune", "/mnt/user/0", "/storage/emulated/0",
        "/tmpfs", "/data/system/users/0", "/data/system/dropbox", "/data/system/usagestats",
        "/data/system_de/0", "/data/system_ce/0", "/data/misc/profiles/cur/0",
        "/data/misc/profiles/ref", "/data/misc/apexdata", "/data/misc/keystore",
        "/data/misc/installd", "/data/misc/user/0", "/data/misc/media", "/data/misc/credstore",
        "/data/misc_ce/0", "/data/misc_de/0", "/data/vendor_ce/0", "/data/vendor_de/0",
        "/data/media/0", "/data/user/0", "/data/user_de/0", "/data/app", "/data/app-private",
        "/data/app-ephemeral", "/data/app-asec", "/data/app-lib", "/data/app-staging",
        "/data/data", "/data/dalvik-cache/arm64", "/data/dalvik-cache/profiles",
        "/data/resource-cache", "/data/anr", "/data/tombstones", "/data/local/tmp",
        "/data/property", "/data/apex/active", "/data/rollback", "/data/server_configurable_flags",
        "/data/backup", "/data/cache", "/data/incremental", "/data/ota", "/data/preloads",
        "/data/ss", "/data/vendor"
    };
    for (const char* sub : dirs) {
        MkdirP(runBase_ + sub);
    }

    WriteFile(runBase_ + "/sys_power/wake_lock", "");
    WriteFile(runBase_ + "/sys_power/wake_unlock", "");
    WriteFile(runBase_ + "/sys_power/state", "mem\n");
    WriteFile(runBase_ + "/sys_power/wakeup_count", "0\n");

    const std::string overrideProp = runBase_ + "/override.prop";
    WriteFile(overrideProp,
        "ro.hardware.egl=swiftshader\n"
        "ro.hardware.gralloc=redroid\n"
        "ro.hardware.vulkan=pastel\n"
        "ro.opengles.version=196608\n"
        "ro.sf.lcd_density=240\n"
        "debug.renderengine.backend=gles\n"
        "debug.sf.nobootanimation=0\n"
        "debug.stagefright.ccodec=0\n"
        "ro.boot.redroid_width=" + std::to_string(width) + "\n"
        "ro.boot.redroid_height=" + std::to_string(height) + "\n"
        "ro.boot.redroid_fps=30\n"
        "ro.boot.use_redroid_stream=1\n"
        "bpf.progs_loaded=1\n"
        "log.tag.libprocessgroup=F\n"
        "sys.use_memfd=false\n");

    unlink((ipcDir + "/hwcomposer.sock").c_str());

    SetStatus("Starting propd & binder daemons...");

    // 1. Start propd
    std::vector<std::string> propArgs = { propd, "--dir", propDir, "--sock", sockDir + "/property_service" };
    for (const std::string& pf : { rootfs_ + "/system/build.prop", rootfs_ + "/vendor/build.prop",
                                   rootfs_ + "/vendor/default.prop", overrideProp }) {
        if (FileExists(pf)) propArgs.push_back(pf);
    }
    Spawn(propArgs, true);

    // 2. Start userspace Binder routers
    Spawn({ binderd, sockDir + "/binder.sock" }, true);
    Spawn({ binderd, sockDir + "/hwbinder.sock" }, true);
    Spawn({ binderd, sockDir + "/vndbinder.sock" }, true);

    WaitForFile(sockDir + "/binder.sock", 2000);
    WaitForFile(sockDir + "/hwbinder.sock", 2000);
    WaitForFile(sockDir + "/property_service", 2000);
    if (!running_.load()) return;

    // Start XComponent HWC frame consumer right away so it connects immediately
    HwcRenderer::Instance().Start(ipcDir + "/hwcomposer.sock", width, height);

    // 3. Start servicemanager & hwservicemanager
    auto runSvbox = [&](const std::vector<std::string>& guestCmd, const std::vector<int>& keepFds = {}) {
        auto args = BaseSvboxArgs();
        args.insert(args.end(), guestCmd.begin(), guestCmd.end());
        return Spawn(args, true, keepFds);
    };

    SetStatus("Starting servicemanager & graphics HALs...");
    runSvbox({ "system/bin/servicemanager" });
    runSvbox({ "system/bin/hwservicemanager" });
    if (!SleepCancelableMs(400)) return;

    // 4. Graphics HALs
    runSvbox({ "system/bin/hw/android.hidl.allocator@1.0-service" });
    runSvbox({ "vendor/bin/hw/android.hardware.graphics.allocator@2.0-service" });
    runSvbox({ "vendor/bin/hw/android.hardware.graphics.composer@2.1-service" });
    if (!SleepCancelableMs(600)) return;

    // 5. SurfaceFlinger
    SetStatus("Starting SurfaceFlinger (SwiftShader GLES 3.0)...");
    runSvbox({ "system/bin/surfaceflinger" });
    if (!SleepCancelableMs(1200)) return;

    if (mode == "bootanim") {
        SetStatus("Running Android 11 BootAnimation -> XComponent (Scheme 1 HWC stream)");
        runSvbox({ "system/bin/bootanimation" });
    } else {
        SetStatus("Starting Android 11 daemons + Zygote64 -> system_server...");
        runSvbox({ "system/bin/installd" });
        runSvbox({ "system/bin/storaged" });
        runSvbox({ "system/bin/idmap2d" });
        runSvbox({ "system/bin/keystore", "/data/misc/keystore" });
        runSvbox({ "system/bin/vold",
                   "--blkid_context=u:r:blkid:s0",
                   "--blkid_untrusted_context=u:r:blkid_untrusted:s0",
                   "--fsck_context=u:r:fsck:s0",
                   "--fsck_untrusted_context=u:r:fsck_untrusted:s0" });
        runSvbox({ "system/bin/hw/android.system.suspend@1.0-service" });
        runSvbox({ "vendor/bin/hw/android.hardware.power.stats@1.0-service.mock" });
        runSvbox({ "vendor/bin/hw/android.hardware.power-service.example" });
        runSvbox({ "vendor/bin/hw/android.hardware.health@2.1-service" });
        runSvbox({ "vendor/bin/hw/android.hardware.gatekeeper@1.0-service.software" });
        runSvbox({ "vendor/bin/hw/android.hardware.keymaster@3.0-service" });
        runSvbox({ "vendor/bin/hw/android.hardware.sensors@2.1-service.mock" });
        runSvbox({ "system/bin/bootanimation" });
        if (!SleepCancelableMs(700)) return;

        int zfd = MakeListenUnix(sockDir + "/zygote");
        int ufd = MakeListenUnix(sockDir + "/usap_pool_primary");
        {
            std::lock_guard<std::mutex> lk(procMu_);
            if (zfd >= 0) ownedFds_.push_back(zfd);
            if (ufd >= 0) ownedFds_.push_back(ufd);
        }

        std::vector<std::string> zygCmd = {
            "-e", "ANDROID_SOCKET_zygote=" + std::to_string(zfd),
            "-e", "ANDROID_SOCKET_usap_pool_primary=" + std::to_string(ufd),
            "system/bin/app_process64",
            "-Xzygote", "/system/bin",
            "--zygote", "--start-system-server", "--socket-name=zygote"
        };
        runSvbox(zygCmd, { zfd, ufd });
        SetStatus("Android 11 Zygote64 + system_server + SurfaceFlinger running");
    }

    size_t procCount = 0;
    {
        std::lock_guard<std::mutex> lk(procMu_);
        procCount = pids_.size();
    }
    OH_LOG_INFO(LOG_APP, "AndroidRuntime started (mode=%s, procs=%zu)", mode.c_str(), procCount);
}

std::string AndroidRuntime::Stop() {
    running_.store(false);
    HwcRenderer::Instance().Stop();
    if (bootThread_.joinable()) {
        bootThread_.join();
    }

    std::vector<pid_t> pidsCopy;
    std::vector<int> fdsCopy;
    {
        std::lock_guard<std::mutex> lk(procMu_);
        pidsCopy.swap(pids_);
        fdsCopy.swap(ownedFds_);
    }

    for (auto it = pidsCopy.rbegin(); it != pidsCopy.rend(); ++it) {
        if (*it > 0) kill(*it, SIGTERM);
    }
    if (!pidsCopy.empty()) {
        usleep(150000);
    }
    for (auto it = pidsCopy.rbegin(); it != pidsCopy.rend(); ++it) {
        if (*it > 0) {
            kill(*it, SIGKILL);
            int st = 0;
            waitpid(*it, &st, WNOHANG);
        }
    }
    for (int fd : fdsCopy) {
        if (fd >= 0) close(fd);
    }
    SetStatus("Stopped");
    return "Stopped";
}
