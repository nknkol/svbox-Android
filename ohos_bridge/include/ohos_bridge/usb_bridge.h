/* SPDX-License-Identifier: MIT */
/* HarmonyOS USB & USB-Serial bridge for svbox (--vdev ...:chr):
 *   /dev/bus/usb/001/002 : standard Linux usbfs device (USBDEVFS_CONTROL/BULK/
 *                          CLAIMINTERFACE/GETDRIVER/CONNECTINFO/CAPABILITIES +
 *                          raw USB descriptor stream for libusb/lsusb)
 *   /dev/ttyUSB0         : standard Linux USB-serial character device (termios,
 *                          winsize, modem lines, bidirectional stream I/O)
 * Supports dynamic HarmonyOS USB DDK probing (libusb_ndk.z.so), host callback
 * delegates (for ArkTS @ohos.usbManager), and built-in virtual loopback gadget
 * mode for deterministic guest testing without physical USB hardware. */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class UsbBridge {
public:
    static UsbBridge& Instance();

    // Optional host USB transfer delegate (e.g., backed by @ohos.usbManager or USB DDK).
    // Returns >=0 bytes transferred, or -errno.
    using ControlFn = std::function<int32_t(uint8_t reqType, uint8_t req,
                                            uint16_t value, uint16_t index,
                                            std::vector<uint8_t>& data, uint32_t timeoutMs)>;
    using BulkFn = std::function<int32_t(uint8_t ep, std::vector<uint8_t>& data, uint32_t timeoutMs)>;

    void Start(const std::string& usbfsSockPath, const std::string& serialSockPath = "");
    void Stop();

    void SetHostDelegates(ControlFn ctrlFn, BulkFn bulkFn);
    void ClearHostDelegates();

    bool IsRunning() const { return running_.load(); }
    bool UsbDdkAvailable() const { return usbDdkAvailable_.load(); }
    uint64_t UsbTransfers() const { return usbTransfers_.load(); }
    uint64_t SerialBytes() const { return serialBytes_.load(); }

private:
    UsbBridge() = default;

    enum class Kind { Usbfs = 0, Serial = 1 };

    struct VdevServer {
        Kind kind = Kind::Usbfs;
        std::string sockPath;
        int listenFd = -1;
        std::thread thread;
    };

    void RunServer(VdevServer* srv);
    void HandleUsbfsConn(int fd);
    void HandleSerialConn(int fd);
    void ProbeOhosUsbDdk();
    std::vector<uint8_t> BuildDescriptors() const;

    std::atomic<bool> running_{false};
    std::atomic<bool> usbDdkAvailable_{false};
    std::atomic<uint64_t> usbTransfers_{0};
    std::atomic<uint64_t> serialBytes_{0};

    VdevServer usbfsSrv_;
    VdevServer serialSrv_;

    std::mutex usbMu_;
    ControlFn hostCtrlFn_;
    BulkFn hostBulkFn_;
    int32_t claimedIface_ = -1;
    uint32_t activeConfig_ = 1;
    std::vector<uint8_t> bulkLoopbackBuf_;

    std::mutex serialMu_;
    uint8_t termios_[36] = {};
    uint16_t winsize_[4] = { 24, 80, 0, 0 };
    uint32_t modemBits_ = 0x06; /* TIOCM_DTR | TIOCM_RTS */
};
