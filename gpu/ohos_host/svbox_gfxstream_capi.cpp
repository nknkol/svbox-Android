/* SPDX-License-Identifier: MIT */
#include "svbox_gfxstream_capi.h"

#include "DisplaySurface.h"
#include "FrameBuffer.h"
#include "GfxStreamAgents.h"
#include "NativeSubWindow.h"
#include "gfxstream/host/Features.h"
#include "host-common/GraphicsAgentFactory.h"
#include "host-common/logging.h"
#include "render-utils/RenderChannel.h"
#include "render-utils/RenderLib.h"
#include "render-utils/Renderer.h"
#include "render-utils/render_api.h"
#include "vulkan/DisplayVk.h"
#include "vulkan/VkCommonOperations.h"
#include "vulkan/VulkanDispatch.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <errno.h>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <string>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include <dlfcn.h>
#include <cstdarg>

static void SvboxGfxLog(int level, const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    using FnHiLog = int (*)(int, int, unsigned int, const char*, const char*, ...);
    static FnHiLog sHiLog = []() -> FnHiLog {
        void* h = dlopen("libhilog_ndk.z.so", RTLD_LAZY | RTLD_LOCAL);
        return h ? reinterpret_cast<FnHiLog>(dlsym(h, "OH_LOG_Print")) : nullptr;
    }();
    if (sHiLog) {
        // LOG_APP=0, LOG_INFO=4, LOG_ERROR=6
        sHiLog(0, level, 0, "SvboxGfx", "%{public}s", msg);
    } else {
        fprintf(stderr, "[SvboxGfx] %s\n", msg);
    }
}
#define GFX_LOGI(fmt, ...) SvboxGfxLog(4, fmt, ##__VA_ARGS__)
#define GFX_LOGE(fmt, ...) SvboxGfxLog(6, fmt, ##__VA_ARGS__)

static void GfxstreamHiLogBridge(char severity, const char* file, unsigned int line,
                                 int64_t /*timestamp_us*/, const char* message) {
    if (severity == 'V' || severity == 'D') return;
    const char* base = file ? std::strrchr(file, '/') : nullptr;
    base = base ? (base + 1) : (file ? file : "?");
    int lvl = (severity == 'E' || severity == 'F') ? 6 : (severity == 'W' ? 5 : 4);
    SvboxGfxLog(lvl, "[%c %s:%u] %s", severity, base, line, message ? message : "");
}

namespace {

using gfxstream::FrameBufferChange;
using gfxstream::FrameBufferChangeEvent;
using gfxstream::RenderChannel;
using gfxstream::RenderChannelPtr;
using gfxstream::RenderLibPtr;
using gfxstream::Renderer;
using gfxstream::RendererPtr;

enum class ConnKind {
    Handshake,
    OpenGles,
    ProcessPipe,
    Refcount,
};

struct ClientConn {
    int fd = -1;
    ConnKind kind = ConnKind::Handshake;
    std::string handshakeBuf;

    // OpenGles channel state
    RenderChannelPtr channel;
    std::deque<std::vector<char>> txQueue;
    size_t txOffset = 0;
    std::deque<RenderChannel::Buffer> pendingToHost;
    bool epollOut = false;

    // GLProcessPipe state
    uint64_t puid = 0;
    uint8_t confirmBuf[4] = {};
    size_t confirmGot = 0;
    bool puidSent = false;

    // Refcount pipe state
    uint8_t refBuf[4] = {};
    size_t refGot = 0;
    uint32_t cbHandle = 0;
    bool hasCbHandle = false;
};

struct SecondaryDisplaySlot {
    std::mutex mu;
    OHNativeWindow* window = nullptr;
    EGLNativeWindowType subWin = 0;
    std::unique_ptr<gfxstream::DisplaySurface> displaySurface;
    std::unique_ptr<gfxstream::vk::DisplayVk> displayVk;
    int32_t surfaceW = 0;
    int32_t surfaceH = 0;
    int32_t bufW = 0;
    int32_t bufH = 0;
    int32_t fbW = 0;
    int32_t fbH = 0;
    uint32_t lastCb = 0;
    std::atomic<bool> visible{true};
    std::atomic<uint64_t> framesPosted{0};
    std::atomic<int32_t> fps{0};
    std::atomic<uint32_t> topBarColor{0};
    std::chrono::steady_clock::time_point fpsWindowStart;
    std::chrono::steady_clock::time_point lastColorSample;
    int32_t fpsFrames = 0;
};

struct PresentPacket {
    uint32_t displaySlot;
    uint32_t cbHandle;
    int32_t width;
    int32_t height;
};

struct HostState {
    std::mutex mu;
    bool initialized = false;
    int mode = SVBOX_GFX_MODE_SWIFTSHADER;
    int32_t fbW = 720;
    int32_t fbH = 1280;
    int32_t surfaceW = 720;
    int32_t surfaceH = 1280;
    int32_t vsyncHz = 60;
    OHNativeWindow* window = nullptr;
    bool windowAttached = false;
    std::atomic<bool> visible{true};
    std::atomic<bool> stop{false};

    RenderLibPtr renderLib;
    RendererPtr renderer;
    gfxstream::OnLastColorBufferRef onLastCbRef = nullptr;
    Renderer::FrameBufferChangeEventListener fbListener;
    bool listenerAdded = false;

    std::string sockPath;
    int listenFd = -1;
    int wakeFd = -1;
    std::thread serverThread;

    std::string presentSockPath;
    int presentListenFd = -1;
    std::thread presentThread;
    SecondaryDisplaySlot secondary[SVBOX_GFX_MAX_DISPLAYS];

    std::atomic<uint64_t> nextPuid{1};
    std::atomic<int> activeChannels{0};
    std::atomic<int> activeProcesses{0};
    std::atomic<uint64_t> framesPosted{0};
    std::atomic<int32_t> fps{0};

