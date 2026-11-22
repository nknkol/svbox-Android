/* SPDX-License-Identifier: MIT */
/* svbox-propd: standalone Android 11 Bionic /dev/__properties__ generator and
 * /dev/socket/property_service daemon (pure MIT C, zero external deps).
 *
 * Loads Android *.prop files into a real Bionic-compatible binary property
 * trie (PROP_AREA_MAGIC / PROP_AREA_VERSION + property_info) and serves live
 * PROP_MSG_SETPROP2 / PROP_MSG_SETPROP requests from setprop / zygote /
 * system_server. */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define PROP_AREA_MAGIC   0x504f5250u
#define PROP_AREA_VERSION 0xfc6ed0abu
#define PROP_VALUE_MAX    92u
#define PROP_LONG_FLAG    (1u << 16)
#define PROP_AREA_SIZE    (512u * 1024u) /* 512 KiB comfortably fits all AOSP props */

#define PROP_MSG_SETPROP  1u
#define PROP_MSG_SETPROP2 0x00020001u

struct prop_area_hdr {
    uint32_t bytes_used;
    uint32_t serial;
    uint32_t magic;
    uint32_t version;
    uint32_t reserved[28];
};

struct prop_bt {
    uint32_t namelen;
    uint32_t prop;
    uint32_t left;
    uint32_t right;
    uint32_t children;
    char name[0];
};

struct prop_info {
    uint32_t serial;
    union {
        char value[PROP_VALUE_MAX];
        struct {
            char error_message[56];
            uint32_t offset;
        } long_property;
    };
    char name[0];
};

static uint8_t *g_serial_area = NULL;
static uint8_t *g_default_area = NULL;
static bool g_verbose = false;

static uint32_t align4(uint32_t n) { return (n + 3u) & ~3u; }

static uint8_t *area_data(uint8_t *area) { return area + sizeof(struct prop_area_hdr); }

static void *area_alloc(uint8_t *area, uint32_t size, uint32_t *out_off)
{
    struct prop_area_hdr *hdr = (struct prop_area_hdr *)area;
    uint32_t need = align4(size);
    uint32_t cap = PROP_AREA_SIZE - (uint32_t)sizeof(struct prop_area_hdr);
    if (hdr->bytes_used + need > cap)
        return NULL;
    uint32_t off = hdr->bytes_used;
    hdr->bytes_used += need;
    void *p = area_data(area) + off;
    memset(p, 0, need);
    *out_off = off;
    return p;
}

static struct prop_bt *bt_at(uint8_t *area, uint32_t off)
{
    return (struct prop_bt *)(area_data(area) + off);
}

static struct prop_info *pi_at(uint8_t *area, uint32_t off)
{
    return (struct prop_info *)(area_data(area) + off);
}

static int cmp_prop_name(const char *a, uint32_t alen, const char *b, uint32_t blen)
{
    if (alen < blen) return -1;
    if (alen > blen) return 1;
    return memcmp(a, b, alen);
}

static struct prop_bt *find_or_create_bt(uint8_t *area, uint32_t *slot,
                                         const char *seg, uint32_t seglen)
{
    uint32_t cur_off = *slot;
    if (cur_off == 0) {
        uint32_t new_off = 0;
        struct prop_bt *bt = (struct prop_bt *)area_alloc(area, sizeof(struct prop_bt) + seglen + 1, &new_off);
        if (!bt) return NULL;
        bt->namelen = seglen;
        memcpy(bt->name, seg, seglen);
        bt->name[seglen] = '\0';
        *slot = new_off;
        return bt;
    }
    struct prop_bt *cur = bt_at(area, cur_off);
    for (;;) {
        int c = cmp_prop_name(seg, seglen, cur->name, cur->namelen);
        if (c == 0)
            return cur;
        uint32_t *next_slot = (c < 0) ? &cur->left : &cur->right;
        if (*next_slot == 0) {
            uint32_t new_off = 0;
            struct prop_bt *bt = (struct prop_bt *)area_alloc(area, sizeof(struct prop_bt) + seglen + 1, &new_off);
            if (!bt) return NULL;
            bt->namelen = seglen;
            memcpy(bt->name, seg, seglen);
            bt->name[seglen] = '\0';
            *next_slot = new_off;
            return bt;
        }
        cur = bt_at(area, *next_slot);
    }
}

