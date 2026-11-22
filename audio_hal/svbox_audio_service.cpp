/* SPDX-License-Identifier: MIT */
/* 64-bit ARM64 Android 11 Audio HIDL service host (replaces Redroid's 32-bit
 * vendor/bin/hw/android.hardware.audio.service). Linked solely against Android
 * 11's libdl.so with PT_INTERP=/system/bin/linker64 so Bionic loads it cleanly
 * and registers stock 64-bit android.hardware.audio@6.0-impl.so +
 * audio.primary.svbox.so + android.hardware.audio.effect@6.0-impl.so. */

#include <stddef.h>
#include <stdint.h>

extern "C" void* dlopen(const char* filename, int flags);
extern "C" void* dlsym(void* handle, const char* symbol);
extern "C" const char* dlerror(void);

#define RTLD_NOW    0x0002
#define RTLD_GLOBAL 0x0100

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

static void log_str(const char* s) {
    if (!s) return;
    sys6(64 /* write */, 2, reinterpret_cast<long>(s), static_cast<long>(cstrlen(s)), 0, 0, 0);
}

static void log_int(int32_t v) {
    char buf[16];
    int pos = 0;
    if (v < 0) {
        buf[pos++] = '-';
        v = -v;
    }
    char rev[16];
    int r = 0;
    do { rev[r++] = static_cast<char>('0' + (v % 10)); v /= 10; } while (v);
    while (r > 0) buf[pos++] = rev[--r];
    buf[pos++] = '\n';
    sys6(64 /* write */, 2, reinterpret_cast<long>(buf), pos, 0, 0, 0);
}

// LLVM libc++ ABI v1 std::__1::basic_string<char> (64-bit LE)
// Long-string layout: w0 = (cap | 1), w1 = len, w2 = data_ptr
struct LibcxxString {
    uint64_t w0;
    uint64_t w1;
    uint64_t w2;
};

static LibcxxString make_str(const char* s, char* buf, size_t cap) {
    size_t len = cstrlen(s);
    if (len >= cap) len = cap - 1;
    for (size_t i = 0; i < len; ++i) buf[i] = s[i];
    buf[len] = '\0';
    LibcxxString out;
    out.w0 = cap | 1ull;
    out.w1 = len;
    out.w2 = reinterpret_cast<uint64_t>(buf);
    return out;
}

using ConfigureFn = void (*)(size_t, bool);
using RegisterFn = int32_t (*)(const LibcxxString&, const LibcxxString&, const LibcxxString&);
using JoinFn = void (*)();

extern "C" __attribute__((noreturn)) void _start() {
    void* lib = dlopen("libhidlbase.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        log_str("[svbox-audio-hal] dlopen(libhidlbase.so) failed: ");
        log_str(dlerror());
        log_str("\n");
        sys6(94 /* exit_group */, 1, 0, 0, 0, 0, 0);
    }
    auto configure = reinterpret_cast<ConfigureFn>(
        dlsym(lib, "_ZN7android8hardware22configureRpcThreadpoolEmb"));
    auto regImpl = reinterpret_cast<RegisterFn>(
        dlsym(lib, "_ZN7android8hardware40registerPassthroughServiceImplementationERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES9_S9_"));
    auto joinPool = reinterpret_cast<JoinFn>(
        dlsym(lib, "_ZN7android8hardware17joinRpcThreadpoolEv"));

    if (!configure || !regImpl || !joinPool) {
        log_str("[svbox-audio-hal] missing libhidlbase symbols\n");
        sys6(94 /* exit_group */, 2, 0, 0, 0, 0, 0);
    }

    configure(8, true);

    char b1[128], b2[128], b3[128], b4[128], b5[128];
    LibcxxString sAudioIf = make_str("android.hardware.audio@6.0::IDevicesFactory", b1, sizeof(b1));
    LibcxxString sAudioExp = make_str("android.hardware.audio@6.0::IDevicesFactory", b4, sizeof(b4));
    LibcxxString sEffectIf = make_str("android.hardware.audio.effect@6.0::IEffectsFactory", b2, sizeof(b2));
    LibcxxString sEffectExp = make_str("android.hardware.audio.effect@6.0::IEffectsFactory", b5, sizeof(b5));
    LibcxxString sDefault = make_str("default", b3, sizeof(b3));

    int32_t rc1 = regImpl(sAudioIf, sAudioExp, sDefault);
    log_str("[svbox-audio-hal] register IDevicesFactory@6.0 -> ");
    log_int(rc1);
    int32_t rc2 = regImpl(sEffectIf, sEffectExp, sDefault);
    log_str("[svbox-audio-hal] register IEffectsFactory@6.0 -> ");
    log_int(rc2);

    joinPool();
    sys6(94 /* exit_group */, 0, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}