    std::mutex fpsMu;
    std::chrono::steady_clock::time_point fpsWindowStart;
    int32_t fpsFrames = 0;

    std::string vendor;
    std::string rendererStr;
    std::string version;
};

HostState& State() {
    static HostState s;
    return s;
}

void SetNonBlocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) {
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
}

void WakeEpoll(HostState& st) {
    if (st.wakeFd >= 0) {
        uint64_t one = 1;
        (void)write(st.wakeFd, &one, sizeof(one));
    }
}

void UpdateEpollEvents(int epfd, ClientConn& c, bool wantOut) {
    if (c.epollOut == wantOut) return;
    struct epoll_event ev = {};
    ev.events = EPOLLIN | EPOLLRDHUP | (wantOut ? EPOLLOUT : 0);
    ev.data.fd = c.fd;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, c.fd, &ev) == 0) {
        c.epollOut = wantOut;
    }
}

bool FlushTx(int epfd, ClientConn& c) {
    while (!c.txQueue.empty()) {
        auto& front = c.txQueue.front();
        const char* p = front.data() + c.txOffset;
        size_t rem = front.size() - c.txOffset;
        ssize_t w = send(c.fd, p, rem, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w > 0) {
            c.txOffset += static_cast<size_t>(w);
            if (c.txOffset >= front.size()) {
                c.txQueue.pop_front();
                c.txOffset = 0;
            }
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            UpdateEpollEvents(epfd, c, true);
            return true;
        }
        return false;
    }
    UpdateEpollEvents(epfd, c, false);
    return true;
}

void DrainChannelToGuest(int epfd, ClientConn& c) {
    if (!c.channel) return;
    for (;;) {
        RenderChannel::Buffer outBuf;
        auto res = c.channel->tryRead(&outBuf);
        if (res == RenderChannel::IoResult::Ok) {
            if (!outBuf.empty()) {
                c.txQueue.emplace_back(outBuf.data(), outBuf.data() + outBuf.size());
            }
            continue;
        }
        break;
    }
    FlushTx(epfd, c);
    // Re-arm one-shot CanRead notification (RenderChannelImpl clears mWantedEvents on fire)
    c.channel->setWantedEvents(RenderChannel::State::CanRead);
}

void DrainPendingToHost(ClientConn& c) {
    if (!c.channel) return;
    while (!c.pendingToHost.empty()) {
        RenderChannel::Buffer pkt = std::move(c.pendingToHost.front());
        auto res = c.channel->tryWrite(std::move(pkt));
        if (res == RenderChannel::IoResult::Ok) {
            c.pendingToHost.pop_front();
            continue;
        }
        if (res == RenderChannel::IoResult::TryAgain) {
            // Re-queue if tryWrite didn't consume it? Note: tryWrite only moves on Ok.
            // Wait: because we moved pkt into tryWrite, check if pkt is still intact.
            // Safer: peek before move? RenderChannelImpl::tryWrite takes Buffer&& and only
            // moves from buffer inside tryPushLocked when space exists.
            c.pendingToHost.front() = std::move(pkt);
            c.channel->setWantedEvents(RenderChannel::State::CanRead | RenderChannel::State::CanWrite);
            return;
        }
        c.pendingToHost.clear();
        return;
    }
}

