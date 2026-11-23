/* SPDX-License-Identifier: MIT */
#include "ohos_bridge/surface_bridge.h"
#include "ohos_bridge/log.h"
#include "ohos_bridge/sched_qos.h"

#include <chrono>
#include <cstring>
#include <errno.h>
#ifndef OHOS_BRIDGE_HEADLESS
#include <native_buffer/native_buffer.h>
#include <native_vsync/native_vsync.h>
#endif
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

constexpr uint32_t kFbMagic = 0x42465653u; /* "SVFB" */

struct SvFbFrameHdr {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t format; /* 0 = RGBA8888 */
};

bool RecvExact(int fd, void* dst, size_t n, const std::atomic<bool>& stop) {
    uint8_t* p = static_cast<uint8_t*>(dst);
    size_t got = 0;
    while (got < n && !stop.load()) {
        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r > 0) {
            got += static_cast<size_t>(r);
            continue;
        }
        if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        return false;
    }
    return got == n;
}

int RecvPrimeFd(int sockFd, const std::atomic<bool>& stop) {
    while (!stop.load()) {
        char dummy[16] = {};
        struct iovec iov = { dummy, sizeof(dummy) };
        alignas(struct cmsghdr) char cbuf[CMSG_SPACE(sizeof(int))] = {};
        struct msghdr msg = {};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cbuf;
        msg.msg_controllen = sizeof(cbuf);

        ssize_t r = recvmsg(sockFd, &msg, MSG_CMSG_CLOEXEC);
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return -1;
        }
        if (r == 0) {
            return -1;
        }
        for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
                c->cmsg_len >= CMSG_LEN(sizeof(int))) {
                int outFd = -1;
                std::memcpy(&outFd, CMSG_DATA(c), sizeof(int));
                return outFd;
            }
        }
        return -1;
    }
    return -1;
}

} // namespace

SurfaceBridge& SurfaceBridge::Instance() {
    static SurfaceBridge inst;
    return inst;
}

void SurfaceBridge::SetWindow(OHNativeWindow* window, int32_t surfaceW, int32_t surfaceH) {
    {
        std::lock_guard<std::mutex> lk(winMu_);
        window_ = window;
        configured_ = false;
    }
    OB_LOG_INFO("SvboxSurface", "XComponent surface ready: %dx%d win=%p",
                surfaceW, surfaceH, static_cast<void*>(window));
    RequestVsyncFrame();
}

void SurfaceBridge::ClearWindow() {
    std::lock_guard<std::mutex> lk(winMu_);
    window_ = nullptr;
    configured_ = false;
}

void SurfaceBridge::SetVisible(bool visible) {
    visible_.store(visible);
    if (visible) {
        RequestVsyncFrame();
    }
}

void SurfaceBridge::OnVsyncFrame(long long /*timestamp*/, void* data) {
    auto* self = static_cast<SurfaceBridge*>(data);
    if (!self) return;
    {
        std::lock_guard<std::mutex> lk(self->vsyncMu_);
        self->vsyncArmed_ = false;
    }
    if (self->stop_.load() || !self->visible_.load()) {
        return;
    }
    self->PresentLatest();
    bool more = false;
    {
        std::lock_guard<std::mutex> lk(self->bufMu_);
        more = self->hasPendingFrame_;
    }
    if (more) {
        self->RequestVsyncFrame();
    }
}

void SurfaceBridge::RequestVsyncFrame() {
    if (stop_.load() || !visible_.load()) return;
#ifndef OHOS_BRIDGE_HEADLESS
    if (usingVsync_.load()) {
        std::lock_guard<std::mutex> lk(vsyncMu_);
        if (vsync_ && !vsyncArmed_) {
            if (OH_NativeVSync_RequestFrame(vsync_, &SurfaceBridge::OnVsyncFrame, this) == 0) {
                vsyncArmed_ = true;
            }
        }
        return;
    }
#endif
    presentCv_.notify_one();
}

