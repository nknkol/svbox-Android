/* SPDX-License-Identifier: MIT */
#include "hwc_renderer.h"
#include "sched_qos.h"

#include <chrono>
#include <cstring>
#include <errno.h>
#include <hilog/log.h>
#include <native_buffer/native_buffer.h>
#include <native_vsync/native_vsync.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#undef LOG_TAG
#define LOG_TAG "SvboxHwc"

static bool RecvExact(int fd, void* dst, size_t n, const std::atomic<bool>& stop) {
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

static int RecvPrimeFd(int sockFd, const std::atomic<bool>& stop) {
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

HwcRenderer& HwcRenderer::Instance() {
    static HwcRenderer inst;
    return inst;
}

void HwcRenderer::SetWindow(OHNativeWindow* window, int32_t surfaceW, int32_t surfaceH) {
    {
        std::lock_guard<std::mutex> lk(winMu_);
        window_ = window;
        configured_ = false;
    }
    OH_LOG_INFO(LOG_APP, "XComponent surface ready: %{public}dx%{public}d win=%{public}p",
                surfaceW, surfaceH, window);
    RequestVsyncFrame();
}

void HwcRenderer::ClearWindow() {
    std::lock_guard<std::mutex> lk(winMu_);
    window_ = nullptr;
    configured_ = false;
}

void HwcRenderer::SetVisible(bool visible) {
    visible_.store(visible);
    if (visible) {
        RequestVsyncFrame();
    }
}

void HwcRenderer::OnVsyncFrame(long long /*timestamp*/, void* data) {
    auto* self = static_cast<HwcRenderer*>(data);
    if (!self) return;
    {
        std::lock_guard<std::mutex> lk(self->vsyncMu_);
        self->vsyncArmed_ = false;
    }
    if (self->stop_.load() || !self->visible_.load()) {
        return;
    }
    self->PresentLatest();
    // If another frame arrived during presentation, arm the next VSync on demand
    bool more = false;
    {
        std::lock_guard<std::mutex> lk(self->bufMu_);
        more = self->hasPendingFrame_;
    }
    if (more) {
        self->RequestVsyncFrame();
    }
}

void HwcRenderer::RequestVsyncFrame() {
    if (stop_.load() || !visible_.load()) return;
    if (usingVsync_.load()) {
        std::lock_guard<std::mutex> lk(vsyncMu_);
        if (vsync_ && !vsyncArmed_) {
            if (OH_NativeVSync_RequestFrame(vsync_, &HwcRenderer::OnVsyncFrame, this) == 0) {
                vsyncArmed_ = true;
            }
        }
    } else {
        presentCv_.notify_one();
    }
}

void HwcRenderer::Start(const std::string& hwcSockPath, int32_t frameW, int32_t frameH) {
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

    {
        std::lock_guard<std::mutex> lk(vsyncMu_);
        const char* name = "svbox_hwc";
        vsync_ = OH_NativeVSync_Create(name, static_cast<unsigned int>(std::strlen(name)));
        vsyncArmed_ = false;
        usingVsync_.store(vsync_ != nullptr);
    }
    OH_LOG_INFO(LOG_APP, "HwcRenderer started (OH_NativeVSync=%{public}d)", usingVsync_.load() ? 1 : 0);

    if (!usingVsync_.load()) {
        fallbackPresentThread_ = std::thread([this]() {
            SchedQos::SetCurrentThreadQoS(SchedQos::Level::UserInteractive, "hwc-present");
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

    worker_ = std::thread(&HwcRenderer::RecvLoop, this, hwcSockPath, frameW, frameH);
}

void HwcRenderer::Stop() {
    stop_.store(true);
    presentCv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    if (fallbackPresentThread_.joinable()) {
        fallbackPresentThread_.join();
    }
    {
        std::lock_guard<std::mutex> lk(vsyncMu_);
        if (vsync_) {
            OH_NativeVSync_Destroy(vsync_);
            vsync_ = nullptr;
        }
        vsyncArmed_ = false;
    }
    usingVsync_.store(false);
    connected_.store(false);
    fps_.store(0);
}

void HwcRenderer::PresentLatest() {
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

bool HwcRenderer::PresentRgbaFrame(const uint8_t* srcRgba, int32_t srcW, int32_t srcH) {
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
}

void HwcRenderer::RecvLoop(std::string sockPath, int32_t w, int32_t h) {
    SchedQos::SetCurrentThreadQoS(SchedQos::Level::UserInteractive, "hwc-recv");
    OH_LOG_INFO(LOG_APP, "HwcRenderer waiting for %{public}s (%{public}dx%{public}d)", sockPath.c_str(), w, h);

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

        OH_LOG_INFO(LOG_APP, "Connected to hwcomposer socket: %{public}s", sockPath.c_str());
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
                    // Copy outside lock into back buffer, then flip frontIdx_
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

            // Immediate ACK decouples SurfaceFlinger from OHNativeWindow swap latency
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
