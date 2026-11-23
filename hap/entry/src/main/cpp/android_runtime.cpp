/* SPDX-License-Identifier: MIT */
/* In-HAP Android 11 container supervisor:
 * Launches svbox-propd, svbox-binderd, servicemanager, hwservicemanager,
 * graphics HALs, SurfaceFlinger, and either BootAnimation demo or full
 * Android 11 (Zygote64 -> system_server) using svbox. */
#include "android_runtime.h"
#include "ohos_bridge/audio_bridge.h"
#include "ohos_bridge/gpu_bridge.h"
#include "ohos_bridge/input_bridge.h"
#include "ohos_bridge/sched_qos.h"
#include "ohos_bridge/surface_bridge.h"
#include "ohos_bridge/usb_bridge.h"
#include "net_proto.h"

#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <hilog/log.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <sys/mman.h>
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

void WriteEthernetStaticIpConfig(const std::string& path) {
    // Standard AOSP IpConfigStore v3 binary format (DataOutputStream big-endian)
    // configuring emulator-style static eth0 (10.0.2.15/24, gw 10.0.2.2, dns 223.5.5.5 & 8.8.8.8).
    std::vector<uint8_t> out;
    auto putBe32 = [&](uint32_t v) {
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xffu));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xffu));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xffu));
        out.push_back(static_cast<uint8_t>(v & 0xffu));
    };
    auto putUtf = [&](const char* s) {
        uint16_t n = static_cast<uint16_t>(std::strlen(s));
        out.push_back(static_cast<uint8_t>((n >> 8) & 0xffu));
        out.push_back(static_cast<uint8_t>(n & 0xffu));
        out.insert(out.end(), s, s + n);
    };
    putBe32(3); // IPCONFIG_FILE_VERSION = 3
    putUtf("ipAssignment"); putUtf("STATIC");
    putUtf("linkAddress");  putUtf("10.0.2.15"); putBe32(24);
    putUtf("gateway");      putBe32(0); putBe32(1); putUtf("10.0.2.2");
    putUtf("dns");          putUtf("223.5.5.5");
    putUtf("dns");          putUtf("8.8.8.8");
    putUtf("proxySettings"); putUtf("NONE");
    putUtf("id");           putUtf("eth0");
    putUtf("eos");
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
        (void)write(fd, out.data(), out.size());
        close(fd);
    }
}

void RemoveDirRecursive(const std::string& path) {
    struct stat st = {};
    if (lstat(path.c_str(), &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) {
        unlink(path.c_str());
        return;
    }
    DIR* d = opendir(path.c_str());
    if (d) {
        while (struct dirent* de = readdir(d)) {
            if (std::strcmp(de->d_name, ".") == 0 || std::strcmp(de->d_name, "..") == 0) {
                continue;
            }
            RemoveDirRecursive(path + "/" + de->d_name);
        }
        closedir(d);
    }
    rmdir(path.c_str());
}

void EnsureOwnerDb(const std::string& path) {
    struct OwnerHdr {
        char magic[8];
        uint32_t capacity;
        uint32_t count;
        uint32_t lock;
        uint32_t version;
        uint8_t pad[40];
    };
    const off_t kExpectedSize = static_cast<off_t>(sizeof(OwnerHdr) + 65536u * 32u);
    int fd = open(path.c_str(), O_RDWR);
    if (fd >= 0) {
        struct stat st = {};
        OwnerHdr hdr = {};
        if (fstat(fd, &st) == 0 && st.st_size >= kExpectedSize &&
            pread(fd, &hdr, sizeof(hdr), 0) == static_cast<ssize_t>(sizeof(hdr)) &&
            std::memcmp(hdr.magic, "ELFOWN1\0", 8) == 0 && hdr.capacity >= 65536u) {
            // Preserve persistent fake UID/GID records across restarts, while clearing any
            // lock or odd seqlock version left behind if a previous container was SIGKILLed.
            if (hdr.lock != 0 || (hdr.version & 1u) != 0) {
                hdr.lock = 0;
                if (hdr.version & 1u) hdr.version += 1u;
                (void)pwrite(fd, &hdr, sizeof(hdr), 0);
            }
            close(fd);
            return;
        }
        close(fd);
    }
    unlink(path.c_str());
    fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    OwnerHdr hdr = {};
    std::memcpy(hdr.magic, "ELFOWN1\0", 8);
    hdr.capacity = 65536u;
    (void)write(fd, &hdr, sizeof(hdr));
    (void)ftruncate(fd, kExpectedSize);
    close(fd);
}

int MakeListenUnix(const std::string& path, int sockType = SOCK_STREAM, mode_t mode = 0666) {
    unlink(path.c_str());
    int fd = socket(AF_UNIX, sockType, 0); // NOT SOCK_CLOEXEC so child inherits it
    if (fd < 0) return -1;
    struct sockaddr_un sa = {};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, path.c_str(), sizeof(sa.sun_path) - 1);
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) < 0 || listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }
    chmod(path.c_str(), mode);
    return fd;
}

bool WaitForFile(const std::string& path, int maxMs) {
    for (int elapsed = 0; elapsed < maxMs; elapsed += 25) {
        if (FileExists(path)) return true;
        usleep(25000);
    }
    return FileExists(path);
}

uint64_t ParseOctal(const char* p, size_t n) {
    uint64_t v = 0;
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\0')) ++i;
    while (i < n && p[i] >= '0' && p[i] <= '7') {
        v = (v << 3) | static_cast<uint64_t>(p[i] - '0');
        ++i;
    }
    return v;
}

std::string CleanTarPath(std::string p) {
    while (!p.empty() && p.back() == '\0') p.pop_back();
    while (p.size() >= 2 && p[0] == '.' && p[1] == '/') p.erase(0, 2);
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    while (!p.empty() && p.back() == '/') p.pop_back();
    if (p == "." || p.find("..") != std::string::npos) return "";
    return p;
}

bool CopyRegularFile(const std::string& src, const std::string& dst) {
    int inFd = open(src.c_str(), O_RDONLY);
    if (inFd < 0) return false;
    struct stat st = {};
    fstat(inFd, &st);
    unlink(dst.c_str());
    int outFd = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, (st.st_mode & 0777) | 0600);
    if (outFd < 0) {
        close(inFd);
        return false;
    }
    char buf[65536];
    ssize_t r;
    while ((r = read(inFd, buf, sizeof(buf))) > 0) {
        (void)write(outFd, buf, static_cast<size_t>(r));
    }
    close(outFd);
    close(inFd);
    return true;
}

std::string ResolveAndroidTarget(const std::string& dstDir,
                                 const std::string& relLink,
                                 const std::string& target) {
    if (target.empty()) return "";
    if (target[0] == '/') {
        if (target.rfind("/apex/com.android.art/", 0) == 0) {
            return dstDir + "/system/apex/com.android.art.debug/" + target.substr(22);
        }
        if (target.rfind("/apex/com.android.vndk.v30/", 0) == 0) {
            return dstDir + "/system/apex/com.android.vndk.current/" + target.substr(27);
        }
        if (target.rfind("/apex/", 0) == 0) {
            return dstDir + "/system/apex/" + target.substr(6);
        }
        return dstDir + target;
    }
    size_t slash = relLink.rfind('/');
    std::string dir = slash != std::string::npos ? (dstDir + "/" + relLink.substr(0, slash)) : dstDir;
    return dir + "/" + target;
}

bool ExtractTarToDir(const std::string& tarPath,
                     const std::string& dstDir,
                     const std::atomic<bool>& running,
                     const std::function<void(int)>& onPercent,
                     std::string& errOut,
                     std::string& diagOut) {
    int fd = open(tarPath.c_str(), O_RDONLY);
    if (fd < 0) {
        errOut = "Cannot open " + tarPath + " (errno=" + std::to_string(errno) + ")";
        return false;
    }
    struct stat st = {};
    fstat(fd, &st);
    const uint64_t totalSize = st.st_size > 0 ? static_cast<uint64_t>(st.st_size) : 1;
    uint64_t readTotal = 0;
    int lastPct = -1;

    MkdirP(dstDir);
    std::vector<char> buf(65536);
    std::string nextLongName;
    std::string nextLongLink;
    int64_t nextPaxSize = -1;
    std::vector<std::pair<std::string, std::string>> deferredSymlinks;
    int symlinkOk = 0, symlinkFail = 0, firstSymErr = 0;

    auto readExact = [&](void* dst, size_t n) -> bool {
        char* p = static_cast<char*>(dst);
        size_t got = 0;
        while (got < n) {
            ssize_t r = read(fd, p + got, n - got);
            if (r <= 0) return false;
            got += static_cast<size_t>(r);
            readTotal += static_cast<uint64_t>(r);
        }
        return true;
    };

    auto skipExact = [&](uint64_t n) -> bool {
        if (n == 0) return true;
        if (lseek(fd, static_cast<off_t>(n), SEEK_CUR) >= 0) {
            readTotal += n;
            return true;
        }
        while (n > 0) {
            size_t chunk = n > buf.size() ? buf.size() : static_cast<size_t>(n);
            if (!readExact(buf.data(), chunk)) return false;
            n -= chunk;
        }
        return true;
    };

    char hdr[512];
    while (running.load()) {
        if (!readExact(hdr, 512)) break;
        bool allZero = true;
        for (int i = 0; i < 512; ++i) {
            if (hdr[i] != 0) { allZero = false; break; }
        }
        if (allZero) break;

        char type = hdr[156];
        uint64_t size = nextPaxSize >= 0 ? static_cast<uint64_t>(nextPaxSize) : ParseOctal(hdr + 124, 12);
        nextPaxSize = -1;
        mode_t mode = static_cast<mode_t>(ParseOctal(hdr + 100, 8));
        uint64_t padded = (size + 511u) & ~511ull;

        std::string name;
        if (!nextLongName.empty()) {
            name.swap(nextLongName);
        } else {
            std::string prefix(hdr + 345, strnlen(hdr + 345, 155));
            std::string base(hdr, strnlen(hdr, 100));
            name = prefix.empty() ? base : (prefix + "/" + base);
        }
        std::string linkName;
        if (!nextLongLink.empty()) {
            linkName.swap(nextLongLink);
        } else {
            linkName.assign(hdr + 157, strnlen(hdr + 157, 100));
        }

        if (type == 'L' || type == 'K') {
            std::string payload(size, '\0');
            if (!readExact(payload.data(), size)) { close(fd); return false; }
            if (!skipExact(padded - size)) { close(fd); return false; }
            while (!payload.empty() && payload.back() == '\0') payload.pop_back();
            if (type == 'L') nextLongName = std::move(payload);
            else nextLongLink = std::move(payload);
            continue;
        }
        if (type == 'x' || type == 'g') {
            std::string payload(size, '\0');
            if (!readExact(payload.data(), size)) { close(fd); return false; }
            if (!skipExact(padded - size)) { close(fd); return false; }
            if (type == 'x') {
                size_t pos = 0;
                while (pos < payload.size()) {
                    size_t sp = payload.find(' ', pos);
                    if (sp == std::string::npos) break;
                    size_t recLen = static_cast<size_t>(std::strtoull(payload.c_str() + pos, nullptr, 10));
                    if (recLen == 0 || pos + recLen > payload.size()) break;
                    std::string kv = payload.substr(sp + 1, pos + recLen - sp - 2);
                    size_t eq = kv.find('=');
                    if (eq != std::string::npos) {
                        std::string k = kv.substr(0, eq);
                        std::string v = kv.substr(eq + 1);
                        if (k == "path") nextLongName = v;
                        else if (k == "linkpath") nextLongLink = v;
                        else if (k == "size") nextPaxSize = std::strtoll(v.c_str(), nullptr, 10);
                    }
                    pos += recLen;
                }
            }
            continue;
        }

        std::string rel = CleanTarPath(name);
        if (rel.empty()) {
            if (!skipExact(padded)) { close(fd); return false; }
            continue;
        }
        std::string full = dstDir + "/" + rel;
        size_t slash = full.rfind('/');
        if (slash != std::string::npos) {
            MkdirP(full.substr(0, slash));
        }

        if (type == '5') {
            MkdirP(full);
            if (mode) chmod(full.c_str(), (mode | 0700) & ~static_cast<mode_t>(0022));
            if (!skipExact(padded)) { close(fd); return false; }
        } else if (type == '2') {
            unlink(full.c_str());
            if (symlink(linkName.c_str(), full.c_str()) == 0) {
                symlinkOk++;
            } else {
                if (firstSymErr == 0) firstSymErr = errno;
                symlinkFail++;
                deferredSymlinks.emplace_back(rel, linkName);
            }
            if (!skipExact(padded)) { close(fd); return false; }
        } else if (type == '1') {
            std::string targetRel = CleanTarPath(linkName);
            if (!targetRel.empty()) {
                unlink(full.c_str());
                link((dstDir + "/" + targetRel).c_str(), full.c_str());
            }
            if (!skipExact(padded)) { close(fd); return false; }
        } else if (type == '0' || type == '\0') {
            unlink(full.c_str());
            int outFd = open(full.c_str(), O_WRONLY | O_CREAT | O_TRUNC, (mode & 0777) ? (mode & 0777) : 0644);
            uint64_t rem = size;
            while (rem > 0) {
                size_t chunk = rem > buf.size() ? buf.size() : static_cast<size_t>(rem);
                if (!readExact(buf.data(), chunk)) {
                    if (outFd >= 0) close(outFd);
                    close(fd);
                    return false;
                }
                if (outFd >= 0) {
                    (void)write(outFd, buf.data(), chunk);
                }
                rem -= chunk;
            }
            if (outFd >= 0) {
                fchmod(outFd, ((mode & 0755) | 0644) & ~static_cast<mode_t>(0022));
                close(outFd);
            }
            if (!skipExact(padded - size)) { close(fd); return false; }
        } else {
            if (!skipExact(padded)) { close(fd); return false; }
        }

        int pct = static_cast<int>((readTotal * 100ull) / totalSize);
        if (pct > 100) pct = 100;
        if (pct != lastPct && (pct % 5 == 0 || pct == 100)) {
            lastPct = pct;
            onPercent(pct);
        }
    }

    close(fd);
    int matOk = 0, matSkip = 0;
    for (int pass = 0; pass < 3 && !deferredSymlinks.empty(); ++pass) {
        std::vector<std::pair<std::string, std::string>> stillPending;
        for (const auto& kv : deferredSymlinks) {
            std::string dstFull = dstDir + "/" + kv.first;
            std::string srcFull = ResolveAndroidTarget(dstDir, kv.first, kv.second);
            struct stat tst = {};
            if (!srcFull.empty() && stat(srcFull.c_str(), &tst) == 0 && S_ISREG(tst.st_mode)) {
                if (link(srcFull.c_str(), dstFull.c_str()) == 0 || CopyRegularFile(srcFull, dstFull)) {
                    matOk++;
                } else {
                    stillPending.push_back(kv);
                }
            } else {
                stillPending.push_back(kv);
            }
        }
        if (stillPending.size() == deferredSymlinks.size()) break;
        deferredSymlinks.swap(stillPending);
    }
    matSkip = static_cast<int>(deferredSymlinks.size());
    diagOut = "symlinks ok=" + std::to_string(symlinkOk) +
              " fail=" + std::to_string(symlinkFail) +
              " (err=" + std::to_string(firstSymErr) + ")" +
              " materialized=" + std::to_string(matOk) +
              " remaining=" + std::to_string(matSkip);
    return running.load();
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

std::string AndroidRuntime::Profile() const {
    std::lock_guard<std::mutex> lk(mu_);
    return profile_;
}

std::string AndroidRuntime::ActiveCpuset() const {
    std::lock_guard<std::mutex> lk(mu_);
    return activeCpuset_;
}

void AndroidRuntime::SetResourceConfig(const std::string& profile,
                                       const std::string& customCpuset,
                                       uint32_t memLimitMb) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!profile.empty()) profile_ = profile;
    customCpuset_ = customCpuset;
    if (memLimitMb >= 512 && memLimitMb <= 32768) {
        memLimitMb_.store(memLimitMb);
    }
}