void ServerLoop() {
    HostState& st = State();
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        GFX_LOGE("epoll_create1 failed: errno=%d", errno);
        return;
    }

    auto addFd = [&](int fd, uint32_t events) {
        struct epoll_event ev = {};
        ev.events = events;
        ev.data.fd = fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    };

    addFd(st.listenFd, EPOLLIN);
    addFd(st.wakeFd, EPOLLIN);

    std::unordered_map<int, std::unique_ptr<ClientConn>> conns;
    std::vector<char> rxBuf(65536);

    auto closeConn = [&](int fd) {
        auto it = conns.find(fd);
        if (it == conns.end()) return;
        auto& c = *it->second;
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
        if (c.kind == ConnKind::OpenGles) {
            if (c.channel) {
                c.channel->setEventCallback([](RenderChannel::State) {});
                c.channel->stop();
                c.channel.reset();
            }
            st.activeChannels.fetch_sub(1);
        } else if (c.kind == ConnKind::ProcessPipe) {
            if (c.puid != 0 && st.renderer) {
                st.renderer->cleanupProcGLObjects(c.puid);
            }
            st.activeProcesses.fetch_sub(1);
        } else if (c.kind == ConnKind::Refcount) {
            if (c.hasCbHandle && c.cbHandle != 0 && st.onLastCbRef) {
                st.onLastCbRef(c.cbHandle);
            }
        }
        close(fd);
        conns.erase(it);
    };

    struct epoll_event events[64];
    while (!st.stop.load()) {
        int nfds = epoll_wait(epfd, events, 64, 50);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            break;
        }

        bool woken = false;
        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (fd == st.wakeFd) {
                uint64_t val = 0;
                while (read(st.wakeFd, &val, sizeof(val)) > 0) {}
                woken = true;
                continue;
            }

            if (fd == st.listenFd) {
                for (;;) {
                    int cfd = accept4(st.listenFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0) break;
                    auto cc = std::make_unique<ClientConn>();
                    cc->fd = cfd;
                    cc->kind = ConnKind::Handshake;
                    addFd(cfd, EPOLLIN | EPOLLRDHUP);
                    conns.emplace(cfd, std::move(cc));
                }
                continue;
            }

            auto it = conns.find(fd);
            if (it == conns.end()) continue;
            ClientConn& c = *it->second;

            if (ev & EPOLLOUT) {
                if (!FlushTx(epfd, c)) {
                    closeConn(fd);
                    continue;
                }
            }

            if (ev & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                bool alive = true;
                for (;;) {
                    ssize_t n = recv(fd, rxBuf.data(), rxBuf.size(), MSG_DONTWAIT);
                    if (n < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        alive = false;
                        break;
                    }
                    if (n == 0) {
                        alive = false;
                        break;
                    }

                    const char* data = rxBuf.data();
                    size_t len = static_cast<size_t>(n);

                    if (c.kind == ConnKind::Handshake) {
                        size_t nulPos = std::string::npos;
                        for (size_t k = 0; k < len; ++k) {
                            if (data[k] == '\0') {
                                nulPos = k;
                                break;
                            }
                        }
                        if (nulPos == std::string::npos) {
                            c.handshakeBuf.append(data, len);
                            if (c.handshakeBuf.size() > 256) {
                                alive = false;
                                break;
                            }
                            continue;
                        }
                        c.handshakeBuf.append(data, nulPos);
                        const char* rest = data + nulPos + 1;
                        size_t restLen = len - (nulPos + 1);

                        // Strip optional "pipe:" prefix
                        std::string svc = c.handshakeBuf;
                        if (svc.rfind("pipe:", 0) == 0) {
                            svc = svc.substr(5);
                        }
                        // Strip optional ":args"
                        size_t colon = svc.find(':');
                        if (colon != std::string::npos) {
                            svc = svc.substr(0, colon);
                        }

                        if (svc == "opengles") {
                            c.kind = ConnKind::OpenGles;
                            c.channel = st.renderer ? st.renderer->createRenderChannel() : nullptr;
                            if (!c.channel) {
                                alive = false;
                                break;
                            }
                            st.activeChannels.fetch_add(1);
                            c.channel->setEventCallback([&st](RenderChannel::State) {
                                WakeEpoll(st);
                            });
                            c.channel->setWantedEvents(RenderChannel::State::CanRead);
                            data = rest;
                            len = restLen;
                        } else if (svc == "GLProcessPipe") {
                            c.kind = ConnKind::ProcessPipe;
                            c.puid = st.nextPuid.fetch_add(1);
                            st.activeProcesses.fetch_add(1);
                            if (st.renderer) {
                                st.renderer->onGuestGraphicsProcessCreate(c.puid);
                            }
                            data = rest;
                            len = restLen;
                        } else if (svc == "refcount") {
                            c.kind = ConnKind::Refcount;
                            data = rest;
                            len = restLen;
                        } else {
                            GFX_LOGE("Unknown pipe service: %s", svc.c_str());
                            alive = false;
                            break;
                        }
                    }

                    if (len == 0) continue;

                    if (c.kind == ConnKind::OpenGles) {
                        RenderChannel::Buffer pkt;
                        pkt.resize_noinit(len);
                        std::memcpy(pkt.data(), data, len);
                        c.pendingToHost.push_back(std::move(pkt));
                        DrainPendingToHost(c);
                    } else if (c.kind == ConnKind::ProcessPipe) {
                        size_t take = std::min(len, sizeof(c.confirmBuf) - c.confirmGot);
                        if (take > 0) {
                            std::memcpy(c.confirmBuf + c.confirmGot, data, take);
                            c.confirmGot += take;
                        }
                        if (c.confirmGot == 4 && !c.puidSent) {
                            c.puidSent = true;
                            std::vector<char> reply(sizeof(uint64_t));
                            std::memcpy(reply.data(), &c.puid, sizeof(uint64_t));
                            c.txQueue.push_back(std::move(reply));
                            if (!FlushTx(epfd, c)) {
                                alive = false;
                                break;
                            }
                        }
                    } else if (c.kind == ConnKind::Refcount) {
                        size_t take = std::min(len, sizeof(c.refBuf) - c.refGot);
                        if (take > 0) {
                            std::memcpy(c.refBuf + c.refGot, data, take);
                            c.refGot += take;
                        }
                        if (c.refGot == 4 && !c.hasCbHandle) {
                            std::memcpy(&c.cbHandle, c.refBuf, 4);
                            c.hasCbHandle = true;
                        }
                    }
                }
                if (!alive) {
                    closeConn(fd);
                }
            }
        }

        if (woken) {
            std::vector<int> deadFds;
            for (auto& kv : conns) {
                ClientConn& c = *kv.second;
                if (c.kind != ConnKind::OpenGles || !c.channel) continue;
                DrainPendingToHost(c);
                DrainChannelToGuest(epfd, c);
                auto s = c.channel->state();
                if ((static_cast<int>(s) & static_cast<int>(RenderChannel::State::Stopped)) &&
                    c.txQueue.empty()) {
                    deadFds.push_back(kv.first);
                }
            }
            for (int dfd : deadFds) {
                closeConn(dfd);
            }
        }
    }

    std::vector<int> allFds;
    allFds.reserve(conns.size());
    for (auto& kv : conns) allFds.push_back(kv.first);
    for (int fd : allFds) closeConn(fd);
    close(epfd);
}

void DetachSecondarySlotLocked(SecondaryDisplaySlot& slot) {
    if (slot.displayVk) {
        slot.displayVk->drainQueues();
        slot.displayVk->unbindFromSurface();
        slot.displayVk.reset();
    }
    slot.displaySurface.reset();
    if (slot.subWin) {
        destroySubWindow(slot.subWin);
        slot.subWin = 0;
    }
    slot.window = nullptr;
    slot.bufW = 0;
    slot.bufH = 0;
}

