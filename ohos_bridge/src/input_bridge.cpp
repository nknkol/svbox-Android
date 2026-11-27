/* SPDX-License-Identifier: MIT */
#include "ohos_bridge/input_bridge.h"
#include "ohos_bridge/log.h"
#include "ohos_bridge/sched_qos.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <errno.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

namespace {

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
#ifndef BTN_SOUTH
#define BTN_SOUTH          0x130
#define BTN_EAST           0x131
#define BTN_NORTH          0x133
#define BTN_WEST           0x134
#define BTN_TL             0x136
#define BTN_TR             0x137
#define BTN_TL2            0x138
#define BTN_TR2            0x139
#define BTN_SELECT         0x13a
#define BTN_START          0x13b
#define BTN_MODE           0x13c
#define BTN_THUMBL         0x13d
#define BTN_THUMBR         0x13e
#endif
#ifndef BTN_DPAD_UP
#define BTN_DPAD_UP        0x220
#define BTN_DPAD_DOWN      0x221
#define BTN_DPAD_LEFT      0x222
#define BTN_DPAD_RIGHT     0x223
#endif

/* Dynamic HarmonyOS GameControllerKit (libohgame_controller.z.so) trampolines */
using GckBtnCb = void (*)(const void* btnEvent);
using GckAxisCb = void (*)(const void* axisEvent);
using GckGetBtnActionFn = int32_t (*)(const void* btnEvent, int32_t* action);
using GckGetAxisValFn = int32_t (*)(const void* axisEvent, double* val);
using GckRegBtnFn = int32_t (*)(GckBtnCb);
using GckUnregFn = int32_t (*)(void);
using GckRegAxisFn = int32_t (*)(GckAxisCb);

struct GckApi {
    GckGetBtnActionFn getBtnAction = nullptr;
    GckGetAxisValFn getX = nullptr;
    GckGetAxisValFn getY = nullptr;
    GckGetAxisValFn getZ = nullptr;
    GckGetAxisValFn getRZ = nullptr;
    GckGetAxisValFn getHatX = nullptr;
    GckGetAxisValFn getHatY = nullptr;
    GckGetAxisValFn getBrake = nullptr;
    GckGetAxisValFn getGas = nullptr;
};

GckApi g_gck;

template <uint16_t LinuxBtn>
void OnGckButton(const void* ev) {
    if (!ev || !g_gck.getBtnAction) return;
    int32_t act = 1;
    if (g_gck.getBtnAction(ev, &act) == 0) {
        InputBridge::Instance().InjectGamepadButton(LinuxBtn, act == 0 /* DOWN */);
    }
}

void OnGckLeftStick(const void* ev) {
    if (!ev || !g_gck.getX || !g_gck.getY) return;
    double x = 0.0, y = 0.0;
    if (g_gck.getX(ev, &x) == 0 && g_gck.getY(ev, &y) == 0) {
        int32_t ax = std::clamp<int32_t>(static_cast<int32_t>(std::lround(x * 32767.0)), -32768, 32767);
        int32_t ay = std::clamp<int32_t>(static_cast<int32_t>(std::lround(y * 32767.0)), -32768, 32767);
        InputBridge::Instance().InjectGamepadFrame({ { ABS_X, ax }, { ABS_Y, ay } }, {});
    }
}

void OnGckRightStick(const void* ev) {
    if (!ev || !g_gck.getZ || !g_gck.getRZ) return;
    double rx = 0.0, ry = 0.0;
    if (g_gck.getZ(ev, &rx) == 0 && g_gck.getRZ(ev, &ry) == 0) {
        int32_t arx = std::clamp<int32_t>(static_cast<int32_t>(std::lround(rx * 32767.0)), -32768, 32767);
        int32_t ary = std::clamp<int32_t>(static_cast<int32_t>(std::lround(ry * 32767.0)), -32768, 32767);
        InputBridge::Instance().InjectGamepadFrame({ { ABS_RX, arx }, { ABS_RY, ary } }, {});
    }
}

void OnGckLeftTrigger(const void* ev) {
    if (!ev || !g_gck.getBrake) return;
    double v = 0.0;
    if (g_gck.getBrake(ev, &v) == 0) {
        int32_t av = std::clamp<int32_t>(static_cast<int32_t>(std::lround(v * 255.0)), 0, 255);
        InputBridge::Instance().InjectGamepadAxis(ABS_Z, av);
    }
}

void OnGckRightTrigger(const void* ev) {
    if (!ev || !g_gck.getGas) return;
    double v = 0.0;
    if (g_gck.getGas(ev, &v) == 0) {
        int32_t av = std::clamp<int32_t>(static_cast<int32_t>(std::lround(v * 255.0)), 0, 255);
        InputBridge::Instance().InjectGamepadAxis(ABS_RZ, av);
    }
}

void OnGckDpadAxis(const void* ev) {
    if (!ev || !g_gck.getHatX || !g_gck.getHatY) return;
    double hx = 0.0, hy = 0.0;
    if (g_gck.getHatX(ev, &hx) == 0 && g_gck.getHatY(ev, &hy) == 0) {
        int32_t ix = hx > 0.5 ? 1 : (hx < -0.5 ? -1 : 0);
        int32_t iy = hy > 0.5 ? 1 : (hy < -0.5 ? -1 : 0);
        InputBridge::Instance().InjectGamepadFrame({ { ABS_HAT0X, ix }, { ABS_HAT0Y, iy } }, {});
    }
}

struct GckBtnBinding {
    const char* regSym;
    const char* unregSym;
    GckBtnCb cb;
};

const GckBtnBinding kGckBtnBindings[] = {
    { "OH_GamePad_ButtonA_RegisterButtonInputMonitor", "OH_GamePad_ButtonA_UnregisterButtonInputMonitor", &OnGckButton<BTN_SOUTH> },
    { "OH_GamePad_ButtonB_RegisterButtonInputMonitor", "OH_GamePad_ButtonB_UnregisterButtonInputMonitor", &OnGckButton<BTN_EAST> },
    { "OH_GamePad_ButtonX_RegisterButtonInputMonitor", "OH_GamePad_ButtonX_UnregisterButtonInputMonitor", &OnGckButton<BTN_WEST> },
    { "OH_GamePad_ButtonY_RegisterButtonInputMonitor", "OH_GamePad_ButtonY_UnregisterButtonInputMonitor", &OnGckButton<BTN_NORTH> },
    { "OH_GamePad_LeftShoulder_RegisterButtonInputMonitor", "OH_GamePad_LeftShoulder_UnregisterButtonInputMonitor", &OnGckButton<BTN_TL> },
    { "OH_GamePad_RightShoulder_RegisterButtonInputMonitor", "OH_GamePad_RightShoulder_UnregisterButtonInputMonitor", &OnGckButton<BTN_TR> },
    { "OH_GamePad_LeftTrigger_RegisterButtonInputMonitor", "OH_GamePad_LeftTrigger_UnregisterButtonInputMonitor", &OnGckButton<BTN_TL2> },
    { "OH_GamePad_RightTrigger_RegisterButtonInputMonitor", "OH_GamePad_RightTrigger_UnregisterButtonInputMonitor", &OnGckButton<BTN_TR2> },
    { "OH_GamePad_ButtonMenu_RegisterButtonInputMonitor", "OH_GamePad_ButtonMenu_UnregisterButtonInputMonitor", &OnGckButton<BTN_START> },
    { "OH_GamePad_ButtonHome_RegisterButtonInputMonitor", "OH_GamePad_ButtonHome_UnregisterButtonInputMonitor", &OnGckButton<BTN_MODE> },
    { "OH_GamePad_LeftThumbstick_RegisterButtonInputMonitor", "OH_GamePad_LeftThumbstick_UnregisterButtonInputMonitor", &OnGckButton<BTN_THUMBL> },
    { "OH_GamePad_RightThumbstick_RegisterButtonInputMonitor", "OH_GamePad_RightThumbstick_UnregisterButtonInputMonitor", &OnGckButton<BTN_THUMBR> },
    { "OH_GamePad_Dpad_UpButton_RegisterButtonInputMonitor", "OH_GamePad_Dpad_UpButton_UnregisterButtonInputMonitor", &OnGckButton<BTN_DPAD_UP> },
    { "OH_GamePad_Dpad_DownButton_RegisterButtonInputMonitor", "OH_GamePad_Dpad_DownButton_UnregisterButtonInputMonitor", &OnGckButton<BTN_DPAD_DOWN> },
    { "OH_GamePad_Dpad_LeftButton_RegisterButtonInputMonitor", "OH_GamePad_Dpad_LeftButton_UnregisterButtonInputMonitor", &OnGckButton<BTN_DPAD_LEFT> },
    { "OH_GamePad_Dpad_RightButton_RegisterButtonInputMonitor", "OH_GamePad_Dpad_RightButton_UnregisterButtonInputMonitor", &OnGckButton<BTN_DPAD_RIGHT> },
};

struct GckAxisBinding {
    const char* regSym;
    const char* unregSym;
    GckAxisCb cb;
};

const GckAxisBinding kGckAxisBindings[] = {
    { "OH_GamePad_LeftThumbstick_RegisterAxisInputMonitor", "OH_GamePad_LeftThumbstick_UnregisterAxisInputMonitor", &OnGckLeftStick },
    { "OH_GamePad_RightThumbstick_RegisterAxisInputMonitor", "OH_GamePad_RightThumbstick_UnregisterAxisInputMonitor", &OnGckRightStick },
    { "OH_GamePad_LeftTrigger_RegisterAxisInputMonitor", "OH_GamePad_LeftTrigger_UnregisterAxisInputMonitor", &OnGckLeftTrigger },
    { "OH_GamePad_RightTrigger_RegisterAxisInputMonitor", "OH_GamePad_RightTrigger_UnregisterAxisInputMonitor", &OnGckRightTrigger },
    { "OH_GamePad_Dpad_RegisterAxisInputMonitor", "OH_GamePad_Dpad_UnregisterAxisInputMonitor", &OnGckDpadAxis },
};

} // namespace

