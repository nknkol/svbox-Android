/* SPDX-License-Identifier: MIT */
#include "ohos_bridge/usb_bridge.h"
#include "ohos_bridge/log.h"
#include "ohos_bridge/sched_qos.h"

#include <algorithm>
#include <cstring>
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

constexpr uint32_t kVdevMagic = 0x56444556u;
constexpr uint32_t kVdevOpOpen = 1;
constexpr uint32_t kVdevOpAttachThread = 2;
constexpr uint32_t kVdevOpIoctl = 4;

constexpr uint32_t kTcGets = 0x5401u;
constexpr uint32_t kTcSets = 0x5402u;
constexpr uint32_t kTcSetsW = 0x5403u;
constexpr uint32_t kTcSetsF = 0x5404u;
constexpr uint32_t kTiocGWinsz = 0x5413u;
constexpr uint32_t kTiocSWinsz = 0x5414u;
constexpr uint32_t kTiocMGet = 0x5415u;
constexpr uint32_t kTiocMBis = 0x5416u;
constexpr uint32_t kTiocMBic = 0x5417u;
constexpr uint32_t kTiocMSet = 0x5418u;

constexpr uint32_t kUsbDevfsControl = 0xc0185500u;
constexpr uint32_t kUsbDevfsBulk = 0xc0185502u;
constexpr uint32_t kUsbDevfsSetInterface = 0x80085504u;
constexpr uint32_t kUsbDevfsSetConfig = 0x80045505u;
constexpr uint32_t kUsbDevfsGetDriver = 0x41045508u;
constexpr uint32_t kUsbDevfsClaimIface = 0x8004550fu;
constexpr uint32_t kUsbDevfsReleaseIface = 0x80045510u;
constexpr uint32_t kUsbDevfsConnectInfo = 0x40085511u;
constexpr uint32_t kUsbDevfsReset = 0x00005514u;
constexpr uint32_t kUsbDevfsClearHalt = 0x80045515u;
constexpr uint32_t kUsbDevfsGetCaps = 0x8004551au;

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

struct UsbCtrlHdr {
    uint8_t bRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
    uint32_t timeout;
    uint32_t pad;
    uint64_t data;
};

struct UsbBulkHdr {
    uint32_t ep;
    uint32_t len;
    uint32_t timeout;
    uint32_t pad;
    uint64_t data;
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

std::vector<uint8_t> MakeUsbStringDesc(const char* ascii) {
    size_t n = std::strlen(ascii);
    std::vector<uint8_t> out(2 + n * 2, 0);
    out[0] = static_cast<uint8_t>(out.size());
    out[1] = 0x03; /* USB_DT_STRING */
    for (size_t i = 0; i < n; ++i) {
        out[2 + i * 2] = static_cast<uint8_t>(ascii[i]);
        out[3 + i * 2] = 0;
    }
    return out;
}

} // namespace

UsbBridge& UsbBridge::Instance() {
    static UsbBridge inst;
    return inst;
}

void UsbBridge::ProbeOhosUsbDdk() {
    void* h = dlopen("libusb_ndk.z.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        usbDdkAvailable_.store(false);
        return;
    }
    auto initFn = reinterpret_cast<int32_t (*)(void)>(dlsym(h, "OH_Usb_Init"));
    auto relFn = reinterpret_cast<void (*)(void)>(dlsym(h, "OH_Usb_Release"));
    bool ok = false;
    if (initFn && initFn() == 0) {
        ok = true;
        if (relFn) relFn();
    }
    dlclose(h);
    usbDdkAvailable_.store(ok);
    OB_LOG_INFO("SvboxUsb", "HarmonyOS USB DDK probe: %s", ok ? "available" : "unavailable/no-ddk-perm (using delegate/loopback)");
}

std::vector<uint8_t> UsbBridge::BuildDescriptors() const {
    // Standard 18-byte USB 2.0 Device Descriptor + 9-byte Config + 9-byte Interface + 2x 7-byte Bulk EPs = 50 bytes.
    return {
        // Device descriptor (18B): VID=0x1d6b (Linux Foundation), PID=0x0104 (svbox virtual USB bridge)
        18, 0x01, 0x00, 0x02, 0xff, 0x00, 0x00, 64,
        0x6b, 0x1d, 0x04, 0x01, 0x00, 0x01, 1, 2, 3, 1,
        // Configuration descriptor (9B): wTotalLength = 32 (9+9+7+7)
        9, 0x02, 32, 0, 1, 1, 0, 0x80, 50,
        // Interface descriptor (9B): bInterfaceNumber=0, 2 endpoints, vendor class
        9, 0x04, 0, 0, 2, 0xff, 0x00, 0x00, 0,
        // Endpoint OUT (7B): EP 0x02, Bulk, wMaxPacketSize=512
        7, 0x05, 0x02, 0x02, 0x00, 0x02, 0,
        // Endpoint IN (7B): EP 0x82, Bulk, wMaxPacketSize=512
        7, 0x05, 0x82, 0x02, 0x00, 0x02, 0,
    };
}

