/* SPDX-License-Identifier: MIT */
#include "input_bridge.h"
#include "sched_qos.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <errno.h>
#include <hilog/log.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#undef LOG_TAG
#define LOG_TAG "SvboxInput"

namespace {

// Must match svbox/src/payload/include/vdev_proto.h
constexpr uint32_t kVdevMagic = 0x56444556u;
constexpr uint32_t kVdevOpOpen = 1;
constexpr uint32_t kVdevOpAttachThread = 2;
constexpr uint32_t kVdevOpIoctl = 4;

struct VdevHdr {
    uint32_t magic;
    uint32_t op;
    int32_t status;
    uint32_t payload_len;
    uint32_t proc_id;
    uint32_t fd_count;
    uint64_t arg0;
    uint64_t arg1;
};

struct VdevOpenReq {
    uint32_t pid;
    uint32_t tid;
    uint32_t uid;
    uint32_t gid;
    uint32_t vdev_type;
    uint32_t flags;
    char guest_path[128];
};

struct VdevOpenRep {
    uint32_t proc_id;
    uint32_t major;
    uint32_t minor;
    uint32_t memfd_size;
};

struct VdevAttachReq {
    uint32_t proc_id;
    uint32_t pid;
    uint32_t tid;
    uint32_t reserved;
};

struct VdevIoctlReq {
    uint64_t cmd;
    uint64_t arg;
    uint32_t in_len;
    uint32_t out_len;
};

bool RecvExact(int fd, void* dst, size_t n) {
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

bool SendAll(int fd, const void* src, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(src);
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = send(fd, p + sent, n - sent, MSG_NOSIGNAL);
        if (w > 0) {
            sent += static_cast<size_t>(w);
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool SendVdevReply(int fd, uint32_t op, uint32_t procId, int32_t status,
                   const void* payload, uint32_t payloadLen) {
    VdevHdr hdr = {};
    hdr.magic = kVdevMagic;
    hdr.op = op;
    hdr.status = status;
    hdr.payload_len = payloadLen;
    hdr.proc_id = procId;
    if (!SendAll(fd, &hdr, sizeof(hdr))) return false;
    if (payloadLen > 0 && payload) {
        return SendAll(fd, payload, payloadLen);
    }
    return true;
}

inline void SetBit(uint8_t* bits, size_t bytes, uint32_t bit) {
    if ((bit / 8u) < bytes) {
        bits[bit / 8u] |= static_cast<uint8_t>(1u << (bit % 8u));
    }
}

struct timeval NowMonotonicTv() {
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    struct timeval tv = {};
    tv.tv_sec = ts.tv_sec;
    tv.tv_usec = ts.tv_nsec / 1000;
    return tv;
}

void PushEv(std::vector<struct input_event>& out, const struct timeval& tv,
            uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev = {};
    ev.time = tv;
    ev.type = type;
    ev.code = code;
    ev.value = value;
    out.push_back(ev);
}

#ifndef ABS_MT_SLOT
#define ABS_MT_SLOT        0x2f
#define ABS_MT_TOUCH_MAJOR 0x30
#define ABS_MT_POSITION_X  0x35
#define ABS_MT_POSITION_Y  0x36
#define ABS_MT_TRACKING_ID 0x39
#define ABS_MT_PRESSURE    0x3a
#endif
#ifndef INPUT_PROP_DIRECT
#define INPUT_PROP_DIRECT  0x01
#endif
#ifndef KEY_APPSELECT
#define KEY_APPSELECT      0x244
#endif

} // namespace

InputBridge& InputBridge::Instance() {
    static InputBridge inst;
    return inst;
}

void InputBridge::Start(const std::string& touchSockPath,
                        const std::string& kbdSockPath,
                        int32_t displayW,
                        int32_t displayH) {
    Stop();
    displayW_.store(displayW > 0 ? displayW : 720);
    displayH_.store(displayH > 0 ? displayH : 1280);
    eventsSent_.store(0);
    touchClients_.store(0);
    kbdClients_.store(0);
    {
        std::lock_guard<std::mutex> lk(touchStateMu_);
        mouseLeftDown_ = false;
        nextTrackingId_ = 1;
        for (int i = 0; i < 10; ++i) {
            slotFinger_[i] = -1;
            slotTrack_[i] = -1;
        }
    }

    running_.store(true);
    touchSrv_.isTouch = true;
    touchSrv_.sockPath = touchSockPath;
    touchSrv_.listenFd = -1;
    touchSrv_.thread = std::thread(&InputBridge::RunServer, this, &touchSrv_);

    kbdSrv_.isTouch = false;
    kbdSrv_.sockPath = kbdSockPath;
    kbdSrv_.listenFd = -1;
    kbdSrv_.thread = std::thread(&InputBridge::RunServer, this, &kbdSrv_);
}

void InputBridge::Stop() {
    running_.store(false);
    auto stopSrv = [](EvdevServer& srv) {
        if (srv.listenFd >= 0) {
            shutdown(srv.listenFd, SHUT_RDWR);
            close(srv.listenFd);
            srv.listenFd = -1;
        }
        if (srv.thread.joinable()) {
            srv.thread.join();
        }
        std::lock_guard<std::mutex> lk(srv.clientsMu);
        for (int fd : srv.streamFds) {
            if (fd >= 0) close(fd);
        }
        srv.streamFds.clear();
        if (!srv.sockPath.empty()) {
            unlink(srv.sockPath.c_str());
        }
    };
    stopSrv(touchSrv_);
    stopSrv(kbdSrv_);
    touchClients_.store(0);
    kbdClients_.store(0);
}

void InputBridge::SetSurfaceSize(int32_t surfaceW, int32_t surfaceH) {
    if (surfaceW > 0 && surfaceH > 0) {
        surfaceW_.store(surfaceW);
        surfaceH_.store(surfaceH);
    }
}

void InputBridge::RunServer(EvdevServer* srv) {
    SchedQos::SetCurrentThreadQoS(SchedQos::Level::UserInteractive,
                                  srv->isTouch ? "input-touch" : "input-kbd");
    unlink(srv->sockPath.c_str());
    int sfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sfd < 0) {
        OH_LOG_ERROR(LOG_APP, "InputBridge socket failed: %{public}d", errno);
        return;
    }
    struct sockaddr_un sa = {};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, srv->sockPath.c_str(), sizeof(sa.sun_path) - 1);
    if (bind(sfd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) < 0 ||
        listen(sfd, 16) < 0) {
        OH_LOG_ERROR(LOG_APP, "InputBridge bind/listen %{public}s failed: %{public}d",
                     srv->sockPath.c_str(), errno);
        close(sfd);
        return;
    }
    chmod(srv->sockPath.c_str(), 0666);
    srv->listenFd = sfd;
    OH_LOG_INFO(LOG_APP, "InputBridge listening on %{public}s", srv->sockPath.c_str());

    while (running_.load()) {
        struct pollfd pfd = { sfd, POLLIN, 0 };
        int r = poll(&pfd, 1, 150);
        if (r <= 0) continue;
        if (!(pfd.revents & POLLIN)) continue;
        int cfd = accept4(sfd, nullptr, nullptr, SOCK_CLOEXEC);
        if (cfd < 0) continue;
        std::thread(&InputBridge::HandleVdevConn, this, srv, cfd).detach();
    }
}

void InputBridge::HandleVdevConn(EvdevServer* srv, int fd) {
    VdevHdr hdr = {};
    if (!RecvExact(fd, &hdr, sizeof(hdr)) || hdr.magic != kVdevMagic) {
        close(fd);
        return;
    }

    if (hdr.op == kVdevOpOpen) {
        VdevOpenReq req = {};
        if (hdr.payload_len > 0) {
            std::vector<uint8_t> tmp(hdr.payload_len);
            if (!RecvExact(fd, tmp.data(), tmp.size())) {
                close(fd);
                return;
            }
            std::memcpy(&req, tmp.data(), std::min(sizeof(req), tmp.size()));
        }
        VdevOpenRep rep = {};
        rep.proc_id = req.pid ? req.pid : 1u;
        rep.major = 13u; // Linux input major
        rep.minor = srv->isTouch ? 64u : 65u;
        rep.memfd_size = 0;
        if (!SendVdevReply(fd, kVdevOpOpen, rep.proc_id, 0, &rep, sizeof(rep))) {
            close(fd);
            return;
        }
        OH_LOG_INFO(LOG_APP, "evdev %{public}s opened by pid=%{public}u",
                    srv->isTouch ? "event0(touch)" : "event1(kbd)", req.pid);
        {
            std::lock_guard<std::mutex> lk(srv->clientsMu);
            srv->streamFds.push_back(fd);
            if (srv->isTouch) touchClients_.store(static_cast<int32_t>(srv->streamFds.size()));
            else kbdClients_.store(static_cast<int32_t>(srv->streamFds.size()));
        }
        // Keep connection open as the live event stream until peer disconnects
        while (running_.load()) {
            struct pollfd pfd = { fd, POLLRDHUP | POLLERR | POLLHUP, 0 };
            int pr = poll(&pfd, 1, 250);
            if (pr > 0 && (pfd.revents & (POLLRDHUP | POLLERR | POLLHUP))) {
                break;
            }
        }
        {
            std::lock_guard<std::mutex> lk(srv->clientsMu);
            auto it = std::find(srv->streamFds.begin(), srv->streamFds.end(), fd);
            if (it != srv->streamFds.end()) srv->streamFds.erase(it);
            if (srv->isTouch) touchClients_.store(static_cast<int32_t>(srv->streamFds.size()));
            else kbdClients_.store(static_cast<int32_t>(srv->streamFds.size()));
        }
        close(fd);
        return;
    }

    if (hdr.op == kVdevOpAttachThread) {
        if (hdr.payload_len > 0) {
            std::vector<uint8_t> tmp(hdr.payload_len);
            if (!RecvExact(fd, tmp.data(), tmp.size())) {
                close(fd);
                return;
            }
        }
        if (!SendVdevReply(fd, kVdevOpAttachThread, hdr.proc_id, 0, nullptr, 0)) {
            close(fd);
            return;
        }
        // Serve VDEV_OP_IOCTL RPC requests on this thread channel
        while (running_.load()) {
            VdevHdr rhdr = {};
            if (!RecvExact(fd, &rhdr, sizeof(rhdr)) || rhdr.magic != kVdevMagic) {
                break;
            }
            std::vector<uint8_t> payload(rhdr.payload_len);
            if (rhdr.payload_len > 0 && !RecvExact(fd, payload.data(), payload.size())) {
                break;
            }
            if (rhdr.op != kVdevOpIoctl || payload.size() < sizeof(VdevIoctlReq)) {
                SendVdevReply(fd, rhdr.op, rhdr.proc_id, -ENOTTY, nullptr, 0);
                continue;
            }
            VdevIoctlReq ireq = {};
            std::memcpy(&ireq, payload.data(), sizeof(ireq));
            uint32_t cmd = static_cast<uint32_t>(ireq.cmd);
            uint32_t type = (cmd >> 8) & 0xffu;
            uint32_t nr = cmd & 0xffu;
            uint32_t outLen = std::min<uint32_t>(ireq.out_len, 4096u);

            if (type != 'E') {
                SendVdevReply(fd, kVdevOpIoctl, rhdr.proc_id, -ENOTTY, nullptr, 0);
                continue;
            }

            std::vector<uint8_t> out(outLen, 0);
            int32_t rc = 0;

            if (nr == 0x01) { // EVIOCGVERSION
                int32_t ver = EV_VERSION;
                if (out.size() >= sizeof(ver)) std::memcpy(out.data(), &ver, sizeof(ver));
            } else if (nr == 0x02) { // EVIOCGID
                struct input_id id = {};
                id.bustype = BUS_VIRTUAL;
                id.vendor = 0x18d1;
                id.product = srv->isTouch ? 0x0001 : 0x0002;
                id.version = 0x0001;
                if (out.size() >= sizeof(id)) std::memcpy(out.data(), &id, sizeof(id));
            } else if (nr == 0x06) { // EVIOCGNAME
                const char* s = srv->isTouch ? "svbox-touchscreen" : "svbox-keyboard";
                size_t n = std::min(out.size(), std::strlen(s) + 1);
                if (n > 0) {
                    std::memcpy(out.data(), s, n);
                    out[n - 1] = '\0';
                }
                out.resize(n);
                rc = static_cast<int32_t>(n);
            } else if (nr == 0x07) { // EVIOCGPHYS
                const char* s = srv->isTouch ? "svbox/input0" : "svbox/input1";
                size_t n = std::min(out.size(), std::strlen(s) + 1);
                if (n > 0) {
                    std::memcpy(out.data(), s, n);
                    out[n - 1] = '\0';
                }
                out.resize(n);
                rc = static_cast<int32_t>(n);
            } else if (nr == 0x08) { // EVIOCGUNIQ
                const char* s = srv->isTouch ? "svbox-touch-0" : "svbox-kbd-1";
                size_t n = std::min(out.size(), std::strlen(s) + 1);
                if (n > 0) {
                    std::memcpy(out.data(), s, n);
                    out[n - 1] = '\0';
                }
                out.resize(n);
                rc = static_cast<int32_t>(n);
            } else if (nr == 0x09) { // EVIOCGPROP
                if (srv->isTouch) {
                    SetBit(out.data(), out.size(), INPUT_PROP_DIRECT);
                }
                rc = static_cast<int32_t>(out.size());
            } else if (nr == 0x18 || nr == 0x19 || nr == 0x1b) { // EVIOCGKEY / LED / SW
                rc = static_cast<int32_t>(out.size());
            } else if (nr >= 0x20 && nr < 0x40) { // EVIOCGBIT(ev, len)
                uint32_t evType = nr - 0x20;
                if (evType == 0) {
                    SetBit(out.data(), out.size(), EV_SYN);
                    SetBit(out.data(), out.size(), EV_KEY);
                    if (srv->isTouch) SetBit(out.data(), out.size(), EV_ABS);
                } else if (evType == EV_KEY) {
                    if (srv->isTouch) {
                        SetBit(out.data(), out.size(), BTN_TOUCH);
                    } else {
                        for (uint32_t k = 1; k <= 248; ++k) SetBit(out.data(), out.size(), k);
                        SetBit(out.data(), out.size(), KEY_APPSELECT);
                    }
                } else if (evType == EV_ABS && srv->isTouch) {
                    SetBit(out.data(), out.size(), ABS_X);
                    SetBit(out.data(), out.size(), ABS_Y);
                    SetBit(out.data(), out.size(), ABS_MT_SLOT);
                    SetBit(out.data(), out.size(), ABS_MT_TOUCH_MAJOR);
                    SetBit(out.data(), out.size(), ABS_MT_POSITION_X);
                    SetBit(out.data(), out.size(), ABS_MT_POSITION_Y);
                    SetBit(out.data(), out.size(), ABS_MT_TRACKING_ID);
                    SetBit(out.data(), out.size(), ABS_MT_PRESSURE);
                }
                rc = static_cast<int32_t>(out.size());
            } else if (nr >= 0x40 && nr < 0x80) { // EVIOCGABS(abs)
                uint32_t absCode = nr - 0x40;
                struct input_absinfo ai = {};
                if (absCode == ABS_MT_POSITION_X || absCode == ABS_X) {
                    ai.minimum = 0;
                    ai.maximum = displayW_.load() - 1;
                } else if (absCode == ABS_MT_POSITION_Y || absCode == ABS_Y) {
                    ai.minimum = 0;
                    ai.maximum = displayH_.load() - 1;
                } else if (absCode == ABS_MT_SLOT) {
                    ai.minimum = 0;
                    ai.maximum = 9;
                } else if (absCode == ABS_MT_TRACKING_ID) {
                    ai.minimum = 0;
                    ai.maximum = 65535;
                } else if (absCode == ABS_MT_TOUCH_MAJOR || absCode == ABS_MT_PRESSURE) {
                    ai.minimum = 0;
                    ai.maximum = 255;
                }
                if (out.size() >= sizeof(ai)) std::memcpy(out.data(), &ai, sizeof(ai));
                rc = 0;
            } else if (nr == 0x03) { // EVIOCGREP / EVIOCSREP
                rc = 0;
                if (out.size() >= 8) std::memset(out.data(), 0, 8);
            } else if (nr == 0x90 || nr == 0xa0) { // EVIOCGRAB / EVIOCSCLOCKID
                rc = 0;
                out.clear();
            } else {
                rc = -ENOTTY;
                out.clear();
            }

            if (!SendVdevReply(fd, kVdevOpIoctl, rhdr.proc_id, rc,
                               out.empty() ? nullptr : out.data(),
                               static_cast<uint32_t>(out.size()))) {
                break;
            }
        }
        close(fd);
        return;
    }

    close(fd);
}

void InputBridge::BroadcastEvents(EvdevServer* srv, const void* data, size_t bytes) {
    std::lock_guard<std::mutex> lk(srv->clientsMu);
    for (auto it = srv->streamFds.begin(); it != srv->streamFds.end();) {
        ssize_t w = send(*it, data, bytes, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            close(*it);
            it = srv->streamFds.erase(it);
        } else {
            ++it;
        }
    }
    if (srv->isTouch) touchClients_.store(static_cast<int32_t>(srv->streamFds.size()));
    else kbdClients_.store(static_cast<int32_t>(srv->streamFds.size()));
    eventsSent_.fetch_add(bytes / sizeof(struct input_event));
}

int InputBridge::MapCoordX(float x) const {
    int32_t sw = surfaceW_.load();
    int32_t dw = displayW_.load();
    if (sw <= 0) sw = dw;
    int v = static_cast<int>(std::lround(static_cast<double>(x) * dw / sw));
    return std::clamp(v, 0, dw - 1);
}

int InputBridge::MapCoordY(float y) const {
    int32_t sh = surfaceH_.load();
    int32_t dh = displayH_.load();
    if (sh <= 0) sh = dh;
    int v = static_cast<int>(std::lround(static_cast<double>(y) * dh / sh));
    return std::clamp(v, 0, dh - 1);
}

void InputBridge::InjectTouch(int32_t fingerId, int action, float x, float y) {
    if (!running_.load()) return;
    int ax = MapCoordX(x);
    int ay = MapCoordY(y);
    struct timeval tv = NowMonotonicTv();
    std::vector<struct input_event> evs;
    evs.reserve(10);

    std::lock_guard<std::mutex> lk(touchStateMu_);
    int slot = -1;
    int activeBefore = 0;
    for (int i = 0; i < 10; ++i) {
        if (slotFinger_[i] >= 0) activeBefore++;
        if (slotFinger_[i] == fingerId) slot = i;
    }

    if (action == 0) { // DOWN
        if (slot < 0) {
            for (int i = 0; i < 10; ++i) {
                if (slotFinger_[i] < 0) { slot = i; break; }
            }
        }
        if (slot < 0) return;
        int32_t tid = nextTrackingId_++;
        if (nextTrackingId_ > 65000) nextTrackingId_ = 1;
        slotFinger_[slot] = fingerId;
        slotTrack_[slot] = tid;
        PushEv(evs, tv, EV_ABS, ABS_MT_SLOT, slot);
        PushEv(evs, tv, EV_ABS, ABS_MT_TRACKING_ID, tid);
        PushEv(evs, tv, EV_ABS, ABS_MT_POSITION_X, ax);
        PushEv(evs, tv, EV_ABS, ABS_MT_POSITION_Y, ay);
        PushEv(evs, tv, EV_ABS, ABS_MT_TOUCH_MAJOR, 16);
        PushEv(evs, tv, EV_ABS, ABS_MT_PRESSURE, 128);
        if (activeBefore == 0) {
            PushEv(evs, tv, EV_KEY, BTN_TOUCH, 1);
        }
        PushEv(evs, tv, EV_SYN, SYN_REPORT, 0);
    } else if (action == 1) { // MOVE
        if (slot < 0) return;
        PushEv(evs, tv, EV_ABS, ABS_MT_SLOT, slot);
        PushEv(evs, tv, EV_ABS, ABS_MT_POSITION_X, ax);
        PushEv(evs, tv, EV_ABS, ABS_MT_POSITION_Y, ay);
        PushEv(evs, tv, EV_SYN, SYN_REPORT, 0);
    } else { // UP or CANCEL
        if (slot < 0) return;
        slotFinger_[slot] = -1;
        slotTrack_[slot] = -1;
        int activeAfter = 0;
        for (int i = 0; i < 10; ++i) {
            if (slotFinger_[i] >= 0) activeAfter++;
        }
        PushEv(evs, tv, EV_ABS, ABS_MT_SLOT, slot);
        PushEv(evs, tv, EV_ABS, ABS_MT_TRACKING_ID, -1);
        if (activeAfter == 0) {
            PushEv(evs, tv, EV_KEY, BTN_TOUCH, 0);
        }
        PushEv(evs, tv, EV_SYN, SYN_REPORT, 0);
    }

    if (!evs.empty()) {
        BroadcastEvents(&touchSrv_, evs.data(), evs.size() * sizeof(struct input_event));
    }
}

void InputBridge::InjectMouse(int action, int button, float x, float y) {
    if (!running_.load()) return;
    constexpr int32_t kMouseFingerId = 9000;
    if (button == 2 /* RIGHT */) {
        if (action == 1 /* PRESS */) {
            TapLinuxKey(KEY_BACK);
        }
        return;
    }
    if (button == 4 /* MIDDLE */) {
        if (action == 1 /* PRESS */) {
            TapLinuxKey(KEY_HOMEPAGE);
        }
        return;
    }
    if (action == 1 /* PRESS */ && button == 1 /* LEFT */) {
        {
            std::lock_guard<std::mutex> lk(touchStateMu_);
            mouseLeftDown_ = true;
        }
        InjectTouch(kMouseFingerId, 0, x, y);
    } else if (action == 3 /* MOVE */) {
        bool down = false;
        {
            std::lock_guard<std::mutex> lk(touchStateMu_);
            down = mouseLeftDown_;
        }
        if (down) {
            InjectTouch(kMouseFingerId, 1, x, y);
        }
    } else if (action == 2 /* RELEASE */ || action == 4 /* CANCEL */) {
        bool down = false;
        {
            std::lock_guard<std::mutex> lk(touchStateMu_);
            down = mouseLeftDown_;
            mouseLeftDown_ = false;
        }
        if (down) {
            InjectTouch(kMouseFingerId, 2, x, y);
        }
    }
}

void InputBridge::InjectScroll(float x, float y, float deltaY) {
    if (!running_.load() || std::fabs(deltaY) < 0.01f) return;
    // Synthesize a smooth short vertical swipe on a dedicated touch finger
    constexpr int32_t kScrollFinger = 9001;
    float step = std::clamp(deltaY * 18.0f, -180.0f, 180.0f);
    InjectTouch(kScrollFinger, 0, x, y);
    for (int i = 1; i <= 4; ++i) {
        InjectTouch(kScrollFinger, 1, x, y - step * (static_cast<float>(i) / 4.0f));
    }
    InjectTouch(kScrollFinger, 2, x, y - step);
}

void InputBridge::InjectLinuxKey(uint16_t linuxKey, bool down) {
    if (!running_.load() || linuxKey == 0) return;
    struct timeval tv = NowMonotonicTv();
    std::vector<struct input_event> evs;
    evs.reserve(2);
    PushEv(evs, tv, EV_KEY, linuxKey, down ? 1 : 0);
    PushEv(evs, tv, EV_SYN, SYN_REPORT, 0);
    BroadcastEvents(&kbdSrv_, evs.data(), evs.size() * sizeof(struct input_event));
}

void InputBridge::TapLinuxKey(uint16_t linuxKey) {
    InjectLinuxKey(linuxKey, true);
    InjectLinuxKey(linuxKey, false);
}

uint16_t InputBridge::MapOhosKeyToLinux(int32_t k) {
    // Map OH_NativeXComponent_KeyCode (native_xcomponent_key_event.h) -> Linux input-event-codes.h
    if (k >= 2017 && k <= 2042) {
        static const uint16_t alpha[26] = {
            KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
            KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
            KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z
        };
        return alpha[k - 2017];
    }
    if (k >= 2000 && k <= 2009) {
        static const uint16_t digits[10] = {
            KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9
        };
        return digits[k - 2000];
    }
    switch (k) {
        case 1: return KEY_HOMEPAGE;
        case 2: return KEY_BACK;
        case 16: return KEY_VOLUMEUP;
        case 17: return KEY_VOLUMEDOWN;
        case 18: return KEY_POWER;
        case 2012: return KEY_UP;
        case 2013: return KEY_DOWN;
        case 2014: return KEY_LEFT;
        case 2015: return KEY_RIGHT;
        case 2016: return KEY_ENTER;
        case 2043: return KEY_COMMA;
        case 2044: return KEY_DOT;
        case 2045: return KEY_LEFTALT;
        case 2046: return KEY_RIGHTALT;
        case 2047: return KEY_LEFTSHIFT;
        case 2048: return KEY_RIGHTSHIFT;
        case 2049: return KEY_TAB;
        case 2050: return KEY_SPACE;
        case 2054: return KEY_ENTER;
        case 2055: return KEY_BACKSPACE;
        case 2056: return KEY_GRAVE;
        case 2057: return KEY_MINUS;
        case 2058: return KEY_EQUAL;
        case 2059: return KEY_LEFTBRACE;
        case 2060: return KEY_RIGHTBRACE;
        case 2061: return KEY_BACKSLASH;
        case 2062: return KEY_SEMICOLON;
        case 2063: return KEY_APOSTROPHE;
        case 2064: return KEY_SLASH;
        case 2067: return KEY_MENU;
        case 2068: return KEY_PAGEUP;
        case 2069: return KEY_PAGEDOWN;
        case 2070: return KEY_BACK; // ESC -> Android Back
        case 2071: return KEY_DELETE;
        case 2072: return KEY_LEFTCTRL;
        case 2073: return KEY_RIGHTCTRL;
        case 2074: return KEY_CAPSLOCK;
        case 2081: return KEY_HOME;
        case 2082: return KEY_END;
        case 2119: return KEY_KPENTER;
        case 2210: return KEY_APPSELECT;
        default: return 0;
    }
}

void InputBridge::InjectOhosKey(int32_t ohosKeyCode, bool down) {
    uint16_t lk = MapOhosKeyToLinux(ohosKeyCode);
    if (lk != 0) {
        InjectLinuxKey(lk, down);
    }
}