InputBridge& InputBridge::Instance() {
    static InputBridge inst;
    return inst;
}

void InputBridge::TryAttachGameControllerKit() {
    if (gckHandle_) return;
    gckHandle_ = dlopen("libohgame_controller.z.so", RTLD_NOW | RTLD_LOCAL);
    if (!gckHandle_) {
        gckActive_.store(false);
        return;
    }
    g_gck.getBtnAction = reinterpret_cast<GckGetBtnActionFn>(dlsym(gckHandle_, "OH_GamePad_ButtonEvent_GetButtonAction"));
    g_gck.getX = reinterpret_cast<GckGetAxisValFn>(dlsym(gckHandle_, "OH_GamePad_AxisEvent_GetXAxisValue"));
    g_gck.getY = reinterpret_cast<GckGetAxisValFn>(dlsym(gckHandle_, "OH_GamePad_AxisEvent_GetYAxisValue"));
    g_gck.getZ = reinterpret_cast<GckGetAxisValFn>(dlsym(gckHandle_, "OH_GamePad_AxisEvent_GetZAxisValue"));
    g_gck.getRZ = reinterpret_cast<GckGetAxisValFn>(dlsym(gckHandle_, "OH_GamePad_AxisEvent_GetRZAxisValue"));
    g_gck.getHatX = reinterpret_cast<GckGetAxisValFn>(dlsym(gckHandle_, "OH_GamePad_AxisEvent_GetHatXAxisValue"));
    g_gck.getHatY = reinterpret_cast<GckGetAxisValFn>(dlsym(gckHandle_, "OH_GamePad_AxisEvent_GetHatYAxisValue"));
    g_gck.getBrake = reinterpret_cast<GckGetAxisValFn>(dlsym(gckHandle_, "OH_GamePad_AxisEvent_GetBrakeAxisValue"));
    g_gck.getGas = reinterpret_cast<GckGetAxisValFn>(dlsym(gckHandle_, "OH_GamePad_AxisEvent_GetGasAxisValue"));

    int registered = 0;
    for (const auto& b : kGckBtnBindings) {
        auto fn = reinterpret_cast<GckRegBtnFn>(dlsym(gckHandle_, b.regSym));
        if (fn && fn(b.cb) == 0) registered++;
    }
    for (const auto& a : kGckAxisBindings) {
        auto fn = reinterpret_cast<GckRegAxisFn>(dlsym(gckHandle_, a.regSym));
        if (fn && fn(a.cb) == 0) registered++;
    }
    gckActive_.store(registered > 0);
    OB_LOG_INFO("SvboxInput", "GameControllerKit attached (%d monitors registered)", registered);
}

void InputBridge::DetachGameControllerKit() {
    if (!gckHandle_) return;
    for (const auto& b : kGckBtnBindings) {
        auto fn = reinterpret_cast<GckUnregFn>(dlsym(gckHandle_, b.unregSym));
        if (fn) fn();
    }
    for (const auto& a : kGckAxisBindings) {
        auto fn = reinterpret_cast<GckUnregFn>(dlsym(gckHandle_, a.unregSym));
        if (fn) fn();
    }
    dlclose(gckHandle_);
    gckHandle_ = nullptr;
    gckActive_.store(false);
}