void SurfaceBridge::StartCommon(int32_t frameW, int32_t frameH) {
    Stop();
    stop_.store(false);
    connected_.store(false);
    frames_.store(0);
    fps_.store(0);
    frameW_.store(frameW);
    frameH_.store(frameH);
    {
        std::lock_guard<std::mutex> lk(bufMu_);
        size_t bytes = static_cast<size_t>(frameW) * static_cast<size_t>(frameH) * 4u;
        staging_[0].assign(bytes, 0);
        staging_[1].assign(bytes, 0);
        frontIdx_ = 0;
        stagedW_ = frameW;
        stagedH_ = frameH;
        hasPendingFrame_ = false;
    }

#ifndef OHOS_BRIDGE_HEADLESS
    {
        std::lock_guard<std::mutex> lk(vsyncMu_);
        const char* name = "svbox_surface";
        vsync_ = OH_NativeVSync_Create(name, static_cast<unsigned int>(std::strlen(name)));
        vsyncArmed_ = false;
        usingVsync_.store(vsync_ != nullptr);
    }
#else
    usingVsync_.store(false);
#endif
    OB_LOG_INFO("SvboxSurface", "SurfaceBridge started (OH_NativeVSync=%d)", usingVsync_.load() ? 1 : 0);

    if (!usingVsync_.load()) {
        fallbackPresentThread_ = std::thread([this]() {
            SchedQos::SetCurrentThreadQoS(SchedQos::Level::UserInteractive, "surface-present");
            while (!stop_.load()) {
                std::unique_lock<std::mutex> lk(vsyncMu_);
                presentCv_.wait_for(lk, std::chrono::milliseconds(50), [this]() {
                    if (stop_.load()) return true;
                    std::lock_guard<std::mutex> blk(bufMu_);
                    return hasPendingFrame_ && visible_.load();
                });
                lk.unlock();
                if (!stop_.load() && visible_.load()) {
                    PresentLatest();
                }
            }
        });
    }
}

void SurfaceBridge::Start(const std::string& hwcSockPath, int32_t frameW, int32_t frameH) {
    StartCommon(frameW, frameH);
    worker_ = std::thread(&SurfaceBridge::RecvLoop, this, hwcSockPath, frameW, frameH);
}

void SurfaceBridge::StartFramebufferServer(const std::string& fbSockPath, int32_t frameW, int32_t frameH) {
    StartCommon(frameW, frameH);
    fbSockPath_ = fbSockPath;
    worker_ = std::thread(&SurfaceBridge::FbServerLoop, this, fbSockPath, frameW, frameH);
}

void SurfaceBridge::Stop() {
    stop_.store(true);
    if (fbListenFd_ >= 0) {
        shutdown(fbListenFd_, SHUT_RDWR);
        close(fbListenFd_);
        fbListenFd_ = -1;
    }
    presentCv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    if (fallbackPresentThread_.joinable()) {
        fallbackPresentThread_.join();
    }
#ifndef OHOS_BRIDGE_HEADLESS
    {
        std::lock_guard<std::mutex> lk(vsyncMu_);
        if (vsync_) {
            OH_NativeVSync_Destroy(vsync_);
            vsync_ = nullptr;
        }
        vsyncArmed_ = false;
    }
#endif
    if (!fbSockPath_.empty()) {
        unlink(fbSockPath_.c_str());
        fbSockPath_.clear();
    }
    usingVsync_.store(false);
    connected_.store(false);
    fps_.store(0);
}

void SurfaceBridge::SubmitRgbaFrame(const uint8_t* rgba, int32_t w, int32_t h) {
    if (!rgba || w <= 0 || h <= 0) return;
    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
    int backIdx = 0;
    {
        std::lock_guard<std::mutex> lk(bufMu_);
        backIdx = 1 - frontIdx_;
        if (staging_[backIdx].size() < bytes) {
            staging_[backIdx].resize(bytes);
        }
    }
    std::memcpy(staging_[backIdx].data(), rgba, bytes);
    {
        std::lock_guard<std::mutex> lk(bufMu_);
        frontIdx_ = backIdx;
        stagedW_ = w;
        stagedH_ = h;
        hasPendingFrame_ = true;
    }
    frameW_.store(w);
    frameH_.store(h);
    frames_.fetch_add(1);
    RequestVsyncFrame();
}

bool SurfaceBridge::CopyLatestFrame(std::vector<uint8_t>& outRgba, int32_t& outW, int32_t& outH) {
    std::lock_guard<std::mutex> lk(bufMu_);
    const size_t bytes = (stagedW_ > 0 && stagedH_ > 0)
        ? static_cast<size_t>(stagedW_) * static_cast<size_t>(stagedH_) * 4u
        : 0u;
    if (bytes == 0 || staging_[frontIdx_].size() < bytes) return false;
    outW = stagedW_;
    outH = stagedH_;
    outRgba.assign(staging_[frontIdx_].begin(), staging_[frontIdx_].begin() + bytes);
    return true;
}