bool AttachSecondarySlotLocked(SecondaryDisplaySlot& slot, int32_t displaySlot,
                               OHNativeWindow* win, int32_t surfW, int32_t surfH) {
    if (!win) {
        DetachSecondarySlotLocked(slot);
        return true;
    }
    auto* fb = gfxstream::FrameBuffer::getFB();
    if (!fb || !fb->hasEmulationVk()) {
        slot.window = win;
        slot.surfaceW = surfW;
        slot.surfaceH = surfH;
        return false;
    }
    const int32_t targetW = (slot.fbW > 0) ? slot.fbW : (surfW > 0 ? surfW : 720);
    const int32_t targetH = (slot.fbH > 0) ? slot.fbH : (surfH > 0 ? surfH : 1280);

    if (slot.window == win && slot.displayVk && slot.displaySurface && slot.subWin) {
        slot.surfaceW = surfW;
        slot.surfaceH = surfH;
        if (slot.bufW != targetW || slot.bufH != targetH) {
            moveSubWindow(nullptr, slot.subWin, 0, 0, targetW, targetH, 1.0f);
            slot.displaySurface->updateSize(static_cast<uint32_t>(targetW),
                                            static_cast<uint32_t>(targetH));
            slot.bufW = targetW;
            slot.bufH = targetH;
        }
        return true;
    }

    DetachSecondarySlotLocked(slot);
    EGLNativeWindowType subWin = createSubWindow(
        win, 0, 0, targetW, targetH, 1.0f, nullptr, nullptr, 0);
    if (!subWin) {
        GFX_LOGE("slot %d: createSubWindow failed", displaySlot);
        return false;
    }
    auto& vkEmu = fb->getEmulationVk();
    auto surface = vkEmu.createDisplaySurface(
        subWin, static_cast<uint32_t>(targetW), static_cast<uint32_t>(targetH));
    if (!surface) {
        GFX_LOGE("slot %d: createDisplaySurface failed", displaySlot);
        destroySubWindow(subWin);
        return false;
    }
    auto dispVk = vkEmu.createSecondaryDisplay();
    if (!dispVk) {
        GFX_LOGE("slot %d: createSecondaryDisplay failed", displaySlot);
        surface.reset();
        destroySubWindow(subWin);
        return false;
    }
    dispVk->bindToSurface(surface.get());
    slot.window = win;
    slot.subWin = subWin;
    slot.displaySurface = std::move(surface);
    slot.displayVk = std::move(dispVk);
    slot.surfaceW = surfW;
    slot.surfaceH = surfH;
    slot.bufW = targetW;
    slot.bufH = targetH;
    GFX_LOGI("slot %d: attached DisplayVk window=%p buf=%dx%d surf=%dx%d",
             displaySlot, win, targetW, targetH, surfW, surfH);
    return true;
}

bool RecvExactFd(int fd, void* dst, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(dst);
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r > 0) {
            got += static_cast<size_t>(r);
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

void HandlePresentClient(int cfd) {
    struct timeval tv = { 5, 0 };
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    HostState& st = State();
    while (!st.stop.load()) {
        PresentPacket pkt = {};
        if (!RecvExactFd(cfd, &pkt, sizeof(pkt))) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        int rc = svbox_gfx_present_cb(
            static_cast<int32_t>(pkt.displaySlot), pkt.cbHandle, pkt.width, pkt.height);
        uint8_t ack = (rc == 0) ? 1u : 0u;
        if (send(cfd, &ack, 1, MSG_NOSIGNAL) != 1) {
            break;
        }
    }
    close(cfd);
}

void PresentServerLoop() {
    HostState& st = State();
    while (!st.stop.load()) {
        struct sockaddr_un peer = {};
        socklen_t plen = sizeof(peer);
        int cfd = accept(st.presentListenFd, reinterpret_cast<struct sockaddr*>(&peer), &plen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(20000);
                continue;
            }
            break;
        }
        int fl = fcntl(cfd, F_GETFL, 0);
        if (fl >= 0) fcntl(cfd, F_SETFL, fl & ~O_NONBLOCK);
        std::thread(HandlePresentClient, cfd).detach();
    }
}

} // namespace