static int set_property(const char *name, const char *value)
{
    if (!name || !name[0] || !value)
        return -EINVAL;
    uint32_t namelen = (uint32_t)strlen(name);
    uint32_t valuelen = (uint32_t)strlen(value);

    /* Root node is always at offset 0 of area_data(). */
    struct prop_bt *cur = bt_at(g_default_area, 0);
    const char *rem = name;
    while (true) {
        const char *dot = strchr(rem, '.');
        uint32_t seglen = dot ? (uint32_t)(dot - rem) : (uint32_t)strlen(rem);
        if (seglen == 0)
            return -EINVAL;
        cur = find_or_create_bt(g_default_area, &cur->children, rem, seglen);
        if (!cur)
            return -ENOMEM;
        if (!dot)
            break;
        rem = dot + 1;
    }

    struct prop_info *pi = NULL;
    if (cur->prop != 0) {
        pi = pi_at(g_default_area, cur->prop);
    } else {
        uint32_t pi_off = 0;
        pi = (struct prop_info *)area_alloc(g_default_area, sizeof(struct prop_info) + namelen + 1, &pi_off);
        if (!pi)
            return -ENOMEM;
        memcpy(pi->name, name, namelen + 1);
        cur->prop = pi_off;
    }

    uint32_t old_serial = pi->serial;
    if (valuelen >= PROP_VALUE_MAX) {
        /* Long property (Android 8+): store string separately and record offset from pi. */
        uint32_t str_off = 0;
        char *dst = (char *)area_alloc(g_default_area, valuelen + 1, &str_off);
        if (!dst)
            return -ENOMEM;
        memcpy(dst, value, valuelen + 1);
        uint32_t pi_off = (uint32_t)((uint8_t *)pi - area_data(g_default_area));
        snprintf(pi->long_property.error_message, sizeof(pi->long_property.error_message),
                 "Must use __system_property_read_callback() to read");
        pi->long_property.offset = str_off - pi_off;
        pi->serial = PROP_LONG_FLAG | (((old_serial & 0xffffu) + 2u) & 0xffffu);
    } else {
        memcpy(pi->value, value, valuelen + 1);
        uint32_t cnt = ((old_serial & 0x00ffffffu) + 2u) & 0x00ffffffu;
        pi->serial = (valuelen << 24) | cnt;
    }

    ((struct prop_area_hdr *)g_default_area)->serial += 2u;
    ((struct prop_area_hdr *)g_serial_area)->serial += 2u;
    if (g_verbose)
        fprintf(stderr, "[propd] set %s=%s\n", name, value);
    return 0;
}

static uint8_t *create_prop_area_file(const char *path)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        perror(path);
        return NULL;
    }
    if (ftruncate(fd, PROP_AREA_SIZE) != 0) {
        perror("ftruncate");
        close(fd);
        return NULL;
    }
    void *p = mmap(NULL, PROP_AREA_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    memset(p, 0, PROP_AREA_SIZE);
    struct prop_area_hdr *hdr = (struct prop_area_hdr *)p;
    hdr->magic = PROP_AREA_MAGIC;
    hdr->version = PROP_AREA_VERSION;
    hdr->bytes_used = align4(sizeof(struct prop_bt) + 1); /* empty root node at offset 0 */
    return (uint8_t *)p;
}

/* Write minimal Android 11 PropertyInfoArea mapping every property to
 * context 0 ("u:object_r:default_prop:s0"). Matches Bionic property_info_parser.h. */
