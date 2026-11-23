/* SPDX-License-Identifier: MIT */
/* Virtual Linux evdev input bridge for svbox (--vdev ...:chr):
 *   /dev/input/event0 : multi-touch touchscreen
 *   /dev/input/event1 : keyboard
 *   /dev/input/event2 : standard Linux gamepad (Xbox 360 layout + OHOS GameControllerKit hook) */
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
               int32_t displayH,
               const std::string& gamepadSockPath = "",
               bool mapEscToBack = true);
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

    // Standard Linux Gamepad (/dev/input/event2) injection
    // btn: Linux BTN_SOUTH..BTN_THUMBR / BTN_DPAD_*
    void InjectGamepadButton(uint16_t linuxBtn, bool down);
    // axis: ABS_X/ABS_Y/ABS_RX/ABS_RY [-32768..32767], ABS_Z/ABS_RZ [0..255], ABS_HAT0X/Y [-1..1]
    void InjectGamepadAxis(uint16_t linuxAbs, int32_t value);
    // Convenience batch update for sticks/triggers/dpad
    void InjectGamepadFrame(const std::vector<std::pair<uint16_t, int32_t>>& absEvents,
                            const std::vector<std::pair<uint16_t, bool>>& btnEvents);

    bool HasTouchClient() const { return touchClients_.load() > 0; }
    bool HasKbdClient() const { return kbdClients_.load() > 0; }
    bool HasGamepadClient() const { return gamepadClients_.load() > 0; }
    bool GameControllerKitActive() const { return gckActive_.load(); }
    uint64_t EventCount() const { return eventsSent_.load(); }

private:
    InputBridge() = default;

    enum class DevKind { Touch = 0, Keyboard = 1, Gamepad = 2 };

    struct EvdevServer {
        DevKind kind = DevKind::Touch;
        std::string sockPath;
        int listenFd = -1;
        std::thread thread;
        std::mutex clientsMu;
        std::vector<int> streamFds;
    };

    void RunServer(EvdevServer* srv);
    void HandleVdevConn(EvdevServer* srv, int fd);
    void BroadcastEvents(EvdevServer* srv, const void* data, size_t bytes);
    void TryAttachGameControllerKit();
    void DetachGameControllerKit();
    int MapCoordX(float x) const;
    int MapCoordY(float y) const;
    uint16_t MapOhosKeyToLinux(int32_t ohosKeyCode) const;

    std::atomic<bool> running_{false};
    std::atomic<bool> mapEscToBack_{true};
    std::atomic<int32_t> displayW_{720};
    std::atomic<int32_t> displayH_{1280};
    std::atomic<int32_t> surfaceW_{720};
    std::atomic<int32_t> surfaceH_{1280};
    std::atomic<int32_t> touchClients_{0};
    std::atomic<int32_t> kbdClients_{0};
    std::atomic<int32_t> gamepadClients_{0};
    std::atomic<bool> gckActive_{false};
    std::atomic<uint64_t> eventsSent_{0};

    EvdevServer touchSrv_;
    EvdevServer kbdSrv_;
    EvdevServer gamepadSrv_;

    void* gckHandle_ = nullptr;

    std::mutex touchStateMu_;
    bool mouseLeftDown_ = false;
    int32_t nextTrackingId_ = 1;
    int32_t slotFinger_[10] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
    int32_t slotTrack_[10] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };

    std::mutex gamepadStateMu_;
    int32_t gamepadAbs_[64] = {};
};
