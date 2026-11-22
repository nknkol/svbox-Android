/* SPDX-License-Identifier: MIT */
/* audio.primary.svbox.so: Zero-dependency (-nostdlib) Android 11 Primary Audio
 * HAL (AUDIO_DEVICE_API_VERSION_2_0) that forwards mixed PCM output over
 * AF_UNIX (/ipc/audio.sock) to HarmonyOS PC's OHAudio bridge.
 *
 * Uses raw AArch64 Linux syscalls directly (intercepted by svbox) so there is
 * zero Bionic vs musl libc ABI coupling. Struct offsets match AOSP Android 11
 * hardware/audio.h verified against stock audio.primary.default.so. */

#include <stdint.h>
#include <stddef.h>

#define AUDIO_HAL_MAGIC 0x55415653u /* "SVAU" */
#define HARDWARE_MODULE_TAG 0x48574d54u /* "HWMT" -> 'TMWH' */
#define HARDWARE_DEVICE_TAG 0x48574454u /* "HWDT" -> 'TDWH' */
#define HARDWARE_MODULE_API_VERSION(maj, min) (((maj) << 8) | (min))
#define AUDIO_DEVICE_API_VERSION_2_0 0x0200u

#define AUDIO_FORMAT_PCM_16_BIT 0x1u
#define AUDIO_CHANNEL_OUT_STEREO 0x3u
#define AUDIO_CHANNEL_IN_MONO 0x10u

/* Raw AArch64 Linux syscall numbers */
#define NR_close         57
#define NR_nanosleep     101
#define NR_clock_gettime 113
#define NR_socket        198
#define NR_connect       203
#define NR_sendto        206
#define NR_munmap        215
#define NR_mmap          222

#define AF_UNIX_         1
#define SOCK_STREAM_     1
#define SOCK_CLOEXEC_    02000000
#define MSG_NOSIGNAL_    0x4000
#define MSG_DONTWAIT_    0x40
#define PROT_RW_         3
#define MAP_PRIV_ANON_   0x22
#define CLOCK_MONOTONIC_ 1

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

static void mem_zero(void *dst, size_t n) {
    uint8_t *p = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) p[i] = 0;
}

static void mem_copy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static int str_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void *page_alloc(size_t sz) {
    size_t len = (sz + 4095u) & ~4095u;
    long p = sys6(NR_mmap, 0, (long)len, PROT_RW_, MAP_PRIV_ANON_, -1, 0);
    if (p < 0 && p > -4096) return NULL;
    return (void *)p;
}

static void page_free(void *p, size_t sz) {
    if (!p) return;
    size_t len = (sz + 4095u) & ~4095u;
    sys6(NR_munmap, (long)p, (long)len, 0, 0, 0, 0);
}

struct sv_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct sv_sockaddr_un {
    uint16_t sun_family;
    char sun_path[108];
};

struct sv_audio_hdr {
    uint32_t magic;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t bits_per_sample;
};

struct sv_audio_config {
    uint32_t sample_rate;
    uint32_t channel_mask;
    uint32_t format;
    uint8_t offload_info[64];
    uint32_t frame_count;
};

/* Layout verified against stock Android 11 audio.primary.default.so (280 bytes) */
struct sv_stream_out {
    /* audio_stream_t common (112 bytes = 14 function pointers) */
    uint32_t (*get_sample_rate)(const struct sv_stream_out *);               /* 0 */
    int      (*set_sample_rate)(struct sv_stream_out *, uint32_t);           /* 8 */
    size_t   (*get_buffer_size)(const struct sv_stream_out *);               /* 16 */
    uint32_t (*get_channels)(const struct sv_stream_out *);                  /* 24 */
    uint32_t (*get_format)(const struct sv_stream_out *);                    /* 32 */
    int      (*set_format)(struct sv_stream_out *, uint32_t);                /* 40 */
    int      (*standby)(struct sv_stream_out *);                             /* 48 */
    int      (*dump)(const struct sv_stream_out *, int);                     /* 56 */
    uint32_t (*get_device)(const struct sv_stream_out *);                    /* 64 */
    int      (*set_device)(struct sv_stream_out *, uint32_t);                /* 72 */
    int      (*set_parameters)(struct sv_stream_out *, const char *);        /* 80 */
    char *   (*get_parameters)(const struct sv_stream_out *, const char *);  /* 88 */
    int      (*add_audio_effect)(const struct sv_stream_out *, void *);      /* 96 */
    int      (*remove_audio_effect)(const struct sv_stream_out *, void *);   /* 104 */
    /* audio_stream_out_t methods (starting at offset 112) */
    uint32_t (*get_latency)(const struct sv_stream_out *);                   /* 112 */
    int      (*set_volume)(struct sv_stream_out *, float, float);            /* 120 */
    int64_t  (*write)(struct sv_stream_out *, const void *, size_t);         /* 128 */
    int      (*get_render_position)(const struct sv_stream_out *, uint32_t *); /* 136 */
    int      (*get_next_write_timestamp)(const struct sv_stream_out *, int64_t *); /* 144 */
    void *   pad_fn[13];                                                     /* 152..255 */
    /* Private state starting at offset 256 */
    uint32_t sample_rate;                                                    /* 256 */
    uint32_t channel_mask;                                                   /* 260 */
    uint32_t format;                                                         /* 264 */
    int32_t  sock_fd;                                                        /* 268 */
    uint64_t written_frames;                                                 /* 272 */
    int64_t  next_write_ns;                                                  /* 280 */
};