void AndroidRuntime::SetGpuMode(const std::string& gpuMode) {
    std::lock_guard<std::mutex> lk(mu_);
    if (gpuMode == "swiftshader" || gpuMode == "gles" || gpuMode == "vulkan") {
        gpuMode_ = gpuMode;
    }
}

std::string AndroidRuntime::GpuMode() const {
    std::lock_guard<std::mutex> lk(mu_);
    return running_.load() ? activeGpuMode_ : gpuMode_;
}

bool AndroidRuntime::UseGfxstream() const {
    std::lock_guard<std::mutex> lk(mu_);
    const std::string& m = running_.load() ? activeGpuMode_ : gpuMode_;
    return (m == "gles" || m == "vulkan");
}

void AndroidRuntime::OnSurfaceChanged(OHNativeWindow* window, int32_t w, int32_t h) {
    bool useGfx = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        surfaceWindow_ = window;
        surfaceW_ = w;
        surfaceH_ = h;
        useGfx = running_.load() ? (activeGpuMode_ != "swiftshader") : (gpuMode_ != "swiftshader");
    }
    if (useGfx) {
        GfxstreamBridge::Instance().SetWindow(window, w, h);
    } else {
        HwcRenderer::Instance().SetWindow(window, w, h);
    }
}

void AndroidRuntime::OnSurfaceDestroyed(OHNativeWindow* window) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (window != nullptr && surfaceWindow_ != nullptr && window != surfaceWindow_) {
            return;
        }
        surfaceWindow_ = nullptr;
        surfaceW_ = 0;
        surfaceH_ = 0;
    }
    HwcRenderer::Instance().ClearWindow();
    GfxstreamBridge::Instance().ClearWindow();
}

std::string AndroidRuntime::SetPaused(bool paused) {
    if (!running_.load()) return "Not running";
    paused_.store(paused);
    std::vector<pid_t> pidsCopy;
    {
        std::lock_guard<std::mutex> lk(procMu_);
        pidsCopy = pids_;
    }
    const int sig = paused ? SIGSTOP : SIGCONT;
    for (pid_t p : pidsCopy) {
        if (p > 0) {
            kill(-p, sig);
            kill(p, sig);
        }
    }
    if (UseGfxstream()) {
        GfxstreamBridge::Instance().SetVisible(!paused);
    } else {
        HwcRenderer::Instance().SetVisible(!paused);
    }
    SetStatus(paused ? "Paused (SIGSTOP · 0% CPU)" : "Running Android 11");
    return StatusText();
}

void AndroidRuntime::SetVisible(bool visible) {
    const bool show = visible && !paused_.load();
    if (UseGfxstream()) {
        GfxstreamBridge::Instance().SetVisible(show);
    } else {
        HwcRenderer::Instance().SetVisible(show);
    }
}

SchedQos::Telemetry AndroidRuntime::GetTelemetry() {
    std::vector<pid_t> pidsCopy;
    {
        std::lock_guard<std::mutex> lk(procMu_);
        pidsCopy = pids_;
    }
    return SchedQos::SampleTelemetry(pidsCopy);
}

bool AndroidRuntime::WaitForConditionMs(int maxMs, const std::function<bool()>& pred) const {
    for (int elapsed = 0; elapsed < maxMs; elapsed += 20) {
        if (!running_.load()) return false;
        if (pred()) return true;
        usleep(20000);
    }
    return running_.load() && pred();
}