extern "C" {

int svbox_gfx_init(int32_t width, int32_t height, int mode, const char* sockPath) {
    if (width <= 0 || height <= 0 || !sockPath || !sockPath[0]) {
        return -1;
    }
    svbox_gfx_stop();

    HostState& st = State();
    std::lock_guard<std::mutex> lk(st.mu);

    st.mode = mode;
    st.fbW = width;
    st.fbH = height;
    st.sockPath = sockPath;
    st.stop.store(false);
    st.framesPosted.store(0);
    st.fps.store(0);
    st.activeChannels.store(0);
    st.activeProcesses.store(0);
    st.fpsWindowStart = std::chrono::steady_clock::now();
    st.fpsFrames = 0;

    // Ensure headless/EGL2EGL environment defaults for HarmonyOS PC Maleoon 916
    setenv("ANDROID_EMU_HEADLESS", "1", 0);
    set_gfxstream_logger(&GfxstreamHiLogBridge);

    // Inject default GfxStream window/multi-display/VM agents before FrameBuffer creation
    static android::emulation::GfxStreamGraphicsAgentFactory sAgents;
    android::emulation::injectGraphicsAgents(sAgents);

    st.renderLib = gfxstream::initLibrary();
    if (!st.renderLib) {
        GFX_LOGE("gfxstream::initLibrary() failed");
        return -2;
    }
    st.renderLib->setVmOps(*sAgents.android_get_QAndroidVmOperations());
    st.renderLib->setWindowOps(*sAgents.android_get_QAndroidEmulatorWindowAgent(),
                               *sAgents.android_get_QAndroidMultiDisplayAgent());
    st.renderLib->setRenderer(SELECTED_RENDERER_HOST);
    st.renderLib->setAvdInfo(true, 34); // phone, Android 14 (API 34)

    gfxstream::host::FeatureSet features;
    features.GlesDynamicVersion.enabled = true;
    features.NativeTextureDecompression.enabled = true;
    features.RefCountPipe.enabled = true;
    features.NoDelayCloseColorBuffer.enabled = true;
    features.HostComposition.enabled = true;
    features.AsyncComposeSupport.enabled = true;
    features.YuvCache.enabled = true;
    features.GlPipeChecksum.enabled = false;
    features.GlDirectMem.enabled = false;
    features.GlDma.enabled = false;
    features.GlDma2.enabled = false;
    features.GlAsyncSwap.enabled = false;

    const bool wantVulkan = (mode == SVBOX_GFX_MODE_VULKAN);
    if (wantVulkan) {
        gfxstream::vk::vkDispatch(false);
        features.Vulkan.enabled = true;
        features.VulkanNullOptionalStrings.enabled = true;
        features.VulkanIgnoredHandles.enabled = true;
        features.VulkanQueueSubmitWithCommands.enabled = true;
        features.VulkanBatchedDescriptorSetUpdate.enabled = true;
        features.VulkanNativeSwapchain.enabled = true;
    }

    st.renderer = st.renderLib->initRenderer(
        width, height, features, /*useSubWindow=*/true, /*egl2egl=*/true);
    if (!st.renderer) {
        GFX_LOGE("gfxstream initRenderer(%dx%d, mode=%d) failed",
                 width, height, mode);
        st.renderLib.reset();
        return -3;
    }

    st.onLastCbRef = st.renderLib->getOnLastColorBufferRef();
    st.renderer->setVsyncHz(st.vsyncHz > 0 ? st.vsyncHz : 60);
    st.renderer->setDisplayConfigs(0, width, height, 320, 320);
    st.renderer->setDisplayActiveConfig(0);

    auto hw = st.renderer->getHardwareStrings();
    st.vendor = hw.vendor;
    st.rendererStr = hw.renderer;
    st.version = hw.version;

    st.fbListener = [&st](const FrameBufferChangeEvent evt) {
        if (evt.change != FrameBufferChange::FrameReady) return;
        st.framesPosted.fetch_add(1);
        std::lock_guard<std::mutex> flk(st.fpsMu);
        st.fpsFrames++;
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - st.fpsWindowStart)
                      .count();
        if (ms >= 1000) {
            st.fps.store(static_cast<int32_t>((st.fpsFrames * 1000LL) / ms));
            st.fpsFrames = 0;
            st.fpsWindowStart = now;
        }
    };
    st.renderer->addListener(&st.fbListener);
    st.listenerAdded = true;

    // Create AF_UNIX server socket for guest pipe connections (supports @abstract for CLI tests)
    const bool isAbstract = (sockPath[0] == '@');
    if (!isAbstract) unlink(sockPath);
    int sfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sfd < 0) {
        GFX_LOGE("socket(AF_UNIX) failed: errno=%d", errno);
        st.renderer->removeListener(&st.fbListener);
        st.listenerAdded = false;
        st.renderer->stop(true);
        st.renderer.reset();
        st.renderLib.reset();
        return -4;
    }
    struct sockaddr_un sa = {};
    sa.sun_family = AF_UNIX;
    socklen_t saLen = sizeof(sa);
    if (isAbstract) {
        size_t slen = std::strlen(sockPath + 1);
        if (slen > sizeof(sa.sun_path) - 2) slen = sizeof(sa.sun_path) - 2;
        sa.sun_path[0] = '\0';
        std::memcpy(sa.sun_path + 1, sockPath + 1, slen);
        saLen = static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + 1 + slen);
    } else {
        std::strncpy(sa.sun_path, sockPath, sizeof(sa.sun_path) - 1);
    }
    if (bind(sfd, reinterpret_cast<struct sockaddr*>(&sa), saLen) < 0 ||
        listen(sfd, 64) < 0) {
        GFX_LOGE("bind/listen(%s) failed: errno=%d", sockPath, errno);
        close(sfd);
        st.renderer->removeListener(&st.fbListener);
        st.listenerAdded = false;
        st.renderer->stop(true);
        st.renderer.reset();
        st.renderLib.reset();
        return -5;
    }
    if (!isAbstract) chmod(sockPath, 0666);

    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd < 0) {
        close(sfd);
        st.renderer->removeListener(&st.fbListener);
        st.listenerAdded = false;
        st.renderer->stop(true);
        st.renderer.reset();
        st.renderLib.reset();
        return -6;
    }

    st.listenFd = sfd;
    st.wakeFd = efd;
    st.initialized = true;

    // Secondary-display zero-copy present server on <sockPath>.present (or <sockPath>_present for abstract)
    st.presentSockPath = isAbstract ? (std::string(sockPath) + "_present") : (std::string(sockPath) + ".present");
    if (!isAbstract) unlink(st.presentSockPath.c_str());
    int pfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (pfd >= 0) {
        struct sockaddr_un psa = {};
        psa.sun_family = AF_UNIX;
        socklen_t psaLen = sizeof(psa);
        if (isAbstract) {
            size_t plen = st.presentSockPath.size() - 1;
            if (plen > sizeof(psa.sun_path) - 2) plen = sizeof(psa.sun_path) - 2;
            psa.sun_path[0] = '\0';
            std::memcpy(psa.sun_path + 1, st.presentSockPath.c_str() + 1, plen);
            psaLen = static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + 1 + plen);
        } else {
            std::strncpy(psa.sun_path, st.presentSockPath.c_str(), sizeof(psa.sun_path) - 1);
        }
        if (bind(pfd, reinterpret_cast<struct sockaddr*>(&psa), psaLen) == 0 && listen(pfd, 32) == 0) {
            if (!isAbstract) chmod(st.presentSockPath.c_str(), 0666);
            st.presentListenFd = pfd;
            st.presentThread = std::thread(PresentServerLoop);
            GFX_LOGI("Secondary display present server listening on %s", st.presentSockPath.c_str());
        } else {
            GFX_LOGE("bind/listen present(%s) failed: errno=%d", st.presentSockPath.c_str(), errno);
            close(pfd);
        }
    }

    // If a window was already registered before init, attach it now.
    // Match native buffer geometry to guest fbW x fbH (1:1 pixel match, OHOS RenderService scales cleanly).
    if (st.window) {
        st.windowAttached = st.renderer->showOpenGLSubwindow(
            st.window, 0, 0, st.fbW, st.fbH, st.fbW, st.fbH, 1.0f, 0.0f, false, false);
        GFX_LOGI("Attached pre-existing OHNativeWindow %p (%dx%d) -> %d",
                 st.window, st.fbW, st.fbH, st.windowAttached ? 1 : 0);
    }
    for (int i = 1; i < SVBOX_GFX_MAX_DISPLAYS; ++i) {
        std::lock_guard<std::mutex> slk(st.secondary[i].mu);
        if (st.secondary[i].window && !st.secondary[i].displayVk) {
            AttachSecondarySlotLocked(st.secondary[i], i, st.secondary[i].window,
                                      st.secondary[i].surfaceW, st.secondary[i].surfaceH);
        }
    }

    st.serverThread = std::thread(ServerLoop);
    GFX_LOGI("gfxstream host ready: %s / %s / %s (mode=%d, sock=%s)",
             st.vendor.c_str(), st.rendererStr.c_str(), st.version.c_str(), mode, sockPath);
    return 0;
}