void UsbBridge::Start(const std::string& usbfsSockPath, const std::string& serialSockPath) {
    Stop();
    usbTransfers_.store(0);
    serialBytes_.store(0);
    {
        std::lock_guard<std::mutex> lk(usbMu_);
        claimedIface_ = -1;
        activeConfig_ = 1;
        bulkLoopbackBuf_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(serialMu_);
        std::memset(termios_, 0, sizeof(termios_));
        uint32_t cflag = 0x000010b2u; /* CS8 | CREAD | CLOCAL | B115200 */
        std::memcpy(termios_ + 8, &cflag, 4);
        winsize_[0] = 24; winsize_[1] = 80; winsize_[2] = 0; winsize_[3] = 0;
        modemBits_ = 0x06;
    }

    ProbeOhosUsbDdk();
    running_.store(true);

    if (!usbfsSockPath.empty()) {
        usbfsSrv_.kind = Kind::Usbfs;
        usbfsSrv_.sockPath = usbfsSockPath;
        usbfsSrv_.listenFd = -1;
        usbfsSrv_.thread = std::thread(&UsbBridge::RunServer, this, &usbfsSrv_);
    }
    if (!serialSockPath.empty()) {
        serialSrv_.kind = Kind::Serial;
        serialSrv_.sockPath = serialSockPath;
        serialSrv_.listenFd = -1;
        serialSrv_.thread = std::thread(&UsbBridge::RunServer, this, &serialSrv_);
    }
}

void UsbBridge::Stop() {
    running_.store(false);
    auto stopSrv = [](VdevServer& srv) {
        if (srv.listenFd >= 0) {
            shutdown(srv.listenFd, SHUT_RDWR);
            close(srv.listenFd);
            srv.listenFd = -1;
        }
        if (srv.thread.joinable()) {
            srv.thread.join();
        }
        if (!srv.sockPath.empty()) {
            unlink(srv.sockPath.c_str());
            srv.sockPath.clear();
        }
    };
    stopSrv(usbfsSrv_);
    stopSrv(serialSrv_);
}

void UsbBridge::SetHostDelegates(ControlFn ctrlFn, BulkFn bulkFn) {
    std::lock_guard<std::mutex> lk(usbMu_);
    hostCtrlFn_ = std::move(ctrlFn);
    hostBulkFn_ = std::move(bulkFn);
}

void UsbBridge::ClearHostDelegates() {
    std::lock_guard<std::mutex> lk(usbMu_);
    hostCtrlFn_ = nullptr;
    hostBulkFn_ = nullptr;
}

void UsbBridge::RunServer(VdevServer* srv) {
    SchedQos::SetCurrentThreadQoS(SchedQos::Level::UserInitiated,
                                  srv->kind == Kind::Usbfs ? "usb-bridge" : "usb-serial");
    unlink(srv->sockPath.c_str());
    int sfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sfd < 0) return;

    struct sockaddr_un sa = {};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, srv->sockPath.c_str(), sizeof(sa.sun_path) - 1);
    if (bind(sfd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) < 0 ||
        listen(sfd, 16) < 0) {
        OB_LOG_ERROR("SvboxUsb", "UsbBridge bind/listen %s failed: %d", srv->sockPath.c_str(), errno);
        close(sfd);
        return;
    }
    chmod(srv->sockPath.c_str(), 0666);
    srv->listenFd = sfd;
    OB_LOG_INFO("SvboxUsb", "UsbBridge listening on %s", srv->sockPath.c_str());

    while (running_.load()) {
        struct pollfd pfd = { sfd, POLLIN, 0 };
        int r = poll(&pfd, 1, 150);
        if (r <= 0 || !(pfd.revents & POLLIN)) continue;
        int cfd = accept4(sfd, nullptr, nullptr, SOCK_CLOEXEC);
        if (cfd < 0) continue;
        if (srv->kind == Kind::Usbfs) {
            std::thread(&UsbBridge::HandleUsbfsConn, this, cfd).detach();
        } else {
            std::thread(&UsbBridge::HandleSerialConn, this, cfd).detach();
        }
    }
}

