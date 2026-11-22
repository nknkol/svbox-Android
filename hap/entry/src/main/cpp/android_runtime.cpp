/* SPDX-License-Identifier: MIT */
/* In-HAP Android 11 container supervisor:
 * Launches svbox-propd, svbox-binderd, servicemanager, hwservicemanager,
 * graphics HALs, SurfaceFlinger, and either BootAnimation demo or full
 * Android 11 (Zygote64 -> system_server) using svbox. */
#include "android_runtime.h"
#include "hwc_renderer.h"

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <hilog/log.h>
#include <poll.h>
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

void InitOwnerDb(const std::string& path) {
    unlink(path.c_str());
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    struct {
        char magic[8];
        uint32_t capacity;
        uint32_t count;
        uint32_t lock;
        uint32_t version;
        uint8_t pad[40];
    } hdr = {};
    std::memcpy(hdr.magic, "ELFOWN1\0", 8);
    hdr.capacity = 65536u;
    (void)write(fd, &hdr, sizeof(hdr));
    (void)ftruncate(fd, static_cast<off_t>(sizeof(hdr) + 65536u * 32u));
    close(fd);
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
            if (mode) chmod(full.c_str(), mode | 0700);
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

std::vector<std::string> AndroidRuntime::BaseSvboxArgs() const {
    const std::string sockDir = runBase_ + "/sockets";
    return {
        svboxPath_,
        "--root", rootfs_,
        "-u", "0:0",
        "--cap-add", "ALL",
        "--kernel-sysname", "Linux",
        "--kernel-release", "5.10.0-android11",
        "--hostname", "localhost",
        "--emulate-mounts",
        "--mount-scratch", runBase_ + "/tmpfs",
        "--owner-db", runBase_ + "/owner.db",
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
        "-v", rootfs_ + "/system/product:/product",
        "-v", rootfs_ + "/system/system_ext:/system_ext",
        "-v", runBase_ + "/__properties__:/dev/__properties__",
        "-v", sockDir + ":/dev/socket",
        "-v", runBase_ + "/dev_input:/dev/input",
        "-v", runBase_ + "/ipc:/ipc",
        "-v", runBase_ + "/data:/data",
        "-v", runBase_ + "/data/data:/data/user/0",
        "-v", runBase_ + "/data/user:/data_mirror/data_ce/null",
        "-v", runBase_ + "/data/data:/data_mirror/data_ce/null/0",
        "-v", runBase_ + "/data/user_de:/data_mirror/data_de/null",
        "-v", runBase_ + "/data/misc/profiles/cur:/data_mirror/cur_profiles",
        "-v", runBase_ + "/data/misc/profiles/ref:/data_mirror/ref_profiles",
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

    const std::string hnpBin = "/data/app/svbox.org/svbox_0.1.0/bin";
    const std::string resolvedSvbox =
        FileExists(hnpBin + "/svbox") ? (hnpBin + "/svbox") : svboxPath;
    const std::string binderd =
        FileExists(hnpBin + "/svbox-binderd") ? (hnpBin + "/svbox-binderd")
                                              : (androidDir + "/binderd/svbox-binderd");
    const std::string propd =
        FileExists(hnpBin + "/svbox-propd") ? (hnpBin + "/svbox-propd")
                                            : (androidDir + "/propd/svbox-propd");
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

    runBase_ = sandboxTempDir + "/svbox-android-hap";
    svboxPath_ = resolvedSvbox;
    androidDir_ = androidDir;
    rootfs_ = rootfs;

    running_.store(true);
    SetStatus("Booting Android 11 (" + mode + ")...");
    bootThread_ = std::thread(&AndroidRuntime::BootWorker, this, mode, width, height);
    return StatusText();
}

void AndroidRuntime::LmkdLoop(std::string sockPath) {
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
    const std::string sockDir = runBase_ + "/sockets";
    const std::string propDir = runBase_ + "/__properties__";
    const std::string ipcDir = runBase_ + "/ipc";

    const char* dirs[] = {
        "", "/sockets", "/__properties__", "/ipc", "/cache", "/acct", "/cgroup",
        "/sys_power", "/dev_input", "/cpuset", "/cpuset/foreground", "/cpuset/background",
        "/cpuset/system-background", "/cpuset/top-app", "/stune", "/stune/foreground",
        "/stune/background", "/stune/top-app", "/mnt/user/0", "/mnt/expand",
        "/mnt/runtime/default/emulated/0", "/mnt/runtime/read/emulated/0",
        "/mnt/runtime/write/emulated/0", "/mnt/runtime/full/emulated/0",
        "/mnt/pass_through/0/emulated/0", "/mnt/installer/0/emulated/0",
        "/mnt/androidwritable/0/emulated/0", "/storage/emulated/0",
        "/tmpfs", "/data/system/users/0", "/data/system/dropbox", "/data/system/usagestats",
        "/data/system_de/0", "/data/system_ce/0", "/data/misc/profiles/cur/0",
        "/data/misc/profiles/ref", "/data/misc/apexdata", "/data/misc/keystore",
        "/data/misc/gatekeeper", "/data/misc/installd", "/data/misc/user/0",
        "/data/misc/media", "/data/misc/credstore",
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

    InitOwnerDb(runBase_ + "/owner.db");

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

    const std::string overrideProp = runBase_ + "/override.prop";
    WriteFile(overrideProp,
        "ro.hardware.egl=swiftshader\n"
        "ro.hardware.gralloc=redroid\n"
        "ro.hardware.vulkan=pastel\n"
        "ro.opengles.version=196610\n"
        "ro.sf.lcd_density=320\n"
        "debug.renderengine.backend=gles\n"
        "debug.sf.nobootanimation=0\n"
        "debug.stagefright.ccodec=0\n"
        "ro.boot.redroid_width=" + std::to_string(width) + "\n"
        "ro.boot.redroid_height=" + std::to_string(height) + "\n"
        "ro.boot.redroid_fps=30\n"
        "ro.boot.use_redroid_stream=1\n"
        "bpf.progs_loaded=1\n"
        "log.tag.libprocessgroup=F\n"
        "sys.use_memfd=false\n"
        "persist.zygote.app_data_isolation=0\n"
        "persist.sys.vold_app_data_isolation_enabled=false\n");

    unlink((ipcDir + "/hwcomposer.sock").c_str());
    unlink((sockDir + "/binder.sock").c_str());
    unlink((sockDir + "/hwbinder.sock").c_str());
    unlink((sockDir + "/vndbinder.sock").c_str());
    unlink((sockDir + "/property_service").c_str());

    const std::string logDir = androidDir_ + "/hap_logs";
    MkdirP(logDir);
    std::ofstream bootLog(logDir + "/boot.log", std::ios::out | std::ios::trunc);
    auto logLine = [&](const std::string& s) {
        if (bootLog) {
            bootLog << s << "\n";
            bootLog.flush();
        }
        OH_LOG_INFO(LOG_APP, "%{public}s", s.c_str());
    };
    logLine("BootWorker start: runBase=" + runBase_ + " mode=" + mode +
            " svbox=" + svboxPath_ + " binderd=" + binderd + " rootfs=" + rootfs_);
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
    // Android 11 Bionic linker64 rejects group/world-writable ld.config.txt (mode & 0022)
    chmod((rootfs_ + "/linkerconfig").c_str(), 0755);
    chmod((rootfs_ + "/linkerconfig/ld.config.txt").c_str(), 0644);
    for (const char* sub : { "com.android.adbd", "com.android.art", "com.android.conscrypt",
                             "com.android.media.swcodec", "com.android.os.statsd",
                             "com.android.runtime", "com.android.sdkext" }) {
        chmod((rootfs_ + "/linkerconfig/" + sub).c_str(), 0755);
        chmod((rootfs_ + "/linkerconfig/" + sub + "/ld.config.txt").c_str(), 0644);
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
    for (const std::string& pf : { rootfs_ + "/system/build.prop", rootfs_ + "/vendor/build.prop",
                                   rootfs_ + "/vendor/default.prop", overrideProp }) {
        if (FileExists(pf)) propArgs.push_back(pf);
    }
    pid_t pidPropd = Spawn(propArgs, false, logDir + "/propd.log");

    // 2. Start userspace Binder routers
    pid_t pidBinder = Spawn({ binderd, sockDir + "/binder.sock" }, false, logDir + "/binderd.log");
    Spawn({ binderd, sockDir + "/hwbinder.sock" }, false, logDir + "/hwbinderd.log");
    Spawn({ binderd, sockDir + "/vndbinder.sock" }, false, logDir + "/vndbinderd.log");

    bool okBinder = WaitForFile(sockDir + "/binder.sock", 2000);
    bool okHwBinder = WaitForFile(sockDir + "/hwbinder.sock", 2000);
    bool okProp = WaitForFile(sockDir + "/property_service", 2000);
    logLine("Sockets ready: binder=" + std::to_string(okBinder) +
            " hwbinder=" + std::to_string(okHwBinder) +
            " prop=" + std::to_string(okProp));
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

    // Start XComponent HWC frame consumer right away so it connects immediately
    logdwThread_ = std::thread(&AndroidRuntime::LogdwLoop, this, sockDir + "/logdw", logDir + "/logcat.txt");
    HwcRenderer::Instance().Start(ipcDir + "/hwcomposer.sock", width, height);

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
    if (!SleepCancelableMs(150)) return;
    runSvbox("hwservicemanager", { "system/bin/hwservicemanager" });
    if (!SleepCancelableMs(350)) return;

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
    if (!SleepCancelableMs(100)) return;
    runSvbox("gralloc", { "-e", sfLdPath, "vendor/bin/hw/android.hardware.graphics.allocator@2.0-service" });
    if (!SleepCancelableMs(100)) return;
    runSvbox("hwcomposer", { "-e", sfLdPath, "vendor/bin/hw/android.hardware.graphics.composer@2.1-service" });
    if (!SleepCancelableMs(500)) return;

    // 5. SurfaceFlinger
    SetStatus("Starting SurfaceFlinger (SwiftShader GLES 3.0)...");
    runSvbox("surfaceflinger", { "-e", sfLdPath, "system/bin/surfaceflinger" });
    if (!SleepCancelableMs(1500)) return;

    if (mode == "bootanim") {
        SetStatus("Running Android 11 BootAnimation -> XComponent (Scheme 1 HWC stream)");
        runSvbox("bootanimation", { "-e", sfLdPath, "system/bin/bootanimation" });
    } else {
        SetStatus("Starting Android 11 daemons + Zygote64 -> system_server...");
        lmkdThread_ = std::thread(&AndroidRuntime::LmkdLoop, this, sockDir + "/lmkd");
        int dnsFd = MakeListenUnix(sockDir + "/dnsproxyd");
        int mdnsFd = MakeListenUnix(sockDir + "/mdns");
        int fwFd = MakeListenUnix(sockDir + "/fwmarkd");
        {
            std::lock_guard<std::mutex> lk(procMu_);
            if (dnsFd >= 0) ownedFds_.push_back(dnsFd);
            if (mdnsFd >= 0) ownedFds_.push_back(mdnsFd);
            if (fwFd >= 0) ownedFds_.push_back(fwFd);
        }
        if (FileExists(netd)) {
            Spawn({ netd, sockDir + "/binder.sock" }, false, logDir + "/netd.log");
        }
        auto launchPaced = [&](const std::string& tag, const std::vector<std::string>& cmd) {
            runSvbox(tag, cmd);
            SleepCancelableMs(60);
        };
        launchPaced("installd", { "-e", sfLdPath, "system/bin/installd" });
        launchPaced("storaged", { "-e", sfLdPath, "system/bin/storaged" });
        launchPaced("idmap2d", { "-e", sfLdPath, "system/bin/idmap2d" });
        launchPaced("keystore", { "-e", sfLdPath, "system/bin/keystore", "/data/misc/keystore" });
        launchPaced("vold", { "-e", sfLdPath, "system/bin/vold",
                   "--blkid_context=u:r:blkid:s0",
                   "--blkid_untrusted_context=u:r:blkid_untrusted:s0",
                   "--fsck_context=u:r:fsck:s0",
                   "--fsck_untrusted_context=u:r:fsck_untrusted:s0" });
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
        if (!SleepCancelableMs(500)) return;

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
    HwcRenderer::Instance().Stop();
    if (bootThread_.joinable()) {
        bootThread_.join();
    }
    if (lmkdThread_.joinable()) {
        lmkdThread_.join();
    }
    if (logdwThread_.joinable()) {
        logdwThread_.join();
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