static int write_property_info_file(const char *path)
{
    /* Binary layout of PropertyInfoArea (see system/core/property_service/libpropertyinfoparser):
     * Header (24 bytes):
     *   u32 current_version = 1
     *   u32 minimum_supported_version = 1
     *   u32 size
     *   u32 contexts_offset
     *   u32 types_offset
     *   u32 root_offset
     * Contexts table at contexts_offset:
     *   u32 num_contexts = 1
     *   u32 ctx_str_offset[1]
     *   char ctx_str[] = "u:object_r:default_prop:s0\0"
     * Types table at types_offset:
     *   u32 num_types = 1
     *   u32 type_str_offset[1]
     *   char type_str[] = "string\0"
     * Root TrieNodeInternal at root_offset (36 bytes):
     *   u32 property_entry.name_offset
     *   u32 property_entry.namelen
     *   u32 property_entry.context_index = 0
     *   u32 property_entry.type_index = 0
     *   u32 num_child_nodes = 0
     *   u32 child_nodes_offset = 0
     *   u32 num_prefixes = 0
     *   u32 prefix_entries_offset = 0
     *   u32 num_exact_matches = 0
     *   u32 exact_match_entries_offset = 0
     */
    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));
    uint32_t pos = 24;

    uint32_t contexts_offset = pos;
    uint32_t num_ctx = 1;
    memcpy(buf + pos, &num_ctx, 4); pos += 4;
    uint32_t ctx_str_off = pos + 4;
    memcpy(buf + pos, &ctx_str_off, 4); pos += 4;
    const char *ctx_str = "u:object_r:default_prop:s0";
    memcpy(buf + pos, ctx_str, strlen(ctx_str) + 1);
    pos = align4(pos + (uint32_t)strlen(ctx_str) + 1);

    uint32_t types_offset = pos;
    uint32_t num_types = 1;
    memcpy(buf + pos, &num_types, 4); pos += 4;
    uint32_t type_str_off = pos + 4;
    memcpy(buf + pos, &type_str_off, 4); pos += 4;
    const char *type_str = "string";
    memcpy(buf + pos, type_str, strlen(type_str) + 1);
    pos = align4(pos + (uint32_t)strlen(type_str) + 1);

    /* Empty name string for root node. */
    uint32_t empty_str_off = pos;
    buf[pos] = '\0';
    pos = align4(pos + 1);

    uint32_t root_offset = pos;
    /* TrieNodeInternal: 10 x u32 = 40 bytes */
    uint32_t node[10] = {
        empty_str_off, /* property_entry.name_offset */
        0,             /* property_entry.namelen */
        0,             /* property_entry.context_index -> u:object_r:default_prop:s0 */
        0,             /* property_entry.type_index -> string */
        0, 0,          /* num_child_nodes, child_nodes_offset */
        0, 0,          /* num_prefixes, prefix_entries_offset */
        0, 0           /* num_exact_matches, exact_match_entries_offset */
    };
    memcpy(buf + pos, node, sizeof(node));
    pos += sizeof(node);

    uint32_t hdr[6] = { 1, 1, pos, contexts_offset, types_offset, root_offset };
    memcpy(buf, hdr, sizeof(hdr));

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        perror(path);
        return -1;
    }
    ssize_t wn = write(fd, buf, pos);
    close(fd);
    return wn == (ssize_t)pos ? 0 : -1;
}

static void load_prop_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[2048];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\0' || *s == '\n' || *s == '\r')
            continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;
        char *end = val + strlen(val);
        while (end > val && (end[-1] == '\n' || end[-1] == '\r'))
            *--end = '\0';
        char *kend = eq;
        while (kend > s && (kend[-1] == ' ' || kend[-1] == '\t'))
            *--kend = '\0';
        if (*s) {
            set_property(s, val);
            count++;
        }
    }
    fclose(f);
    fprintf(stderr, "[propd] loaded %d props from %s\n", count, path);
}