void SurfaceBridge::PresentLatest() {
    int idx = 0;
    int32_t w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lk(bufMu_);
        if (!hasPendingFrame_) return;
        idx = frontIdx_;
        w = stagedW_;
        h = stagedH_;
        hasPendingFrame_ = false;
    }
    if (w > 0 && h > 0 && staging_[idx].size() >= static_cast<size_t>(w) * static_cast<size_t>(h) * 4u) {
        PresentRgbaFrame(staging_[idx].data(), w, h);
    }
}

bool SurfaceBridge::PresentRgbaFrame(const uint8_t* srcRgba, int32_t srcW, int32_t srcH) {
#ifdef OHOS_BRIDGE_HEADLESS
    (void)srcRgba; (void)srcW; (void)srcH;
    return true;
#else
    std::lock_guard<std::mutex> lk(winMu_);
    if (!window_ || !srcRgba || srcW <= 0 || srcH <= 0) {
        return false;
    }

    if (!configured_ || cfgW_ != srcW || cfgH_ != srcH) {
        uint64_t usage = NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE | NATIVEBUFFER_USAGE_MEM_DMA;
        OH_NativeWindow_NativeWindowHandleOpt(window_, SET_USAGE, usage);
        OH_NativeWindow_NativeWindowHandleOpt(window_, SET_BUFFER_GEOMETRY, srcW, srcH);
        OH_NativeWindow_NativeWindowHandleOpt(window_, SET_FORMAT, NATIVEBUFFER_PIXEL_FMT_RGBA_8888);
        cfgW_ = srcW;
        cfgH_ = srcH;
        configured_ = true;
    }

    OHNativeWindowBuffer* buffer = nullptr;
    int fenceFd = -1;
    int32_t rc = OH_NativeWindow_NativeWindowRequestBuffer(window_, &buffer, &fenceFd);
    if (rc != 0 || !buffer) {
        if (fenceFd >= 0) close(fenceFd);
        return false;
    }
    if (fenceFd >= 0) {
        close(fenceFd);
    }

    BufferHandle* handle = OH_NativeWindow_GetBufferHandleFromNative(buffer);
    if (!handle) {
        OH_NativeWindow_NativeWindowAbortBuffer(window_, buffer);
        return false;
    }

    bool mappedLocally = false;
    uint8_t* dst = static_cast<uint8_t*>(handle->virAddr);
    if (!dst && handle->fd >= 0 && handle->size > 0) {
        void* p = mmap(nullptr, static_cast<size_t>(handle->size),
                       PROT_READ | PROT_WRITE, MAP_SHARED, handle->fd, 0);
        if (p != MAP_FAILED) {
            dst = static_cast<uint8_t*>(p);
            mappedLocally = true;
        }
    }

    if (dst) {
        const size_t srcStride = static_cast<size_t>(srcW) * 4u;
        const size_t dstStride = handle->stride > 0 ? static_cast<size_t>(handle->stride) : srcStride;
        const size_t copyBytes = srcStride < dstStride ? srcStride : dstStride;
        const int32_t rows = handle->height > 0 && handle->height < srcH ? handle->height : srcH;
        if (srcStride == dstStride) {
            std::memcpy(dst, srcRgba, copyBytes * static_cast<size_t>(rows));
        } else {
            for (int32_t y = 0; y < rows; ++y) {
                std::memcpy(dst + static_cast<size_t>(y) * dstStride,
                            srcRgba + static_cast<size_t>(y) * srcStride,
                            copyBytes);
            }
        }
        if (mappedLocally) {
            munmap(dst, static_cast<size_t>(handle->size));
        }
    }

    Region region = { nullptr, 0 };
    OH_NativeWindow_NativeWindowFlushBuffer(window_, buffer, -1, region);
    return true;
#endif
}

