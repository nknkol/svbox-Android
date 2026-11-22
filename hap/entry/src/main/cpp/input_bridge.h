/* SPDX-License-Identifier: MIT */
/* Virtual Linux evdev input bridge (/dev/input/event0 touchscreen +
 * /dev/input/event1 keyboard) using svbox's generic --vdev chr wire protocol. */
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class InputBridge {
public:
    static InputBridge& Instance();

    void Start(const std::string& touchSockPath,
               const std::string& kbdSockPath,
               int32_t displayW,
               int32_t displayH);
    void Stop();

    void SetSurfaceSize(int32_t surfaceW, int32_t surfaceH);

    // Touch: action 0=down, 1=move, 2=up, 3=cancel
    void InjectTouch(int32_t fingerId, int action, float x, float y);
    // Mouse: action 1=press, 2=release, 3=move, 4=cancel; button 1=left, 2=right, 4=middle
    void InjectMouse(int action, int button, float x, float y);
    // Scroll wheel: deltaY > 0 scrolls down (swipe up), < 0 scrolls up
    void InjectScroll(float x, float y, float deltaY);
    // HarmonyOS OH_NativeXComponent_KeyCode -> Linux KEY_*
    void InjectOhosKey(int32_t ohosKeyCode, bool down);
    // Direct Linux evdev KEY_* injection
    void InjectLinuxKey(uint16_t linuxKey, bool down);
    void TapLinuxKey(uint16_t linuxKey);

    bool HasTouchClient() const { return touchClients_.load() > 0; }
    bool HasKbdClient() const { return kbdClients_.load() > 0; }
    uint64_t EventCount() const { return eventsSent_.load(); }

private:
    InputBridge() = default;

    struct EvdevServer {
        bool isTouch = true;
        std::string sockPath;
        int listenFd = -1;
        std::thread thread;
        std::mutex clientsMu;
        std::vector<int> streamFds; // connected VDEV_OP_OPEN stream FDs
    };

    void RunServer(EvdevServer* srv);
    void HandleVdevConn(EvdevServer* srv, int fd);
    void BroadcastEvents(EvdevServer* srv, const void* data, size_t bytes);
    int MapCoordX(float x) const;
    int MapCoordY(float y) const;
    static uint16_t MapOhosKeyToLinux(int32_t ohosKeyCode);

    std::atomic<bool> running_{false};
    std::atomic<int32_t> displayW_{720};
    std::atomic<int32_t> displayH_{1280};
    std::atomic<int32_t> surfaceW_{720};
    std::atomic<int32_t> surfaceH_{1280};
    std::atomic<int32_t> touchClients_{0};
    std::atomic<int32_t> kbdClients_{0};
    std::atomic<uint64_t> eventsSent_{0};

    EvdevServer touchSrv_;
    EvdevServer kbdSrv_;

    std::mutex touchStateMu_;
    bool mouseLeftDown_ = false;
    int32_t nextTrackingId_ = 1;
    int32_t slotFinger_[10] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
    int32_t slotTrack_[10] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
};
