/* SPDX-License-Identifier: MIT */
/*
 * libsvbox_wm_jni.cpp - Zero-copy AImageReader -> gfxstream ColorBuffer presenter
 * + AF_UNIX filesystem socket helper for svbox-wm-agent (Android 14 ARM64).
 */

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>

typedef struct native_handle {
    int version;
    int numFds;
    int numInts;
    int data[0];
} native_handle_t;
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <media/NdkImageReader.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <mutex>
#include <string>

#define LOG_TAG "svbox_wm_jni"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" {
// Declared in libnativewindow.so (API 29+)
const native_handle_t* AHardwareBuffer_getNativeHandle(const AHardwareBuffer* buffer);
}

namespace {

constexpr uint32_t kCbMagic30 = 0xABFABF05u;
constexpr uint32_t kCbMagicMask = 0xFFFFFFF0u;
constexpr uint32_t kCbMagicBase = 0xABFABFA0u;

struct PresentPacket {
    uint32_t displaySlot;
    uint32_t cbHandle;
    int32_t width;
    int32_t height;
};

uint32_t ExtractHostCbHandle(const native_handle_t* nh) {
    if (!nh || nh->numFds < 0 || nh->numInts < 2 || nh->numFds > 32 || nh->numInts > 128) {
        return 0;
    }
    const int start = nh->numFds;
    const int end = nh->numFds + nh->numInts - 1;
    for (int i = start; i < end; ++i) {
        uint32_t v = static_cast<uint32_t>(nh->data[i]);
        if (v == kCbMagic30 || (v & kCbMagicMask) == kCbMagicBase) {
            return static_cast<uint32_t>(nh->data[i + 1]);
        }
    }
    return 0;
}

int ConnectUnixSocket(const std::string& path) {
    if (path.empty()) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa = {};
    sa.sun_family = AF_UNIX;
    socklen_t saLen = sizeof(sa);
    if (path[0] == '@') {
        size_t len = path.size() - 1;
        if (len > sizeof(sa.sun_path) - 2) len = sizeof(sa.sun_path) - 2;
        sa.sun_path[0] = '\0';
        memcpy(sa.sun_path + 1, path.c_str() + 1, len);
        saLen = static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + 1 + len);
    } else {
        strncpy(sa.sun_path, path.c_str(), sizeof(sa.sun_path) - 1);
    }
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&sa), saLen) < 0) {
        close(fd);
        return -1;
    }
    struct timeval tv = { 3, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

bool SendAllFd(int fd, const void* buf, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
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

struct DisplayReaderCtx {
    std::mutex mu;
    int32_t slot = 0;
    int32_t width = 0;
    int32_t height = 0;
    std::string presentSock;
    int sockFd = -1;
    AImageReader* reader = nullptr;
    AImage* heldImage = nullptr;
    AImageReader_ImageListener listener{};
    uint64_t frames = 0;
    bool closed = false;
};

void OnImageAvailableCB(void* context, AImageReader* reader) {
    auto* ctx = static_cast<DisplayReaderCtx*>(context);
    if (!ctx) return;
    std::lock_guard<std::mutex> lk(ctx->mu);
    if (ctx->closed || ctx->reader != reader) return;

    AImage* img = nullptr;
    media_status_t st = AImageReader_acquireLatestImage(reader, &img);
    if (st != AMEDIA_OK || !img) {
        return;
    }

    AHardwareBuffer* hb = nullptr;
    st = AImage_getHardwareBuffer(img, &hb);
    if (st != AMEDIA_OK || !hb) {
        AImage_delete(img);
        return;
    }

    const native_handle_t* nh = AHardwareBuffer_getNativeHandle(hb);
    uint32_t cbHandle = ExtractHostCbHandle(nh);
    int32_t w = ctx->width;
    int32_t h = ctx->height;
    AImage_getWidth(img, &w);
    AImage_getHeight(img, &h);

    if (cbHandle != 0) {
        if (ctx->sockFd < 0) {
            ctx->sockFd = ConnectUnixSocket(ctx->presentSock);
        }
        if (ctx->sockFd >= 0) {
            PresentPacket pkt = {
                static_cast<uint32_t>(ctx->slot),
                cbHandle,
                w,
                h,
            };
            uint8_t ack = 0;
            bool ok = SendAllFd(ctx->sockFd, &pkt, sizeof(pkt));
            if (ok) {
                ssize_t r = recv(ctx->sockFd, &ack, 1, 0);
                if (r != 1) ok = false;
            }
            if (!ok) {
                close(ctx->sockFd);
                ctx->sockFd = -1;
            } else {
                ctx->frames++;
                if (ctx->frames == 1 || ctx->frames == 10 || (ctx->frames % 300 == 0)) {
                    ALOGI("slot=%d presented frame #%llu (cb=0x%x %dx%d ack=%u)",
                          ctx->slot, static_cast<unsigned long long>(ctx->frames),
                          cbHandle, w, h, ack);
                }
            }
        }
    } else if (ctx->frames == 0) {
        ALOGW("slot=%d: failed to extract host cbHandle from AHardwareBuffer (nh=%p)",
              ctx->slot, nh);
    }

    // Release previous image after host finished presenting new frame; keep current held
    // so ColorBuffer remains pinned until next frame or destroy.
    if (ctx->heldImage) {
        AImage_delete(ctx->heldImage);
    }
    ctx->heldImage = img;
}

} // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_org_svbox_wm_SvboxWmAgent_nativeCreateReader(
    JNIEnv* env, jclass, jint slot, jint width, jint height, jstring jPresentSock) {
    if (width <= 0) width = 720;
    if (height <= 0) height = 1280;
    const char* sockChars = jPresentSock ? env->GetStringUTFChars(jPresentSock, nullptr) : nullptr;
    std::string presentSock = sockChars ? sockChars : "";
    if (sockChars) env->ReleaseStringUTFChars(jPresentSock, sockChars);

    auto* ctx = new DisplayReaderCtx();
    ctx->slot = slot;
    ctx->width = width;
    ctx->height = height;
    ctx->presentSock = presentSock;

    const uint64_t usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                           AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
    media_status_t st = AImageReader_newWithUsage(
        width, height, AIMAGE_FORMAT_RGBA_8888, usage, /*maxImages=*/4, &ctx->reader);
    if (st != AMEDIA_OK || !ctx->reader) {
        ALOGE("AImageReader_newWithUsage(slot=%d, %dx%d) failed: %d", slot, width, height, st);
        delete ctx;
        return 0;
    }

    ctx->listener.context = ctx;
    ctx->listener.onImageAvailable = OnImageAvailableCB;
    AImageReader_setImageListener(ctx->reader, &ctx->listener);
    ctx->sockFd = ConnectUnixSocket(presentSock);

    ALOGI("Created AImageReader slot=%d %dx%d sock=%s (fd=%d)",
          slot, width, height, presentSock.c_str(), ctx->sockFd);
    return reinterpret_cast<jlong>(ctx);
}