void InputBridge::Start(const std::string& touchSockPath,
                        const std::string& kbdSockPath,
                        int32_t displayW,
                        int32_t displayH,
                        const std::string& gamepadSockPath,
                        bool mapEscToBack) {
    Stop();
    displayW_.store(displayW > 0 ? displayW : 720);
    displayH_.store(displayH > 0 ? displayH : 1280);
    display0Rotation_.store(0);
    hostCtrlHeld_.store(false);
    hostAltHeld_.store(false);
    for (int i = 0; i <= kMaxSecondaryDisplays; ++i) {
        hostImeActive_[i].store(false);
        lastPtrX_[i].store(-1.0f);
        lastPtrY_[i].store(-1.0f);
    }
    mapEscToBack_.store(mapEscToBack);
    eventsSent_.store(0);
    touchClients_.store(0);
    kbdClients_.store(0);
    gamepadClients_.store(0);
    {
        std::lock_guard<std::mutex> lk(touchStateMu_);
        mouseLeftDown_ = false;
        nextTrackingId_ = 1;
        for (int i = 0; i < 10; ++i) {
            slotFinger_[i] = -1;
            slotTrack_[i] = -1;
        }
    }
    {
        std::lock_guard<std::mutex> lk(gamepadStateMu_);
        std::memset(gamepadAbs_, 0, sizeof(gamepadAbs_));
    }

    running_.store(true);
    if (!touchSockPath.empty()) {
        touchSrv_.kind = DevKind::Touch;
        touchSrv_.displaySlot = 0;
        touchSrv_.sockPath = touchSockPath;
        touchSrv_.listenFd = -1;
        touchSrv_.thread = std::thread(&InputBridge::RunServer, this, &touchSrv_);
    }
    if (!kbdSockPath.empty()) {
        kbdSrv_.kind = DevKind::Keyboard;
        kbdSrv_.displaySlot = 0;
        kbdSrv_.sockPath = kbdSockPath;
        kbdSrv_.listenFd = -1;
        kbdSrv_.thread = std::thread(&InputBridge::RunServer, this, &kbdSrv_);
    }
    if (!gamepadSockPath.empty()) {
        gamepadSrv_.kind = DevKind::Gamepad;
        gamepadSrv_.displaySlot = 0;
        gamepadSrv_.sockPath = gamepadSockPath;
        gamepadSrv_.listenFd = -1;
        gamepadSrv_.thread = std::thread(&InputBridge::RunServer, this, &gamepadSrv_);
        TryAttachGameControllerKit();
    }
}

void InputBridge::StartSecondaryTouch(int displaySlot, const std::string& sockPath) {
    if (displaySlot < 1 || displaySlot > kMaxSecondaryDisplays || sockPath.empty()) return;
    if (!running_.load()) return;
    int idx = displaySlot - 1;
    auto& st = secondaryTouchState_[idx];
    {
        std::lock_guard<std::mutex> lk(st.mu);
        st.mouseLeftDown = false;
        st.nextTrackingId = 1;
        for (int i = 0; i < 10; ++i) {
            st.slotFinger[i] = -1;
            st.slotTrack[i] = -1;
        }
    }
    auto& srv = secondaryTouchSrvs_[idx];
    if (srv.thread.joinable()) return;
    srv.kind = DevKind::Touch;
    srv.displaySlot = displaySlot;
    srv.sockPath = sockPath;
    srv.listenFd = -1;
    srv.thread = std::thread(&InputBridge::RunServer, this, &srv);
}

void InputBridge::Stop() {
    running_.store(false);
    DetachGameControllerKit();
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
            srv.sockPath.clear();
        }
    };
    stopSrv(touchSrv_);
    stopSrv(kbdSrv_);
    stopSrv(gamepadSrv_);
    for (int i = 0; i < kMaxSecondaryDisplays; ++i) {
        stopSrv(secondaryTouchSrvs_[i]);
    }
    touchClients_.store(0);
    kbdClients_.store(0);
    gamepadClients_.store(0);
}

void InputBridge::SetSurfaceSize(int32_t surfaceW, int32_t surfaceH) {
    SetDisplaySurfaceSize(0, surfaceW, surfaceH);
}

void InputBridge::SetDisplaySurfaceSize(int displaySlot, int32_t surfaceW, int32_t surfaceH) {
    if (surfaceW <= 0 || surfaceH <= 0) return;
    if (displaySlot == 0) {
        surfaceW_.store(surfaceW);
        surfaceH_.store(surfaceH);
    } else if (displaySlot >= 1 && displaySlot <= kMaxSecondaryDisplays) {
        secondaryTouchState_[displaySlot - 1].surfaceW.store(surfaceW);
        secondaryTouchState_[displaySlot - 1].surfaceH.store(surfaceH);
    }
}