int svbox_gfx_set_window(void* ohNativeWindow, int32_t surfaceW, int32_t surfaceH) {
    HostState& st = State();
    std::lock_guard<std::mutex> lk(st.mu);
    st.window = static_cast<OHNativeWindow*>(ohNativeWindow);
    if (surfaceW > 0) st.surfaceW = surfaceW;
    if (surfaceH > 0) st.surfaceH = surfaceH;
    if (!st.initialized || !st.renderer || !st.window) {
        return 0;
    }
    bool ok = st.renderer->showOpenGLSubwindow(
        st.window, 0, 0, st.fbW, st.fbH, st.fbW, st.fbH, 1.0f, 0.0f, st.windowAttached, false);
    st.windowAttached = ok;
    GFX_LOGI("svbox_gfx_set_window(%p, fb=%dx%d surf=%dx%d) -> %d",
             ohNativeWindow, st.fbW, st.fbH, surfaceW, surfaceH, ok ? 1 : 0);
    return ok ? 0 : -1;
}

void svbox_gfx_clear_window(void) {
    HostState& st = State();
    std::lock_guard<std::mutex> lk(st.mu);
    if (st.initialized && st.renderer && st.windowAttached) {
        st.renderer->destroyOpenGLSubwindow();
        st.windowAttached = false;
    }
    st.window = nullptr;
}

int svbox_gfx_set_display_window(int32_t displaySlot, void* ohNativeWindow,
                                 int32_t surfaceW, int32_t surfaceH) {
    if (displaySlot == 0) {
        return svbox_gfx_set_window(ohNativeWindow, surfaceW, surfaceH);
    }
    if (displaySlot < 1 || displaySlot >= SVBOX_GFX_MAX_DISPLAYS) {
        return -1;
    }
    HostState& st = State();
    auto& sec = st.secondary[displaySlot];
    uint32_t repaintCb = 0;
    int32_t repaintW = 0;
    int32_t repaintH = 0;
    {
        std::lock_guard<std::mutex> slk(sec.mu);
        if (!st.initialized) {
            sec.window = static_cast<OHNativeWindow*>(ohNativeWindow);
            sec.surfaceW = surfaceW;
            sec.surfaceH = surfaceH;
            return 0;
        }
        const bool alreadyAttached = (sec.window == static_cast<OHNativeWindow*>(ohNativeWindow) &&
                                      sec.displayVk != nullptr && sec.displaySurface != nullptr);
        const int32_t prevBufW = sec.bufW;
        const int32_t prevBufH = sec.bufH;
        bool ok = AttachSecondarySlotLocked(
            sec, displaySlot, static_cast<OHNativeWindow*>(ohNativeWindow), surfaceW, surfaceH);
        if (!ok) return -1;
        if (!alreadyAttached || sec.bufW != prevBufW || sec.bufH != prevBufH) {
            repaintCb = sec.lastCb;
            repaintW = sec.fbW;
            repaintH = sec.fbH;
        }
    }
    if (repaintCb != 0) {
        svbox_gfx_present_cb(displaySlot, repaintCb, repaintW, repaintH);
    }
    return 0;
}

void svbox_gfx_clear_display_window(int32_t displaySlot) {
    if (displaySlot == 0) {
        svbox_gfx_clear_window();
        return;
    }
    if (displaySlot < 1 || displaySlot >= SVBOX_GFX_MAX_DISPLAYS) {
        return;
    }
    HostState& st = State();
    auto& sec = st.secondary[displaySlot];
    std::lock_guard<std::mutex> slk(sec.mu);
    DetachSecondarySlotLocked(sec);
    sec.lastCb = 0;
    sec.fbW = 0;
    sec.fbH = 0;
    sec.fps.store(0);
    sec.topBarColor.store(0);
}

void svbox_gfx_set_display_visible(int32_t displaySlot, int visible) {
    if (displaySlot == 0) {
        svbox_gfx_set_visible(visible);
        return;
    }
    if (displaySlot < 1 || displaySlot >= SVBOX_GFX_MAX_DISPLAYS) {
        return;
    }
    HostState& st = State();
    auto& sec = st.secondary[displaySlot];
    sec.visible.store(visible != 0);
    if (visible) {
        uint32_t cb = 0;
        int32_t w = 0, h = 0;
        {
            std::lock_guard<std::mutex> slk(sec.mu);
            cb = sec.lastCb;
            w = sec.fbW;
            h = sec.fbH;
        }
        if (cb != 0) {
            svbox_gfx_present_cb(displaySlot, cb, w, h);
        }
    }
}