std::vector<std::string> AndroidRuntime::BaseSvboxArgs(bool includeStorageSubmounts) const {
    const std::string sockDir = runBase_ + "/sockets";
    std::string cpuset;
    int32_t niceVal = 0x7fffffff;
    std::string schedPol;
    {
        std::lock_guard<std::mutex> lk(mu_);
        cpuset = activeCpuset_;
        niceVal = activeNice_;
        schedPol = activeSched_;
    }
    uint32_t memMb = memLimitMb_.load();
    uint32_t diskMb = diskLimitMb_.load();

    std::vector<std::string> args = {
        svboxPath_,
        "--root", rootUpper_,
        "--lower", injectLayer_,
        "--lower", rootfs_,
        "-u", "0:0",
        "--cap-add", "ALL",
        "--trap-sigsys",
        "--kernel-sysname", "Linux",
        "--kernel-release", "5.10.0-android11",
        "--hostname", "localhost",
        "--emulate-mounts",
        "--mount-scratch", runBase_ + "/tmpfs",
        "--owner-db", dataBase_ + "/owner.db",
    };
    if (!cpuset.empty()) {
        args.push_back("--cpuset");
        args.push_back(cpuset);
    }
    if (memMb > 0) {
        args.push_back("--mem-mb");
        args.push_back(std::to_string(memMb));
    }
    if (diskMb > 0) {
        args.push_back("--disk-mb");
        args.push_back(std::to_string(diskMb));
    }
    if (niceVal != 0x7fffffff) {
        args.push_back("--nice");
        args.push_back(std::to_string(niceVal));
    }
    if (!schedPol.empty()) {
        args.push_back("--sched");
        args.push_back(schedPol);
    }

    std::vector<std::string> mounts = {
        "-v", "/proc:/proc",
        "-v", "/dev/null:/dev/null",
        "-v", "/dev/zero:/dev/zero",
        "-v", "/dev/urandom:/dev/urandom",
        "-v", "/dev/random:/dev/random",
        "-v", "/dev/ashmem:/dev/ashmem",
        "-v", rootfs_ + "/system/apex:/apex",
        "-v", rootfs_ + "/system/apex/com.android.art.debug:/apex/com.android.art",
        "-v", rootfs_ + "/system/apex/com.android.vndk.current:/apex/com.android.vndk.v30",
        "-v", rootfs_ + "/system/etc:/etc",
        "-v", runBase_ + "/resolv.conf:/etc/resolv.conf",
        "-v", rootfs_ + "/system/product:/product",
        "-v", rootfs_ + "/system/system_ext:/system_ext",
        "-v", runBase_ + "/__properties__:/dev/__properties__",
        "-v", sockDir + ":/dev/socket",
        "-v", runBase_ + "/dev_input:/dev/input",
        "-v", runBase_ + "/dev_bus_usb:/dev/bus/usb",
        "-v", runBase_ + "/ipc:/ipc",
        "-v", dataBase_ + "/data:/data",
        "-v", dataBase_ + "/data/data:/data/user/0",
        "-v", dataBase_ + "/data/user:/data_mirror/data_ce/null",
        "-v", dataBase_ + "/data/data:/data_mirror/data_ce/null/0",
        "-v", dataBase_ + "/data/user_de:/data_mirror/data_de/null",
        "-v", dataBase_ + "/data/misc/profiles/cur:/data_mirror/cur_profiles",
        "-v", dataBase_ + "/data/misc/profiles/ref:/data_mirror/ref_profiles",
        "-v", runBase_ + "/cache:/cache",
        "-v", runBase_ + "/acct:/acct",
        "-v", runBase_ + "/cgroup:/sys/fs/cgroup",
        "-v", runBase_ + "/sys_power:/sys/power",
        "-v", runBase_ + "/cpuset:/dev/cpuset",
        "-v", runBase_ + "/stune:/dev/stune",
        "-v", runBase_ + "/mnt:/mnt",
        "-v", runBase_ + "/storage:/storage",
    };
    if (includeStorageSubmounts) {
        mounts.push_back("-v");
        mounts.push_back(dataBase_ + "/data/media/0:/sdcard");
        mounts.push_back("-v");
        mounts.push_back(dataBase_ + "/data/media/0:/storage/emulated/0");
        mounts.push_back("-v");
        mounts.push_back(dataBase_ + "/data/media/0:/storage/self/primary");
    }
    if (!shareHostDir_.empty() && FileExists(shareHostDir_)) {
        mounts.push_back("-v");
        mounts.push_back(shareHostDir_ + ":/data/media/0/Download/HarmonyShare");
        if (includeStorageSubmounts) {
            mounts.push_back("-v");
            mounts.push_back(shareHostDir_ + ":/sdcard/Download/HarmonyShare");
            mounts.push_back("-v");
            mounts.push_back(shareHostDir_ + ":/storage/emulated/0/Download/HarmonyShare");
            mounts.push_back("-v");
            mounts.push_back(shareHostDir_ + ":/storage/self/primary/Download/HarmonyShare");
        }
    }
    args.insert(args.end(), mounts.begin(), mounts.end());

    std::vector<std::string> vdevsAndEnv = {
        "--vdev", "/dev/binder:" + sockDir + "/binder.sock:binder",
        "--vdev", "/dev/hwbinder:" + sockDir + "/hwbinder.sock:binder",
        "--vdev", "/dev/vndbinder:" + sockDir + "/vndbinder.sock:binder",
        "--vdev", "/dev/input/event0:" + sockDir + "/input_touch.sock:chr",
        "--vdev", "/dev/input/event1:" + sockDir + "/input_kbd.sock:chr",
        "--vdev", "/dev/input/event2:" + sockDir + "/input_gamepad.sock:chr",
        "--vdev", "/dev/bus/usb/001/002:" + sockDir + "/usb_001_002.sock:chr",
        "--vdev", "/dev/ttyUSB0:" + sockDir + "/ttyusb0.sock:chr",
    };
    if (!netSockPath_.empty()) {
        vdevsAndEnv.push_back("--net-sock");
        vdevsAndEnv.push_back(netSockPath_);
        vdevsAndEnv.push_back("--vdev");
        vdevsAndEnv.push_back("/dev/tun:" + netSockPath_ + ":tun");
        vdevsAndEnv.push_back("--vdev");
        vdevsAndEnv.push_back("/dev/net/tun:" + netSockPath_ + ":tun");
    }
    std::vector<std::string> envArgs = {
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
    args.insert(args.end(), vdevsAndEnv.begin(), vdevsAndEnv.end());
    args.insert(args.end(), envArgs.begin(), envArgs.end());
    return args;
}

pid_t AndroidRuntime::Spawn(const std::vector<std::string>& argv,
                            bool nullStdio,
                            const std::string& logPath,
                            const std::vector<int>& keepFds) {
    if (argv.empty()) return -1;
    std::vector<char*> cargs;
    cargs.reserve(argv.size() + 1);
    for (const auto& s : argv) {
        cargs.push_back(const_cast<char*>(s.c_str()));
    }
    cargs.push_back(nullptr);

    pid_t pid = -1;
    for (int attempt = 0; attempt < 4; ++attempt) {
        pid = fork();
        if (pid >= 0) break;
        if (errno != ENOMEM && errno != EAGAIN) break;
        usleep(100000);
    }
    if (pid == 0) {
        // Place each container daemon / zygote in its own process group so Stop()
        // and Pause()/Resume() cleanly signal the entire descendant tree.
        setpgid(0, 0);
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) {
            dup2(dn, 0);
            if (nullStdio || logPath.empty()) {
                dup2(dn, 1);
                dup2(dn, 2);
            }
            if (dn > 2) close(dn);
        }
        if (!nullStdio && !logPath.empty()) {
            int lfd = open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (lfd >= 0) {
                dup2(lfd, 1);
                dup2(lfd, 2);
                if (lfd > 2) close(lfd);
            }
        }
        // Close extra FDs (except 0..2 and keepFds) so Android Zygote FD whitelist passes.
        // HarmonyOS HAPs can allocate FDs > 1024 (e.g. 1402), so iterate /proc/self/fd.
        DIR* fddir = opendir("/proc/self/fd");
        if (fddir) {
            int dirfdNum = dirfd(fddir);
            std::vector<int> toClose;
            while (struct dirent* de = readdir(fddir)) {
                if (de->d_name[0] == '.') continue;
                int fd = atoi(de->d_name);
                if (fd <= 2 || fd == dirfdNum) continue;
                bool keep = false;
                for (int k : keepFds) {
                    if (k == fd) { keep = true; break; }
                }
                if (!keep) toClose.push_back(fd);
            }
            closedir(fddir);
            for (int fd : toClose) close(fd);
        } else {
            long maxFd = sysconf(_SC_OPEN_MAX);
            if (maxFd < 0 || maxFd > 4096) maxFd = 4096;
            for (int fd = 3; fd < static_cast<int>(maxFd); ++fd) {
                bool keep = false;
                for (int k : keepFds) {
                    if (k == fd) { keep = true; break; }
                }
                if (!keep) close(fd);
            }
        }
        execv(cargs[0], cargs.data());
        int err = errno;
        char msg[256];
        int n = snprintf(msg, sizeof(msg), "execv(%s) failed: errno=%d (%s)\n",
                         cargs[0], err, strerror(err));
        if (n > 0) {
            (void)write(2, msg, static_cast<size_t>(n));
        }
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
    if (lmkdThread_.joinable()) {
        lmkdThread_.join();
    }
    if (logdwThread_.joinable()) {
        logdwThread_.join();
    }
    if (dnsThread_.joinable()) {
        dnsThread_.join();
    }

    const std::string hnpBin = "/data/app/svbox.org/svbox_0.1.0/bin";
    const std::string resolvedSvbox =
        FileExists(hnpBin + "/svbox") ? (hnpBin + "/svbox") : svboxPath;
    const std::string binderd =
        FileExists(hnpBin + "/svbox-binderd") ? (hnpBin + "/svbox-binderd")
                                              : (androidDir + "/binderd/svbox-binderd");
    const std::string propd =
        FileExists(hnpBin + "/svbox-propd") ? (hnpBin + "/svbox-propd")
                                            : (androidDir + "/propd/svbox-propd");
    const std::string audioHal =
        FileExists(hnpBin + "/audio.primary.svbox.so")
            ? (hnpBin + "/audio.primary.svbox.so")
            : (androidDir + "/audio_hal/audio.primary.svbox.so");
    const std::string audioSvc =
        FileExists(hnpBin + "/svbox-audio-service")
            ? (hnpBin + "/svbox-audio-service")
            : (androidDir + "/audio_hal/svbox-audio-service");
    const std::string cameraSvc =
        FileExists(hnpBin + "/svbox-cameraserver")
            ? (hnpBin + "/svbox-cameraserver")
            : (androidDir + "/camera_stub/svbox-cameraserver");
    const std::string hnpGpuGuest = "/data/app/svbox.org/svbox_0.1.0/gpu_guest/lib64";
    const std::string devGpuGuest = androidDir + "/gpu/build_guest/out/lib64";
    const std::string gpuGuestDir =
        FileExists(hnpGpuGuest + "/egl/libEGL_emulation.so")
            ? hnpGpuGuest
            : (FileExists(devGpuGuest + "/egl/libEGL_emulation.so") ? devGpuGuest : "");
    const std::string rootfsTar = androidDir + "/rootfs.tar";
    std::string sandboxFiles = sandboxTempDir;
    if (sandboxFiles.size() >= 5 && sandboxFiles.compare(sandboxFiles.size() - 5, 5, "/temp") == 0) {
        sandboxFiles.replace(sandboxFiles.size() - 5, 5, "/files");
    } else {
        sandboxFiles = "/data/storage/el2/base/haps/entry/files";
    }
    const std::string sandboxRootfs = sandboxFiles + "/rootfs";
    const std::string externalRootfs = androidDir + "/rootfs";
    const bool useSandboxTar = FileExists(sandboxRootfs + "/.imported_v2") || FileExists(rootfsTar);
    const std::string rootfs = useSandboxTar ? sandboxRootfs : externalRootfs;

    if (!FileExists(resolvedSvbox)) return "Missing svbox binary: " + resolvedSvbox;
    if (!FileExists(binderd)) return "Missing svbox-binderd: " + binderd;
    if (!FileExists(propd)) return "Missing svbox-propd: " + propd;
    if (!useSandboxTar && !FileExists(externalRootfs + "/system/bin/surfaceflinger")) {
        return "Missing Android 11 rootfs or rootfs.tar at: " + androidDir;
    }

    sandboxFiles_ = sandboxFiles;
    runBase_ = sandboxTempDir + "/svbox-android-hap";
    dataBase_ = sandboxFiles + "/userdata";
    rootUpper_ = runBase_ + "/root_upper";
    injectLayer_ = runBase_ + "/inject_layer";
    shareHostDir_ = "/storage/Users/currentUser/Download/svbox-share";
    svboxPath_ = resolvedSvbox;
    audioHalPath_ = FileExists(audioHal) ? audioHal : "";
    audioSvcPath_ = FileExists(audioSvc) ? audioSvc : "";
    cameraSvcPath_ = FileExists(cameraSvc) ? cameraSvc : "";
    gpuGuestDir_ = gpuGuestDir;
    androidDir_ = androidDir;
    rootfs_ = rootfs;

    // Resolve active cpuset/nice/sched and GPU mode
    std::string chosenGpu;
    {
        std::lock_guard<std::mutex> lk(mu_);
        activeCpuset_ = SchedQos::ResolveCpuset(profile_, customCpuset_, &activeNice_, &activeSched_);
        activeGpuMode_ = gpuMode_;
        chosenGpu = activeGpuMode_;
    }

    paused_.store(false);
    running_.store(true);
    SetStatus("Booting Android 11 (" + mode + " · " + chosenGpu + ")...");
    bootThread_ = std::thread(&AndroidRuntime::BootWorker, this, mode, width, height);
    return StatusText();
}

void AndroidRuntime::DnsProxyLoop(int listenFd) {
    SchedQos::SetCurrentThreadQoS(SchedQos::Level::Utility, "dns-proxy");
    if (listenFd < 0) return;

    auto sendAll = [](int fd, const void* p, size_t n) -> bool {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        while (n > 0) {
            ssize_t w = send(fd, b, n, MSG_NOSIGNAL);
            if (w <= 0) {
                if (w < 0 && errno == EINTR) continue;
                return false;
            }
            b += w;
            n -= static_cast<size_t>(w);
        }
        return true;
    };

    auto recvAll = [](int fd, void* p, size_t n) -> bool {
        uint8_t* b = static_cast<uint8_t*>(p);
        while (n > 0) {
            ssize_t r = recv(fd, b, n, 0);
            if (r <= 0) {
                if (r < 0 && errno == EINTR) continue;
                return false;
            }
            b += r;
            n -= static_cast<size_t>(r);
        }
        return true;
    };

    // Map svbox-net shared state (read-only) so we can detect active TUN routes
    // and check UID bypass rules without per-query IPC overhead.
    std::mutex stateMu;
    std::atomic<int> activeWorkers{0};
    const sv_net_state* netState = nullptr;
    size_t netStateSize = 0;
    auto ensureNetState = [&]() -> const sv_net_state* {
        std::lock_guard<std::mutex> lk(stateMu);
        if (netState || netSockPath_.empty()) return netState;
        int nfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (nfd < 0) return nullptr;
        struct sockaddr_un sa = {};
        sa.sun_family = AF_UNIX;
        std::strncpy(sa.sun_path, netSockPath_.c_str(), sizeof(sa.sun_path) - 1);
        if (connect(nfd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) < 0) {
            close(nfd);
            return nullptr;
        }
        struct vdev_hdr req = {};
        req.magic = VDEV_PROTO_MAGIC;
        req.op = NET_OP_GET_STATE;
        if (!sendAll(nfd, &req, sizeof(req))) { close(nfd); return nullptr; }
        struct vdev_hdr rep = {};
        struct iovec iov = { &rep, sizeof(rep) };
        union { struct cmsghdr align; char data[CMSG_SPACE(sizeof(int) * 2)]; } cbuf = {};
        struct msghdr msg = {};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cbuf.data;
        msg.msg_controllen = sizeof(cbuf.data);
        ssize_t rn = recvmsg(nfd, &msg, MSG_CMSG_CLOEXEC);
        close(nfd);
        int mfd = -1;
        if (rn == static_cast<ssize_t>(sizeof(rep)) && rep.magic == VDEV_PROTO_MAGIC && rep.status == 0) {
            for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
                    c->cmsg_len >= CMSG_LEN(sizeof(int))) {
                    std::memcpy(&mfd, CMSG_DATA(c), sizeof(int));
                    break;
                }
            }
        }
        if (mfd >= 0) {
            size_t sz = rep.arg0 >= sizeof(sv_net_state) ? static_cast<size_t>(rep.arg0) : sizeof(sv_net_state);
            void* p = mmap(nullptr, sz, PROT_READ, MAP_SHARED, mfd, 0);
            close(mfd);
            if (p != MAP_FAILED) {
                const auto* st = static_cast<const sv_net_state*>(p);
                if (st->magic == SV_NET_STATE_MAGIC && st->version == SV_NET_STATE_VERSION) {
                    netState = st;
                    netStateSize = sz;
                } else {
                    munmap(p, sz);
                }
            }
        }
        return netState;
    };

    auto callerRoutedToTun = [&](int clientFd, const sv_net_state* st) -> bool {
        if (!st || !st->tun_active) return false;
        if (st->uid_mode == 0) return true;
        struct ucred uc = {};
        socklen_t ulen = sizeof(uc);
        if (getsockopt(clientFd, SOL_SOCKET, SO_PEERCRED, &uc, &ulen) != 0 || uc.pid <= 0)
            return true;
        uint32_t fakeUid = 0;
        std::string ppath = runBase_ + "/tmpfs/.pid_ids/" + std::to_string(uc.pid);
        int pfd = open(ppath.c_str(), O_RDONLY | O_CLOEXEC);
        if (pfd >= 0) {
            uint32_t ids[8] = {};
            if (read(pfd, ids, sizeof(ids)) == static_cast<ssize_t>(sizeof(ids))) {
                fakeUid = ids[1]; // effective fake UID
            }
            close(pfd);
        }
        uint32_t cnt = st->uid_rule_count > SV_NET_MAX_UID_RULES ? SV_NET_MAX_UID_RULES : st->uid_rule_count;
        bool matched = false;
        for (uint32_t i = 0; i < cnt; ++i) {
            if (fakeUid >= st->uid_ranges[i].start && fakeUid <= st->uid_ranges[i].stop) {
                matched = true;
                break;
            }
        }
        return (st->uid_mode == 1) ? matched : !matched;
    };

    auto queryRawDnsOverTun = [&](const uint8_t* wire, size_t wlen, std::vector<uint8_t>& ansOut) -> bool {
        if (netSockPath_.empty() || !wire || wlen < 12 || wlen > 4096) return false;
        int nfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (nfd < 0) return false;
        struct timeval tv = { 3, 0 };
        setsockopt(nfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(nfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        struct sockaddr_un sa = {};
        sa.sun_family = AF_UNIX;
        std::strncpy(sa.sun_path, netSockPath_.c_str(), sizeof(sa.sun_path) - 1);
        if (connect(nfd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) < 0) {
            close(nfd);
            return false;
        }
        struct sv_net_dns_query_req qreq = {};
        qreq.server_v4 = 0; // use svbox-net configured TUN DNS or 8.8.8.8
        qreq.port = 53;
        qreq.timeout_ms = 2500;
        struct vdev_hdr hdr = {};
        hdr.magic = VDEV_PROTO_MAGIC;
        hdr.op = NET_OP_DNS_QUERY;
        hdr.payload_len = static_cast<uint32_t>(sizeof(qreq) + wlen);
        bool ok = sendAll(nfd, &hdr, sizeof(hdr)) &&
                  sendAll(nfd, &qreq, sizeof(qreq)) &&
                  sendAll(nfd, wire, wlen);
        struct vdev_hdr rep = {};
        if (ok && recvAll(nfd, &rep, sizeof(rep)) &&
            rep.magic == VDEV_PROTO_MAGIC && rep.status == 0 &&
            rep.payload_len >= 12 && rep.payload_len <= 4096) {
            ansOut.resize(rep.payload_len);
            ok = recvAll(nfd, ansOut.data(), ansOut.size());
        } else {
            ok = false;
        }
        close(nfd);
        return ok;
    };

    auto queryRawDnsDirectUdp = [&](const uint8_t* wire, size_t wlen, std::vector<uint8_t>& ansOut) -> bool {
        if (!wire || wlen < 12 || wlen > 4096) return false;
        const char* servers[] = { "223.5.5.5", "8.8.8.8" };
        for (const char* srv : servers) {
            int ufd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
            if (ufd < 0) continue;
            struct timeval tv = { 2, 0 };
            setsockopt(ufd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            struct sockaddr_in sin = {};
            sin.sin_family = AF_INET;
            sin.sin_port = htons(53);
            inet_pton(AF_INET, srv, &sin.sin_addr);
            if (sendto(ufd, wire, wlen, 0, reinterpret_cast<struct sockaddr*>(&sin), sizeof(sin)) == static_cast<ssize_t>(wlen)) {
                uint8_t rbuf[4096];
                ssize_t r = recv(ufd, rbuf, sizeof(rbuf), 0);
                if (r >= 12) {
                    ansOut.assign(rbuf, rbuf + r);
                    close(ufd);
                    return true;
                }
            }
            close(ufd);
        }
        return false;
    };

    auto queryDnsOverTun = [&](const std::string& host, std::vector<uint32_t>& ipsOut) -> bool {
        if (netSockPath_.empty() || host.empty() || host.size() > 250) return false;
        static std::atomic<uint16_t> sTxId{0x1000};
        uint16_t txid = sTxId.fetch_add(1);
        std::vector<uint8_t> q;
        q.reserve(64 + host.size());
        q.push_back(static_cast<uint8_t>(txid >> 8));
        q.push_back(static_cast<uint8_t>(txid & 0xff));
        q.push_back(0x01); q.push_back(0x00); // RD=1
        q.push_back(0x00); q.push_back(0x01); // QDCOUNT=1
        q.push_back(0x00); q.push_back(0x00);
        q.push_back(0x00); q.push_back(0x00);
        q.push_back(0x00); q.push_back(0x00);
        size_t pos = 0;
        while (pos < host.size()) {
            size_t dot = host.find('.', pos);
            if (dot == std::string::npos) dot = host.size();
            size_t llen = dot - pos;
            if (llen == 0 || llen > 63) return false;
            q.push_back(static_cast<uint8_t>(llen));
            q.insert(q.end(), host.begin() + static_cast<ptrdiff_t>(pos),
                     host.begin() + static_cast<ptrdiff_t>(dot));
            pos = dot + 1;
        }
        q.push_back(0x00);
        q.push_back(0x00); q.push_back(0x01); // QTYPE=A
        q.push_back(0x00); q.push_back(0x01); // QCLASS=IN

        std::vector<uint8_t> ans;
        if (!queryRawDnsOverTun(q.data(), q.size(), ans)) return false;

        auto skipName = [&](size_t off) -> size_t {
            int hops = 0;
            while (off < ans.size() && hops++ < 64) {
                uint8_t b = ans[off];
                if (b == 0) return off + 1;
                if ((b & 0xc0u) == 0xc0u) return off + 2;
                off += 1u + b;
            }
            return ans.size() + 1;
        };
        uint16_t qdc = static_cast<uint16_t>((ans[4] << 8) | ans[5]);
        uint16_t anc = static_cast<uint16_t>((ans[6] << 8) | ans[7]);
        size_t off = 12;
        for (uint16_t i = 0; i < qdc; ++i) {
            off = skipName(off);
            if (off + 4 > ans.size()) return false;
            off += 4;
        }
        for (uint16_t i = 0; i < anc; ++i) {
            off = skipName(off);
            if (off + 10 > ans.size()) break;
            uint16_t rtype = static_cast<uint16_t>((ans[off] << 8) | ans[off + 1]);
            uint16_t rdlen = static_cast<uint16_t>((ans[off + 8] << 8) | ans[off + 9]);
            off += 10;
            if (off + rdlen > ans.size()) break;
            if (rtype == 1 && rdlen == 4) {
                uint32_t ip4 = 0;
                std::memcpy(&ip4, ans.data() + off, 4);
                ipsOut.push_back(ip4);
            }
            off += rdlen;
        }
        return !ipsOut.empty();
    };

    auto handleClient = [&](int cfd) {
        char reqBuf[1024] = {};
        ssize_t rn = recv(cfd, reqBuf, sizeof(reqBuf) - 1, 0);
        if (rn > 0) {
            reqBuf[rn] = '\0';
            std::istringstream iss(reqBuf);
            std::string cmd;
            iss >> cmd;
            if (cmd == "getaddrinfo") {
                std::string host, serv;
                int hFlags = -1, hFam = -1, hSock = -1, hProto = -1;
                iss >> host >> serv >> hFlags >> hFam >> hSock >> hProto;
                if (hFlags < 0) hFlags = 0;
                if (hFam < 0) hFam = 0;
                if (hSock < 0) hSock = 0;
                if (hProto < 0) hProto = 0;
                const char* node = (host.empty() || host == "^") ? nullptr : host.c_str();
                const char* service = (serv.empty() || serv == "^") ? nullptr : serv.c_str();

                // Check if this lookup should go through the active container TUN DNS
                // (enabling FakeIP & domain routing while preventing loops for VPN apps).
                bool isNumeric = false;
                if (node) {
                    struct in_addr a4;
                    struct in6_addr a6;
                    isNumeric = (inet_pton(AF_INET, node, &a4) == 1 || inet_pton(AF_INET6, node, &a6) == 1);
                }
                const sv_net_state* nst = ensureNetState();
                std::vector<uint32_t> tunIps;
                if (node && !isNumeric && hFam != AF_INET6 &&
                    callerRoutedToTun(cfd, nst) && queryDnsOverTun(host, tunIps)) {
                    uint16_t portBe = 0;
                    if (service) {
                        struct addrinfo ph = {}, *pr = nullptr;
                        ph.ai_family = AF_INET;
                        ph.ai_socktype = hSock;
                        ph.ai_protocol = hProto;
                        if (getaddrinfo("127.0.0.1", service, &ph, &pr) == 0 && pr) {
                            portBe = reinterpret_cast<const struct sockaddr_in*>(pr->ai_addr)->sin_port;
                            freeaddrinfo(pr);
                        }
                    }
                    int outSock = hSock ? hSock : SOCK_STREAM;
                    int outProto = hProto ? hProto : (outSock == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP);
                    const char okHdr[4] = "222";
                    sendAll(cfd, okHdr, 4);
                    for (uint32_t ip4 : tunIps) {
                        struct sockaddr_in sin = {};
                        sin.sin_family = AF_INET;
                        sin.sin_port = portBe;
                        sin.sin_addr.s_addr = ip4;
                        uint32_t rec[5] = {
                            htonl(0),
                            htonl(AF_INET),
                            htonl(static_cast<uint32_t>(outSock)),
                            htonl(static_cast<uint32_t>(outProto)),
                            htonl(static_cast<uint32_t>(sizeof(sin))),
                        };
                        uint32_t mark = htonl(1);
                        uint32_t clen = 0;
                        sendAll(cfd, &mark, 4);
                        sendAll(cfd, rec, sizeof(rec));
                        sendAll(cfd, &sin, sizeof(sin));
                        sendAll(cfd, &clen, 4);
                    }
                    uint32_t endMark = 0;
                    sendAll(cfd, &endMark, 4);
                    close(cfd);
                    return;
                }

                // Translate Android Bionic AI_* flags to HarmonyOS musl AI_* flags
                // (Bionic AI_NUMERICSERV=0x08 vs musl 0x400; Bionic AI_ADDRCONFIG=0x400 vs musl 0x20).
                int muslFlags = 0;
                if (hFlags & 0x01) muslFlags |= AI_PASSIVE;
                if (hFlags & 0x02) muslFlags |= AI_CANONNAME;
                if (hFlags & 0x04) muslFlags |= AI_NUMERICHOST;
                if (hFlags & 0x08) muslFlags |= AI_NUMERICSERV;

                struct addrinfo hints = {};
                hints.ai_flags = muslFlags;
                hints.ai_family = hFam;
                hints.ai_socktype = hSock;
                hints.ai_protocol = hProto;
                struct addrinfo* res = nullptr;
                int gai = getaddrinfo(node, service, &hints, &res);
                if (gai == 0 && res) {
                    const char okHdr[4] = "222";
                    sendAll(cfd, okHdr, 4); // exact 4 bytes: "222\0"
                    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
                        uint32_t rec[5] = {
                            htonl(0u),
                            htonl(static_cast<uint32_t>(ai->ai_family)),
                            htonl(static_cast<uint32_t>(ai->ai_socktype)),
                            htonl(static_cast<uint32_t>(ai->ai_protocol)),
                            htonl(static_cast<uint32_t>(ai->ai_addrlen)),
                        };
                        uint32_t mark = htonl(1);
                        sendAll(cfd, &mark, 4);
                        sendAll(cfd, rec, sizeof(rec));
                        if (ai->ai_addrlen > 0 && ai->ai_addr) {
                            sendAll(cfd, ai->ai_addr, ai->ai_addrlen);
                        }
                        uint32_t clen = ai->ai_canonname ? static_cast<uint32_t>(std::strlen(ai->ai_canonname) + 1) : 0u;
                        uint32_t nclen = htonl(clen);
                        sendAll(cfd, &nclen, 4);
                        if (clen > 0) sendAll(cfd, ai->ai_canonname, clen);
                    }
                    uint32_t endMark = 0;
                    sendAll(cfd, &endMark, 4);
                    freeaddrinfo(res);
                } else {
                    const char errHdr[4] = "401";
                    uint32_t errCode = htonl(7u);
                    sendAll(cfd, errHdr, 4);
                    sendAll(cfd, &errCode, 4);
                }
            } else if (cmd == "resnsend") {
                uint32_t netId = 0, flags = 0;
                std::string b64;
                iss >> netId >> flags >> b64;
                auto b64Dec = [](const std::string& in) -> std::vector<uint8_t> {
                    static const int8_t T[128] = {
                        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
                        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
                        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
                        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
                        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
                        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
                    };
                    std::vector<uint8_t> out;
                    uint32_t val = 0;
                    int valb = -8;
                    for (unsigned char c : in) {
                        if (c == '=') break;
                        if (c >= 128 || T[c] < 0) continue;
                        val = (val << 6) | static_cast<uint32_t>(T[c]);
                        valb += 6;
                        if (valb >= 0) {
                            out.push_back(static_cast<uint8_t>((val >> valb) & 0xffu));
                            valb -= 8;
                        }
                    }
                    return out;
                };
                std::vector<uint8_t> qwire = b64Dec(b64);
                std::vector<uint8_t> awire;
                const sv_net_state* nst = ensureNetState();
                bool ok = false;
                if (callerRoutedToTun(cfd, nst)) {
                    ok = queryRawDnsOverTun(qwire.data(), qwire.size(), awire);
                }
                if (!ok) {
                    ok = queryRawDnsDirectUdp(qwire.data(), qwire.size(), awire);
                }
                if (ok && awire.size() >= 12) {
                    uint32_t rcodeBe = htonl(static_cast<uint32_t>(awire[3] & 0x0fu));
                    uint32_t lenBe = htonl(static_cast<uint32_t>(awire.size()));
                    sendAll(cfd, &rcodeBe, 4);
                    sendAll(cfd, &lenBe, 4);
                    sendAll(cfd, awire.data(), awire.size());
                } else {
                    int32_t errBe = static_cast<int32_t>(htonl(static_cast<uint32_t>(-ETIMEDOUT)));
                    sendAll(cfd, &errBe, 4);
                }
            } else if (cmd == "gethostbyname") {
                uint32_t netId = 0;
                std::string host;
                int af = AF_INET;
                iss >> netId >> host >> af;
                (void)netId;
                std::vector<std::vector<uint8_t>> addrs;
                const uint32_t addrLen = (af == AF_INET6) ? 16u : 4u;
                if (!host.empty() && host != "^" && (af == AF_INET || af == AF_INET6)) {
                    uint8_t numBuf[16] = {};
                    if (inet_pton(af, host.c_str(), numBuf) == 1) {
                        addrs.emplace_back(numBuf, numBuf + addrLen);
                    } else {
                        const sv_net_state* nst = ensureNetState();
                        std::vector<uint32_t> tunIps;
                        if (af == AF_INET && callerRoutedToTun(cfd, nst) && queryDnsOverTun(host, tunIps)) {
                            for (uint32_t ip4 : tunIps) {
                                uint8_t b[4];
                                std::memcpy(b, &ip4, 4);
                                addrs.emplace_back(b, b + 4);
                            }
                        } else {
                            struct addrinfo hints = {};
                            hints.ai_family = af;
                            hints.ai_socktype = SOCK_STREAM;
                            struct addrinfo* res = nullptr;
                            if (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res) {
                                for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
                                    const uint8_t* ptr = nullptr;
                                    if (af == AF_INET && ai->ai_family == AF_INET && ai->ai_addrlen >= sizeof(struct sockaddr_in)) {
                                        ptr = reinterpret_cast<const uint8_t*>(&reinterpret_cast<const struct sockaddr_in*>(ai->ai_addr)->sin_addr);
                                    } else if (af == AF_INET6 && ai->ai_family == AF_INET6 && ai->ai_addrlen >= sizeof(struct sockaddr_in6)) {
                                        ptr = reinterpret_cast<const uint8_t*>(&reinterpret_cast<const struct sockaddr_in6*>(ai->ai_addr)->sin6_addr);
                                    }
                                    if (ptr) {
                                        std::vector<uint8_t> a(ptr, ptr + addrLen);
                                        bool dup = false;
                                        for (const auto& ex : addrs) {
                                            if (ex == a) { dup = true; break; }
                                        }
                                        if (!dup) addrs.push_back(std::move(a));
                                    }
                                }
                                freeaddrinfo(res);
                            }
                        }
                    }
                }
                if (!addrs.empty()) {
                    const char okHdr[4] = "222";
                    uint32_t nameLen = static_cast<uint32_t>(host.size() + 1);
                    uint32_t nNameLen = htonl(nameLen);
                    uint32_t zero = 0;
                    uint32_t nAf = htonl(static_cast<uint32_t>(af));
                    uint32_t nAlen = htonl(addrLen);
                    sendAll(cfd, okHdr, 4);
                    sendAll(cfd, &nNameLen, 4);
                    sendAll(cfd, host.c_str(), nameLen);
                    sendAll(cfd, &zero, 4); // 0 aliases
                    sendAll(cfd, &nAf, 4);
                    sendAll(cfd, &nAlen, 4);
                    for (const auto& a : addrs) {
                        sendAll(cfd, &nAlen, 4);
                        sendAll(cfd, a.data(), a.size());
                    }
                    sendAll(cfd, &zero, 4); // end of addresses
                } else {
                    const char errHdr[4] = "401";
                    uint32_t errCode = htonl(1u);
                    sendAll(cfd, errHdr, 4);
                    sendAll(cfd, &errCode, 4);
                }
            } else if (cmd == "getdnsnetid") {
                const char okHdr[4] = "222";
                uint32_t nidBe = htonl(100u);
                sendAll(cfd, okHdr, 4);
                sendAll(cfd, &nidBe, 4);
            } else {
                const char errHdr[4] = "401";
                uint32_t errCode = htonl(7u);
                sendAll(cfd, errHdr, 4);
                sendAll(cfd, &errCode, 4);
            }
        }
        close(cfd);
    };

    while (running_.load()) {
        struct pollfd pfd = { listenFd, POLLIN, 0 };
        if (poll(&pfd, 1, 150) <= 0) continue;
        int cfd = accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
        if (cfd < 0) continue;
        if (activeWorkers.load() < 32) {
            activeWorkers.fetch_add(1);
            std::thread([&, cfd]() {
                handleClient(cfd);
                activeWorkers.fetch_sub(1);
            }).detach();
        } else {
            handleClient(cfd);
        }
    }
    for (int i = 0; i < 300 && activeWorkers.load() > 0; ++i) {
        usleep(10000);
    }
    if (netState && netStateSize > 0) {
        munmap(const_cast<sv_net_state*>(netState), netStateSize);
    }
}

