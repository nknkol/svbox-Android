/* SPDX-License-Identifier: MIT */
/* Real-device smoke test for libgfxstream_host.so on HarmonyOS PC (Maleoon 916). */
#include "svbox_gfxstream_capi.h"

#include <cstdio>
#include <cstring>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

static int ConnectPipe(const char* sockPath, const char* pipeName) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa = {};
    sa.sun_family = AF_UNIX;
    socklen_t saLen = sizeof(sa);
    if (sockPath[0] == '@') {
        size_t slen = std::strlen(sockPath + 1);
        sa.sun_path[0] = '\0';
        std::memcpy(sa.sun_path + 1, sockPath + 1, slen);
        saLen = static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + 1 + slen);
    } else {
        std::strncpy(sa.sun_path, sockPath, sizeof(sa.sun_path) - 1);
    }
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&sa), saLen) < 0) {
        close(fd);
        return -1;
    }
    char hdr[64];
    int n = snprintf(hdr, sizeof(hdr), "pipe:%s", pipeName);
    if (send(fd, hdr, static_cast<size_t>(n) + 1, 0) != n + 1) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool ReadExact(int fd, void* dst, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(dst);
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

static int TestMode(const char* label, int mode, const char* sockPath) {
    printf("\n=== Smoke Test: %s (mode=%d) ===\n", label, mode);
    int rc = svbox_gfx_init(720, 1280, mode, sockPath);
    if (rc != 0) {
        printf("FAIL: svbox_gfx_init returned %d\n", rc);
        return 1;
    }
    SvboxGfxStats st = {};
    svbox_gfx_get_stats(&st);
    printf("Host initialized: vendor='%s' renderer='%s' version='%s' vk=%d\n",
           st.vendor, st.renderer, st.version, st.vulkanEnabled);

    // 1. Test GLProcessPipe handshake
    int pfd = ConnectPipe(sockPath, "GLProcessPipe");
    if (pfd < 0) {
        printf("FAIL: connect GLProcessPipe\n");
        svbox_gfx_stop();
        return 2;
    }
    int32_t confirm = 100;
    send(pfd, &confirm, 4, 0);
    uint64_t puid = 0;
    if (!ReadExact(pfd, &puid, sizeof(puid)) || puid == 0) {
        printf("FAIL: read puid\n");
        close(pfd);
        svbox_gfx_stop();
        return 3;
    }
    printf("GLProcessPipe OK: puid=%llu\n", static_cast<unsigned long long>(puid));

    // 2. Test opengles channel: send clientFlags=0 + OP_rcGetRendererVersion (10000)
    int gfd = ConnectPipe(sockPath, "opengles");
    if (gfd < 0) {
        printf("FAIL: connect opengles\n");
        close(pfd);
        svbox_gfx_stop();
        return 4;
    }
    uint32_t clientFlags = 0;
    send(gfd, &clientFlags, 4, 0);

    // OP_rcGetRendererVersion = 10000, packet size = 8 bytes (opcode + size)
    uint32_t reqVer[2] = { 10000u, 8u };
    send(gfd, reqVer, sizeof(reqVer), 0);
    int32_t rcVer = -1;
    if (!ReadExact(gfd, &rcVer, sizeof(rcVer))) {
        printf("FAIL: rcGetRendererVersion recv\n");
        close(gfd);
        close(pfd);
        svbox_gfx_stop();
        return 5;
    }
    printf("rcGetRendererVersion() -> %d\n", rcVer);

    // OP_rcCreateColorBuffer = 10012: uint32 opcode, uint32 size(20), uint32 w(16), uint32 h(16), uint32 internalFormat(GL_RGBA=0x1908)
    uint32_t reqCreateCb[5] = { 10012u, 20u, 16u, 16u, 0x1908u };
    send(gfd, reqCreateCb, sizeof(reqCreateCb), 0);
    uint32_t cbHandle = 0;
    if (!ReadExact(gfd, &cbHandle, sizeof(cbHandle)) || cbHandle == 0) {
        printf("FAIL: rcCreateColorBuffer recv (cb=%u)\n", cbHandle);
        close(gfd);
        close(pfd);
        svbox_gfx_stop();
        return 6;
    }
    printf("rcCreateColorBuffer(16x16, GL_RGBA) -> handle=%u\n", cbHandle);

    // Test refcount pipe with cbHandle
    int rfd = ConnectPipe(sockPath, "refcount");
    if (rfd >= 0) {
        send(rfd, &cbHandle, 4, 0);
        printf("Refcount pipe registered handle=%u\n", cbHandle);
    }

    // OP_rcUpdateColorBuffer = 10024:
    //   opcode(4) + pktSize(4) + cb(4) + x(4) + y(4) + w(4) + h(4) + format(4) + type(4) + pixelsLen(4) + pixels(16*16*4)
    std::vector<uint8_t> pixels(16 * 16 * 4);
    for (size_t i = 0; i < 16 * 16; ++i) {
        pixels[i * 4 + 0] = 0x12;
        pixels[i * 4 + 1] = 0x34;
        pixels[i * 4 + 2] = 0x56;
        pixels[i * 4 + 3] = 0xFF;
    }
    uint32_t updHdr[10] = {
        10024u,
        static_cast<uint32_t>(40 + pixels.size()),
        cbHandle, 0u, 0u, 16u, 16u,
        0x1908u /* GL_RGBA */, 0x1401u /* GL_UNSIGNED_BYTE */,
        static_cast<uint32_t>(pixels.size()),
    };
    send(gfd, updHdr, sizeof(updHdr), 0);
    send(gfd, pixels.data(), pixels.size(), 0);
    int32_t updRet = -999;
    if (!ReadExact(gfd, &updRet, sizeof(updRet))) {
        printf("FAIL: rcUpdateColorBuffer recv\n");
        if (rfd >= 0) close(rfd);
        close(gfd);
        close(pfd);
        svbox_gfx_stop();
        return 7;
    }
    printf("rcUpdateColorBuffer() -> %d\n", updRet);

    // OP_rcReadColorBuffer = 10023:
    //   opcode(4) + pktSize(4) + cb(4) + x(4) + y(4) + w(4) + h(4) + format(4) + type(4) + outLen(4)
    uint32_t rdHdr[10] = {
        10023u, 40u, cbHandle, 0u, 0u, 16u, 16u,
        0x1908u, 0x1401u, static_cast<uint32_t>(pixels.size()),
    };
    send(gfd, rdHdr, sizeof(rdHdr), 0);
    std::vector<uint8_t> rdOut(pixels.size(), 0);
    if (!ReadExact(gfd, rdOut.data(), rdOut.size())) {
        printf("FAIL: rcReadColorBuffer recv\n");
        if (rfd >= 0) close(rfd);
        close(gfd);
        close(pfd);
        svbox_gfx_stop();
        return 8;
    }
    bool match = (rdOut[0] == 0x12 && rdOut[1] == 0x34 && rdOut[2] == 0x56 && rdOut[3] == 0xFF);
    printf("rcReadColorBuffer() pixel[0] = 0x%02x%02x%02x%02x (%s)\n",
           rdOut[0], rdOut[1], rdOut[2], rdOut[3], match ? "MATCH!" : "MISMATCH!");

    if (rfd >= 0) close(rfd);
    close(gfd);
    close(pfd);
    usleep(50000);
    svbox_gfx_stop();
    printf("=== %s: %s ===\n", label, match ? "PASS" : "FAIL");
    return match ? 0 : 9;
}

int main() {
    const char* sock = "@svbox_gfx_smoke_test";
    int r1 = TestMode("GLES (Maleoon 916 EGL/GLES 3.2)", SVBOX_GFX_MODE_GLES, sock);
    int r2 = TestMode("Vulkan (Maleoon 916 Vulkan 1.2 + GLES 3.2)", SVBOX_GFX_MODE_VULKAN, sock);
    return (r1 == 0 && r2 == 0) ? 0 : 1;
}