int svbox_gfx_present_cb(int32_t displaySlot, uint32_t cbHandle, int32_t width, int32_t height) {
    if (displaySlot < 1 || displaySlot >= SVBOX_GFX_MAX_DISPLAYS || cbHandle == 0) {
        return -1;
    }
    HostState& st = State();
    if (!st.initialized) return -2;
    auto* fb = gfxstream::FrameBuffer::getFB();
    if (!fb) return -3;

    auto& sec = st.secondary[displaySlot];
    std::lock_guard<std::mutex> slk(sec.mu);
    sec.lastCb = cbHandle;
    if (width > 0) sec.fbW = width;
    if (height > 0) sec.fbH = height;

    auto now = std::chrono::steady_clock::now();
    if (sec.framesPosted.load() == 0) {
        sec.fpsWindowStart = now;
    }
    uint64_t total = sec.framesPosted.fetch_add(1) + 1;
    sec.fpsFrames++;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - sec.fpsWindowStart).count();
    if (ms >= 1000) {
        sec.fps.store(static_cast<int32_t>((sec.fpsFrames * 1000LL) / ms));
        sec.fpsFrames = 0;
        sec.fpsWindowStart = now;
    }

    if (!sec.visible.load() || !sec.displayVk || !sec.displaySurface) {
        return 0;
    }

    if (sec.fbW > 0 && sec.fbH > 0 && (sec.bufW != sec.fbW || sec.bufH != sec.fbH) && sec.subWin) {
        moveSubWindow(nullptr, sec.subWin, 0, 0, sec.fbW, sec.fbH, 1.0f);
        sec.displaySurface->updateSize(static_cast<uint32_t>(sec.fbW),
                                       static_cast<uint32_t>(sec.fbH));
        sec.bufW = sec.fbW;
        sec.bufH = sec.fbH;
    }

    for (int retry = 0; retry < 2; ++retry) {
        auto imageInfo = fb->borrowColorBufferForDisplay(cbHandle);
        if (!imageInfo) {
            if (total <= 5) {
                GFX_LOGE("slot %d: borrowColorBufferForDisplay(0x%x) returned null", displaySlot, cbHandle);
            }
            return -4;
        }
        auto result = sec.displayVk->post(imageInfo.get());
        if (result.success) {
            if (result.postCompletedWaitable.valid()) {
                result.postCompletedWaitable.wait();
            }
            if (total == 1 || total == 10 || (total % 300 == 0)) {
                GFX_LOGI("slot %d: presented frame #%llu (cb=0x%x, %dx%d)",
                         displaySlot, static_cast<unsigned long long>(total), cbHandle, sec.fbW, sec.fbH);
            }
            // Periodic 1-scanline top-edge color sample for immersive titlebar tinting
            auto colorMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               now - sec.lastColorSample).count();
            if (sec.fbW >= 32 && sec.fbH >= 16 &&
                (total == 2 || total == 10 || total == 25 || colorMs >= 450)) {
                sec.lastColorSample = now;
                const uint32_t rowBytes = static_cast<uint32_t>(sec.fbW) * 4u;
                std::vector<uint8_t> row(rowBytes, 0);
                fb->readColorBuffer(cbHandle, 0, 4, sec.fbW, 1, 0x1908 /*GL_RGBA*/,
                                    0x1401 /*GL_UNSIGNED_BYTE*/, row.data(), rowBytes);
                uint8_t rs[16], gs[16], bs[16];
                int cnt = 0;
                for (int k = 0; k < 16; ++k) {
                    int x = ((k + 1) * sec.fbW) / 17;
                    if (x < 0) x = 0;
                    if (x >= sec.fbW) x = sec.fbW - 1;
                    const uint8_t* px = row.data() + x * 4;
                    if (px[3] >= 16) {
                        rs[cnt] = px[0];
                        gs[cnt] = px[1];
                        bs[cnt] = px[2];
                        cnt++;
                    }
                }
                if (cnt > 0) {
                    std::sort(rs, rs + cnt);
                    std::sort(gs, gs + cnt);
                    std::sort(bs, bs + cnt);
                    uint32_t r = rs[cnt / 2];
                    uint32_t g = gs[cnt / 2];
                    uint32_t b = bs[cnt / 2];
                    sec.topBarColor.store(0xFF000000u | (r << 16) | (g << 8) | b);
                }
            }
            return 0;
        }
    }
    return -5;
}

void svbox_gfx_get_display_stats(int32_t displaySlot, SvboxGfxDisplayStats* outStats) {
    if (!outStats) return;
    std::memset(outStats, 0, sizeof(*outStats));
    outStats->displaySlot = displaySlot;
    HostState& st = State();
    if (displaySlot == 0) {
        std::lock_guard<std::mutex> lk(st.mu);
        outStats->attached = st.windowAttached ? 1 : 0;
        outStats->visible = st.visible.load() ? 1 : 0;
        outStats->width = st.fbW;
        outStats->height = st.fbH;
        outStats->framesPosted = st.framesPosted.load();
        outStats->fps = st.fps.load();
        return;
    }
    if (displaySlot < 1 || displaySlot >= SVBOX_GFX_MAX_DISPLAYS) return;
    auto& sec = st.secondary[displaySlot];
    std::lock_guard<std::mutex> slk(sec.mu);
    outStats->attached = (sec.displayVk != nullptr) ? 1 : 0;
    outStats->visible = sec.visible.load() ? 1 : 0;
    outStats->width = sec.fbW;
    outStats->height = sec.fbH;
    outStats->lastCb = sec.lastCb;
    outStats->framesPosted = sec.framesPosted.load();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - sec.fpsWindowStart)
                  .count();
    if (ms > 1500 && sec.fpsFrames == 0) {
        sec.fps.store(0);
    }
    outStats->fps = sec.fps.load();
    outStats->topBarColor = sec.topBarColor.load();
}