void AndroidRuntime::LmkdLoop(std::string sockPath) {
    SchedQos::SetCurrentThreadQoS(SchedQos::Level::Background, "lmkd");
    unlink(sockPath.c_str());
    int srv = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (srv < 0) return;
    struct sockaddr_un sa = {};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, sockPath.c_str(), sizeof(sa.sun_path) - 1);
    if (bind(srv, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) < 0 || listen(srv, 8) < 0) {
        close(srv);
        return;
    }
    chmod(sockPath.c_str(), 0666);

    std::vector<int> clients;
    while (running_.load()) {
        std::vector<struct pollfd> pfds;
        pfds.push_back({ srv, POLLIN, 0 });
        for (int c : clients) {
            pfds.push_back({ c, POLLIN, 0 });
        }
        int r = poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 100);
        if (r <= 0) continue;
        if (pfds[0].revents & POLLIN) {
            int c = accept4(srv, nullptr, nullptr, SOCK_CLOEXEC);
            if (c >= 0) clients.push_back(c);
        }
        for (size_t i = clients.size(); i > 0; --i) {
            if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                char sink[256];
                ssize_t n = recv(clients[i - 1], sink, sizeof(sink), MSG_DONTWAIT);
                if (n <= 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    close(clients[i - 1]);
                    clients.erase(clients.begin() + static_cast<ptrdiff_t>(i - 1));
                }
            }
        }
    }
    for (int c : clients) close(c);
    close(srv);
}

