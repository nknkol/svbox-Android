/* SPDX-License-Identifier: MIT */
/* Lightweight 64-bit ARM64 Android 14 AIDL V2 Sensors HAL
 * (android.hardware.sensors.ISensors/default) exposing standard virtual
 * motion and environment sensors over FMQ + libbinder_ndk.so.
 *
 * Linked solely against Android's Bionic libc.so + libdl.so with
 * PT_INTERP=/system/bin/linker64 (matching camera_stub/svbox_cameraserver.c)
 * to avoid any host-vs-Android C++ STL ABI mismatch. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern void* dlopen(const char* filename, int flags);
extern void* dlsym(void* handle, const char* symbol);
extern int pthread_create(unsigned long* thread, const void* attr,
                          void* (*start_routine)(void*), void* arg);

#define RTLD_NOW 0x0002

/* Linux AArch64 syscall numbers */
#define SYS_close         57
#define SYS_write         64
#define SYS_futex         98
#define SYS_nanosleep    101
#define SYS_clock_gettime 113
#define SYS_munmap       215
#define SYS_mmap         222
#define SYS_exit_group    94

#define CLOCK_BOOTTIME     7
#define PROT_READ          1
#define PROT_WRITE         2
#define MAP_SHARED         1
#define FUTEX_WAKE         1

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
typedef void (*FnMarkVintf)(AIBinder*);
typedef binder_status_t (*FnAddService)(AIBinder*, const char*);
typedef void (*FnSetMaxThreads)(uint32_t);
typedef void (*FnStartPool)(void);
typedef void (*FnJoinPool)(void);

typedef int32_t (*FnGetDataPos)(const AParcel*);
typedef binder_status_t (*FnSetDataPos)(const AParcel*, int32_t);
typedef binder_status_t (*FnReadInt32)(const AParcel*, int32_t*);
typedef binder_status_t (*FnReadInt64)(const AParcel*, int64_t*);
typedef binder_status_t (*FnReadBool)(const AParcel*, bool*);
typedef binder_status_t (*FnReadPfd)(const AParcel*, int*);
typedef binder_status_t (*FnWriteInt32)(AParcel*, int32_t);
typedef binder_status_t (*FnWriteFloat)(AParcel*, float);
typedef binder_status_t (*FnWriteString)(AParcel*, const char*, int32_t);
typedef AStatus* (*FnStatusFromSSE)(int32_t);
typedef binder_status_t (*FnWriteStatusHeader)(AParcel*, const AStatus*);
typedef void (*FnStatusDelete)(AStatus*);

static FnGetDataPos g_getPos = NULL;
static FnSetDataPos g_setPos = NULL;
static FnReadInt32 g_readInt32 = NULL;
static FnReadInt64 g_readInt64 = NULL;
static FnReadBool g_readBool = NULL;
static FnReadPfd g_readPfd = NULL;
static FnWriteInt32 g_writeInt32 = NULL;
static FnWriteFloat g_writeFloat = NULL;
static FnWriteString g_writeString = NULL;
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