int svbox_gfx_read_display_rgba(int32_t displaySlot, void* outRgba, uint32_t bufBytes,
                                int32_t* outW, int32_t* outH) {
    if (displaySlot < 1 || displaySlot >= SVBOX_GFX_MAX_DISPLAYS || !outRgba) return -1;
    HostState& st = State();
    auto* fb = gfxstream::FrameBuffer::getFB();
    if (!st.initialized || !fb) return -2;

    uint32_t cb = 0;
    int32_t w = 0, h = 0;
    {
        auto& sec = st.secondary[displaySlot];
        std::lock_guard<std::mutex> slk(sec.mu);
        cb = sec.lastCb;
        w = sec.fbW;
        h = sec.fbH;
    }
    if (cb == 0 || w <= 0 || h <= 0) return -3;
    const uint64_t need = static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 4u;
    if (bufBytes < need) return -4;
    // GL_RGBA = 0x1908, GL_UNSIGNED_BYTE = 0x1401
    fb->readColorBuffer(cb, 0, 0, w, h, 0x1908, 0x1401, outRgba, bufBytes);
    if (outW) *outW = w;
    if (outH) *outH = h;
    return 0;
}

void svbox_gfx_repaint(void) {
    HostState& st = State();
    std::lock_guard<std::mutex> lk(st.mu);
    if (st.initialized && st.renderer && st.windowAttached && st.visible.load()) {
        st.renderer->repaintOpenGLDisplay();
    }
}

void svbox_gfx_set_visible(int visible) {
    HostState& st = State();
    st.visible.store(visible != 0);
    if (visible) {
        svbox_gfx_repaint();
    }
}

void svbox_gfx_set_vsync_hz(int32_t hz) {
    if (hz < 30) hz = 60;
    if (hz > 240) hz = 120;
    HostState& st = State();
    std::lock_guard<std::mutex> lk(st.mu);
    st.vsyncHz = hz;
    if (st.initialized && st.renderer) {
        st.renderer->setVsyncHz(hz);
        GFX_LOGI("svbox_gfx_set_vsync_hz(%d)", hz);
    }
}

void svbox_gfx_stop(void) {
    HostState& st = State();
    st.stop.store(true);
    WakeEpoll(st);
    if (st.presentListenFd >= 0) {
        shutdown(st.presentListenFd, SHUT_RDWR);
    }
    if (st.serverThread.joinable()) {
        st.serverThread.join();
    }
    if (st.presentThread.joinable()) {
        st.presentThread.join();
    }
    for (int i = 1; i < SVBOX_GFX_MAX_DISPLAYS; ++i) {
        std::lock_guard<std::mutex> slk(st.secondary[i].mu);
        DetachSecondarySlotLocked(st.secondary[i]);
        st.secondary[i].lastCb = 0;
        st.secondary[i].fbW = 0;
        st.secondary[i].fbH = 0;
        st.secondary[i].framesPosted.store(0);
        st.secondary[i].fps.store(0);
    }
    std::lock_guard<std::mutex> lk(st.mu);
    if (st.presentListenFd >= 0) {
        close(st.presentListenFd);
        st.presentListenFd = -1;
    }
    if (!st.presentSockPath.empty()) {
        if (st.presentSockPath[0] != '@') unlink(st.presentSockPath.c_str());
        st.presentSockPath.clear();
    }
    if (st.listenFd >= 0) {
        close(st.listenFd);
        st.listenFd = -1;
    }
    if (st.wakeFd >= 0) {
        close(st.wakeFd);
        st.wakeFd = -1;
    }
    if (!st.sockPath.empty()) {
        unlink(st.sockPath.c_str());
        st.sockPath.clear();
    }
    if (st.renderer) {
        if (st.listenerAdded) {
            st.renderer->removeListener(&st.fbListener);
            st.listenerAdded = false;
        }
        if (st.windowAttached) {
            st.renderer->destroyOpenGLSubwindow();
            st.windowAttached = false;
        }
        st.renderer->stop(true);
        st.renderer.reset();
    }
    st.renderLib.reset();
    st.initialized = false;
    st.fps.store(0);
}

void svbox_gfx_get_stats(SvboxGfxStats* outStats) {
    if (!outStats) return;
    std::memset(outStats, 0, sizeof(*outStats));
    HostState& st = State();
    std::lock_guard<std::mutex> lk(st.mu);
    outStats->initialized = st.initialized ? 1 : 0;
    outStats->mode = st.mode;
    outStats->vulkanEnabled = (st.mode == SVBOX_GFX_MODE_VULKAN && st.initialized) ? 1 : 0;
    outStats->activeChannels = st.activeChannels.load();
    outStats->activeProcesses = st.activeProcesses.load();
    outStats->framesPosted = st.framesPosted.load();
    {
        std::lock_guard<std::mutex> flk(st.fpsMu);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - st.fpsWindowStart)
                      .count();
        if (ms > 1500 && st.fpsFrames == 0) {
            st.fps.store(0);
        }
    }
    outStats->fps = st.fps.load();
    outStats->width = st.fbW;
    outStats->height = st.fbH;
    std::strncpy(outStats->vendor, st.vendor.c_str(), sizeof(outStats->vendor) - 1);
    std::strncpy(outStats->renderer, st.rendererStr.c_str(), sizeof(outStats->renderer) - 1);
    std::strncpy(outStats->version, st.version.c_str(), sizeof(outStats->version) - 1);
}

} // extern "C"