void AndroidRuntime::LogdwLoop(std::string sockPath, std::string outPath) {
    SchedQos::SetCurrentThreadQoS(SchedQos::Level::Background, "logdw");
    unlink(sockPath.c_str());
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct sockaddr_un sa = {};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, sockPath.c_str(), sizeof(sa.sun_path) - 1);
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) < 0) {
        close(fd);
        return;
    }
    chmod(sockPath.c_str(), 0666);

    std::ofstream out(outPath, std::ios::out | std::ios::trunc);
    char buf[4096];
    const char prios[] = "??VDIWEF";
    while (running_.load()) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        if (poll(&pfd, 1, 150) <= 0) continue;
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n < 13) continue;
        uint8_t logId = static_cast<uint8_t>(buf[0]);
        if (logId == 2) continue; // binary events
        uint16_t tid = 0;
        std::memcpy(&tid, buf + 1, 2);
        uint8_t prio = static_cast<uint8_t>(buf[11]);
        char pc = (prio < 8) ? prios[prio] : '?';
        buf[n] = '\0';
        const char* tag = buf + 12;
        size_t tagLen = strnlen(tag, static_cast<size_t>(n - 12));
        const char* msg = (12 + tagLen + 1 < static_cast<size_t>(n)) ? (tag + tagLen + 1) : "";
        size_t msgLen = strnlen(msg, static_cast<size_t>(n) - (12 + tagLen + 1));
        while (msgLen > 0 && (msg[msgLen - 1] == '\n' || msg[msgLen - 1] == '\r')) --msgLen;
        if (out) {
            out << "[" << pc << "/" << std::string(tag, tagLen) << "(" << tid << ")] "
                << std::string(msg, msgLen) << "\n";
            out.flush();
        }
    }
    close(fd);
}