static int read_exact(int fd, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static void handle_prop_client(int cfd)
{
    uint32_t cmd = 0;
    if (read_exact(cfd, &cmd, 4) < 0) {
        close(cfd);
        return;
    }
    uint32_t resp = 0; /* PROP_SUCCESS */
    if (cmd == PROP_MSG_SETPROP2) {
        uint32_t nlen = 0, vlen = 0;
        if (read_exact(cfd, &nlen, 4) == 0 && nlen < 256) {
            char name[256] = {};
            if (read_exact(cfd, name, nlen) == 0 &&
                read_exact(cfd, &vlen, 4) == 0 && vlen < 1024) {
                char val[1024] = {};
                if (vlen == 0 || read_exact(cfd, val, vlen) == 0) {
                    name[nlen] = '\0';
                    val[vlen] = '\0';
                    if (set_property(name, val) != 0)
                        resp = 0x0804; /* error */
                }
            }
        }
        (void)write(cfd, &resp, 4);
    } else if (cmd == PROP_MSG_SETPROP) {
        char name[32] = {}, val[92] = {};
        if (read_exact(cfd, name, 32) == 0 && read_exact(cfd, val, 92) == 0) {
            name[31] = '\0';
            val[91] = '\0';
            set_property(name, val);
        }
    }
    close(cfd);
}

int main(int argc, char **argv)
{
    const char *prop_dir = NULL;
    const char *sock_path = NULL;
    const char *prop_files[32];
    int nfiles = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            g_verbose = true;
        } else if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            prop_dir = argv[++i];
        } else if (strcmp(argv[i], "--sock") == 0 && i + 1 < argc) {
            sock_path = argv[++i];
        } else if (nfiles < 32) {
            prop_files[nfiles++] = argv[i];
        }
    }
    if (!prop_dir) {
        fprintf(stderr, "Usage: %s [-v] --dir <__properties__-dir> [--sock <property_service.sock>] [file.prop ...]\n", argv[0]);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    mkdir(prop_dir, 0755);

    char path[512];
    snprintf(path, sizeof(path), "%s/properties_serial", prop_dir);
    g_serial_area = create_prop_area_file(path);
    snprintf(path, sizeof(path), "%s/u:object_r:default_prop:s0", prop_dir);
    g_default_area = create_prop_area_file(path);
    snprintf(path, sizeof(path), "%s/property_info", prop_dir);
    if (!g_serial_area || !g_default_area || write_property_info_file(path) != 0)
        return 1;

    for (int i = 0; i < nfiles; i++)
        load_prop_file(prop_files[i]);

    /* Essential container overrides for Android 11 on svbox */
    set_property("ro.property_service.version", "2");
    set_property("ro.zygote", "zygote64");
    set_property("ro.product.cpu.abi", "arm64-v8a");
    set_property("ro.product.cpu.abilist", "arm64-v8a");
    set_property("ro.product.cpu.abilist64", "arm64-v8a");
    set_property("ro.product.cpu.abilist32", "");
    set_property("ro.dalvik.vm.native.bridge", "0");
    set_property("ro.crypto.state", "unsupported");
    set_property("ro.boot.hardware", "redroid");
    set_property("ro.hardware", "redroid");
    set_property("ro.hardware.egl", "swiftshader");
    set_property("ro.debuggable", "1");
    set_property("ro.secure", "0");
    set_property("sys.boot_completed", "0");

    if (!sock_path)
        return 0;

    unlink(sock_path);
    int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd < 0) { perror("socket"); return 1; }
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, sock_path, sizeof(sa.sun_path) - 1);
    if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { perror("bind"); return 1; }
    if (listen(lfd, 32) < 0) { perror("listen"); return 1; }
    fprintf(stderr, "[propd] serving %s (dir=%s)\n", sock_path, prop_dir);

    for (;;) {
        int cfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        handle_prop_client(cfd);
    }
    return 0;
}