JNIEXPORT jobject JNICALL
Java_org_svbox_wm_SvboxWmAgent_nativeGetReaderSurface(JNIEnv* env, jclass, jlong handle) {
    auto* ctx = reinterpret_cast<DisplayReaderCtx*>(handle);
    if (!ctx || !ctx->reader) return nullptr;
    ANativeWindow* anw = nullptr;
    if (AImageReader_getWindow(ctx->reader, &anw) != AMEDIA_OK || !anw) {
        ALOGE("AImageReader_getWindow failed for slot=%d", ctx->slot);
        return nullptr;
    }
    return ANativeWindow_toSurface(env, anw);
}

JNIEXPORT jlong JNICALL
Java_org_svbox_wm_SvboxWmAgent_nativeGetReaderFrames(JNIEnv*, jclass, jlong handle) {
    auto* ctx = reinterpret_cast<DisplayReaderCtx*>(handle);
    if (!ctx) return 0;
    std::lock_guard<std::mutex> lk(ctx->mu);
    return static_cast<jlong>(ctx->frames);
}

JNIEXPORT void JNICALL
Java_org_svbox_wm_SvboxWmAgent_nativeDestroyReader(JNIEnv*, jclass, jlong handle) {
    auto* ctx = reinterpret_cast<DisplayReaderCtx*>(handle);
    if (!ctx) return;
    AImageReader* reader = nullptr;
    {
        std::lock_guard<std::mutex> lk(ctx->mu);
        ctx->closed = true;
        reader = ctx->reader;
        ctx->reader = nullptr;
        if (ctx->heldImage) {
            AImage_delete(ctx->heldImage);
            ctx->heldImage = nullptr;
        }
        if (ctx->sockFd >= 0) {
            close(ctx->sockFd);
            ctx->sockFd = -1;
        }
    }
    if (reader) {
        AImageReader_ImageListener empty = {};
        AImageReader_setImageListener(reader, &empty);
        AImageReader_delete(reader);
    }
    ALOGI("Destroyed AImageReader slot=%d (frames=%llu)",
          ctx->slot, static_cast<unsigned long long>(ctx->frames));
    delete ctx;
}