void UsbBridge::HandleUsbfsConn(int fd) {
    VdevHdr hdr = {};
    if (!RecvExact(fd, &hdr, sizeof(hdr)) || hdr.magic != kVdevMagic) {
        close(fd);
        return;
    }

    if (hdr.op == kVdevOpOpen) {
        VdevOpenReq req = {};
        if (hdr.payload_len > 0) {
            std::vector<uint8_t> tmp(hdr.payload_len);
            if (!RecvExact(fd, tmp.data(), tmp.size())) { close(fd); return; }
            std::memcpy(&req, tmp.data(), std::min(sizeof(req), tmp.size()));
        }
        VdevOpenRep rep = { req.pid ? req.pid : 1u, 189u, 1u, 0u };
        if (!SendVdevReply(fd, kVdevOpOpen, rep.proc_id, 0, &rep, sizeof(rep))) {
            close(fd);
            return;
        }
        // Linux usbfs returns raw device + configuration descriptors on read() of the node.
        auto desc = BuildDescriptors();
        SendAll(fd, desc.data(), desc.size());
        while (running_.load()) {
            struct pollfd pfd = { fd, POLLRDHUP | POLLERR | POLLHUP, 0 };
            int pr = poll(&pfd, 1, 250);
            if (pr > 0 && (pfd.revents & (POLLRDHUP | POLLERR | POLLHUP))) break;
        }
        close(fd);
        return;
    }

    if (hdr.op == kVdevOpAttachThread) {
        if (hdr.payload_len > 0) {
            std::vector<uint8_t> tmp(hdr.payload_len);
            if (!RecvExact(fd, tmp.data(), tmp.size())) { close(fd); return; }
        }
        if (!SendVdevReply(fd, kVdevOpAttachThread, hdr.proc_id, 0, nullptr, 0)) {
            close(fd);
            return;
        }
        while (running_.load()) {
            VdevHdr rhdr = {};
            if (!RecvExact(fd, &rhdr, sizeof(rhdr)) || rhdr.magic != kVdevMagic) break;
            std::vector<uint8_t> payload(rhdr.payload_len);
            if (rhdr.payload_len > 0 && !RecvExact(fd, payload.data(), payload.size())) break;
            if (rhdr.op != kVdevOpIoctl || payload.size() < sizeof(VdevIoctlReq)) {
                SendVdevReply(fd, rhdr.op, rhdr.proc_id, -ENOTTY, nullptr, 0);
                continue;
            }
            VdevIoctlReq ireq = {};
            std::memcpy(&ireq, payload.data(), sizeof(ireq));
            const uint8_t* inData = payload.data() + sizeof(VdevIoctlReq);
            size_t inAvail = payload.size() - sizeof(VdevIoctlReq);
            uint32_t cmd = static_cast<uint32_t>(ireq.cmd);

            std::vector<uint8_t> out;
            int32_t rc = 0;

            if (cmd == kUsbDevfsGetCaps) {
                uint32_t caps = 0x01u | 0x02u; /* ZERO_PACKET | BULK_CONTINUATION */
                out.resize(4);
                std::memcpy(out.data(), &caps, 4);
            } else if (cmd == kUsbDevfsConnectInfo) {
                uint32_t devnum = 2;
                uint8_t slow = 0;
                out.assign(8, 0);
                std::memcpy(out.data(), &devnum, 4);
                out[4] = slow;
            } else if (cmd == kUsbDevfsGetDriver) {
                // Return -ENODATA when unclaimed (libusb convention), or driver name when claimed.
                uint32_t iface = 0;
                if (inAvail >= 4) std::memcpy(&iface, inData, 4);
                std::lock_guard<std::mutex> lk(usbMu_);
                if (claimedIface_ == static_cast<int32_t>(iface)) {
                    out.assign(260, 0);
                    std::memcpy(out.data(), &iface, 4);
                    std::strncpy(reinterpret_cast<char*>(out.data() + 4), "usbfs", 255);
                } else {
                    rc = -ENODATA;
                }
            } else if (cmd == kUsbDevfsClaimIface) {
                uint32_t iface = 0;
                if (inAvail >= 4) std::memcpy(&iface, inData, 4);
                std::lock_guard<std::mutex> lk(usbMu_);
                claimedIface_ = static_cast<int32_t>(iface);
            } else if (cmd == kUsbDevfsReleaseIface) {
                std::lock_guard<std::mutex> lk(usbMu_);
                claimedIface_ = -1;
            } else if (cmd == kUsbDevfsSetConfig) {
                uint32_t cfg = 1;
                if (inAvail >= 4) std::memcpy(&cfg, inData, 4);
                std::lock_guard<std::mutex> lk(usbMu_);
                activeConfig_ = cfg;
            } else if (cmd == kUsbDevfsSetInterface || cmd == kUsbDevfsClearHalt || cmd == kUsbDevfsReset) {
                rc = 0;
            } else if (cmd == kUsbDevfsControl && inAvail >= sizeof(UsbCtrlHdr)) {
                UsbCtrlHdr ct = {};
                std::memcpy(&ct, inData, sizeof(ct));
                bool isIn = (ct.bRequestType & 0x80u) != 0;
                std::vector<uint8_t> xbuf;
                if (!isIn && inAvail > sizeof(UsbCtrlHdr)) {
                    size_t n = std::min<size_t>(ct.wLength, inAvail - sizeof(UsbCtrlHdr));
                    xbuf.assign(inData + sizeof(UsbCtrlHdr), inData + sizeof(UsbCtrlHdr) + n);
                } else if (isIn) {
                    xbuf.resize(ct.wLength, 0);
                }
                ControlFn fn;
                {
                    std::lock_guard<std::mutex> lk(usbMu_);
                    fn = hostCtrlFn_;
                }
                if (fn) {
                    rc = fn(ct.bRequestType, ct.bRequest, ct.wValue, ct.wIndex, xbuf, ct.timeout);
                    if (rc >= 0 && isIn) {
                        xbuf.resize(static_cast<size_t>(rc));
                        out = std::move(xbuf);
                    }
                } else if (isIn && ct.bRequest == 0x06 /* GET_DESCRIPTOR */) {
                    uint8_t dtype = static_cast<uint8_t>(ct.wValue >> 8);
                    uint8_t didx = static_cast<uint8_t>(ct.wValue & 0xffu);
                    auto all = BuildDescriptors();
                    std::vector<uint8_t> rep;
                    if (dtype == 0x01) {
                        rep.assign(all.begin(), all.begin() + 18);
                    } else if (dtype == 0x02) {
                        rep.assign(all.begin() + 18, all.end());
                    } else if (dtype == 0x03) {
                        if (didx == 0) rep = { 4, 0x03, 0x09, 0x04 };
                        else if (didx == 1) rep = MakeUsbStringDesc("svbox-ohos");
                        else if (didx == 2) rep = MakeUsbStringDesc("HarmonyOS Virtual USB Bridge");
                        else rep = MakeUsbStringDesc("SVBOX-USB-0001");
                    }
                    if (rep.size() > ct.wLength) rep.resize(ct.wLength);
                    rc = static_cast<int32_t>(rep.size());
                    out = std::move(rep);
                } else if (!isIn) {
                    rc = static_cast<int32_t>(xbuf.size());
                } else {
                    rc = -EPIPE;
                }
                usbTransfers_.fetch_add(1);
            } else if (cmd == kUsbDevfsBulk && inAvail >= sizeof(UsbBulkHdr)) {
                UsbBulkHdr bt = {};
                std::memcpy(&bt, inData, sizeof(bt));
                bool isIn = (bt.ep & 0x80u) != 0;
                std::vector<uint8_t> xbuf;
                if (!isIn && inAvail > sizeof(UsbBulkHdr)) {
                    size_t n = std::min<size_t>(bt.len, inAvail - sizeof(UsbBulkHdr));
                    xbuf.assign(inData + sizeof(UsbBulkHdr), inData + sizeof(UsbBulkHdr) + n);
                } else if (isIn) {
                    xbuf.resize(bt.len, 0);
                }
                BulkFn fn;
                {
                    std::lock_guard<std::mutex> lk(usbMu_);
                    fn = hostBulkFn_;
                }
                if (fn) {
                    rc = fn(static_cast<uint8_t>(bt.ep), xbuf, bt.timeout);
                    if (rc >= 0 && isIn) {
                        xbuf.resize(static_cast<size_t>(rc));
                        out = std::move(xbuf);
                    }
                } else {
                    std::lock_guard<std::mutex> lk(usbMu_);
                    if (!isIn) {
                        bulkLoopbackBuf_ = xbuf;
                        rc = static_cast<int32_t>(xbuf.size());
                    } else {
                        size_t n = std::min<size_t>(bt.len, bulkLoopbackBuf_.size());
                        out.assign(bulkLoopbackBuf_.begin(), bulkLoopbackBuf_.begin() + n);
                        bulkLoopbackBuf_.erase(bulkLoopbackBuf_.begin(), bulkLoopbackBuf_.begin() + n);
                        rc = static_cast<int32_t>(n);
                    }
                }
                usbTransfers_.fetch_add(1);
            } else {
                rc = -ENOTTY;
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

void UsbBridge::HandleSerialConn(int fd) {
    VdevHdr hdr = {};
    if (!RecvExact(fd, &hdr, sizeof(hdr)) || hdr.magic != kVdevMagic) {
        close(fd);
        return;
    }

    if (hdr.op == kVdevOpOpen) {
        VdevOpenReq req = {};
        if (hdr.payload_len > 0) {
            std::vector<uint8_t> tmp(hdr.payload_len);
            if (!RecvExact(fd, tmp.data(), tmp.size())) { close(fd); return; }
            std::memcpy(&req, tmp.data(), std::min(sizeof(req), tmp.size()));
        }
        VdevOpenRep rep = { req.pid ? req.pid : 1u, 188u, 0u, 0u };
        if (!SendVdevReply(fd, kVdevOpOpen, rep.proc_id, 0, &rep, sizeof(rep))) {
            close(fd);
            return;
        }
        // Bidirectional virtual serial loopback/AT responder on stream FD.
        uint8_t buf[1024];
        while (running_.load()) {
            struct pollfd pfd = { fd, POLLIN | POLLRDHUP | POLLERR | POLLHUP, 0 };
            int pr = poll(&pfd, 1, 200);
            if (pr <= 0) continue;
            if (pfd.revents & POLLIN) {
                ssize_t n = recv(fd, buf, sizeof(buf), 0);
                if (n <= 0) break;
                serialBytes_.fetch_add(static_cast<uint64_t>(n));
                if (n >= 2 && buf[0] == 'A' && buf[1] == 'T') {
                    const char* ok = "OK\r\n";
                    SendAll(fd, ok, std::strlen(ok));
                } else {
                    SendAll(fd, buf, static_cast<size_t>(n));
                }
            }
            if (pfd.revents & (POLLRDHUP | POLLERR | POLLHUP)) break;
        }
        close(fd);
        return;
    }

    if (hdr.op == kVdevOpAttachThread) {
        if (hdr.payload_len > 0) {
            std::vector<uint8_t> tmp(hdr.payload_len);
            if (!RecvExact(fd, tmp.data(), tmp.size())) { close(fd); return; }
        }
        if (!SendVdevReply(fd, kVdevOpAttachThread, hdr.proc_id, 0, nullptr, 0)) {
            close(fd);
            return;
        }
        while (running_.load()) {
            VdevHdr rhdr = {};
            if (!RecvExact(fd, &rhdr, sizeof(rhdr)) || rhdr.magic != kVdevMagic) break;
            std::vector<uint8_t> payload(rhdr.payload_len);
            if (rhdr.payload_len > 0 && !RecvExact(fd, payload.data(), payload.size())) break;
            if (rhdr.op != kVdevOpIoctl || payload.size() < sizeof(VdevIoctlReq)) {
                SendVdevReply(fd, rhdr.op, rhdr.proc_id, -ENOTTY, nullptr, 0);
                continue;
            }
            VdevIoctlReq ireq = {};
            std::memcpy(&ireq, payload.data(), sizeof(ireq));
            const uint8_t* inData = payload.data() + sizeof(VdevIoctlReq);
            size_t inAvail = payload.size() - sizeof(VdevIoctlReq);
            uint32_t cmd = static_cast<uint32_t>(ireq.cmd);

            std::vector<uint8_t> out;
            int32_t rc = 0;
            std::lock_guard<std::mutex> lk(serialMu_);
            if (cmd == kTcGets) {
                out.assign(termios_, termios_ + 36);
            } else if (cmd == kTcSets || cmd == kTcSetsW || cmd == kTcSetsF) {
                if (inAvail >= 36) std::memcpy(termios_, inData, 36);
            } else if (cmd == kTiocGWinsz) {
                out.resize(8);
                std::memcpy(out.data(), winsize_, 8);
            } else if (cmd == kTiocSWinsz) {
                if (inAvail >= 8) std::memcpy(winsize_, inData, 8);
            } else if (cmd == kTiocMGet) {
                out.resize(4);
                std::memcpy(out.data(), &modemBits_, 4);
            } else if (cmd == kTiocMSet && inAvail >= 4) {
                std::memcpy(&modemBits_, inData, 4);
            } else if (cmd == kTiocMBis && inAvail >= 4) {
                uint32_t m = 0; std::memcpy(&m, inData, 4); modemBits_ |= m;
            } else if (cmd == kTiocMBic && inAvail >= 4) {
                uint32_t m = 0; std::memcpy(&m, inData, 4); modemBits_ &= ~m;
            } else {
                rc = -ENOTTY;
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