void InputBridge::RunServer(EvdevServer* srv) {
    const char* label = srv->kind == DevKind::Touch ? "input-touch" :
                        (srv->kind == DevKind::Keyboard ? "input-kbd" : "input-gamepad");
    SchedQos::SetCurrentThreadQoS(SchedQos::Level::UserInteractive, label);
    unlink(srv->sockPath.c_str());
    int sfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sfd < 0) {
        OB_LOG_ERROR("SvboxInput", "InputBridge socket failed: %d", errno);
        return;
    }
    struct sockaddr_un sa = {};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, srv->sockPath.c_str(), sizeof(sa.sun_path) - 1);
    if (bind(sfd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) < 0 ||
        listen(sfd, 16) < 0) {
        OB_LOG_ERROR("SvboxInput", "InputBridge bind/listen %s failed: %d",
                     srv->sockPath.c_str(), errno);
        close(sfd);
        return;
    }
    chmod(srv->sockPath.c_str(), 0666);
    srv->listenFd = sfd;
    OB_LOG_INFO("SvboxInput", "InputBridge listening on %s", srv->sockPath.c_str());

    while (running_.load()) {
        struct pollfd pfd = { sfd, POLLIN, 0 };
        int r = poll(&pfd, 1, 150);
        if (r <= 0 || !(pfd.revents & POLLIN)) continue;
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

    auto updateClientCount = [this, srv](int32_t cnt) {
        if (srv->kind == DevKind::Touch && srv->displaySlot == 0) touchClients_.store(cnt);
        else if (srv->kind == DevKind::Keyboard) kbdClients_.store(cnt);
        else if (srv->kind == DevKind::Gamepad) gamepadClients_.store(cnt);
    };

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
        rep.major = 13u;
        if (srv->kind == DevKind::Touch && srv->displaySlot > 0) {
            rep.minor = static_cast<uint32_t>(73 + srv->displaySlot); // event10..event17
        } else {
            rep.minor = (srv->kind == DevKind::Touch) ? 64u :
                        (srv->kind == DevKind::Keyboard ? 65u : 66u);
        }
        rep.memfd_size = 0;
        if (!SendVdevReply(fd, kVdevOpOpen, rep.proc_id, 0, &rep, sizeof(rep))) {
            close(fd);
            return;
        }
        if (srv->kind == DevKind::Touch && srv->displaySlot > 0) {
            OB_LOG_INFO("SvboxInput", "evdev secondary touch slot=%d opened by pid=%u",
                        srv->displaySlot, req.pid);
        } else {
            const char* tag = srv->kind == DevKind::Touch ? "event0(touch)" :
                              (srv->kind == DevKind::Keyboard ? "event1(kbd)" : "event2(gamepad)");
            OB_LOG_INFO("SvboxInput", "evdev %s opened by pid=%u", tag, req.pid);
        }
        {
            std::lock_guard<std::mutex> lk(srv->clientsMu);
            srv->streamFds.push_back(fd);
            updateClientCount(static_cast<int32_t>(srv->streamFds.size()));
        }
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
            updateClientCount(static_cast<int32_t>(srv->streamFds.size()));
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
                if (srv->kind == DevKind::Gamepad) {
                    id.bustype = BUS_USB;
                    id.vendor = 0x045e;  // Microsoft Xbox 360 standard gamepad profile
                    id.product = 0x028e;
                    id.version = 0x0114;
                } else {
                    id.bustype = BUS_VIRTUAL;
                    id.vendor = 0x18d1;
                    id.product = srv->kind == DevKind::Touch ? 0x0001 : 0x0002;
                    id.version = 0x0001;
                }
                if (out.size() >= sizeof(id)) std::memcpy(out.data(), &id, sizeof(id));
            } else if (nr == 0x06) { // EVIOCGNAME
                char buf[64];
                const char* s = nullptr;
                if (srv->kind == DevKind::Touch && srv->displaySlot > 0) {
                    snprintf(buf, sizeof(buf), "svbox-touchscreen-%d", srv->displaySlot);
                    s = buf;
                } else {
                    s = srv->kind == DevKind::Touch ? "svbox-touchscreen" :
                        (srv->kind == DevKind::Keyboard ? "svbox-keyboard" : "svbox-gamepad");
                }
                size_t n = std::min(out.size(), std::strlen(s) + 1);
                if (n > 0) {
                    std::memcpy(out.data(), s, n);
                    out[n - 1] = '\0';
                }
                out.resize(n);
                rc = static_cast<int32_t>(n);
            } else if (nr == 0x07) { // EVIOCGPHYS
                char buf[64];
                const char* s = nullptr;
                if (srv->kind == DevKind::Touch && srv->displaySlot > 0) {
                    snprintf(buf, sizeof(buf), "svbox-disp-%d", srv->displaySlot);
                    s = buf;
                } else {
                    s = srv->kind == DevKind::Touch ? "svbox/input0" :
                        (srv->kind == DevKind::Keyboard ? "svbox/input1" : "svbox/input2");
                }
                size_t n = std::min(out.size(), std::strlen(s) + 1);
                if (n > 0) {
                    std::memcpy(out.data(), s, n);
                    out[n - 1] = '\0';
                }
                out.resize(n);
                rc = static_cast<int32_t>(n);
            } else if (nr == 0x08) { // EVIOCGUNIQ
                char buf[64];
                const char* s = nullptr;
                if (srv->kind == DevKind::Touch && srv->displaySlot > 0) {
                    snprintf(buf, sizeof(buf), "svbox-touch-%d", srv->displaySlot);
                    s = buf;
                } else {
                    s = srv->kind == DevKind::Touch ? "svbox-touch-0" :
                        (srv->kind == DevKind::Keyboard ? "svbox-kbd-1" : "svbox-gamepad-2");
                }
                size_t n = std::min(out.size(), std::strlen(s) + 1);
                if (n > 0) {
                    std::memcpy(out.data(), s, n);
                    out[n - 1] = '\0';
                }
                out.resize(n);
                rc = static_cast<int32_t>(n);
            } else if (nr == 0x09) { // EVIOCGPROP
                if (srv->kind == DevKind::Touch) {
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
                    if (srv->kind == DevKind::Touch || srv->kind == DevKind::Gamepad) {
                        SetBit(out.data(), out.size(), EV_ABS);
                    }
                } else if (evType == EV_KEY) {
                    if (srv->kind == DevKind::Touch) {
                        SetBit(out.data(), out.size(), BTN_TOUCH);
                    } else if (srv->kind == DevKind::Keyboard) {
                        for (uint32_t k = 1; k <= 248; ++k) SetBit(out.data(), out.size(), k);
                        SetBit(out.data(), out.size(), KEY_APPSELECT);
                    } else { // Gamepad
                        const uint32_t btns[] = {
                            BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST,
                            BTN_TL, BTN_TR, BTN_TL2, BTN_TR2,
                            BTN_SELECT, BTN_START, BTN_MODE,
                            BTN_THUMBL, BTN_THUMBR,
                            BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT
                        };
                        for (uint32_t b : btns) SetBit(out.data(), out.size(), b);
                    }
                } else if (evType == EV_ABS) {
                    if (srv->kind == DevKind::Touch) {
                        SetBit(out.data(), out.size(), ABS_X);
                        SetBit(out.data(), out.size(), ABS_Y);
                        SetBit(out.data(), out.size(), ABS_MT_SLOT);
                        SetBit(out.data(), out.size(), ABS_MT_TOUCH_MAJOR);
                        SetBit(out.data(), out.size(), ABS_MT_POSITION_X);
                        SetBit(out.data(), out.size(), ABS_MT_POSITION_Y);
                        SetBit(out.data(), out.size(), ABS_MT_TRACKING_ID);
                        SetBit(out.data(), out.size(), ABS_MT_PRESSURE);
                    } else if (srv->kind == DevKind::Gamepad) {
                        SetBit(out.data(), out.size(), ABS_X);
                        SetBit(out.data(), out.size(), ABS_Y);
                        SetBit(out.data(), out.size(), ABS_Z);
                        SetBit(out.data(), out.size(), ABS_RX);
                        SetBit(out.data(), out.size(), ABS_RY);
                        SetBit(out.data(), out.size(), ABS_RZ);
                        SetBit(out.data(), out.size(), ABS_HAT0X);
                        SetBit(out.data(), out.size(), ABS_HAT0Y);
                    }
                }
                rc = static_cast<int32_t>(out.size());
            } else if (nr >= 0x40 && nr < 0x80) { // EVIOCGABS(abs)
                uint32_t absCode = nr - 0x40;
                struct input_absinfo ai = {};
                if (srv->kind == DevKind::Gamepad) {
                    if (absCode == ABS_X || absCode == ABS_Y || absCode == ABS_RX || absCode == ABS_RY) {
                        ai.minimum = -32768;
                        ai.maximum = 32767;
                        ai.fuzz = 16;
                        ai.flat = 128;
                    } else if (absCode == ABS_Z || absCode == ABS_RZ) {
                        ai.minimum = 0;
                        ai.maximum = 255;
                    } else if (absCode == ABS_HAT0X || absCode == ABS_HAT0Y) {
                        ai.minimum = -1;
                        ai.maximum = 1;
                    }
                    if (absCode < 64) {
                        std::lock_guard<std::mutex> lk(gamepadStateMu_);
                        ai.value = gamepadAbs_[absCode];
                    }
                } else {
                    const int32_t maxX = (srv->displaySlot > 0) ? 32767 : (displayW_.load() - 1);
                    const int32_t maxY = (srv->displaySlot > 0) ? 32767 : (displayH_.load() - 1);
                    if (absCode == ABS_MT_POSITION_X || absCode == ABS_X) {
                        ai.minimum = 0;
                        ai.maximum = maxX;
                    } else if (absCode == ABS_MT_POSITION_Y || absCode == ABS_Y) {
                        ai.minimum = 0;
                        ai.maximum = maxY;
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
    int32_t sz = static_cast<int32_t>(srv->streamFds.size());
    if (srv->kind == DevKind::Touch && srv->displaySlot == 0) touchClients_.store(sz);
    else if (srv->kind == DevKind::Keyboard) kbdClients_.store(sz);
    else if (srv->kind == DevKind::Gamepad) gamepadClients_.store(sz);
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

int InputBridge::MapSecondaryCoord(float coord, int32_t surfaceSpan) const {
    if (surfaceSpan <= 0) surfaceSpan = 1000;
    int v = static_cast<int>(std::lround(static_cast<double>(coord) * 32767.0 / surfaceSpan));
    return std::clamp(v, 0, 32767);
}

void InputBridge::InjectTouch(int32_t fingerId, int action, float x, float y) {
    InjectTouchForDisplay(0, fingerId, action, x, y);
}

void InputBridge::InjectTouchForDisplay(int displaySlot, int32_t fingerId, int action, float x, float y) {
    if (!running_.load()) return;
    if (displaySlot < 0 || displaySlot > kMaxSecondaryDisplays) return;

    EvdevServer* targetSrv = &touchSrv_;
    std::mutex* stateMu = &touchStateMu_;
    int32_t* slotFinger = slotFinger_;
    int32_t* slotTrack = slotTrack_;
    int32_t* nextTid = &nextTrackingId_;
    bool* mouseLeftDownPtr = &mouseLeftDown_;
    int64_t* lastMouseDownUsPtr = &lastMouseDownUs_;
    int* lastMouseAxPtr = &lastMouseAx_;
    int* lastMouseAyPtr = &lastMouseAy_;
    int ax = 0;
    int ay = 0;

    if (displaySlot == 0) {
        ax = MapCoordX(x);
        ay = MapCoordY(y);
    } else {
        int idx = displaySlot - 1;
        targetSrv = &secondaryTouchSrvs_[idx];
        auto& st = secondaryTouchState_[idx];
        stateMu = &st.mu;
        slotFinger = st.slotFinger;
        slotTrack = st.slotTrack;
        nextTid = &st.nextTrackingId;
        mouseLeftDownPtr = &st.mouseLeftDown;
        lastMouseDownUsPtr = &st.lastMouseDownUs;
        lastMouseAxPtr = &st.lastMouseAx;
        lastMouseAyPtr = &st.lastMouseAy;
        ax = MapSecondaryCoord(x, st.surfaceW.load());
        ay = MapSecondaryCoord(y, st.surfaceH.load());
    }

    struct timeval tv = NowMonotonicTv();
    int64_t nowUs = static_cast<int64_t>(tv.tv_sec) * 1000000LL + static_cast<int64_t>(tv.tv_usec);
    std::vector<struct input_event> evs;
    evs.reserve(10);

    std::lock_guard<std::mutex> lk(*stateMu);
    // Drop synthetic touch DOWN duplicated alongside an active mouse click at the same coordinate
    // so Chromium/WebView does not misclassify a single mouse click as a 2-finger pinch gesture.
    if (fingerId != 9000 && action == 0 && *mouseLeftDownPtr &&
        (nowUs - *lastMouseDownUsPtr) >= 0 && (nowUs - *lastMouseDownUsPtr) < 40000 &&
        std::abs(ax - *lastMouseAxPtr) <= 6 && std::abs(ay - *lastMouseAyPtr) <= 6) {
        return;
    }
    int slot = -1;
    int activeBefore = 0;
    for (int i = 0; i < 10; ++i) {
        if (slotFinger[i] >= 0) activeBefore++;
        if (slotFinger[i] == fingerId) slot = i;
    }

    if (action == 0) {
        lastPtrX_[displaySlot].store(x);
        lastPtrY_[displaySlot].store(y);
        if (slot < 0) {
            for (int i = 0; i < 10; ++i) {
                if (slotFinger[i] < 0) { slot = i; break; }
            }
        }
        if (slot < 0) return;
        int32_t tid = (*nextTid)++;
        if (*nextTid > 65000) *nextTid = 1;
        slotFinger[slot] = fingerId;
        slotTrack[slot] = tid;
        PushEv(evs, tv, EV_ABS, ABS_MT_SLOT, slot);
        PushEv(evs, tv, EV_ABS, ABS_MT_TRACKING_ID, tid);
        PushEv(evs, tv, EV_ABS, ABS_MT_POSITION_X, ax);
        PushEv(evs, tv, EV_ABS, ABS_MT_POSITION_Y, ay);
        PushEv(evs, tv, EV_ABS, ABS_MT_TOUCH_MAJOR, 16);
        PushEv(evs, tv, EV_ABS, ABS_MT_PRESSURE, 128);
        if (activeBefore == 0) PushEv(evs, tv, EV_KEY, BTN_TOUCH, 1);
        PushEv(evs, tv, EV_SYN, SYN_REPORT, 0);
    } else if (action == 1) {
        if (slot < 0) return;
        PushEv(evs, tv, EV_ABS, ABS_MT_SLOT, slot);
        PushEv(evs, tv, EV_ABS, ABS_MT_POSITION_X, ax);
        PushEv(evs, tv, EV_ABS, ABS_MT_POSITION_Y, ay);
        PushEv(evs, tv, EV_SYN, SYN_REPORT, 0);
    } else {
        if (slot < 0) return;
        slotFinger[slot] = -1;
        slotTrack[slot] = -1;
        int activeAfter = 0;
        for (int i = 0; i < 10; ++i) {
            if (slotFinger[i] >= 0) activeAfter++;
        }
        PushEv(evs, tv, EV_ABS, ABS_MT_SLOT, slot);
        PushEv(evs, tv, EV_ABS, ABS_MT_TRACKING_ID, -1);
        if (activeAfter == 0) PushEv(evs, tv, EV_KEY, BTN_TOUCH, 0);
        PushEv(evs, tv, EV_SYN, SYN_REPORT, 0);
    }

    if (!evs.empty()) {
        BroadcastEvents(targetSrv, evs.data(), evs.size() * sizeof(struct input_event));
    }
}

void InputBridge::InjectMouse(int action, int button, float x, float y) {
    InjectMouseForDisplay(0, action, button, x, y);
}

void InputBridge::InjectMouseForDisplay(int displaySlot, int action, int button, float x, float y) {
    if (!running_.load()) return;
    if (displaySlot < 0 || displaySlot > kMaxSecondaryDisplays) return;
    constexpr int32_t kMouseFingerId = 9000;
    if (button == 2) {
        if (action == 1) TapLinuxKey(KEY_BACK);
        return;
    }
    if (button == 4) {
        if (action == 1) TapLinuxKey(KEY_HOMEPAGE);
        return;
    }
    std::mutex* stateMu = (displaySlot == 0) ? &touchStateMu_ : &secondaryTouchState_[displaySlot - 1].mu;
    bool* leftDown = (displaySlot == 0) ? &mouseLeftDown_ : &secondaryTouchState_[displaySlot - 1].mouseLeftDown;
    int64_t* lastDownUs = (displaySlot == 0) ? &lastMouseDownUs_ : &secondaryTouchState_[displaySlot - 1].lastMouseDownUs;
    int* lastAx = (displaySlot == 0) ? &lastMouseAx_ : &secondaryTouchState_[displaySlot - 1].lastMouseAx;
    int* lastAy = (displaySlot == 0) ? &lastMouseAy_ : &secondaryTouchState_[displaySlot - 1].lastMouseAy;

    if (action == 1 && button == 1) {
        struct timeval tv = NowMonotonicTv();
        int64_t nowUs = static_cast<int64_t>(tv.tv_sec) * 1000000LL + static_cast<int64_t>(tv.tv_usec);
        int ax = (displaySlot == 0) ? MapCoordX(x)
                                    : MapSecondaryCoord(x, secondaryTouchState_[displaySlot - 1].surfaceW.load());
        int ay = (displaySlot == 0) ? MapCoordY(y)
                                    : MapSecondaryCoord(y, secondaryTouchState_[displaySlot - 1].surfaceH.load());
        {
            std::lock_guard<std::mutex> lk(*stateMu);
            *leftDown = true;
            *lastDownUs = nowUs;
            *lastAx = ax;
            *lastAy = ay;
        }
        InjectTouchForDisplay(displaySlot, kMouseFingerId, 0, x, y);
    } else if (action == 3) {
        bool down = false;
        {
            std::lock_guard<std::mutex> lk(*stateMu);
            down = *leftDown;
        }
        if (down) InjectTouchForDisplay(displaySlot, kMouseFingerId, 1, x, y);
    } else if (action == 2 || action == 4) {
        bool down = false;
        {
            std::lock_guard<std::mutex> lk(*stateMu);
            down = *leftDown;
            *leftDown = false;
        }
        if (down) InjectTouchForDisplay(displaySlot, kMouseFingerId, 2, x, y);
    }
}

void InputBridge::InjectScroll(float x, float y, float deltaY) {
    InjectScrollForDisplay(0, x, y, deltaY);
}

void InputBridge::InjectScrollForDisplay(int displaySlot, float x, float y, float deltaY) {
    if (!running_.load() || std::fabs(deltaY) < 0.01f) return;
    if (displaySlot < 0 || displaySlot > kMaxSecondaryDisplays) return;
    constexpr int32_t kScrollFinger = 9001;
    float step = std::clamp(deltaY * 18.0f, -180.0f, 180.0f);
    InjectTouchForDisplay(displaySlot, kScrollFinger, 0, x, y);
    for (int i = 1; i <= 4; ++i) {
        InjectTouchForDisplay(displaySlot, kScrollFinger, 1, x, y - step * (static_cast<float>(i) / 4.0f));
    }
    InjectTouchForDisplay(displaySlot, kScrollFinger, 2, x, y - step);
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

void InputBridge::InjectGamepadButton(uint16_t linuxBtn, bool down) {
    if (!running_.load() || linuxBtn == 0) return;
    struct timeval tv = NowMonotonicTv();
    std::vector<struct input_event> evs;
    evs.reserve(2);
    PushEv(evs, tv, EV_KEY, linuxBtn, down ? 1 : 0);
    PushEv(evs, tv, EV_SYN, SYN_REPORT, 0);
    BroadcastEvents(&gamepadSrv_, evs.data(), evs.size() * sizeof(struct input_event));
}

void InputBridge::InjectGamepadAxis(uint16_t linuxAbs, int32_t value) {
    if (!running_.load()) return;
    if (linuxAbs < 64) {
        std::lock_guard<std::mutex> lk(gamepadStateMu_);
        gamepadAbs_[linuxAbs] = value;
    }
    struct timeval tv = NowMonotonicTv();
    std::vector<struct input_event> evs;
    evs.reserve(2);
    PushEv(evs, tv, EV_ABS, linuxAbs, value);
    PushEv(evs, tv, EV_SYN, SYN_REPORT, 0);
    BroadcastEvents(&gamepadSrv_, evs.data(), evs.size() * sizeof(struct input_event));
}

void InputBridge::InjectGamepadFrame(const std::vector<std::pair<uint16_t, int32_t>>& absEvents,
                                     const std::vector<std::pair<uint16_t, bool>>& btnEvents) {
    if (!running_.load() || (absEvents.empty() && btnEvents.empty())) return;
    struct timeval tv = NowMonotonicTv();
    std::vector<struct input_event> evs;
    evs.reserve(absEvents.size() + btnEvents.size() + 1);
    {
        std::lock_guard<std::mutex> lk(gamepadStateMu_);
        for (const auto& kv : absEvents) {
            if (kv.first < 64) gamepadAbs_[kv.first] = kv.second;
            PushEv(evs, tv, EV_ABS, kv.first, kv.second);
        }
    }
    for (const auto& kv : btnEvents) {
        PushEv(evs, tv, EV_KEY, kv.first, kv.second ? 1 : 0);
    }
    PushEv(evs, tv, EV_SYN, SYN_REPORT, 0);
    BroadcastEvents(&gamepadSrv_, evs.data(), evs.size() * sizeof(struct input_event));
}

uint16_t InputBridge::MapOhosKeyToLinux(int32_t k) const {
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
        case 2070: return mapEscToBack_.load() ? KEY_BACK : KEY_ESC;
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

void InputBridge::SetHostImeActive(int displaySlot, bool active) {
    if (displaySlot < 0 || displaySlot > kMaxSecondaryDisplays) return;
    hostImeActive_[displaySlot].store(active);
    if (!active) {
        hostCtrlHeld_.store(false);
        hostAltHeld_.store(false);
    }
}

bool InputBridge::IsHostImeActive(int displaySlot) const {
    if (displaySlot < 0 || displaySlot > kMaxSecondaryDisplays) return false;
    return hostImeActive_[displaySlot].load();
}

bool InputBridge::ShouldSuppressKeyForHostIme(int displaySlot, int32_t k, bool down) {
    if (k == 2072 || k == 2073) {
        hostCtrlHeld_.store(down);
        return false;
    }
    if (k == 2045 || k == 2046) {
        hostAltHeld_.store(down);
        return false;
    }
    if (displaySlot < 0 || displaySlot > kMaxSecondaryDisplays) return false;
    if (!hostImeActive_[displaySlot].load()) return false;
    // Allow Ctrl/Alt shortcuts (Ctrl+C/V/A/X/Z, etc.) to pass through cleanly
    if (hostCtrlHeld_.load() || hostAltHeld_.load()) return false;
    // Letters A-Z, digits 0-9
    if ((k >= 2017 && k <= 2042) || (k >= 2000 && k <= 2009)) return true;
    // Punctuation, Space, Backspace, Delete, Enter, Shift, CapsLock, Arrows
    switch (k) {
        case 2012: case 2013: case 2014: case 2015: // Arrow keys (candidate navigation / moveCursor)
        case 2016: case 2054: case 2119:            // Enter / Numpad Enter
        case 2043: case 2044:                       // Comma, Period
        case 2047: case 2048: case 2074:            // Shift, CapsLock (CN/EN IME toggle)
        case 2050: case 2055: case 2071:            // Space, Backspace, Delete
        case 2056: case 2057: case 2058: case 2059:
        case 2060: case 2061: case 2062: case 2063:
        case 2064:
            return true;
        default:
            return false;
    }
}

void InputBridge::SetDisplay0Rotation(int rotation) {
    display0Rotation_.store(rotation & 3);
}

void InputBridge::GetLastPointerPos(int displaySlot, float& outX, float& outY) const {
    if (displaySlot < 0 || displaySlot > kMaxSecondaryDisplays) {
        outX = -1.0f;
        outY = -1.0f;
        return;
    }
    outX = lastPtrX_[displaySlot].load();
    outY = lastPtrY_[displaySlot].load();
}

void InputBridge::InjectNormalizedTouch(int displaySlot, int32_t fingerId, int action, float nx, float ny) {
    if (!running_.load()) return;
    if (displaySlot < 0 || displaySlot > kMaxSecondaryDisplays) return;
    float cx = std::clamp(nx, 0.0f, 1.0f);
    float cy = std::clamp(ny, 0.0f, 1.0f);
    if (displaySlot == 0) {
        int rot = display0Rotation_.load() & 3;
        if (rot == 1) {
            float px = 1.0f - cy;
            float py = cx;
            cx = px;
            cy = py;
        } else if (rot == 2) {
            cx = 1.0f - cx;
            cy = 1.0f - cy;
        } else if (rot == 3) {
            float px = cy;
            float py = 1.0f - cx;
            cx = px;
            cy = py;
        }
    }
    float spanW = 0.0f;
    float spanH = 0.0f;
    if (displaySlot == 0) {
        int32_t sw = surfaceW_.load();
        int32_t sh = surfaceH_.load();
        spanW = static_cast<float>(sw > 0 ? sw : displayW_.load());
        spanH = static_cast<float>(sh > 0 ? sh : displayH_.load());
    } else {
        auto& st = secondaryTouchState_[displaySlot - 1];
        int32_t sw = st.surfaceW.load();
        int32_t sh = st.surfaceH.load();
        spanW = static_cast<float>(sw > 0 ? sw : 1000);
        spanH = static_cast<float>(sh > 0 ? sh : 1000);
    }
    InjectTouchForDisplay(displaySlot, fingerId, action, cx * spanW, cy * spanH);
}

void InputBridge::SetKeymapConfig(int displaySlot, bool enabled, const std::string& rulesSpec) {
    if (displaySlot < 0 || displaySlot > kMaxSecondaryDisplays) return;
    std::vector<int32_t> releaseFingers;
    {
        std::lock_guard<std::mutex> lk(keymapMu_);
        auto& km = keymaps_[displaySlot];
        if (km.dpad.fingerActive) releaseFingers.push_back(500);
        for (const auto& t : km.taps) {
            if (t.held) releaseFingers.push_back(t.fingerId);
        }
        if (km.rmb.held) releaseFingers.push_back(519);

        km = KeymapSlotState{};
        km.enabled = enabled;
        int32_t nextFinger = 501;

        size_t pos = 0;
        while (pos < rulesSpec.size()) {
            size_t nl = rulesSpec.find('\n', pos);
            std::string line = (nl == std::string::npos)
                ? rulesSpec.substr(pos)
                : rulesSpec.substr(pos, nl - pos);
            pos = (nl == std::string::npos) ? rulesSpec.size() : (nl + 1);
            if (line.empty()) continue;

            std::vector<std::string> tok;
            size_t p = 0;
            while (p <= line.size()) {
                size_t bar = line.find('|', p);
                if (bar == std::string::npos) {
                    tok.push_back(line.substr(p));
                    break;
                }
                tok.push_back(line.substr(p, bar - p));
                p = bar + 1;
            }
            if (tok.empty()) continue;
            if (tok[0] == "dpad" && tok.size() >= 8) {
                km.dpad.present = true;
                km.dpad.upKey = std::atoi(tok[1].c_str());
                km.dpad.downKey = std::atoi(tok[2].c_str());
                km.dpad.leftKey = std::atoi(tok[3].c_str());
                km.dpad.rightKey = std::atoi(tok[4].c_str());
                km.dpad.cx = std::clamp(std::strtof(tok[5].c_str(), nullptr), 0.02f, 0.98f);
                km.dpad.cy = std::clamp(std::strtof(tok[6].c_str(), nullptr), 0.02f, 0.98f);
                km.dpad.radius = std::clamp(std::strtof(tok[7].c_str(), nullptr), 0.04f, 0.35f);
            } else if (tok[0] == "tap" && tok.size() >= 4 && nextFinger <= 518) {
                KeymapTap t;
                t.keyCode = std::atoi(tok[1].c_str());
                t.nx = std::clamp(std::strtof(tok[2].c_str(), nullptr), 0.01f, 0.99f);
                t.ny = std::clamp(std::strtof(tok[3].c_str(), nullptr), 0.01f, 0.99f);
                t.fingerId = nextFinger++;
                if (t.keyCode > 0) km.taps.push_back(t);
            } else if (tok[0] == "rmb" && tok.size() >= 5) {
                km.rmb.present = true;
                km.rmb.cx = std::clamp(std::strtof(tok[1].c_str(), nullptr), 0.02f, 0.98f);
                km.rmb.cy = std::clamp(std::strtof(tok[2].c_str(), nullptr), 0.02f, 0.98f);
                km.rmb.radius = std::clamp(std::strtof(tok[3].c_str(), nullptr), 0.04f, 0.35f);
                km.rmb.direct = (std::atoi(tok[4].c_str()) != 0);
            }
        }
    }
    for (int32_t fid : releaseFingers) {
        InjectNormalizedTouch(displaySlot, fid, 2, 0.5f, 0.5f);
    }
}

bool InputBridge::IsKeymapEnabled(int displaySlot) const {
    if (displaySlot < 0 || displaySlot > kMaxSecondaryDisplays) return false;
    std::lock_guard<std::mutex> lk(keymapMu_);
    return keymaps_[displaySlot].enabled;
}

bool InputBridge::HandleKeymapKey(int displaySlot, int32_t ohosKeyCode, bool down) {
    if (displaySlot < 0 || displaySlot > kMaxSecondaryDisplays) return false;
    struct PendingTouch {
        int32_t fid;
        int action;
        float nx;
        float ny;
    };
    std::vector<PendingTouch> actions;
    bool consumed = false;

    {
        std::lock_guard<std::mutex> lk(keymapMu_);
        auto& km = keymaps_[displaySlot];
        if (!km.enabled) return false;

        if (km.dpad.present) {
            bool matched = false;
            if (ohosKeyCode == km.dpad.upKey) { km.dpad.heldUp = down; matched = true; }
            else if (ohosKeyCode == km.dpad.downKey) { km.dpad.heldDown = down; matched = true; }
            else if (ohosKeyCode == km.dpad.leftKey) { km.dpad.heldLeft = down; matched = true; }
            else if (ohosKeyCode == km.dpad.rightKey) { km.dpad.heldRight = down; matched = true; }

            if (matched) {
                consumed = true;
                float dx = (km.dpad.heldRight ? 1.0f : 0.0f) - (km.dpad.heldLeft ? 1.0f : 0.0f);
                float dy = (km.dpad.heldDown ? 1.0f : 0.0f) - (km.dpad.heldUp ? 1.0f : 0.0f);
                float mag = std::sqrt(dx * dx + dy * dy);
                if (mag > 0.01f) {
                    float tx = std::clamp(km.dpad.cx + (dx / mag) * km.dpad.radius, 0.01f, 0.99f);
                    float ty = std::clamp(km.dpad.cy + (dy / mag) * km.dpad.radius, 0.01f, 0.99f);
                    if (!km.dpad.fingerActive) {
                        km.dpad.fingerActive = true;
                        actions.push_back({ 500, 0, km.dpad.cx, km.dpad.cy });
                        actions.push_back({ 500, 1, tx, ty });
                    } else {
                        actions.push_back({ 500, 1, tx, ty });
                    }
                } else if (km.dpad.fingerActive) {
                    km.dpad.fingerActive = false;
                    actions.push_back({ 500, 2, km.dpad.cx, km.dpad.cy });
                }
            }
        }

        if (!consumed) {
            for (auto& t : km.taps) {
                if (t.keyCode == ohosKeyCode) {
                    consumed = true;
                    if (down && !t.held) {
                        t.held = true;
                        actions.push_back({ t.fingerId, 0, t.nx, t.ny });
                    } else if (!down && t.held) {
                        t.held = false;
                        actions.push_back({ t.fingerId, 2, t.nx, t.ny });
                    }
                    break;
                }
            }
        }
    }

    for (const auto& a : actions) {
        InjectNormalizedTouch(displaySlot, a.fid, a.action, a.nx, a.ny);
    }
    return consumed;
}

bool InputBridge::HandleKeymapMouse(int displaySlot, int action, int button, float x, float y) {
    if (displaySlot < 0 || displaySlot > kMaxSecondaryDisplays) return false;
    float spanW = 0.0f;
    float spanH = 0.0f;
    if (displaySlot == 0) {
        int32_t sw = surfaceW_.load();
        int32_t sh = surfaceH_.load();
        spanW = static_cast<float>(sw > 0 ? sw : displayW_.load());
        spanH = static_cast<float>(sh > 0 ? sh : displayH_.load());
    } else {
        auto& st = secondaryTouchState_[displaySlot - 1];
        int32_t sw = st.surfaceW.load();
        int32_t sh = st.surfaceH.load();
        spanW = static_cast<float>(sw > 0 ? sw : 1000);
        spanH = static_cast<float>(sh > 0 ? sh : 1000);
    }
    float nx = std::clamp(x / std::max(1.0f, spanW), 0.0f, 1.0f);
    float ny = std::clamp(y / std::max(1.0f, spanH), 0.0f, 1.0f);

    int emitAction = -1;
    float ex = 0.5f, ey = 0.5f;
    float startX = 0.5f, startY = 0.5f;
    bool emitDownFirst = false;

    {
        std::lock_guard<std::mutex> lk(keymapMu_);
        auto& km = keymaps_[displaySlot];
        if (!km.enabled || !km.rmb.present) return false;

        auto computeTarget = [&]() {
            if (km.rmb.direct) {
                ex = nx;
                ey = ny;
                startX = nx;
                startY = ny;
            } else {
                // MOBA right-click move: project vector from screen center (0.5, 0.5) onto joystick ring
                float dx = nx - 0.5f;
                float dy = ny - 0.5f;
                float mag = std::sqrt(dx * dx + dy * dy);
                startX = km.rmb.cx;
                startY = km.rmb.cy;
                if (mag > 0.01f) {
                    ex = std::clamp(km.rmb.cx + (dx / mag) * km.rmb.radius, 0.01f, 0.99f);
                    ey = std::clamp(km.rmb.cy + (dy / mag) * km.rmb.radius, 0.01f, 0.99f);
                } else {
                    ex = km.rmb.cx;
                    ey = km.rmb.cy;
                }
            }
        };

        if (action == 1 && button == 2) {
            km.rmb.held = true;
            computeTarget();
            emitDownFirst = true;
            emitAction = 1;
        } else if (action == 3 && km.rmb.held) {
            computeTarget();
            emitAction = 1;
        } else if ((action == 2 || action == 4) && (button == 2 || km.rmb.held)) {
            if (!km.rmb.held) return false;
            km.rmb.held = false;
            computeTarget();
            emitAction = 2;
        } else {
            return false;
        }
    }

    if (emitDownFirst) {
        InjectNormalizedTouch(displaySlot, 519, 0, startX, startY);
    }
    if (emitAction >= 0) {
        InjectNormalizedTouch(displaySlot, 519, emitAction, ex, ey);
    }
    return true;
}