static size_t cstrlen(const char* s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void mem_zero(void* dst, size_t n) {
    uint8_t* p = (uint8_t*)dst;
    for (size_t i = 0; i < n; i++) p[i] = 0;
}

static void mem_copy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static void log_str(const char* s) {
    size_t n = cstrlen(s);
    if (n > 0) sys6(SYS_write, 2, (long)s, (long)n, 0, 0, 0);
}

static void log_i64(int64_t v) {
    char buf[32];
    int pos = 0;
    uint64_t u;
    if (v < 0) {
        buf[pos++] = '-';
        u = (uint64_t)(-(v + 1)) + 1ull;
    } else {
        u = (uint64_t)v;
    }
    char rev[24];
    int r = 0;
    do { rev[r++] = (char)('0' + (u % 10ull)); u /= 10ull; } while (u);
    while (r > 0) buf[pos++] = rev[--r];
    sys6(SYS_write, 2, (long)buf, pos, 0, 0, 0);
}

static int64_t boottime_ns(void) {
    struct { int64_t tv_sec; int64_t tv_nsec; } ts = {0, 0};
    sys6(SYS_clock_gettime, CLOCK_BOOTTIME, (long)&ts, 0, 0, 0, 0);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void sleep_ms(int64_t ms) {
    struct { int64_t tv_sec; int64_t tv_nsec; } req;
    req.tv_sec = ms / 1000LL;
    req.tv_nsec = (ms % 1000LL) * 1000000LL;
    sys6(SYS_nanosleep, (long)&req, 0, 0, 0, 0, 0);
}

/* ---- Virtual Sensor Table ---- */

enum {
    PAYLOAD_VEC3   = 0,
    PAYLOAD_VEC4   = 1,
    PAYLOAD_META   = 3,
    PAYLOAD_SCALAR = 4,
};

struct VirtualSensor {
    int32_t handle;
    int32_t type;
    const char* name;
    const char* vendor;
    const char* type_str;
    float max_range;
    float resolution;
    float power;
    int32_t min_delay_us;
    int32_t max_delay_us;
    int32_t flags; /* 0 = continuous, 2 = on-change */
    uint8_t payload_tag;
    float v[4];
};

static const struct VirtualSensor kSensors[] = {
    {
        1, 1 /* ACCELEROMETER */,
        "Virtual 3-axis Accelerometer", "AOSP", "android.sensor.accelerometer",
        39.2266f, 0.0012f, 0.25f, 5000, 500000, 0,
        PAYLOAD_VEC3, { 0.0f, 0.0f, 9.80665f, 0.0f }
    },
    {
        2, 2 /* MAGNETIC_FIELD */,
        "Virtual 3-axis Magnetometer", "AOSP", "android.sensor.magnetic_field",
        4900.0f, 0.15f, 0.50f, 10000, 500000, 0,
        PAYLOAD_VEC3, { 0.0f, 22.0f, -42.0f, 0.0f }
    },
    {
        3, 4 /* GYROSCOPE */,
        "Virtual 3-axis Gyroscope", "AOSP", "android.sensor.gyroscope",
        34.9066f, 0.0011f, 0.55f, 5000, 500000, 0,
        PAYLOAD_VEC3, { 0.0f, 0.0f, 0.0f, 0.0f }
    },
    {
        4, 5 /* LIGHT */,
        "Virtual Ambient Light Sensor", "AOSP", "android.sensor.light",
        40000.0f, 1.0f, 0.15f, 0, 0, 2 /* ON_CHANGE */,
        PAYLOAD_SCALAR, { 250.0f, 0.0f, 0.0f, 0.0f }
    },
    {
        5, 8 /* PROXIMITY */,
        "Virtual Proximity Sensor", "AOSP", "android.sensor.proximity",
        5.0f, 1.0f, 0.20f, 0, 0, 2 /* ON_CHANGE */,
        PAYLOAD_SCALAR, { 5.0f, 0.0f, 0.0f, 0.0f }
    },
    {
        6, 9 /* GRAVITY */,
        "Virtual Gravity Sensor", "AOSP", "android.sensor.gravity",
        19.6133f, 0.0012f, 0.25f, 5000, 500000, 0,
        PAYLOAD_VEC3, { 0.0f, 0.0f, 9.80665f, 0.0f }
    },
    {
        7, 10 /* LINEAR_ACCELERATION */,
        "Virtual Linear Acceleration", "AOSP", "android.sensor.linear_acceleration",
        39.2266f, 0.0012f, 0.25f, 5000, 500000, 0,
        PAYLOAD_VEC3, { 0.0f, 0.0f, 0.0f, 0.0f }
    },
    {
        8, 15 /* GAME_ROTATION_VECTOR */,
        "Virtual Game Rotation Vector", "AOSP", "android.sensor.game_rotation_vector",
        1.0f, 0.0001f, 0.55f, 5000, 500000, 0,
        PAYLOAD_VEC4, { 0.0f, 0.0f, 0.0f, 1.0f }
    },
};

#define NUM_SENSORS ((int)(sizeof(kSensors) / sizeof(kSensors[0])))

struct SensorRuntimeState {
    volatile int32_t active;
    volatile int64_t period_ns;
    int64_t last_emit_ns;
};

static struct SensorRuntimeState g_state[NUM_SENSORS];

/* ---- FMQ Event Ring Buffer State ---- */

struct FmqGrantor {
    int32_t fd_index;
    int32_t offset;
    int64_t extent;
};

struct FmqMap {
    uint8_t* base;
    size_t len;
};

struct FmqState {
    volatile int lock;
    int ready;
    uint32_t quantum;
    uint64_t data_size;
    volatile uint64_t* read_ptr;
    volatile uint64_t* write_ptr;
    uint8_t* data;
    volatile uint32_t* ev_flag;
    struct FmqMap maps[4];
    int map_count;
};

static struct FmqState g_fmq;

static void fmq_lock(void) {
    while (__sync_lock_test_and_set(&g_fmq.lock, 1))
        __asm__ volatile("yield" ::: "memory");
}

static void fmq_unlock(void) {
    __sync_lock_release(&g_fmq.lock);
}

static void fmq_unmap_locked(void) {
    g_fmq.ready = 0;
    g_fmq.read_ptr = NULL;
    g_fmq.write_ptr = NULL;
    g_fmq.data = NULL;
    g_fmq.ev_flag = NULL;
    for (int i = 0; i < g_fmq.map_count; i++) {
        if (g_fmq.maps[i].base && g_fmq.maps[i].len > 0) {
            sys6(SYS_munmap, (long)g_fmq.maps[i].base, (long)g_fmq.maps[i].len, 0, 0, 0, 0);
        }
        g_fmq.maps[i].base = NULL;
        g_fmq.maps[i].len = 0;
    }
    g_fmq.map_count = 0;
}

static int parse_mq_descriptor(const AParcel* in, int map_queue) {
    int32_t non_null = 0;
    if (g_readInt32(in, &non_null) != 0 || !non_null) return -1;

    int32_t mq_start = g_getPos(in);
    int32_t mq_size = 0;
    if (g_readInt32(in, &mq_size) != 0 || mq_size < 4) return -1;

    struct FmqGrantor grantors[8];
    int32_t ng = 0;
    if (g_readInt32(in, &ng) != 0 || ng < 0) {
        g_setPos(in, mq_start + mq_size);
        return -1;
    }
    for (int32_t i = 0; i < ng; i++) {
        int32_t gn = 0;
        if (g_readInt32(in, &gn) != 0) break;
        if (gn) {
            int32_t gs = g_getPos(in);
            int32_t gsz = 0;
            int32_t fdi = 0, off = 0;
            int64_t ext = 0;
            g_readInt32(in, &gsz);
            g_readInt32(in, &fdi);
            g_readInt32(in, &off);
            g_readInt64(in, &ext);
            if (i < 8) {
                grantors[i].fd_index = fdi;
                grantors[i].offset = off;
                grantors[i].extent = ext;
            }
            if (gsz >= 4) g_setPos(in, gs + gsz);
        }
    }

    int fds[4] = { -1, -1, -1, -1 };
    int32_t nfds = 0;
    int32_t hn = 0;
    if (g_readInt32(in, &hn) == 0 && hn) {
        int32_t hs = g_getPos(in);
        int32_t hsz = 0;
        g_readInt32(in, &hsz);
        int32_t cnt = 0;
        if (g_readInt32(in, &cnt) == 0 && cnt > 0) {
            for (int32_t i = 0; i < cnt; i++) {
                int fd = -1;
                if (g_readPfd(in, &fd) == 0 && fd >= 0) {
                    if (nfds < 4) {
                        fds[nfds++] = fd;
                    } else {
                        sys6(SYS_close, fd, 0, 0, 0, 0, 0);
                    }
                }
            }
        }
        if (hsz >= 4) g_setPos(in, hs + hsz);
    }

    int32_t quantum = 0, flags = 0;
    g_readInt32(in, &quantum);
    g_readInt32(in, &flags);
    g_setPos(in, mq_start + mq_size);

    int rc = 0;
    if (map_queue && ng >= 3 && nfds > 0 && quantum >= 96 && quantum <= 256) {
        size_t max_end[4] = { 0, 0, 0, 0 };
        int lim = ng < 4 ? ng : 4;
        for (int i = 0; i < lim; i++) {
            int fi = grantors[i].fd_index;
            if (fi >= 0 && fi < nfds && grantors[i].offset >= 0 && grantors[i].extent > 0) {
                size_t end = (size_t)grantors[i].offset + (size_t)grantors[i].extent;
                if (end > max_end[fi]) max_end[fi] = end;
            }
        }

        fmq_lock();
        fmq_unmap_locked();
        int ok = 1;
        for (int fi = 0; fi < nfds; fi++) {
            if (max_end[fi] == 0) continue;
            size_t mlen = (max_end[fi] + 4095u) & ~(size_t)4095u;
            long addr = sys6(SYS_mmap, 0, (long)mlen, PROT_READ | PROT_WRITE, MAP_SHARED, fds[fi], 0);
            if (addr < 0 && addr > -4096) {
                ok = 0;
                break;
            }
            g_fmq.maps[fi].base = (uint8_t*)addr;
            g_fmq.maps[fi].len = mlen;
            if (fi + 1 > g_fmq.map_count) g_fmq.map_count = fi + 1;
        }
        if (ok &&
            grantors[0].fd_index >= 0 && grantors[0].fd_index < g_fmq.map_count && g_fmq.maps[grantors[0].fd_index].base &&
            grantors[1].fd_index >= 0 && grantors[1].fd_index < g_fmq.map_count && g_fmq.maps[grantors[1].fd_index].base &&
            grantors[2].fd_index >= 0 && grantors[2].fd_index < g_fmq.map_count && g_fmq.maps[grantors[2].fd_index].base) {
            g_fmq.read_ptr  = (volatile uint64_t*)(g_fmq.maps[grantors[0].fd_index].base + grantors[0].offset);
            g_fmq.write_ptr = (volatile uint64_t*)(g_fmq.maps[grantors[1].fd_index].base + grantors[1].offset);
            g_fmq.data      = g_fmq.maps[grantors[2].fd_index].base + grantors[2].offset;
            g_fmq.data_size = (uint64_t)grantors[2].extent;
            g_fmq.quantum   = (uint32_t)quantum;
            if (ng >= 4 && grantors[3].fd_index >= 0 && grantors[3].fd_index < g_fmq.map_count &&
                g_fmq.maps[grantors[3].fd_index].base) {
                g_fmq.ev_flag = (volatile uint32_t*)(g_fmq.maps[grantors[3].fd_index].base + grantors[3].offset);
            }
            g_fmq.ready = 1;
            log_str("[svbox-sensors] FMQ mapped: quantum=");
            log_i64(quantum);
            log_str(" ring=");
            log_i64((int64_t)g_fmq.data_size);
            log_str(" evflag=");
            log_i64(g_fmq.ev_flag ? 1 : 0);
            log_str("\n");
        } else {
            fmq_unmap_locked();
            rc = -1;
        }
        fmq_unlock();
    }

    for (int i = 0; i < nfds; i++) {
        if (fds[i] >= 0) sys6(SYS_close, fds[i], 0, 0, 0, 0, 0);
    }
    return rc;
}

static void fmq_push_event(const uint8_t* ev, uint32_t len) {
    fmq_lock();
    if (!g_fmq.ready || !g_fmq.data || g_fmq.quantum == 0 || g_fmq.data_size < g_fmq.quantum) {
        fmq_unlock();
        return;
    }
    uint32_t q = g_fmq.quantum;
    uint64_t r = __atomic_load_n(g_fmq.read_ptr, __ATOMIC_ACQUIRE);
    uint64_t w = __atomic_load_n(g_fmq.write_ptr, __ATOMIC_RELAXED);
    if (w - r + q <= g_fmq.data_size) {
        uint8_t slot[256];
        mem_zero(slot, q <= sizeof(slot) ? q : sizeof(slot));
        uint32_t cpy = len < q ? len : q;
        mem_copy(slot, ev, cpy);
        uint64_t off = w % g_fmq.data_size;
        if (off + q <= g_fmq.data_size) {
            mem_copy(g_fmq.data + off, slot, q);
        } else {
            size_t first = (size_t)(g_fmq.data_size - off);
            mem_copy(g_fmq.data + off, slot, first);
            mem_copy(g_fmq.data, slot + first, q - first);
        }
        __atomic_store_n(g_fmq.write_ptr, w + q, __ATOMIC_RELEASE);
        if (g_fmq.ev_flag) {
            __atomic_fetch_or(g_fmq.ev_flag, 1u /* EventQueueFlagBits::READ_AND_PROCESS */, __ATOMIC_RELEASE);
            sys6(SYS_futex, (long)g_fmq.ev_flag, FUTEX_WAKE, 0x7fffffff, 0, 0, 0);
        }
    }
    fmq_unlock();
}

static uint32_t g_noise_seed = 0x13579bdfu;
static float micro_noise(float amp) {
    uint32_t x = g_noise_seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_noise_seed = x;
    int32_t s = (int32_t)(x & 0xffu) - 128;
    return ((float)s / 128.0f) * amp;
}

static void emit_sensor_sample(int idx, int64_t ts_ns) {
    if (idx < 0 || idx >= NUM_SENSORS) return;
    const struct VirtualSensor* s = &kSensors[idx];
    uint8_t ev[128];
    mem_zero(ev, sizeof(ev));

    /* AIDL V2 Event layout (verified against android.hardware.sensors-V2-ndk.so):
     *   0: int64_t timestamp
     *   8: int32_t sensorHandle
     *  12: int32_t sensorType
     *  16: uint8_t payload._tag (0=vec3, 1=vec4, 3=meta, 4=scalar)
     *  24: union payload data */
    mem_copy(ev + 0, &ts_ns, 8);
    mem_copy(ev + 8, &s->handle, 4);
    mem_copy(ev + 12, &s->type, 4);
    ev[16] = s->payload_tag;

    if (s->payload_tag == PAYLOAD_VEC3) {
        float x = s->v[0], y = s->v[1], z = s->v[2];
        if (s->type == 1 /* ACCEL */) {
            x += micro_noise(0.002f);
            y += micro_noise(0.002f);
            z += micro_noise(0.002f);
        } else if (s->type == 4 /* GYRO */) {
            x += micro_noise(0.0005f);
            y += micro_noise(0.0005f);
            z += micro_noise(0.0005f);
        }
        mem_copy(ev + 24, &x, 4);
        mem_copy(ev + 28, &y, 4);
        mem_copy(ev + 32, &z, 4);
        ev[36] = 3; /* SENSOR_STATUS_ACCURACY_HIGH */
    } else if (s->payload_tag == PAYLOAD_VEC4) {
        mem_copy(ev + 24, &s->v[0], 16);
    } else if (s->payload_tag == PAYLOAD_SCALAR) {
        mem_copy(ev + 24, &s->v[0], 4);
    }
    fmq_push_event(ev, sizeof(ev));
}

static void emit_flush_complete(int32_t handle) {
    uint8_t ev[128];
    mem_zero(ev, sizeof(ev));
    int64_t ts = boottime_ns();
    int32_t meta_type = 0; /* SensorType::META_DATA */
    int32_t what = 1;      /* MetaDataEventType::META_DATA_FLUSH_COMPLETE */
    mem_copy(ev + 0, &ts, 8);
    mem_copy(ev + 8, &handle, 4);
    mem_copy(ev + 12, &meta_type, 4);
    ev[16] = PAYLOAD_META;
    mem_copy(ev + 24, &what, 4);
    fmq_push_event(ev, sizeof(ev));
}

static void* poll_thread(void* arg) {
    (void)arg;
    for (;;) {
        int any_active = 0;
        int64_t now = boottime_ns();
        for (int i = 0; i < NUM_SENSORS; i++) {
            if (!__atomic_load_n(&g_state[i].active, __ATOMIC_ACQUIRE)) continue;
            if (kSensors[i].flags & 2 /* ON_CHANGE */) continue;
            any_active = 1;
            int64_t period = __atomic_load_n(&g_state[i].period_ns, __ATOMIC_RELAXED);
            if (period < 20000000LL) period = 20000000LL;       /* cap at 50 Hz */
            if (period > 1000000000LL) period = 1000000000LL;   /* floor at 1 Hz */
            if (now - g_state[i].last_emit_ns >= period) {
                g_state[i].last_emit_ns = now;
                emit_sensor_sample(i, now);
            }
        }
        sleep_ms(any_active ? 20 : 100);
    }
    return NULL;
}

static int find_sensor_by_handle(int32_t handle) {
    for (int i = 0; i < NUM_SENSORS; i++) {
        if (kSensors[i].handle == handle) return i;
    }
    return -1;
}

/* ---- AIDL V2 Parcel Writers ---- */

static void write_sensor_info(AParcel* out, const struct VirtualSensor* s) {
    g_writeInt32(out, 1); /* non-null parcelable */
    int32_t start_pos = g_getPos(out);
    g_writeInt32(out, 0); /* parcelable size placeholder */

    g_writeInt32(out, s->handle);
    g_writeString(out, s->name, (int32_t)cstrlen(s->name));
    g_writeString(out, s->vendor, (int32_t)cstrlen(s->vendor));
    g_writeInt32(out, 1); /* version */
    g_writeInt32(out, s->type);
    g_writeString(out, s->type_str, (int32_t)cstrlen(s->type_str));
    g_writeFloat(out, s->max_range);
    g_writeFloat(out, s->resolution);
    g_writeFloat(out, s->power);
    g_writeInt32(out, s->min_delay_us);
    g_writeInt32(out, 0); /* fifoReservedEventCount */
    g_writeInt32(out, 0); /* fifoMaxEventCount */
    g_writeString(out, "", 0); /* requiredPermission */
    g_writeInt32(out, s->max_delay_us);
    g_writeInt32(out, s->flags);

    int32_t end_pos = g_getPos(out);
    g_setPos(out, start_pos);
    g_writeInt32(out, end_pos - start_pos);
    g_setPos(out, end_pos);
}

static void write_service_error(AParcel* out, int32_t err) {
    if (g_statusFromSSE && g_writeStatusHeader && g_statusDelete) {
        AStatus* st = g_statusFromSSE(err);
        g_writeStatusHeader(out, st);
        g_statusDelete(st);
    } else {
        g_writeInt32(out, -8 /* EX_SERVICE_SPECIFIC */);
        g_writeInt32(out, -1 /* null message */);
        g_writeInt32(out, err);
    }
}

static void* on_create(void* args) { return args; }
static void on_destroy(void* userData) { (void)userData; }

static binder_status_t on_transact(AIBinder* binder, transaction_code_t code,
                                   const AParcel* in, AParcel* out) {
    (void)binder;
    switch (code) {
    case 1: { /* activate(int32_t sensorHandle, bool enabled) */
        int32_t handle = 0;
        bool enabled = false;
        g_readInt32(in, &handle);
        g_readBool(in, &enabled);
        int idx = find_sensor_by_handle(handle);
        if (idx < 0) {
            write_service_error(out, 2 /* STATUS_BAD_VALUE */);
            return 0;
        }
        __atomic_store_n(&g_state[idx].active, enabled ? 1 : 0, __ATOMIC_RELEASE);
        if (enabled) {
            int64_t now = boottime_ns();
            g_state[idx].last_emit_ns = now;
            emit_sensor_sample(idx, now);
        }
        g_writeInt32(out, 0 /* EX_NONE */);
        return 0;
    }
    case 2: { /* batch(int32_t handle, int64_t samplingPeriodNs, int64_t maxReportLatencyNs) */
        int32_t handle = 0;
        int64_t period_ns = 20000000LL;
        int64_t latency_ns = 0;
        g_readInt32(in, &handle);
        g_readInt64(in, &period_ns);
        g_readInt64(in, &latency_ns);
        (void)latency_ns;
        int idx = find_sensor_by_handle(handle);
        if (idx < 0) {
            write_service_error(out, 2 /* STATUS_BAD_VALUE */);
            return 0;
        }
        if (period_ns > 0) {
            __atomic_store_n(&g_state[idx].period_ns, period_ns, __ATOMIC_RELAXED);
        }
        g_writeInt32(out, 0 /* EX_NONE */);
        return 0;
    }
    case 3: /* configDirectReport -> unsupported */
    case 8: /* registerDirectChannel -> unsupported */
        write_service_error(out, 6 /* STATUS_INVALID_OPERATION */);
        return 0;
    case 4: { /* flush(int32_t handle) */
        int32_t handle = 0;
        g_readInt32(in, &handle);
        int idx = find_sensor_by_handle(handle);
        if (idx < 0) {
            write_service_error(out, 2 /* STATUS_BAD_VALUE */);
            return 0;
        }
        emit_flush_complete(handle);
        g_writeInt32(out, 0 /* EX_NONE */);
        return 0;
    }
    case 5: { /* getSensorsList() -> SensorInfo[] */
        g_writeInt32(out, 0 /* EX_NONE */);
        g_writeInt32(out, NUM_SENSORS);
        for (int i = 0; i < NUM_SENSORS; i++) {
            write_sensor_info(out, &kSensors[i]);
        }
        return 0;
    }
    case 6: { /* initialize(eventQueueDescriptor, wakeLockDescriptor, callback) */
        for (int i = 0; i < NUM_SENSORS; i++) {
            __atomic_store_n(&g_state[i].active, 0, __ATOMIC_RELEASE);
        }
        int rc = parse_mq_descriptor(in, 1);
        /* Drain wakeLockDescriptor so its transferred FD is closed cleanly. */
        (void)parse_mq_descriptor(in, 0);
        if (rc != 0) {
            write_service_error(out, 2 /* STATUS_BAD_VALUE */);
            return 0;
        }
        g_writeInt32(out, 0 /* EX_NONE */);
        return 0;
    }
    case 7: /* injectSensorData */
    case 9: /* setOperationMode */
    case 10: /* unregisterDirectChannel */
        g_writeInt32(out, 0 /* EX_NONE */);
        return 0;
    case 16777215u: /* getInterfaceVersion -> 2 */
        g_writeInt32(out, 0 /* EX_NONE */);
        g_writeInt32(out, 2);
        return 0;
    case 16777214u: { /* getInterfaceHash */
        static const char kHash[] = "8cdc9f2ea08fd57e6c60a600fea28a51a2ff12f9";
        g_writeInt32(out, 0 /* EX_NONE */);
        g_writeString(out, kHash, (int32_t)(sizeof(kHash) - 1));
        return 0;
    }
    default:
        write_service_error(out, 6 /* STATUS_INVALID_OPERATION */);
        return 0;
    }
}

__attribute__((used, visibility("default")))
void _start(void) {
    void* lib = dlopen("/system/lib64/libbinder_ndk.so", RTLD_NOW);
    if (!lib) {
        log_str("[svbox-sensors] dlopen libbinder_ndk.so failed\n");
        sys6(SYS_exit_group, 1, 0, 0, 0, 0, 0);
    }

    FnClassDefine classDefine = (FnClassDefine)dlsym(lib, "AIBinder_Class_define");
    FnBinderNew binderNew = (FnBinderNew)dlsym(lib, "AIBinder_new");
    FnMarkVintf markVintf = (FnMarkVintf)dlsym(lib, "AIBinder_markVintfStability");
    FnAddService addService = (FnAddService)dlsym(lib, "AServiceManager_addService");
    FnSetMaxThreads setMaxThreads = (FnSetMaxThreads)dlsym(lib, "ABinderProcess_setThreadPoolMaxThreadCount");
    FnStartPool startPool = (FnStartPool)dlsym(lib, "ABinderProcess_startThreadPool");
    FnJoinPool joinPool = (FnJoinPool)dlsym(lib, "ABinderProcess_joinThreadPool");

    g_getPos = (FnGetDataPos)dlsym(lib, "AParcel_getDataPosition");
    g_setPos = (FnSetDataPos)dlsym(lib, "AParcel_setDataPosition");
    g_readInt32 = (FnReadInt32)dlsym(lib, "AParcel_readInt32");
    g_readInt64 = (FnReadInt64)dlsym(lib, "AParcel_readInt64");
    g_readBool = (FnReadBool)dlsym(lib, "AParcel_readBool");
    g_readPfd = (FnReadPfd)dlsym(lib, "AParcel_readParcelFileDescriptor");
    g_writeInt32 = (FnWriteInt32)dlsym(lib, "AParcel_writeInt32");
    g_writeFloat = (FnWriteFloat)dlsym(lib, "AParcel_writeFloat");
    g_writeString = (FnWriteString)dlsym(lib, "AParcel_writeString");
    g_statusFromSSE = (FnStatusFromSSE)dlsym(lib, "AStatus_fromServiceSpecificError");
    g_writeStatusHeader = (FnWriteStatusHeader)dlsym(lib, "AParcel_writeStatusHeader");
    g_statusDelete = (FnStatusDelete)dlsym(lib, "AStatus_delete");

    if (!classDefine || !binderNew || !addService || !joinPool ||
        !g_getPos || !g_setPos || !g_readInt32 || !g_readInt64 || !g_readBool ||
        !g_readPfd || !g_writeInt32 || !g_writeFloat || !g_writeString) {
        log_str("[svbox-sensors] missing libbinder_ndk symbols\n");
        sys6(SYS_exit_group, 2, 0, 0, 0, 0, 0);
    }

    for (int i = 0; i < NUM_SENSORS; i++) {
        g_state[i].active = 0;
        g_state[i].period_ns = 50000000LL;
        g_state[i].last_emit_ns = 0;
    }

    if (setMaxThreads) setMaxThreads(4);
    if (startPool) startPool();

    unsigned long tid = 0;
    (void)pthread_create(&tid, NULL, poll_thread, NULL);

    AIBinder_Class* clazz = classDefine("android.hardware.sensors.ISensors",
                                        on_create, on_destroy, on_transact);
    AIBinder* svc = clazz ? binderNew(clazz, NULL) : NULL;
    if (!svc) {
        log_str("[svbox-sensors] AIBinder_new failed\n");
        sys6(SYS_exit_group, 3, 0, 0, 0, 0, 0);
    }
    if (markVintf) markVintf(svc);

    binder_status_t rc = addService(svc, "android.hardware.sensors.ISensors/default");
    if (rc == 0) {
        log_str("[svbox-sensors] registered android.hardware.sensors.ISensors/default (8 sensors)\n");
    } else {
        log_str("[svbox-sensors] addService failed rc=");
        log_i64(rc);
        log_str("\n");
        sys6(SYS_exit_group, 4, 0, 0, 0, 0, 0);
    }

    joinPool();
    sys6(SYS_exit_group, 0, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}
