/* SPDX-License-Identifier: MIT */
/*
 * svbox_pipe_guest.cpp - Blocking AF_UNIX transport for goldfish-opengl guest.
 *
 * Replaces /dev/goldfish_pipe with a direct AF_UNIX stream connection to
 * svbox-Android's host gfxstream server (/ipc/gfxstream.sock or
 * $SVBOX_GFXSTREAM_SOCK, supporting "@abstract" names for CLI smoke tests).
 */

#include <qemu_pipe_bp.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <log/log.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#undef LOG_TAG
#define LOG_TAG "svbox_qemupipe"

static const char* get_socket_path() {
    const char* env = getenv("SVBOX_GFXSTREAM_SOCK");
    if (env && env[0] != '\0') {
        return env;
    }
    return "/ipc/gfxstream.sock";
}

extern "C" {

int qemu_pipe_try_again(int ret) {
    if (ret >= 0) {
        return 0;
    }
    return (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) ? 1 : 0;
}

int qemu_pipe_read(QEMU_PIPE_HANDLE pipe, void* buffer, int size) {
    if (pipe < 0 || size < 0) {
        errno = EINVAL;
        return -1;
    }
    if (size == 0) {
        return 0;
    }
    ssize_t n;
    do {
        n = recv(pipe, buffer, static_cast<size_t>(size), 0);
    } while (n < 0 && errno == EINTR);
    return static_cast<int>(n);
}

int qemu_pipe_write(QEMU_PIPE_HANDLE pipe, const void* buffer, int size) {
    if (pipe < 0 || size < 0) {
        errno = EINVAL;
        return -1;
    }
    if (size == 0) {
        return 0;
    }
    ssize_t n;
    do {
        n = send(pipe, buffer, static_cast<size_t>(size), MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    return static_cast<int>(n);
}

int qemu_pipe_read_fully(QEMU_PIPE_HANDLE pipe, void* buffer, int size) {
    char* p = static_cast<char*>(buffer);
    int left = size;
    while (left > 0) {
        int n = qemu_pipe_read(pipe, p, left);
        if (n < 0) {
            if (qemu_pipe_try_again(n)) {
                continue;
            }
            return n;
        }
        if (n == 0) {
            errno = ECONNRESET;
            return -1;
        }
        p += n;
        left -= n;
    }
    return 0;
}

int qemu_pipe_write_fully(QEMU_PIPE_HANDLE pipe, const void* buffer, int size) {
    const char* p = static_cast<const char*>(buffer);
    int left = size;
    while (left > 0) {
        int n = qemu_pipe_write(pipe, p, left);
        if (n < 0) {
            if (qemu_pipe_try_again(n)) {
                continue;
            }
            return n;
        }
        if (n == 0) {
            errno = EPIPE;
            return -1;
        }
        p += n;
        left -= n;
    }
    return 0;
}

QEMU_PIPE_HANDLE qemu_pipe_open_ns(const char* ns, const char* pipeName, int /*flags*/) {
    if (!pipeName || pipeName[0] == '\0') {
        errno = EINVAL;
        return QEMU_PIPE_INVALID_HANDLE;
    }

    char handshake[256];
    int handshakeLen = 0;
    if (ns && ns[0] != '\0') {
        handshakeLen = snprintf(handshake, sizeof(handshake), "pipe:%s:%s", ns, pipeName);
    } else {
        handshakeLen = snprintf(handshake, sizeof(handshake), "pipe:%s", pipeName);
    }
    if (handshakeLen <= 0 || handshakeLen >= static_cast<int>(sizeof(handshake))) {
        errno = ENAMETOOLONG;
        return QEMU_PIPE_INVALID_HANDLE;
    }

    // Always open blocking (ignore O_NONBLOCK from caller) so QemuPipeStream
    // blocks cleanly in recv()/send() instead of busy-spinning on EAGAIN.
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        ALOGE("socket(AF_UNIX) failed for pipe '%s': %s", pipeName, strerror(errno));
        return QEMU_PIPE_INVALID_HANDLE;
    }

    // Increase socket buffers for high-throughput GL/Vulkan command & texture streaming.
    const int bufBytes = 2 * 1024 * 1024;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufBytes, sizeof(bufBytes));
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufBytes, sizeof(bufBytes));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    const char* sockPath = get_socket_path();
    socklen_t addrLen = 0;
    if (sockPath[0] == '@') {
        size_t nameLen = strlen(sockPath + 1);
        if (nameLen >= sizeof(addr.sun_path) - 1) {
            close(fd);
            errno = ENAMETOOLONG;
            return QEMU_PIPE_INVALID_HANDLE;
        }
        addr.sun_path[0] = '\0';
        memcpy(addr.sun_path + 1, sockPath + 1, nameLen);
        addrLen = static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + 1 + nameLen);
    } else {
        size_t pathLen = strlen(sockPath);
        if (pathLen >= sizeof(addr.sun_path)) {
            close(fd);
            errno = ENAMETOOLONG;
            return QEMU_PIPE_INVALID_HANDLE;
        }
        memcpy(addr.sun_path, sockPath, pathLen + 1);
        addrLen = static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + pathLen + 1);
    }

    int rc = -1;
    do {
        rc = connect(fd, reinterpret_cast<const struct sockaddr*>(&addr), addrLen);
    } while (rc < 0 && errno == EINTR);

    if (rc < 0) {
        int saved = errno;
        ALOGE("connect('%s') failed for pipe '%s': %s", sockPath, pipeName, strerror(saved));
        close(fd);
        errno = saved;
        return QEMU_PIPE_INVALID_HANDLE;
    }

    // Write NUL-terminated pipe handshake ("pipe:<name>\0"), matching
    // goldfish-opengl's qemu_pipe_guest.cpp and svbox_gfxstream_capi.cpp.
    if (qemu_pipe_write_fully(fd, handshake, handshakeLen + 1) != 0) {
        int saved = errno;
        ALOGE("handshake '%s' failed: %s", handshake, strerror(saved));
        close(fd);
        errno = saved;
        return QEMU_PIPE_INVALID_HANDLE;
    }

    return fd;
}

QEMU_PIPE_HANDLE qemu_pipe_open(const char* pipeName) {
    return qemu_pipe_open_ns(nullptr, pipeName, O_RDWR);
}

void qemu_pipe_close(QEMU_PIPE_HANDLE pipe) {
    if (pipe >= 0) {
        close(pipe);
    }
}

void qemu_pipe_print_error(QEMU_PIPE_HANDLE pipe) {
    ALOGE("qemu_pipe error on fd=%d: %s (errno=%d)", pipe, strerror(errno), errno);
}

}  // extern "C"