void AndroidRuntime::BootWorker(std::string mode, int32_t width, int32_t height) {
    const std::string hnpBin = "/data/app/svbox.org/svbox_0.1.0/bin";
    const std::string binderd =
        FileExists(hnpBin + "/svbox-binderd") ? (hnpBin + "/svbox-binderd")
                                              : (androidDir_ + "/binderd/svbox-binderd");
    const std::string propd =
        FileExists(hnpBin + "/svbox-propd") ? (hnpBin + "/svbox-propd")
                                            : (androidDir_ + "/propd/svbox-propd");
    const std::string netd =
        FileExists(hnpBin + "/svbox-netd") ? (hnpBin + "/svbox-netd")
                                           : (androidDir_ + "/netd/svbox-netd");
    const std::string svboxNet =
        FileExists(hnpBin + "/svbox-net") ? (hnpBin + "/svbox-net")
                                          : "/storage/Users/currentUser/Workspace/svbox/build/svbox-net";
    const std::string sockDir = runBase_ + "/sockets";
    const std::string propDir = runBase_ + "/__properties__";
    const std::string ipcDir = runBase_ + "/ipc";

    // Ephemeral per-boot runtime directories (under sandboxTempDir)
    const char* runDirs[] = {
        "", "/sockets", "/__properties__", "/ipc", "/cache", "/acct", "/cgroup",
        "/sys_power", "/dev_input", "/dev_bus_usb", "/dev_bus_usb/001", "/cpuset", "/cpuset/foreground", "/cpuset/background",
        "/cpuset/system-background", "/cpuset/top-app", "/stune", "/stune/foreground",
        "/stune/background", "/stune/top-app", "/mnt/user/0", "/mnt/expand",
        "/mnt/runtime/default/emulated/0", "/mnt/runtime/read/emulated/0",
        "/mnt/runtime/write/emulated/0", "/mnt/runtime/full/emulated/0",
        "/mnt/pass_through/0/emulated/0", "/mnt/installer/0/emulated/0",
        "/mnt/androidwritable/0/emulated/0",
        "/storage/emulated/0", "/storage/self/primary",
        "/tmpfs"
    };
    for (const char* sub : runDirs) {
        MkdirP(runBase_ + sub);
    }

    // Rebuild ephemeral overlay upper + injection layer cleanly on every boot
    // so GPU mode switches never leave stale HALs and rootfs stays strictly read-only.
    RemoveDirRecursive(rootUpper_);
    MkdirP(rootUpper_);
    RemoveDirRecursive(injectLayer_);
    MkdirP(injectLayer_);

    // Persistent Android /data partition (under sandboxFilesDir/userdata)
    const char* dataDirs[] = {
        "", "/data", "/data/system/users/0", "/data/system/dropbox", "/data/system/usagestats",
        "/data/system_de/0", "/data/system_ce/0", "/data/misc/profiles/cur/0",
        "/data/misc/profiles/ref", "/data/misc/apexdata", "/data/misc/keystore",
        "/data/misc/gatekeeper", "/data/misc/installd", "/data/misc/user/0",
        "/data/misc/media", "/data/misc/credstore", "/data/misc/ethernet",
        "/data/misc_ce/0", "/data/misc_de/0", "/data/vendor_ce/0", "/data/vendor_de/0",
        "/data/media/0", "/data/media/0/Download", "/data/media/0/Download/HarmonyShare",
        "/data/media/0/Pictures", "/data/media/0/DCIM", "/data/media/0/DCIM/Camera",
        "/data/media/0/Documents", "/data/media/0/Movies", "/data/media/0/Music",
        "/data/media/0/Android/data", "/data/media/0/Android/obb",
        "/data/user/0", "/data/user_de/0", "/data/app", "/data/app-private",
        "/data/app-ephemeral", "/data/app-asec", "/data/app-lib", "/data/app-staging",
        "/data/data", "/data/dalvik-cache/arm64", "/data/dalvik-cache/profiles",
        "/data/resource-cache", "/data/anr", "/data/tombstones", "/data/local/tmp",
        "/data/property", "/data/apex/active", "/data/rollback", "/data/server_configurable_flags",
        "/data/backup", "/data/cache", "/data/incremental", "/data/ota", "/data/preloads",
        "/data/ss", "/data/vendor"
    };
    for (const char* sub : dataDirs) {
        MkdirP(dataBase_ + sub);
    }
    if (!shareHostDir_.empty()) {
        MkdirP(shareHostDir_);
    }

    EnsureOwnerDb(dataBase_ + "/owner.db");
    WriteEthernetStaticIpConfig(dataBase_ + "/data/misc/ethernet/ipconfig.txt");

    WriteFile(runBase_ + "/sys_power/wake_lock", "");
    WriteFile(runBase_ + "/sys_power/wake_unlock", "");
    WriteFile(runBase_ + "/sys_power/state", "mem\n");
    WriteFile(runBase_ + "/sys_power/wakeup_count", "0\n");

    for (const char* sub : { "", "/foreground", "/background", "/system-background", "/top-app" }) {
        const std::string base = runBase_ + "/cpuset" + sub;
        WriteFile(base + "/tasks", "");
        WriteFile(base + "/cgroup.procs", "");
        WriteFile(base + "/cpus", "0-7\n");
        WriteFile(base + "/mems", "0\n");
    }

    WriteFile(runBase_ + "/dev_input/event0", "");
    WriteFile(runBase_ + "/dev_input/event1", "");
    WriteFile(runBase_ + "/dev_input/event2", "");
    WriteFile(runBase_ + "/dev_bus_usb/001/002", "");
    WriteFile(runBase_ + "/resolv.conf", "nameserver 223.5.5.5\nnameserver 8.8.8.8\n");

    std::string gpuModeSnap;
    OHNativeWindow* winSnap = nullptr;
    int32_t winWSnap = 0, winHSnap = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        gpuModeSnap = activeGpuMode_;
        winSnap = surfaceWindow_;
        winWSnap = surfaceW_;
        winHSnap = surfaceH_;
    }
    const bool useGfx = (gpuModeSnap == "gles" || gpuModeSnap == "vulkan");
    const bool useVk = (gpuModeSnap == "vulkan");

    const std::string overrideProp = runBase_ + "/override.prop";
    std::string gpuProps;
    if (useGfx) {
        gpuProps =
            "ro.hardware.egl=emulation\n"
            "ro.hardware.gralloc=ranchu\n"
            "ro.hardware.hwcomposer=ranchu\n"
            "ro.hardware.vulkan=" + std::string(useVk ? "ranchu" : "pastel") + "\n"
            "ro.opengles.version=196609\n"
            "debug.hwui.renderer=" + std::string(useVk ? "skiavk" : "skiagl") + "\n"
            "debug.renderengine.backend=gles\n"
            "ro.boot.qemu.gltransport=pipe\n";
    } else {
        gpuProps =
            "ro.hardware.egl=swiftshader\n"
            "ro.hardware.gralloc=redroid\n"
            "ro.hardware.hwcomposer=redroid\n"
            "ro.hardware.vulkan=pastel\n"
            "ro.opengles.version=196610\n"
            "debug.hwui.renderer=skiagl\n"
            "debug.renderengine.backend=gles\n"
            "ro.boot.use_redroid_stream=1\n";
    }
    WriteFile(overrideProp,
        gpuProps +
        "ro.sf.lcd_density=320\n"
        "debug.sf.nobootanimation=0\n"
        "debug.stagefright.ccodec=0\n"
        "ro.boot.redroid_width=" + std::to_string(width) + "\n"
        "ro.boot.redroid_height=" + std::to_string(height) + "\n"
        "ro.boot.redroid_fps=60\n"
        "net.dns1=223.5.5.5\n"
        "net.dns2=8.8.8.8\n"
        "bpf.progs_loaded=1\n"
        "log.tag.libprocessgroup=F\n"
        "sys.use_memfd=false\n"
        "persist.zygote.app_data_isolation=0\n"
        "persist.sys.vold_app_data_isolation_enabled=false\n"
        "ro.secure=0\n"
        "ro.adb.secure=0\n"
        "ro.debuggable=1\n"
        "service.adb.root=1\n"
        "service.adb.tcp.port=5555\n"
        "persist.adb.tcp.port=5555\n");

    unlink((ipcDir + "/hwcomposer.sock").c_str());
    unlink((ipcDir + "/gfxstream.sock").c_str());
    unlink((ipcDir + "/audio.sock").c_str());
    unlink((sockDir + "/binder.sock").c_str());
    unlink((sockDir + "/hwbinder.sock").c_str());
    unlink((sockDir + "/vndbinder.sock").c_str());
    unlink((sockDir + "/property_service").c_str());
    unlink((sockDir + "/adbd").c_str());
    unlink((sockDir + "/fwmarkd").c_str());
    unlink((sockDir + "/net.sock").c_str());
    unlink((sockDir + "/mdns").c_str());
    unlink((sockDir + "/input_touch.sock").c_str());
    unlink((sockDir + "/input_kbd.sock").c_str());

    const std::string logDir = androidDir_ + "/hap_logs";
    MkdirP(logDir);
    int bootLogFd = open((logDir + "/boot.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    struct ScopedFd { int fd; ~ScopedFd() { if (fd >= 0) close(fd); } } bootLogGuard{bootLogFd};
    auto logLine = [&](const std::string& s) {
        if (bootLogFd >= 0) {
            std::string line = s + "\n";
            (void)write(bootLogFd, line.data(), line.size());
        }
        OH_LOG_INFO(LOG_APP, "%{public}s", s.c_str());
    };
    logLine("BootWorker start: runBase=" + runBase_ + " dataBase=" + dataBase_ +
            " mode=" + mode + " gpu=" + gpuModeSnap +
            " svbox=" + svboxPath_ + " binderd=" + binderd + " rootfs=" + rootfs_ +
            " gpuGuest=" + gpuGuestDir_ + " shareHost=" + shareHostDir_);
    const std::string rootfsTar = androidDir_ + "/rootfs.tar";
    if (!FileExists(rootfs_ + "/.imported_v2") && FileExists(rootfsTar)) {
        logLine("Importing Android 11 rootfs into sandbox from " + rootfsTar + " ...");
        SetStatus("Importing Android 11 rootfs into sandbox (0%)...");
        std::string errMsg, diagMsg;
        bool ok = ExtractTarToDir(
            rootfsTar, rootfs_, running_,
            [&](int pct) {
                SetStatus("Importing Android 11 rootfs into sandbox (" + std::to_string(pct) + "%)...");
            },
            errMsg, diagMsg);
        if (!ok) {
            logLine("Rootfs import failed: " + errMsg);
            SetStatus("Rootfs import failed: " + errMsg);
            running_.store(false);
            return;
        }
        WriteFile(rootfs_ + "/.imported_v2", "ok\n");
        logLine("Rootfs import complete -> " + rootfs_ + " (" + diagMsg + ")");
    }
    // One-time cleanup of stale injected files left in sandbox rootfs by pre-Step-5 builds.
    // Afterwards rootfs_ is treated as strictly read-only and never modified again.
    static const char* kGuestGpuLibs[] = {
        "libandroidemu.so",
        "libOpenglCodecCommon.so",
        "lib_renderControl_enc.so",
        "libGLESv1_enc.so",
        "libGLESv2_enc.so",
        "libvulkan_enc.so",
        "libOpenglSystemCommon.so",
        "libGoldfishProfiler.so",
        "egl/libEGL_emulation.so",
        "egl/libGLESv1_CM_emulation.so",
        "egl/libGLESv2_emulation.so",
        "hw/gralloc.ranchu.so",
        "hw/hwcomposer.ranchu.so",
        "hw/vulkan.ranchu.so",
    };
    const std::string cleanMarker = sandboxFiles_ + "/.rootfs_clean_v1";
    if (!FileExists(cleanMarker)) {
        int removed = 0;
        for (const char* rel : kGuestGpuLibs) {
            if (unlink((rootfs_ + "/vendor/lib64/" + rel).c_str()) == 0) ++removed;
        }
        if (unlink((rootfs_ + "/vendor/bin/hw/svbox-audio-service").c_str()) == 0) ++removed;
        if (unlink((rootfs_ + "/system/bin/svbox-cameraserver").c_str()) == 0) ++removed;
        WriteFile(cleanMarker, "cleaned=" + std::to_string(removed) + "\n");
        logLine("One-time rootfs cleanup removed " + std::to_string(removed) + " legacy injected files");
    }
    // Android 11 Bionic linker64 rejects group/world-writable ld.config.txt (mode & 0022).
    // Only fix permissions if actually group/world-writable so normal boots do zero writes to rootfs_.
    auto ensureNotGroupWorldWritable = [](const std::string& p, mode_t targetMode) {
        struct stat st = {};
        if (stat(p.c_str(), &st) == 0 && (st.st_mode & 0022) != 0) {
            chmod(p.c_str(), targetMode);
        }
    };
    ensureNotGroupWorldWritable(rootfs_ + "/linkerconfig", 0755);
    ensureNotGroupWorldWritable(rootfs_ + "/linkerconfig/ld.config.txt", 0644);
    for (const char* sub : { "com.android.adbd", "com.android.art", "com.android.conscrypt",
                             "com.android.media.swcodec", "com.android.os.statsd",
                             "com.android.runtime", "com.android.sdkext" }) {
        ensureNotGroupWorldWritable(rootfs_ + "/linkerconfig/" + sub, 0755);
        ensureNotGroupWorldWritable(rootfs_ + "/linkerconfig/" + sub + "/ld.config.txt", 0644);
    }
    {
        struct stat st1 = {}, st2 = {};
        int r1 = stat((rootfs_ + "/linkerconfig/ld.config.txt").c_str(), &st1);
        int r2 = stat((rootfs_ + "/system/apex/com.android.os.statsd/lib64/libstatssocket.so").c_str(), &st2);
        logLine("ld.config stat=" + std::to_string(r1) + " size=" + std::to_string(st1.st_size) +
                " mode=" + std::to_string(st1.st_mode) +
                " | statsd stat=" + std::to_string(r2) + " size=" + std::to_string(st2.st_size));
    }
    {
        char lbuf[256] = {};
        ssize_t lr = readlink((rootfs_ + "/system/bin/linker64").c_str(), lbuf, sizeof(lbuf) - 1);
        int lerr = errno;
        struct stat lst = {};
        int sr = lstat((rootfs_ + "/system/bin/linker64").c_str(), &lst);
        int serr = errno;
        logLine("linker64 probe: readlink=" + std::to_string(lr) + " (err=" + std::to_string(lerr) +
                " val=" + std::string(lr > 0 ? lbuf : "") + ") lstat=" + std::to_string(sr) +
                " (err=" + std::to_string(serr) + " mode=" + std::to_string(lst.st_mode) + ")");
    }

    SetStatus("Starting propd & binder daemons...");

    // 1. Start propd
    std::vector<std::string> propArgs = { propd, "--dir", propDir, "--sock", sockDir + "/property_service" };
    for (const std::string& pf : { rootfs_ + "/system/etc/prop.default",
                                   rootfs_ + "/default.prop",
                                   rootfs_ + "/system/build.prop",
                                   rootfs_ + "/vendor/build.prop",
                                   rootfs_ + "/vendor/default.prop",
                                   overrideProp }) {
        if (FileExists(pf)) propArgs.push_back(pf);
    }
    pid_t pidPropd = Spawn(propArgs, false, logDir + "/propd.log");

    // 2. Start userspace Binder routers & generic virtual network stack (svbox-net)
    pid_t pidBinder = Spawn({ binderd, sockDir + "/binder.sock" }, false, logDir + "/binderd.log");
    Spawn({ binderd, sockDir + "/hwbinder.sock" }, false, logDir + "/hwbinderd.log");
    Spawn({ binderd, sockDir + "/vndbinder.sock" }, false, logDir + "/vndbinderd.log");
    netSockPath_.clear();
    if (FileExists(svboxNet)) {
        Spawn({ svboxNet,
                "--sock", sockDir + "/net.sock",
                "--iface", "eth0:10.0.2.15/24:10.0.2.2:02:00:00:00:00:01",
                "--dns", "223.5.5.5",
                "--log-level", "2" },
              false, logDir + "/svbox_net.log");
        if (WaitForFile(sockDir + "/net.sock", 2000)) {
            netSockPath_ = sockDir + "/net.sock";
        }
    }

    bool okBinder = WaitForFile(sockDir + "/binder.sock", 2000);
    bool okHwBinder = WaitForFile(sockDir + "/hwbinder.sock", 2000);
    bool okProp = WaitForFile(sockDir + "/property_service", 2000);
    logLine("Sockets ready: binder=" + std::to_string(okBinder) +
            " hwbinder=" + std::to_string(okHwBinder) +
            " prop=" + std::to_string(okProp) +
            " net=" + std::to_string(!netSockPath_.empty()));
    if (!running_.load()) return;

    if (!okBinder || !okProp) {
        int stB = 0, stP = 0;
        pid_t rB = pidBinder > 0 ? waitpid(pidBinder, &stB, WNOHANG) : 0;
        pid_t rP = pidPropd > 0 ? waitpid(pidPropd, &stP, WNOHANG) : 0;
        std::string errMsg = "Daemon startup failed (binder=" + std::to_string(okBinder) +
                             ", prop=" + std::to_string(okProp) +
                             ", bExit=" + std::to_string(rB > 0 ? WEXITSTATUS(stB) : -1) +
                             ", pExit=" + std::to_string(rP > 0 ? WEXITSTATUS(stP) : -1) +
                             ") — see hap_logs/";
        logLine(errMsg);
        SetStatus(errMsg);
        return;
    }

    // Stage all Android-compat injected HALs/services/configs into ephemeral injectLayer_
    // (mounted as svbox --lower #1 above strictly read-only rootfs_ as --lower #2).
    {
        MkdirP(injectLayer_ + "/system/usr/idc");
        const std::string idcDst = injectLayer_ + "/system/usr/idc/svbox-touchscreen.idc";
        WriteFile(idcDst,
            "touch.deviceType = touchScreen\n"
            "touch.orientationAware = 1\n"
            "device.internal = 1\n");
        chmod(idcDst.c_str(), 0644);

        if (!audioHalPath_.empty() && FileExists(audioHalPath_)) {
            MkdirP(injectLayer_ + "/vendor/lib64/hw");
            const std::string dst = injectLayer_ + "/vendor/lib64/hw/audio.primary.default.so";
            if (CopyRegularFile(audioHalPath_, dst)) {
                chmod(dst.c_str(), 0644);
            }
        }
        if (!audioSvcPath_.empty() && FileExists(audioSvcPath_)) {
            MkdirP(injectLayer_ + "/vendor/bin/hw");
            const std::string dst1 = injectLayer_ + "/vendor/bin/hw/svbox-audio-service";
            const std::string dst2 = injectLayer_ + "/vendor/bin/hw/android.hardware.audio.service";
            if (CopyRegularFile(audioSvcPath_, dst1)) chmod(dst1.c_str(), 0755);
            if (CopyRegularFile(audioSvcPath_, dst2)) chmod(dst2.c_str(), 0755);
        }
        if (!cameraSvcPath_.empty() && FileExists(cameraSvcPath_)) {
            MkdirP(injectLayer_ + "/system/bin");
            const std::string dst = injectLayer_ + "/system/bin/svbox-cameraserver";
            if (CopyRegularFile(cameraSvcPath_, dst)) {
                chmod(dst.c_str(), 0755);
            }
        }
        int gpuCopied = 0;
        if (useGfx) {
            if (gpuGuestDir_.empty()) {
                logLine("Missing gpu_guest libraries for hardware GPU mode!");
                SetStatus("Missing gpu_guest libraries in HNP");
                running_.store(false);
                return;
            }
            MkdirP(injectLayer_ + "/vendor/lib64/egl");
            MkdirP(injectLayer_ + "/vendor/lib64/hw");
            for (const char* rel : kGuestGpuLibs) {
                const std::string src = gpuGuestDir_ + "/" + rel;
                const std::string dst = injectLayer_ + "/vendor/lib64/" + rel;
                if (!FileExists(src)) {
                    logLine("Missing guest GPU lib: " + src);
                    SetStatus("Missing guest GPU lib: " + std::string(rel));
                    running_.store(false);
                    return;
                }
                if (!CopyRegularFile(src, dst)) {
                    logLine("Failed copying guest GPU lib: " + src + " -> " + dst);
                    SetStatus("Failed staging guest GPU lib");
                    running_.store(false);
                    return;
                }
                chmod(dst.c_str(), 0644);
                ++gpuCopied;
            }
        }
        logLine("Staged injectLayer=" + injectLayer_ + " (gpuLibs=" + std::to_string(gpuCopied) +
                ", audioHal=" + std::to_string(!audioHalPath_.empty()) +
                ", cameraStub=" + std::to_string(!cameraSvcPath_.empty()) + ")");
    }

    // Start Layer 2 ohos_bridge services (display, evdev touch/kbd/gamepad, OHAudio, USB/serial)
    logdwThread_ = std::thread(&AndroidRuntime::LogdwLoop, this, sockDir + "/logdw", logDir + "/logcat.txt");
    InputBridge::Instance().Start(sockDir + "/input_touch.sock", sockDir + "/input_kbd.sock",
                                  width, height, sockDir + "/input_gamepad.sock", true);
    AudioBridge::Instance().Start(ipcDir + "/audio.sock");
    UsbBridge::Instance().Start(sockDir + "/usb_001_002.sock", sockDir + "/ttyusb0.sock");
    if (useGfx) {
        HwcRenderer::Instance().ClearWindow();
        if (winSnap) {
            GfxstreamBridge::Instance().SetWindow(winSnap, winWSnap, winHSnap);
        }
        const SvboxGfxMode gfxMode = useVk ? SVBOX_GFX_MODE_VULKAN : SVBOX_GFX_MODE_GLES;
        if (!GfxstreamBridge::Instance().Start(ipcDir + "/gfxstream.sock", width, height, gfxMode)) {
            const std::string err = GfxstreamBridge::Instance().LastError();
            logLine("GfxstreamBridge start failed: " + err);
            SetStatus("GPU init failed: " + err);
            running_.store(false);
            return;
        }
        logLine("GfxstreamBridge started: " + GfxstreamBridge::Instance().RendererName() +
                " (mode=" + gpuModeSnap + ")");
    } else {
        GfxstreamBridge::Instance().ClearWindow();
        if (winSnap) {
            HwcRenderer::Instance().SetWindow(winSnap, winWSnap, winHSnap);
        }
        HwcRenderer::Instance().Start(ipcDir + "/hwcomposer.sock", width, height);
    }
    WaitForFile(sockDir + "/input_touch.sock", 500);
    WaitForFile(sockDir + "/input_kbd.sock", 500);
    WaitForFile(sockDir + "/input_gamepad.sock", 500);
    WaitForFile(sockDir + "/usb_001_002.sock", 500);
    WaitForFile(sockDir + "/ttyusb0.sock", 500);

    // 3. Start servicemanager & hwservicemanager
    auto runSvbox = [&](const std::string& tag,
                        const std::vector<std::string>& guestCmd,
                        bool nullStdio = false,
                        const std::vector<int>& keepFds = {}) {
        auto args = BaseSvboxArgs();
        args.insert(args.end(), guestCmd.begin(), guestCmd.end());
        return Spawn(args, nullStdio, nullStdio ? "" : (logDir + "/" + tag + ".log"), keepFds);
    };

    SetStatus("Starting servicemanager & graphics HALs...");
    runSvbox("servicemanager", { "system/bin/servicemanager" });
    if (!SleepCancelableMs(80)) return;
    runSvbox("hwservicemanager", { "system/bin/hwservicemanager" });
    if (!SleepCancelableMs(150)) return;

    const std::string sfLdPath =
        "LD_LIBRARY_PATH=/system/lib64:/system_ext/lib64:/product/lib64:"
        "/vendor/lib64:/vendor/lib64/egl:/vendor/lib64/hw:"
        "/apex/com.android.vndk.v30/lib64:/apex/com.android.vndk.current/lib64:"
        "/apex/com.android.art/lib64:/apex/com.android.adbd/lib64:"
        "/apex/com.android.runtime/lib64:/apex/com.android.runtime/lib64/bionic:"
        "/apex/com.android.os.statsd/lib64:/apex/com.android.conscrypt/lib64:"
        "/apex/com.android.i18n/lib64:/apex/com.android.media/lib64:"
        "/apex/com.android.media.swcodec/lib64:/apex/com.android.neuralnetworks/lib64:"
        "/apex/com.android.resolv/lib64";

    // 4. Graphics HALs
    runSvbox("hidl_alloc", { "-e", sfLdPath, "system/bin/hw/android.hidl.allocator@1.0-service" });
    if (!SleepCancelableMs(40)) return;
    runSvbox("gralloc", { "-e", sfLdPath, "vendor/bin/hw/android.hardware.graphics.allocator@2.0-service" });
    if (!SleepCancelableMs(60)) return;
    runSvbox("hwcomposer", { "-e", sfLdPath, "vendor/bin/hw/android.hardware.graphics.composer@2.1-service" });
    if (useGfx) {
        if (!SleepCancelableMs(180)) return;
    } else {
        WaitForFile(ipcDir + "/hwcomposer.sock", 1500);
    }
    if (!running_.load()) return;

    // 5. SurfaceFlinger
    SetStatus("Starting SurfaceFlinger (" + gpuModeSnap + ")...");
    runSvbox("surfaceflinger", { "-e", sfLdPath, "system/bin/surfaceflinger" });
    if (useGfx) {
        WaitForConditionMs(3000, []() { return GfxstreamBridge::Instance().IsConnected(); });
    } else {
        WaitForConditionMs(1500, []() { return HwcRenderer::Instance().IsConnected(); });
    }
    if (!running_.load()) return;

    if (mode == "bootanim") {
        SetStatus("Running Android 11 BootAnimation -> XComponent (Async VSync)");
        runSvbox("bootanimation", { "-e", sfLdPath, "system/bin/bootanimation" });
    } else {
        SetStatus("Starting Android 11 services + Zygote64...");
        lmkdThread_ = std::thread(&AndroidRuntime::LmkdLoop, this, sockDir + "/lmkd");
        // Keep /dev/socket/mdns listening so SystemServer's NsdService.awaitConnection() unblocks
        // immediately on connect(). Do NOT create an unserved /dev/socket/fwmarkd: Android Bionic
        // libnetd_client.so connects to fwmarkd on accept4()/socket() and blocks forever in
        // recvfrom() if nothing answers (hanging adbd and any guest TCP server).
        int dnsFd = MakeListenUnix(sockDir + "/dnsproxyd");
        int mdnsFd = MakeListenUnix(sockDir + "/mdns");
        {
            std::lock_guard<std::mutex> lk(procMu_);
            if (dnsFd >= 0) {
                ownedFds_.push_back(dnsFd);
                dnsListenFd_ = dnsFd;
            }
            if (mdnsFd >= 0) ownedFds_.push_back(mdnsFd);
        }
        if (dnsFd >= 0) {
            dnsThread_ = std::thread(&AndroidRuntime::DnsProxyLoop, this, dnsFd);
        }
        const bool useRealAudio = !audioHalPath_.empty() && FileExists(audioHalPath_) &&
                                  !audioSvcPath_.empty() && FileExists(audioSvcPath_);
        if (FileExists(netd)) {
            std::vector<std::string> netdArgs = { netd, sockDir + "/binder.sock" };
            if (!netSockPath_.empty()) {
                netdArgs.push_back("--net-sock");
                netdArgs.push_back(netSockPath_);
                netdArgs.push_back("--fwmark-sock");
                netdArgs.push_back(sockDir + "/fwmarkd");
            }
            if (useRealAudio) netdArgs.push_back("--no-audio-stubs");
            Spawn(netdArgs, false, logDir + "/netd.log");
            if (!netSockPath_.empty()) {
                WaitForFile(sockDir + "/fwmarkd", 1000);
            }
        }
        auto launchPaced = [&](const std::string& tag, const std::vector<std::string>& cmd) {
            runSvbox(tag, cmd);
            SleepCancelableMs(25);
        };
        if (useRealAudio) {
            launchPaced("audio_hal", { "-e", sfLdPath, "vendor/bin/hw/svbox-audio-service" });
            SleepCancelableMs(150);
            launchPaced("audioserver", { "-e", sfLdPath, "system/bin/audioserver" });
        }
        if (!cameraSvcPath_.empty() && FileExists(cameraSvcPath_)) {
            launchPaced("cameraserver", { "-e", sfLdPath, "system/bin/svbox-cameraserver" });
        }
        // HarmonyOS PC enforces W^X (denies anonymous PROT_EXEC for unsigned HAPs),
        // so configure Chromium WebView V8 to run --jitless instead of crashing on CodeRange mmap.
        {
            const std::string wvFlags = dataBase_ + "/data/local/tmp/webview-command-line";
            WriteFile(wvFlags, "_ --js-flags=--jitless\n");
            chmod(wvFlags.c_str(), 0644);
        }
        // Native Android 11 64-bit adbd on TCP 127.0.0.1:5555 (also serves @jdwp-control
        // before zygote64 starts so apps/system_server never spam JDWP connect errors).
        // Exec via sh with an absolute path so Bionic linker64 matches
        // /linkerconfig/com.android.adbd/ld.config.txt (dir.com.android.adbd = /apex/com.android.adbd/bin)
        // cleanly without polluting LD_LIBRARY_PATH for child adb shell commands.
        if (FileExists(rootfs_ + "/system/apex/com.android.adbd/bin/adbd")) {
            int adbdFd = MakeListenUnix(sockDir + "/adbd", SOCK_SEQPACKET, 0666);
            if (adbdFd >= 0) {
                std::lock_guard<std::mutex> lk(procMu_);
                ownedFds_.push_back(adbdFd);
            }
            std::vector<std::string> adbdCmd;
            if (adbdFd >= 0) {
                adbdCmd.push_back("-e");
                adbdCmd.push_back("ANDROID_SOCKET_adbd=" + std::to_string(adbdFd));
            }
            adbdCmd.push_back("--");
            adbdCmd.push_back("system/bin/sh");
            adbdCmd.push_back("-c");
            adbdCmd.push_back("exec /apex/com.android.adbd/bin/adbd --root_seclabel=u:r:su:s0");
            runSvbox("adbd", adbdCmd, false,
                     adbdFd >= 0 ? std::vector<int>{ adbdFd } : std::vector<int>{});
            SleepCancelableMs(80);
        }
        launchPaced("installd", { "-e", sfLdPath, "system/bin/installd" });
        launchPaced("storaged", { "-e", sfLdPath, "system/bin/storaged" });
        launchPaced("idmap2d", { "-e", sfLdPath, "system/bin/idmap2d" });
        launchPaced("keystore", { "-e", sfLdPath, "system/bin/keystore", "/data/misc/keystore" });
        // Launch vold without /storage/* sub-mounts in its /proc/mounts view so vold's
        // startup unmountAll() does not waste 5s/mount trying to ForceUnmount them.
        {
            auto voldArgs = BaseSvboxArgs(/*includeStorageSubmounts=*/false);
            const std::vector<std::string> voldCmd = {
                "-e", sfLdPath, "system/bin/vold",
                "--blkid_context=u:r:blkid:s0",
                "--blkid_untrusted_context=u:r:blkid_untrusted:s0",
                "--fsck_context=u:r:fsck:s0",
                "--fsck_untrusted_context=u:r:fsck_untrusted:s0"
            };
            voldArgs.insert(voldArgs.end(), voldCmd.begin(), voldCmd.end());
            Spawn(voldArgs, false, logDir + "/vold.log");
            SleepCancelableMs(25);
        }
        launchPaced("suspend", { "-e", sfLdPath, "system/bin/hw/android.system.suspend@1.0-service" });
        launchPaced("power_stats", { "-e", sfLdPath, "vendor/bin/hw/android.hardware.power.stats@1.0-service.mock" });
        launchPaced("power", { "-e", sfLdPath, "vendor/bin/hw/android.hardware.power-service.example" });
        launchPaced("health", { "-e", sfLdPath, "vendor/bin/hw/android.hardware.health@2.1-service" });
        launchPaced("gatekeeper", { "-e", sfLdPath, "vendor/bin/hw/android.hardware.gatekeeper@1.0-service.software" });
        launchPaced("gatekeeperd", { "-e", sfLdPath, "system/bin/gatekeeperd", "/data/misc/gatekeeper" });
        launchPaced("keymaster", { "-e", sfLdPath, "vendor/bin/hw/android.hardware.keymaster@3.0-service" });
        launchPaced("sensors", { "-e", sfLdPath, "vendor/bin/hw/android.hardware.sensors@2.1-service.mock" });
        launchPaced("gnss", { "-e", sfLdPath, "vendor/bin/hw/android.hardware.gnss@2.1-service" });
        launchPaced("wifi", { "-e", sfLdPath, "vendor/bin/hw/android.hardware.wifi@1.0-service" });
        launchPaced("bt", { "-e", sfLdPath, "vendor/bin/hw/android.hardware.bluetooth@1.1-service.sim" });
        launchPaced("bootanimation", { "-e", sfLdPath, "system/bin/bootanimation" });
        if (!SleepCancelableMs(100)) return;

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
        runSvbox("zygote64", zygCmd, true, { zfd, ufd });
        SetStatus("Booting Full Android 11 (Zygote64 -> system_server -> Launcher3)...");
    }

    size_t procCount = 0;
    {
        std::lock_guard<std::mutex> lk(procMu_);
        procCount = pids_.size();
    }
    logLine("AndroidRuntime started (mode=" + mode + ", procs=" + std::to_string(procCount) + ")");
}

std::string AndroidRuntime::Stop() {
    running_.store(false);
    paused_.store(false);
    HwcRenderer::Instance().Stop();
    InputBridge::Instance().Stop();
    AudioBridge::Instance().Stop();
    UsbBridge::Instance().Stop();
    if (dnsListenFd_ >= 0) {
        shutdown(dnsListenFd_, SHUT_RDWR);
        dnsListenFd_ = -1;
    }
    if (bootThread_.joinable()) {
        bootThread_.join();
    }
    if (lmkdThread_.joinable()) {
        lmkdThread_.join();
    }
    if (logdwThread_.joinable()) {
        logdwThread_.join();
    }
    if (dnsThread_.joinable()) {
        dnsThread_.join();
    }

    std::vector<pid_t> pidsCopy;
    std::vector<int> fdsCopy;
    {
        std::lock_guard<std::mutex> lk(procMu_);
        pidsCopy.swap(pids_);
        fdsCopy.swap(ownedFds_);
    }

    // Wake any SIGSTOP-paused process groups first, then SIGTERM -> SIGKILL the entire tree
    for (auto it = pidsCopy.rbegin(); it != pidsCopy.rend(); ++it) {
        if (*it > 0) {
            kill(-(*it), SIGCONT);
            kill(*it, SIGCONT);
            kill(-(*it), SIGTERM);
            kill(*it, SIGTERM);
        }
    }
    if (!pidsCopy.empty()) {
        usleep(120000);
    }
    for (auto it = pidsCopy.rbegin(); it != pidsCopy.rend(); ++it) {
        if (*it > 0) {
            kill(-(*it), SIGKILL);
            kill(*it, SIGKILL);
            int st = 0;
            waitpid(*it, &st, WNOHANG);
        }
    }
    for (int fd : fdsCopy) {
        if (fd >= 0) close(fd);
    }
    GfxstreamBridge::Instance().Stop();
    SetStatus("Stopped");
    return "Stopped";
}

bool AndroidRuntime::RunGuestCmd(const std::string& shCmd, const std::string& outPath) {
    if (!running_.load() || shCmd.empty()) return false;
    std::vector<std::string> argv = BaseSvboxArgs();
    argv.push_back("--");
    argv.push_back("system/bin/sh");
    argv.push_back("-c");
    argv.push_back(shCmd);
    std::string logOut = outPath.empty() ? (androidDir_ + "/hap_logs/guest_cmd.log") : outPath;
    std::thread([this, argv, logOut]() {
        pid_t p = Spawn(argv, false, logOut);
        if (p > 0) {
            int st = 0;
            waitpid(p, &st, 0);
        }
    }).detach();
    return true;
}

bool AndroidRuntime::DumpProcs(const std::string& outPath) {
    std::string dest = outPath.empty() ? (androidDir_ + "/hap_logs/proc_dump.log") : outPath;
    int fd = open(dest.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    auto writeStr = [&](const std::string& s) {
        if (!s.empty()) (void)write(fd, s.data(), s.size());
    };
    auto readSmall = [](const std::string& p) -> std::string {
        int rfd = open(p.c_str(), O_RDONLY | O_CLOEXEC);
        if (rfd < 0) return "";
        char buf[4096];
        ssize_t n = read(rfd, buf, sizeof(buf) - 1);
        close(rfd);
        if (n <= 0) return "";
        buf[n] = '\0';
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == '\0') buf[i] = ' ';
        }
        return std::string(buf, static_cast<size_t>(n));
    };

    uid_t myUid = getuid();
    DIR* pd = opendir("/proc");
    if (pd) {
        while (struct dirent* de = readdir(pd)) {
            if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
            std::string pdir = std::string("/proc/") + de->d_name;
            struct stat st = {};
            if (stat(pdir.c_str(), &st) != 0 || st.st_uid != myUid) continue;
            std::string comm = readSmall(pdir + "/comm");
            while (!comm.empty() && (comm.back() == '\n' || comm.back() == ' ')) comm.pop_back();
            std::string cmd = readSmall(pdir + "/cmdline");
            if (cmd.size() > 160) cmd.resize(160);
            writeStr("=== PID " + std::string(de->d_name) + " comm=" + comm + " cmd=" + cmd + "\n");
            DIR* td = opendir((pdir + "/task").c_str());
            if (td) {
                while (struct dirent* te = readdir(td)) {
                    if (te->d_name[0] < '0' || te->d_name[0] > '9') continue;
                    std::string tdir = pdir + "/task/" + te->d_name;
                    std::string tcomm = readSmall(tdir + "/comm");
                    while (!tcomm.empty() && (tcomm.back() == '\n' || tcomm.back() == ' ')) tcomm.pop_back();
                    std::string wchan = readSmall(tdir + "/wchan");
                    while (!wchan.empty() && (wchan.back() == '\n' || wchan.back() == ' ')) wchan.pop_back();
                    std::string statStr = readSmall(tdir + "/stat");
                    char state = '?';
                    auto rp = statStr.rfind(')');
                    if (rp != std::string::npos && rp + 2 < statStr.size()) state = statStr[rp + 2];
                    std::string sysc = readSmall(tdir + "/syscall");
                    while (!sysc.empty() && (sysc.back() == '\n' || sysc.back() == ' ')) sysc.pop_back();
                    if (sysc.size() > 96) sysc.resize(96);
                    writeStr("  TID " + std::string(te->d_name) + " [" + std::string(1, state) + "] " +
                             tcomm + " wchan=" + wchan + " sys=" + sysc + "\n");
                }
                closedir(td);
            }
        }
        closedir(pd);
    }
    close(fd);
    return true;
}