void SurfaceBridge::RecvLoop(std::string sockPath, int32_t w, int32_t h) {
    SchedQos::SetCurrentThreadQoS(SchedQos::Level::UserInteractive, "surface-recv");
    OB_LOG_INFO("SvboxSurface", "SurfaceBridge waiting for %s (%dx%d)", sockPath.c_str(), w, h);

    while (!stop_.load()) {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            usleep(100000);
            continue;
        }
        struct timeval tv = { 0, 500000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        struct sockaddr_un sa = {};
        sa.sun_family = AF_UNIX;
        std::strncpy(sa.sun_path, sockPath.c_str(), sizeof(sa.sun_path) - 1);

        if (connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) < 0) {
            close(fd);
            usleep(50000);
            continue;
        }

        OB_LOG_INFO("SvboxSurface", "Connected to prime-FD frame socket: %s", sockPath.c_str());
        connected_.store(true);

        auto fpsWindowStart = std::chrono::steady_clock::now();
        int32_t fpsFrames = 0;
        const size_t frameBytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;

        while (!stop_.load()) {
            int32_t hdr[16] = {};
            if (!RecvExact(fd, hdr, sizeof(hdr), stop_)) {
                break;
            }
            int primeFd = RecvPrimeFd(fd, stop_);
            if (primeFd < 0) {
                break;
            }

            size_t mapSize = hdr[6] > 0 ? static_cast<size_t>(hdr[6]) : frameBytes;
            if (mapSize < frameBytes) mapSize = frameBytes;

            if (visible_.load()) {
                void* mapped = mmap(nullptr, mapSize, PROT_READ, MAP_SHARED, primeFd, 0);
                if (mapped != MAP_FAILED) {
                    int backIdx = 0;
                    {
                        std::lock_guard<std::mutex> lk(bufMu_);
                        backIdx = 1 - frontIdx_;
                        if (staging_[backIdx].size() < frameBytes) {
                            staging_[backIdx].resize(frameBytes);
                        }
                    }
                    std::memcpy(staging_[backIdx].data(), mapped, frameBytes);
                    munmap(mapped, mapSize);
                    {
                        std::lock_guard<std::mutex> lk(bufMu_);
                        frontIdx_ = backIdx;
                        stagedW_ = w;
                        stagedH_ = h;
                        hasPendingFrame_ = true;
                    }
                    RequestVsyncFrame();
                }
            }
            close(primeFd);

            const char ack[2] = { 'o', 'k' };
            if (send(fd, ack, 2, MSG_NOSIGNAL) != 2) {
                break;
            }

            frames_.fetch_add(1);
            fpsFrames++;
            auto now = std::chrono::steady_clock::now();
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - fpsWindowStart).count();
            if (elapsedMs >= 1000) {
                fps_.store(static_cast<int32_t>((fpsFrames * 1000LL) / elapsedMs));
                fpsFrames = 0;
                fpsWindowStart = now;
            }
        }

        connected_.store(false);
        fps_.store(0);
        close(fd);
        if (!stop_.load()) {
            usleep(100000);
        }
    }
}

void SurfaceBridge::FbServerLoop(std::string sockPath, int32_t defaultW, int32_t defaultH) {
    SchedQos::SetCurrentThreadQoS(SchedQos::Level::UserInteractive, "surface-fb");
    unlink(sockPath.c_str());
    int sfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sfd < 0) return;
    struct sockaddr_un sa = {};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, sockPath.c_str(), sizeof(sa.sun_path) - 1);
    if (bind(sfd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) < 0 || listen(sfd, 4) < 0) {
        close(sfd);
        return;
    }
    chmod(sockPath.c_str(), 0666);
    fbListenFd_ = sfd;
    OB_LOG_INFO("SvboxSurface", "Generic RGBA framebuffer listening on %s (%dx%d)",
                sockPath.c_str(), defaultW, defaultH);

    std::vector<uint8_t> rxBuf;
    while (!stop_.load()) {
        struct pollfd pfd = { sfd, POLLIN, 0 };
        int r = poll(&pfd, 1, 150);
        if (r <= 0 || !(pfd.revents & POLLIN)) continue;
        int cfd = accept4(sfd, nullptr, nullptr, SOCK_CLOEXEC);
        if (cfd < 0) continue;
        connected_.store(true);
        while (!stop_.load()) {
            SvFbFrameHdr fhdr = {};
            if (!RecvExact(cfd, &fhdr, sizeof(fhdr), stop_) || fhdr.magic != kFbMagic) break;
            if (fhdr.width == 0 || fhdr.height == 0 || fhdr.width > 4096 || fhdr.height > 4096) break;
            size_t nbytes = static_cast<size_t>(fhdr.width) * static_cast<size_t>(fhdr.height) * 4u;
            rxBuf.resize(nbytes);
            if (!RecvExact(cfd, rxBuf.data(), nbytes, stop_)) break;
            SubmitRgbaFrame(rxBuf.data(), static_cast<int32_t>(fhdr.width), static_cast<int32_t>(fhdr.height));
            const char ack[2] = { 'o', 'k' };
            if (send(cfd, ack, 2, MSG_NOSIGNAL) != 2) break;
        }
        connected_.store(false);
        close(cfd);
    }
}