JNIEXPORT jint JNICALL
Java_org_svbox_wm_SvboxWmAgent_nativeListenUnix(JNIEnv* env, jclass, jstring jPath) {
    if (!jPath) return -1;
    const char* path = env->GetStringUTFChars(jPath, nullptr);
    if (!path) return -1;
    std::string p(path);
    env->ReleaseStringUTFChars(jPath, path);

    unlink(p.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa = {};
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, p.c_str(), sizeof(sa.sun_path) - 1);
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) < 0 ||
        listen(fd, 16) < 0) {
        ALOGE("bind/listen(%s) failed: errno=%d", p.c_str(), errno);
        close(fd);
        return -1;
    }
    chmod(p.c_str(), 0666);
    return fd;
}

JNIEXPORT jint JNICALL
Java_org_svbox_wm_SvboxWmAgent_nativeAcceptUnix(JNIEnv*, jclass, jint listenFd) {
    if (listenFd < 0) return -1;
    while (true) {
        int cfd = accept(listenFd, nullptr, nullptr);
        if (cfd >= 0) return cfd;
        if (errno == EINTR) continue;
        return -1;
    }
}

JNIEXPORT jstring JNICALL
Java_org_svbox_wm_SvboxWmAgent_nativeReadLine(JNIEnv* env, jclass, jint fd) {
    if (fd < 0) return nullptr;
    std::string line;
    line.reserve(256);
    char ch = 0;
    while (true) {
        ssize_t r = read(fd, &ch, 1);
        if (r == 1) {
            if (ch == '\n') break;
            if (ch != '\r') line.push_back(ch);
            if (line.size() > 65536) break;
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else {
            if (line.empty()) return nullptr;
            break;
        }
    }
    return env->NewStringUTF(line.c_str());
}

JNIEXPORT jboolean JNICALL
Java_org_svbox_wm_SvboxWmAgent_nativeWriteLine(JNIEnv* env, jclass, jint fd, jstring jLine) {
    if (fd < 0 || !jLine) return JNI_FALSE;
    const char* s = env->GetStringUTFChars(jLine, nullptr);
    if (!s) return JNI_FALSE;
    std::string msg(s);
    env->ReleaseStringUTFChars(jLine, s);
    msg.push_back('\n');
    return SendAllFd(fd, msg.data(), msg.size()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_org_svbox_wm_SvboxWmAgent_nativeCloseFd(JNIEnv*, jclass, jint fd) {
    if (fd >= 0) close(fd);
}

} // extern "C"
