/* SPDX-License-Identifier: MIT */
/* Lightweight 64-bit ARM64 Android 11 stub for 'media.camera'
 * (android.hardware.ICameraService) reporting 0 cameras.
 * Replaces Redroid's 32-bit /system/bin/cameraserver so Gallery3D, Camera2,
 * and BOOT_COMPLETED DisableCameraReceiver return immediately instead of
 * blocking 60s+ polling ServiceManager for media.camera. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern void* dlopen(const char* filename, int flags);
extern void* dlsym(void* handle, const char* symbol);

#define RTLD_NOW 0x0002

typedef struct AIBinder AIBinder;
typedef struct AIBinder_Class AIBinder_Class;
typedef struct AParcel AParcel;
typedef struct AStatus AStatus;
typedef int32_t binder_status_t;
typedef uint32_t transaction_code_t;

typedef void* (*FnOnCreate)(void*);
typedef void (*FnOnDestroy)(void*);
typedef binder_status_t (*FnOnTransact)(AIBinder*, transaction_code_t, const AParcel*, AParcel*);

typedef AIBinder_Class* (*FnClassDefine)(const char*, FnOnCreate, FnOnDestroy, FnOnTransact);
typedef AIBinder* (*FnBinderNew)(const AIBinder_Class*, void*);
typedef binder_status_t (*FnAddService)(AIBinder*, const char*);
typedef void (*FnSetMaxThreads)(uint32_t);
typedef void (*FnStartPool)(void);
typedef void (*FnJoinPool)(void);
typedef binder_status_t (*FnWriteInt32)(AParcel*, int32_t);
typedef binder_status_t (*FnWriteBool)(AParcel*, bool);
typedef AStatus* (*FnStatusFromSSE)(int32_t);
typedef binder_status_t (*FnWriteStatusHeader)(AParcel*, const AStatus*);
typedef void (*FnStatusDelete)(AStatus*);

static FnWriteInt32 g_writeInt32 = NULL;
static FnWriteBool g_writeBool = NULL;
static FnStatusFromSSE g_statusFromSSE = NULL;
static FnWriteStatusHeader g_writeStatusHeader = NULL;
static FnStatusDelete g_statusDelete = NULL;

static inline long sys6(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x5 __asm__("x5") = a5;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory");
    return x0;
}

static void log_str(const char* s) {
    size_t n = 0;
    while (s && s[n]) n++;
    if (n > 0) sys6(64 /* write */, 2, (long)s, (long)n, 0, 0, 0);
}

static void* on_create(void* args) { return args; }
static void on_destroy(void* userData) { (void)userData; }

static binder_status_t on_transact(AIBinder* binder, transaction_code_t code,
                                   const AParcel* in, AParcel* out) {
    (void)binder;
    (void)in;
    switch (code) {
    case 1: /* getNumberOfCameras(int type) -> int */
        g_writeInt32(out, 0); /* EX_NONE */
        g_writeInt32(out, 0); /* 0 cameras */
        return 0;
    case 6: /* addListener(listener) -> CameraStatus[] */
    case 7: /* getConcurrentCameraIds() -> CameraIdCombination[] */
        g_writeInt32(out, 0); /* EX_NONE */
        g_writeInt32(out, 0); /* empty array */
        return 0;
    case 8:  /* isConcurrentSessionConfigurationSupported -> bool */
    case 14: /* supportsCameraApi -> bool */
    case 15: /* isHiddenPhysicalCamera -> bool */
        g_writeInt32(out, 0); /* EX_NONE */
        g_writeBool(out, false);
        return 0;
    case 9: /* removeListener */
        g_writeInt32(out, 0); /* EX_NONE */
        return 0;
    case 11: /* getCameraVendorTagDescriptor -> parcelable */
    case 12: /* getCameraVendorTagCache -> parcelable */
        g_writeInt32(out, 0); /* EX_NONE */
        g_writeInt32(out, 0); /* null parcelable */
        return 0;
    case 17: /* notifySystemEvent (oneway) */
    case 18: /* notifyDeviceStateChange (oneway) */
        return 0;
    default:
        if (g_statusFromSSE && g_writeStatusHeader && g_statusDelete) {
            AStatus* st = g_statusFromSSE(4 /* ERROR_DISCONNECTED */);
            g_writeStatusHeader(out, st);
            g_statusDelete(st);
        } else {
            g_writeInt32(out, -8 /* EX_SERVICE_SPECIFIC */);
            g_writeInt32(out, -1 /* null message */);
            g_writeInt32(out, 4 /* ERROR_DISCONNECTED */);
        }
        return 0;
    }
}

__attribute__((used, visibility("default")))
void _start(void) {
    void* lib = dlopen("/system/lib64/libbinder_ndk.so", RTLD_NOW);
    if (!lib) {
        log_str("[svbox-cameraserver] dlopen libbinder_ndk.so failed\n");
        sys6(94 /* exit_group */, 1, 0, 0, 0, 0, 0);
    }

    FnClassDefine classDefine = (FnClassDefine)dlsym(lib, "AIBinder_Class_define");
    FnBinderNew binderNew = (FnBinderNew)dlsym(lib, "AIBinder_new");
    FnAddService addService = (FnAddService)dlsym(lib, "AServiceManager_addService");
    FnSetMaxThreads setMaxThreads = (FnSetMaxThreads)dlsym(lib, "ABinderProcess_setThreadPoolMaxThreadCount");
    FnStartPool startPool = (FnStartPool)dlsym(lib, "ABinderProcess_startThreadPool");
    FnJoinPool joinPool = (FnJoinPool)dlsym(lib, "ABinderProcess_joinThreadPool");
    g_writeInt32 = (FnWriteInt32)dlsym(lib, "AParcel_writeInt32");
    g_writeBool = (FnWriteBool)dlsym(lib, "AParcel_writeBool");
    g_statusFromSSE = (FnStatusFromSSE)dlsym(lib, "AStatus_fromServiceSpecificError");
    g_writeStatusHeader = (FnWriteStatusHeader)dlsym(lib, "AParcel_writeStatusHeader");
    g_statusDelete = (FnStatusDelete)dlsym(lib, "AStatus_delete");

    if (!classDefine || !binderNew || !addService || !joinPool || !g_writeInt32 || !g_writeBool) {
        log_str("[svbox-cameraserver] missing libbinder_ndk symbols\n");
        sys6(94 /* exit_group */, 2, 0, 0, 0, 0, 0);
    }

    if (setMaxThreads) setMaxThreads(4);
    if (startPool) startPool();

    AIBinder_Class* clazz = classDefine("android.hardware.ICameraService",
                                        on_create, on_destroy, on_transact);
    AIBinder* svc = clazz ? binderNew(clazz, NULL) : NULL;
    if (!svc) {
        log_str("[svbox-cameraserver] AIBinder_new failed\n");
        sys6(94 /* exit_group */, 3, 0, 0, 0, 0, 0);
    }

    binder_status_t rc = addService(svc, "media.camera");
    if (rc == 0) {
        log_str("[svbox-cameraserver] registered media.camera (0 cameras)\n");
    } else {
        log_str("[svbox-cameraserver] addService(media.camera) failed\n");
        sys6(94 /* exit_group */, 4, 0, 0, 0, 0, 0);
    }

    joinPool();
    sys6(94 /* exit_group */, 0, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}