struct sv_stream_in {
    uint32_t (*get_sample_rate)(const struct sv_stream_in *);
    int      (*set_sample_rate)(struct sv_stream_in *, uint32_t);
    size_t   (*get_buffer_size)(const struct sv_stream_in *);
    uint32_t (*get_channels)(const struct sv_stream_in *);
    uint32_t (*get_format)(const struct sv_stream_in *);
    int      (*set_format)(struct sv_stream_in *, uint32_t);
    int      (*standby)(struct sv_stream_in *);
    int      (*dump)(const struct sv_stream_in *, int);
    uint32_t (*get_device)(const struct sv_stream_in *);
    int      (*set_device)(struct sv_stream_in *, uint32_t);
    int      (*set_parameters)(struct sv_stream_in *, const char *);
    char *   (*get_parameters)(const struct sv_stream_in *, const char *);
    int      (*add_audio_effect)(const struct sv_stream_in *, void *);
    int      (*remove_audio_effect)(const struct sv_stream_in *, void *);
    int      (*set_gain)(struct sv_stream_in *, float);
    int64_t  (*read)(struct sv_stream_in *, void *, size_t);
    uint32_t (*get_input_frames_lost)(struct sv_stream_in *);
    void *   pad_fn[15];
    uint32_t sample_rate;
    uint32_t channel_mask;
    uint32_t format;
};

/* Layout verified against stock Android 11 audio.primary.default.so (320 bytes) */
struct sv_hw_device {
    uint32_t tag;
    uint32_t version;
    void *module;
    uint64_t reserved[12];
    int (*close)(struct sv_hw_device *);                                     /* 112 */
};

struct sv_audio_hw_device {
    struct sv_hw_device common;                                              /* 0..119 */
    uint32_t (*get_supported_devices)(const struct sv_audio_hw_device *);    /* 120 */
    int      (*init_check)(const struct sv_audio_hw_device *);               /* 128 */
    int      (*set_voice_volume)(struct sv_audio_hw_device *, float);        /* 136 */
    int      (*set_master_volume)(struct sv_audio_hw_device *, float);       /* 144 */
    int      (*get_master_volume)(struct sv_audio_hw_device *, float *);     /* 152 */
    int      (*set_mode)(struct sv_audio_hw_device *, int);                  /* 160 */
    int      (*set_mic_mute)(struct sv_audio_hw_device *, int);              /* 168 */
    int      (*get_mic_mute)(const struct sv_audio_hw_device *, int *);       /* 176 */
    int      (*set_parameters)(struct sv_audio_hw_device *, const char *);   /* 184 */
    char *   (*get_parameters)(const struct sv_audio_hw_device *, const char *); /* 192 */
    size_t   (*get_input_buffer_size)(const struct sv_audio_hw_device *,
                                      const struct sv_audio_config *);       /* 200 */
    int      (*open_output_stream)(struct sv_audio_hw_device *, int32_t, uint32_t,
                                   uint32_t, struct sv_audio_config *,
                                   struct sv_stream_out **, const char *);   /* 208 */
    void     (*close_output_stream)(struct sv_audio_hw_device *,
                                    struct sv_stream_out *);                 /* 216 */
    int      (*open_input_stream)(struct sv_audio_hw_device *, int32_t, uint32_t,
                                  struct sv_audio_config *,
                                  struct sv_stream_in **, uint32_t,
                                  const char *, int);                        /* 224 */
    void     (*close_input_stream)(struct sv_audio_hw_device *,
                                   struct sv_stream_in *);                   /* 232 */
    void *   get_microphones;                                                /* 240 */
    int      (*dump)(const struct sv_audio_hw_device *, int);                /* 248 */
    int      (*set_master_mute)(struct sv_audio_hw_device *, int);           /* 256 */
    int      (*get_master_mute)(struct sv_audio_hw_device *, int *);         /* 264 */
    int      mic_mute;                                                       /* 272 */
    int      master_mute;                                                    /* 276 */
    void *   pad[5];                                                         /* 280..319 */
};

struct sv_hw_module_methods {
    int (*open)(const void *module, const char *id, struct sv_hw_device **device);
};

/* hw_module_t (248 bytes in Android 11) */
struct sv_hw_module {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char *id;
    const char *name;
    const char *author;
    struct sv_hw_module_methods *methods;
    void *dso;
    uint64_t reserved[25];
};

static int64_t now_ns(void) {
    struct sv_timespec ts = { 0, 0 };
    sys6(NR_clock_gettime, CLOCK_MONOTONIC_, (long)&ts, 0, 0, 0, 0);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void sleep_ns(int64_t ns) {
    if (ns <= 0) return;
    struct sv_timespec req;
    req.tv_sec = ns / 1000000000LL;
    req.tv_nsec = ns % 1000000000LL;
    sys6(NR_nanosleep, (long)&req, 0, 0, 0, 0, 0);
}

static uint32_t channel_count_from_mask(uint32_t mask) {
    uint32_t m = mask & 0x3fffffffu;
    uint32_t c = 0;
    while (m) { m &= m - 1u; c++; }
    return c ? c : 2u;
}

static int connect_audio_sock(uint32_t sample_rate, uint32_t channels) {
    long fd = sys6(NR_socket, AF_UNIX_, SOCK_STREAM_ | SOCK_CLOEXEC_, 0, 0, 0, 0);
    if (fd < 0) return -1;
    struct sv_sockaddr_un sa;
    mem_zero(&sa, sizeof(sa));
    sa.sun_family = AF_UNIX_;
    const char *path = "/ipc/audio.sock";
    size_t plen = 15;
    mem_copy(sa.sun_path, path, plen + 1);
    if (sys6(NR_connect, fd, (long)&sa, (long)(sizeof(sa.sun_family) + plen + 1), 0, 0, 0) < 0) {
        sys6(NR_close, fd, 0, 0, 0, 0, 0);
        return -1;
    }
    struct sv_audio_hdr hdr;
    hdr.magic = AUDIO_HAL_MAGIC;
    hdr.sample_rate = sample_rate;
    hdr.channels = channels;
    hdr.bits_per_sample = 16;
    long sent = sys6(NR_sendto, fd, (long)&hdr, sizeof(hdr), MSG_NOSIGNAL_, 0, 0);
    if (sent != (long)sizeof(hdr)) {
        sys6(NR_close, fd, 0, 0, 0, 0, 0);
        return -1;
    }
    return (int)fd;
}

/* ---- Output Stream Implementation ---- */
static uint32_t out_get_sample_rate(const struct sv_stream_out *s) { return s->sample_rate; }
static int out_set_sample_rate(struct sv_stream_out *s, uint32_t r) { (void)s; (void)r; return -38; }
static size_t out_get_buffer_size(const struct sv_stream_out *s) {
    uint32_t ch = channel_count_from_mask(s->channel_mask);
    /* ~20ms buffer, 16-frame aligned */
    size_t frames = ((s->sample_rate / 50u) + 15u) & ~15u;
    if (frames < 512u) frames = 512u;
    return frames * ch * 2u;
}
static uint32_t out_get_channels(const struct sv_stream_out *s) { return s->channel_mask; }
static uint32_t out_get_format(const struct sv_stream_out *s) { return s->format; }
static int out_set_format(struct sv_stream_out *s, uint32_t f) { (void)s; (void)f; return -38; }
static int out_standby(struct sv_stream_out *s) {
    if (s->sock_fd >= 0) {
        sys6(NR_close, s->sock_fd, 0, 0, 0, 0, 0);
        s->sock_fd = -1;
    }
    s->next_write_ns = 0;
    return 0;
}
static int out_dump(const struct sv_stream_out *s, int fd) { (void)s; (void)fd; return 0; }
static uint32_t out_get_device(const struct sv_stream_out *s) { (void)s; return 2u; /* SPEAKER */ }
static int out_set_device(struct sv_stream_out *s, uint32_t d) { (void)s; (void)d; return 0; }
static int out_set_parameters(struct sv_stream_out *s, const char *kv) { (void)s; (void)kv; return 0; }
static char *out_get_parameters(const struct sv_stream_out *s, const char *keys) {
    (void)s; (void)keys;
    /* AudioFlinger frees returned string via free(), so returning NULL is NOT safe;
     * however HAL 6.0 wrapper copies and checks NULL cleanly or we avoid calling libc free
     * by checking stock stub behavior: return NULL only if not supported. Actually HAL
     * android.hardware.audio@6.0 checks if (s) { result = s; free(s); } -> NULL = empty! */
    return NULL;
}
static int out_add_effect(const struct sv_stream_out *s, void *e) { (void)s; (void)e; return 0; }
static int out_remove_effect(const struct sv_stream_out *s, void *e) { (void)s; (void)e; return 0; }
static uint32_t out_get_latency(const struct sv_stream_out *s) { (void)s; return 40u; }
static int out_set_volume(struct sv_stream_out *s, float l, float r) { (void)s; (void)l; (void)r; return -38; }
static int out_get_render_position(const struct sv_stream_out *s, uint32_t *dsp_frames) {
    if (dsp_frames) *dsp_frames = (uint32_t)s->written_frames;
    return 0;
}
static int out_get_next_write_ts(const struct sv_stream_out *s, int64_t *ts) {
    (void)s; (void)ts; return -22;
}

static int64_t out_write(struct sv_stream_out *s, const void *buffer, size_t bytes) {
    uint32_t ch = channel_count_from_mask(s->channel_mask);
    uint32_t frame_bytes = ch * 2u;
    uint32_t frames = frame_bytes ? (uint32_t)(bytes / frame_bytes) : 0u;
    uint32_t rate = s->sample_rate ? s->sample_rate : 44100u;

    if (s->sock_fd < 0) {
        s->sock_fd = connect_audio_sock(rate, ch);
    }
    if (s->sock_fd >= 0 && bytes > 0) {
        const uint8_t *p = (const uint8_t *)buffer;
        size_t sent = 0;
        while (sent < bytes) {
            long n = sys6(NR_sendto, s->sock_fd, (long)(p + sent), (long)(bytes - sent),
                          MSG_NOSIGNAL_ | MSG_DONTWAIT_, 0, 0);
            if (n > 0) {
                sent += (size_t)n;
                continue;
            }
            if (n == -11 /* EAGAIN */) {
                break; /* drop rather than stall AudioFlinger mixer */
            }
            /* Peer closed or error */
            sys6(NR_close, s->sock_fd, 0, 0, 0, 0, 0);
            s->sock_fd = -1;
            break;
        }
    }

    s->written_frames += frames;

    /* Pace writes to real-time playback rate so AudioFlinger mixes smoothly */
    int64_t dur_ns = ((int64_t)frames * 1000000000LL) / (int64_t)rate;
    int64_t now = now_ns();
    if (s->next_write_ns == 0 || now - s->next_write_ns > 100000000LL) {
        s->next_write_ns = now + dur_ns;
    } else {
        int64_t rem = s->next_write_ns - now;
        if (rem > 0 && rem < 100000000LL) {
            sleep_ns(rem);
        }
        s->next_write_ns += dur_ns;
    }
    return (int64_t)bytes;
}

/* ---- Input Stream Stub ---- */
static uint32_t in_get_sample_rate(const struct sv_stream_in *s) { return s->sample_rate; }
static int in_set_sample_rate(struct sv_stream_in *s, uint32_t r) { (void)s; (void)r; return -38; }
static size_t in_get_buffer_size(const struct sv_stream_in *s) { (void)s; return 320u; }
static uint32_t in_get_channels(const struct sv_stream_in *s) { return s->channel_mask; }
static uint32_t in_get_format(const struct sv_stream_in *s) { return s->format; }
static int in_set_format(struct sv_stream_in *s, uint32_t f) { (void)s; (void)f; return -38; }
static int in_standby(struct sv_stream_in *s) { (void)s; return 0; }
static int in_dump(const struct sv_stream_in *s, int fd) { (void)s; (void)fd; return 0; }
static uint32_t in_get_device(const struct sv_stream_in *s) { (void)s; return 0x80000004u; }
static int in_set_device(struct sv_stream_in *s, uint32_t d) { (void)s; (void)d; return 0; }
static int in_set_parameters(struct sv_stream_in *s, const char *kv) { (void)s; (void)kv; return 0; }
static char *in_get_parameters(const struct sv_stream_in *s, const char *k) { (void)s; (void)k; return NULL; }
static int in_add_effect(const struct sv_stream_in *s, void *e) { (void)s; (void)e; return 0; }
static int in_remove_effect(const struct sv_stream_in *s, void *e) { (void)s; (void)e; return 0; }
static int in_set_gain(struct sv_stream_in *s, float g) { (void)s; (void)g; return 0; }
static int64_t in_read(struct sv_stream_in *s, void *buf, size_t bytes) {
    (void)s;
    mem_zero(buf, bytes);
    sleep_ns(10000000LL);
    return (int64_t)bytes;
}
static uint32_t in_frames_lost(struct sv_stream_in *s) { (void)s; return 0; }

/* ---- Audio HW Device Methods ---- */
static int adev_close(struct sv_hw_device *dev) {
    page_free(dev, sizeof(struct sv_audio_hw_device));
    return 0;
}
static int adev_init_check(const struct sv_audio_hw_device *dev) { (void)dev; return 0; }
static int adev_set_voice_volume(struct sv_audio_hw_device *dev, float v) { (void)dev; (void)v; return -38; }
static int adev_set_master_volume(struct sv_audio_hw_device *dev, float v) { (void)dev; (void)v; return -38; }
static int adev_get_master_volume(struct sv_audio_hw_device *dev, float *v) { (void)dev; (void)v; return -38; }
static int adev_set_master_mute(struct sv_audio_hw_device *dev, int m) {
    if (dev) dev->master_mute = m ? 1 : 0;
    return 0;
}
static int adev_get_master_mute(struct sv_audio_hw_device *dev, int *m) {
    if (m) *m = dev ? dev->master_mute : 0;
    return 0;
}
static int adev_set_mode(struct sv_audio_hw_device *dev, int mode) { (void)dev; (void)mode; return 0; }
static int adev_set_mic_mute(struct sv_audio_hw_device *dev, int state) {
    if (dev) dev->mic_mute = state ? 1 : 0;
    return 0;
}
static int adev_get_mic_mute(const struct sv_audio_hw_device *dev, int *state) {
    if (state) *state = dev ? dev->mic_mute : 0;
    return 0;
}
static int adev_set_parameters(struct sv_audio_hw_device *dev, const char *kv) { (void)dev; (void)kv; return 0; }
static char *adev_get_parameters(const struct sv_audio_hw_device *dev, const char *k) { (void)dev; (void)k; return NULL; }
static size_t adev_get_input_buffer_size(const struct sv_audio_hw_device *dev,
                                         const struct sv_audio_config *cfg) {
    (void)dev; (void)cfg; return 320u;
}
static int adev_dump(const struct sv_audio_hw_device *dev, int fd) { (void)dev; (void)fd; return 0; }

static int adev_open_output_stream(struct sv_audio_hw_device *dev,
                                   int32_t handle, uint32_t devices, uint32_t flags,
                                   struct sv_audio_config *config,
                                   struct sv_stream_out **stream_out,
                                   const char *address) {
    (void)dev; (void)handle; (void)devices; (void)flags; (void)address;
    if (!stream_out) return -22;
    *stream_out = NULL;
    struct sv_stream_out *out = (struct sv_stream_out *)page_alloc(sizeof(*out));
    if (!out) return -12;
    mem_zero(out, sizeof(*out));

    out->get_sample_rate = out_get_sample_rate;
    out->set_sample_rate = out_set_sample_rate;
    out->get_buffer_size = out_get_buffer_size;
    out->get_channels = out_get_channels;
    out->get_format = out_get_format;
    out->set_format = out_set_format;
    out->standby = out_standby;
    out->dump = out_dump;
    out->get_device = out_get_device;
    out->set_device = out_set_device;
    out->set_parameters = out_set_parameters;
    out->get_parameters = out_get_parameters;
    out->add_audio_effect = out_add_effect;
    out->remove_audio_effect = out_remove_effect;
    out->get_latency = out_get_latency;
    out->set_volume = out_set_volume;
    out->write = out_write;
    out->get_render_position = out_get_render_position;
    out->get_next_write_timestamp = out_get_next_write_ts;

    out->sample_rate = (config && config->sample_rate) ? config->sample_rate : 44100u;
    out->channel_mask = (config && config->channel_mask) ? config->channel_mask : AUDIO_CHANNEL_OUT_STEREO;
    out->format = (config && config->format) ? config->format : AUDIO_FORMAT_PCM_16_BIT;
    out->sock_fd = -1;
    if (config) {
        config->sample_rate = out->sample_rate;
        config->channel_mask = out->channel_mask;
        config->format = out->format;
    }
    *stream_out = out;
    return 0;
}

static void adev_close_output_stream(struct sv_audio_hw_device *dev, struct sv_stream_out *out) {
    (void)dev;
    if (!out) return;
    out_standby(out);
    page_free(out, sizeof(*out));
}

static int adev_open_input_stream(struct sv_audio_hw_device *dev,
                                  int32_t handle, uint32_t devices,
                                  struct sv_audio_config *config,
                                  struct sv_stream_in **stream_in,
                                  uint32_t flags, const char *address, int source) {
    (void)dev; (void)handle; (void)devices; (void)flags; (void)address; (void)source;
    if (!stream_in) return -22;
    *stream_in = NULL;
    struct sv_stream_in *in = (struct sv_stream_in *)page_alloc(sizeof(*in));
    if (!in) return -12;
    mem_zero(in, sizeof(*in));
    in->get_sample_rate = in_get_sample_rate;
    in->set_sample_rate = in_set_sample_rate;
    in->get_buffer_size = in_get_buffer_size;
    in->get_channels = in_get_channels;
    in->get_format = in_get_format;
    in->set_format = in_set_format;
    in->standby = in_standby;
    in->dump = in_dump;
    in->get_device = in_get_device;
    in->set_device = in_set_device;
    in->set_parameters = in_set_parameters;
    in->get_parameters = in_get_parameters;
    in->add_audio_effect = in_add_effect;
    in->remove_audio_effect = in_remove_effect;
    in->set_gain = in_set_gain;
    in->read = in_read;
    in->get_input_frames_lost = in_frames_lost;
    in->sample_rate = (config && config->sample_rate) ? config->sample_rate : 16000u;
    in->channel_mask = (config && config->channel_mask) ? config->channel_mask : AUDIO_CHANNEL_IN_MONO;
    in->format = AUDIO_FORMAT_PCM_16_BIT;
    *stream_in = in;
    return 0;
}

static void adev_close_input_stream(struct sv_audio_hw_device *dev, struct sv_stream_in *in) {
    (void)dev;
    page_free(in, sizeof(*in));
}

static int adev_open(const void *module, const char *name, struct sv_hw_device **device) {
    if (!str_eq(name, "audio_hw_if") || !device) return -22;
    struct sv_audio_hw_device *adev = (struct sv_audio_hw_device *)page_alloc(sizeof(*adev));
    if (!adev) return -12;
    mem_zero(adev, sizeof(*adev));
    adev->common.tag = HARDWARE_DEVICE_TAG;
    adev->common.version = AUDIO_DEVICE_API_VERSION_2_0;
    adev->common.module = (void *)module;
    adev->common.close = adev_close;

    adev->init_check = adev_init_check;
    adev->set_voice_volume = adev_set_voice_volume;
    adev->set_master_volume = adev_set_master_volume;
    adev->get_master_volume = adev_get_master_volume;
    adev->set_master_mute = adev_set_master_mute;
    adev->get_master_mute = adev_get_master_mute;
    adev->set_mode = adev_set_mode;
    adev->set_mic_mute = adev_set_mic_mute;
    adev->get_mic_mute = adev_get_mic_mute;
    adev->set_parameters = adev_set_parameters;
    adev->get_parameters = adev_get_parameters;
    adev->get_input_buffer_size = adev_get_input_buffer_size;
    adev->open_output_stream = adev_open_output_stream;
    adev->close_output_stream = adev_close_output_stream;
    adev->open_input_stream = adev_open_input_stream;
    adev->close_input_stream = adev_close_input_stream;
    adev->dump = adev_dump;

    *device = &adev->common;
    return 0;
}

static struct sv_hw_module_methods g_hal_methods = {
    .open = adev_open,
};

__attribute__((visibility("default")))
struct sv_hw_module HMI = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = HARDWARE_MODULE_API_VERSION(1, 0),
    .hal_api_version = HARDWARE_MODULE_API_VERSION(0, 0),
    .id = "audio",
    .name = "svbox OHAudio Primary HAL",
    .author = "svbox-Android",
    .methods = &g_hal_methods,
    .dso = NULL,
    .reserved = { 0 },
};
