/* SPDX-License-Identifier: MIT */
/* svbox-netd: Lightweight unprivileged Android 11 INetd + IDnsResolver Binder
 * stub daemon for svbox-Android.
 *
 * Host HarmonyOS PC already provides full TCP/IP & DNS networking natively;
 * unprivileged containers cannot invoke CAP_NET_ADMIN / iptables / netlink
 * RouteController. This daemon connects directly to svbox-binderd's binder.sock,
 * registers "netd" (android.net.INetd) and "dnsresolver" (android.net.IDnsResolver)
 * with servicemanager, and answers system_server's AIDL calls cleanly so boot
 * proceeds past NetworkManagementService to Launcher3 / SystemUI. */

#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "vdev_proto.h"

#define FLAT_BINDER_FLAG_ACCEPTS_FDS 0x100u

#define PING_TRANSACTION        0x5f504e47u
#define INTERFACE_TRANSACTION   0x5f4e5446u
#define GET_INTERFACE_VERSION   0x00ffffffu
#define GET_INTERFACE_HASH      0x00fffffeu

#define NODE_NETD               0x1001ull
#define NODE_DNSRESOLVER        0x1002ull
#define NODE_OEMNETD            0x1003ull
#define NODE_AUDIO_POLICY       0x1004ull
#define NODE_AUDIO_FLINGER      0x1005ull

static int g_ctrl_sock = -1;
static int g_rpc_sock = -1;
static uint32_t g_proc_id = 0;
static uint8_t *g_mmap_base = NULL;
static size_t g_mmap_len = 0;

static int send_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    while (n > 0) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int recv_msg_fds(int fd, void *buf, size_t n, int *out_fds, int max_fds, int *got_fds) {
    if (got_fds) *got_fds = 0;
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < n) {
        struct iovec iov = { p + got, n - got };
        union {
            struct cmsghdr align;
            char data[CMSG_SPACE(sizeof(int) * VDEV_MAX_FDS)];
        } cbuf;
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cbuf.data;
        msg.msg_controllen = sizeof(cbuf.data);
        ssize_t r = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        if (out_fds && got_fds) {
            for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                    size_t nbytes = c->cmsg_len - CMSG_LEN(0);
                    int cnt = (int)(nbytes / sizeof(int));
                    for (int i = 0; i < cnt; i++) {
                        int rfd = -1;
                        memcpy(&rfd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
                        if (*got_fds < max_fds) out_fds[(*got_fds)++] = rfd;
                        else close(rfd);
                    }
                }
            }
        }
        got += (size_t)r;
    }
    return 0;
}

static int connect_unix(const char *sock_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, sock_path, sizeof(sa.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int vdev_init(const char *sock_path) {
    int cfd = connect_unix(sock_path);
    if (cfd < 0) return -1;

    struct vdev_open_req oreq;
    memset(&oreq, 0, sizeof(oreq));
    oreq.pid = (uint32_t)getpid();
    oreq.tid = (uint32_t)getpid();
    oreq.uid = 0;
    oreq.gid = 0;
    oreq.vdev_type = VDEV_TYPE_BINDER;
    oreq.flags = 2; /* O_RDWR */
    strncpy(oreq.guest_path, "/dev/binder", sizeof(oreq.guest_path) - 1);

    struct vdev_hdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = VDEV_PROTO_MAGIC;
    hdr.op = VDEV_OP_OPEN;
    hdr.payload_len = sizeof(oreq);
    if (send_all(cfd, &hdr, sizeof(hdr)) < 0 || send_all(cfd, &oreq, sizeof(oreq)) < 0) {
        close(cfd);
        return -1;
    }

    struct vdev_hdr rsp;
    int fds[4];
    int nfds = 0;
    if (recv_msg_fds(cfd, &rsp, sizeof(rsp), fds, 4, &nfds) < 0 || rsp.status < 0) {
        for (int i = 0; i < nfds; i++) close(fds[i]);
        close(cfd);
        return -1;
    }
    struct vdev_open_rep orep;
    memset(&orep, 0, sizeof(orep));
    if (rsp.payload_len >= sizeof(orep)) {
        if (recv_msg_fds(cfd, &orep, sizeof(orep), NULL, 0, NULL) < 0) {
            for (int i = 0; i < nfds; i++) close(fds[i]);
            close(cfd);
            return -1;
        }
    }
    /* fds[0] = notify_fd (if present), fds[1] = memfd (or fds[0] if nfds==1) */
    int memfd = -1;
    if (nfds >= 2) {
        close(fds[0]);
        memfd = fds[1];
    } else if (nfds == 1) {
        memfd = fds[0];
    }
    g_proc_id = orep.proc_id;
    size_t len = orep.memfd_size ? (size_t)orep.memfd_size : (1024u * 1024u);

    if (memfd < 0) {
        close(cfd);
        return -1;
    }
    void *mapped = mmap(NULL, len, PROT_READ, MAP_SHARED, memfd, 0);
    close(memfd);
    if (mapped == MAP_FAILED) {
        close(cfd);
        return -1;
    }
    g_mmap_base = (uint8_t *)mapped;
    g_mmap_len = len;

    /* Open dedicated RPC thread connection via VDEV_OP_ATTACH_THREAD first
     * (cfd became CONN_NOTIFY after VDEV_OP_OPEN). */
    int rfd = connect_unix(sock_path);
    if (rfd < 0) {
        close(cfd);
        return -1;
    }
    struct vdev_attach_req areq = {
        .proc_id = g_proc_id,
        .pid = (uint32_t)getpid(),
        .tid = (uint32_t)getpid(),
        .reserved = 0,
    };
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = VDEV_PROTO_MAGIC;
    hdr.op = VDEV_OP_ATTACH_THREAD;
    hdr.proc_id = g_proc_id;
    hdr.payload_len = sizeof(areq);
    if (send_all(rfd, &hdr, sizeof(hdr)) < 0 || send_all(rfd, &areq, sizeof(areq)) < 0) {
        close(rfd);
        close(cfd);
        return -1;
    }
    if (recv_msg_fds(rfd, &rsp, sizeof(rsp), NULL, 0, NULL) < 0 || rsp.status < 0) {
        close(rfd);
        close(cfd);
        return -1;
    }

    /* Inform binderd of our guest_base via VDEV_OP_MMAP on RPC socket */
    struct vdev_mmap_req mreq = {
        .guest_base = (uint64_t)(uintptr_t)mapped,
        .length = (uint64_t)len,
    };
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = VDEV_PROTO_MAGIC;
    hdr.op = VDEV_OP_MMAP;
    hdr.proc_id = g_proc_id;
    hdr.payload_len = sizeof(mreq);
    if (send_all(rfd, &hdr, sizeof(hdr)) < 0 || send_all(rfd, &mreq, sizeof(mreq)) < 0) {
        close(rfd);
        close(cfd);
        return -1;
    }
    if (recv_msg_fds(rfd, &rsp, sizeof(rsp), NULL, 0, NULL) < 0 || rsp.status < 0) {
        close(rfd);
        close(cfd);
        return -1;
    }
    g_ctrl_sock = cfd;
    g_rpc_sock = rfd;
    return 0;
}

static int binder_wr(const uint8_t *packed, size_t packed_len, size_t raw_write_size,
                     uint8_t *rbuf, size_t rcap, size_t *rconsumed) {
    struct vdev_binder_wr_req wreq = {
        .tid = (uint32_t)getpid(),
        .uid = 0,
        .pid = (uint32_t)getpid(),
        .read_cap = (uint32_t)rcap,
        .packed_len = (uint32_t)packed_len,
        .raw_write_size = (uint32_t)raw_write_size,
    };
    struct vdev_hdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = VDEV_PROTO_MAGIC;
    hdr.op = VDEV_OP_BINDER_WR;
    hdr.proc_id = g_proc_id;
    hdr.payload_len = (uint32_t)(sizeof(wreq) + packed_len);
    if (send_all(g_rpc_sock, &hdr, sizeof(hdr)) < 0 ||
        send_all(g_rpc_sock, &wreq, sizeof(wreq)) < 0)
        return -1;
    if (packed_len > 0 && send_all(g_rpc_sock, packed, packed_len) < 0)
        return -1;

    struct vdev_hdr rsp;
    int fds[VDEV_MAX_FDS];
    int nfds = 0;
    if (recv_msg_fds(g_rpc_sock, &rsp, sizeof(rsp), fds, VDEV_MAX_FDS, &nfds) < 0) {
        for (int i = 0; i < nfds; i++) close(fds[i]);
        return -1;
    }
    for (int i = 0; i < nfds; i++) close(fds[i]);
    if (rsp.status < 0 || rsp.payload_len < sizeof(struct vdev_binder_wr_rep))
        return -1;

    struct vdev_binder_wr_rep wrep;
    if (recv_msg_fds(g_rpc_sock, &wrep, sizeof(wrep), NULL, 0, NULL) < 0)
        return -1;

    size_t skip_patches = (size_t)wrep.fd_patch_count * sizeof(uint64_t);
    if (skip_patches > 0) {
        uint8_t sink[256];
        size_t rem = skip_patches;
        while (rem > 0) {
            size_t ch = rem > sizeof(sink) ? sizeof(sink) : rem;
            if (recv_msg_fds(g_rpc_sock, sink, ch, NULL, 0, NULL) < 0) return -1;
            rem -= ch;
        }
    }
    size_t to_read = wrep.read_bytes <= rcap ? (size_t)wrep.read_bytes : rcap;
    if (to_read > 0 && recv_msg_fds(g_rpc_sock, rbuf, to_read, NULL, 0, NULL) < 0)
        return -1;
    if (wrep.read_bytes > to_read) {
        uint8_t sink[256];
        size_t rem = wrep.read_bytes - to_read;
        while (rem > 0) {
            size_t ch = rem > sizeof(sink) ? sizeof(sink) : rem;
            if (recv_msg_fds(g_rpc_sock, sink, ch, NULL, 0, NULL) < 0) return -1;
            rem -= ch;
        }
    }
    if (rconsumed) *rconsumed = to_read;
    return 0;
}

static void free_buffer(uint64_t ptr) {
    if (!ptr) return;
    uint8_t buf[12];
    uint32_t cmd = SV_BC_FREE_BUFFER;
    memcpy(buf, &cmd, 4);
    memcpy(buf + 4, &ptr, 8);
    binder_wr(buf, sizeof(buf), sizeof(buf), NULL, 0, NULL);
}

/* Parcel writer helpers */
struct parcel {
    uint8_t buf[1024];
    size_t len;
    uint64_t offs[8];
    size_t noffs;
};

static void p_i32(struct parcel *p, int32_t v) {
    memcpy(p->buf + p->len, &v, 4);
    p->len += 4;
}

static void p_u32(struct parcel *p, uint32_t v) {
    memcpy(p->buf + p->len, &v, 4);
    p->len += 4;
}

static void p_str16(struct parcel *p, const char *s) {
    int32_t n = (int32_t)strlen(s);
    p_i32(p, n);
    for (int32_t i = 0; i <= n; i++) {
        uint16_t ch = (uint16_t)(unsigned char)s[i];
        memcpy(p->buf + p->len, &ch, 2);
        p->len += 2;
    }
    while (p->len & 3) {
        p->buf[p->len++] = 0;
    }
}

static void p_iface_token(struct parcel *p, const char *iface) {
    p_u32(p, 0x80000000u);
    p_i32(p, -1);
    p_u32(p, 0x53595354u); /* 'SYST' */
    p_str16(p, iface);
}

static void p_binder(struct parcel *p, uint64_t node_ptr, uint64_t cookie) {
    p->offs[p->noffs++] = p->len;
    struct sv_flat_binder_object obj;
    memset(&obj, 0, sizeof(obj));
    obj.type = SV_BINDER_TYPE_BINDER;
    obj.flags = 0x7fu | FLAT_BINDER_FLAG_ACCEPTS_FDS;
    obj.binder = node_ptr;
    obj.cookie = cookie;
    memcpy(p->buf + p->len, &obj, sizeof(obj));
    p->len += sizeof(obj);
    p_i32(p, 0x0c); /* Android 11 Stability::SYSTEM */
}

static int add_service(const char *name, uint64_t node_ptr) {
    struct parcel p;
    memset(&p, 0, sizeof(p));
    p_iface_token(&p, "android.os.IServiceManager");
    p_str16(&p, name);
    p_binder(&p, node_ptr, node_ptr);
    p_i32(&p, 0); /* allowIsolated = false */
    p_i32(&p, 8); /* DUMP_FLAG_PRIORITY_DEFAULT */

    size_t offs_bytes = p.noffs * sizeof(uint64_t);
    uint8_t packed[4 + sizeof(struct sv_binder_transaction_data) + 1024 + 64];
    size_t plen = 0;
    uint32_t cmd = SV_BC_TRANSACTION;
    struct sv_binder_transaction_data tr;
    memset(&tr, 0, sizeof(tr));
    tr.target.handle = 0;
    tr.code = 3; /* ADD_SERVICE_TRANSACTION */
    tr.flags = SV_TF_ACCEPT_FDS;
    tr.data_size = p.len;
    tr.offsets_size = offs_bytes;

    memcpy(packed + plen, &cmd, 4); plen += 4;
    memcpy(packed + plen, &tr, sizeof(tr)); plen += sizeof(tr);
    memcpy(packed + plen, p.buf, p.len); plen += p.len;
    if (offs_bytes > 0) {
        memcpy(packed + plen, p.offs, offs_bytes);
        plen += offs_bytes;
    }

    uint8_t rbuf[512];
    size_t rcons = 0;
    if (binder_wr(packed, plen, 4 + sizeof(tr), rbuf, sizeof(rbuf), &rcons) < 0)
        return -1;

    for (int tries = 0; tries < 8; tries++) {
        size_t pos = 0;
        int got_reply = 0;
        while (pos + 4 <= rcons) {
            uint32_t bcmd = 0;
            memcpy(&bcmd, rbuf + pos, 4);
            pos += 4;
            if (bcmd == SV_BR_NOOP || bcmd == SV_BR_TRANSACTION_COMPLETE ||
                bcmd == SV_BR_SPAWN_LOOPER) {
                continue;
            } else if (bcmd == SV_BR_INCREFS || bcmd == SV_BR_ACQUIRE ||
                       bcmd == SV_BR_RELEASE || bcmd == SV_BR_DECREFS) {
                pos += 16;
            } else if (bcmd == SV_BR_REPLY) {
                struct sv_binder_transaction_data rtr;
                memcpy(&rtr, rbuf + pos, sizeof(rtr));
                pos += sizeof(rtr);
                got_reply = 1;
                free_buffer(rtr.data.ptr.buffer);
            } else {
                break;
            }
        }
        if (got_reply) return 0;
        if (binder_wr(NULL, 0, 0, rbuf, sizeof(rbuf), &rcons) < 0)
            return -1;
    }
    return 0;
}

static void handle_transaction(const struct sv_binder_transaction_data *tr) {
    struct parcel reply;
    memset(&reply, 0, sizeof(reply));

    const uint64_t node = tr->target.ptr;
    const uint32_t code = tr->code;

    if (code == PING_TRANSACTION) {
        /* empty */
    } else if (code == INTERFACE_TRANSACTION) {
        if (node == NODE_DNSRESOLVER) p_str16(&reply, "android.net.IDnsResolver");
        else if (node == NODE_OEMNETD) p_str16(&reply, "com.android.internal.net.IOemNetd");
        else if (node == NODE_AUDIO_POLICY) p_str16(&reply, "android.media.IAudioPolicyService");
        else if (node == NODE_AUDIO_FLINGER) p_str16(&reply, "android.media.IAudioFlinger");
        else p_str16(&reply, "android.net.INetd");
    } else if (code == GET_INTERFACE_VERSION) {
        p_i32(&reply, 0);
        p_i32(&reply, node == NODE_DNSRESOLVER ? 6 : (node == NODE_OEMNETD ? 1 : 4));
    } else if (code == GET_INTERFACE_HASH) {
        p_i32(&reply, 0);
        p_str16(&reply, "");
    } else if (node == NODE_AUDIO_POLICY || node == NODE_AUDIO_FLINGER) {
        fprintf(stderr, "[svbox-netd] %s code=%u flags=0x%x data=%llu\n",
                node == NODE_AUDIO_POLICY ? "AudioPolicy" : "AudioFlinger",
                code, tr->flags, (unsigned long long)tr->data_size);
        fflush(stderr);
        /* Return NO_ERROR (0) + safe zeros */
        p_i32(&reply, 0);
        p_i32(&reply, 0);
        p_i32(&reply, 0);
        p_i32(&reply, 0);
    } else if (node == NODE_NETD) {
        fprintf(stderr, "[svbox-netd] INetd code=%u flags=0x%x data=%llu\n",
                code, tr->flags, (unsigned long long)tr->data_size);
        fflush(stderr);
        if (code == 1 /* isAlive */ || code == 2 /* firewallReplaceUidChain */ ||
            code == 3 /* bandwidthEnableDataSaver */ || code == 12 /* tetherApplyDnsInterfaces */) {
            p_i32(&reply, 0);
            p_i32(&reply, 1);
        } else if (code == 16 /* getProcSysNet */) {
            p_i32(&reply, 0);
            p_str16(&reply, "0");
        } else {
            p_i32(&reply, 0);
            p_i32(&reply, 0);
            p_i32(&reply, 0);
            p_i32(&reply, 0);
        }
    } else if (node == NODE_DNSRESOLVER) {
        fprintf(stderr, "[svbox-netd] IDnsResolver code=%u flags=0x%x data=%llu\n",
                code, tr->flags, (unsigned long long)tr->data_size);
        fflush(stderr);
        if (code == 1 /* isAlive */) {
            p_i32(&reply, 0);
            p_i32(&reply, 1);
        } else {
            p_i32(&reply, 0);
            p_i32(&reply, 0);
            p_i32(&reply, 0);
            p_i32(&reply, 0);
        }
    } else {
        p_i32(&reply, 0);
        p_i32(&reply, 0);
    }

    if ((tr->flags & SV_TF_ONE_WAY) == 0) {
        size_t offs_bytes = reply.noffs * sizeof(uint64_t);
        uint8_t packed[4 + sizeof(struct sv_binder_transaction_data) + 1024 + 64 + 12];
        size_t plen = 0;
        size_t raw_w = 0;

        uint32_t rcmd = SV_BC_REPLY;
        struct sv_binder_transaction_data rtr;
        memset(&rtr, 0, sizeof(rtr));
        rtr.data_size = reply.len;
        rtr.offsets_size = offs_bytes;
        memcpy(packed + plen, &rcmd, 4); plen += 4;
        memcpy(packed + plen, &rtr, sizeof(rtr)); plen += sizeof(rtr);
        raw_w += 4 + sizeof(rtr);
        if (reply.len > 0) {
            memcpy(packed + plen, reply.buf, reply.len);
            plen += reply.len;
        }
        if (offs_bytes > 0) {
            memcpy(packed + plen, reply.offs, offs_bytes);
            plen += offs_bytes;
        }
        if (tr->data.ptr.buffer) {
            uint32_t fcmd = SV_BC_FREE_BUFFER;
            memcpy(packed + plen, &fcmd, 4); plen += 4;
            memcpy(packed + plen, &tr->data.ptr.buffer, 8); plen += 8;
            raw_w += 12;
        }
        uint8_t sink[128];
        size_t scons = 0;
        binder_wr(packed, plen, raw_w, sink, sizeof(sink), &scons);
    } else {
        free_buffer(tr->data.ptr.buffer);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <binder.sock>\n", argv[0]);
        return 1;
    }
    const char *sock_path = argv[1];
    for (int i = 0; i < 40; i++) {
        if (vdev_init(sock_path) == 0) break;
        usleep(100000);
    }
    if (g_rpc_sock < 0) {
        fprintf(stderr, "[svbox-netd] failed to connect to %s\n", sock_path);
        return 1;
    }

    uint32_t enter = SV_BC_ENTER_LOOPER;
    binder_wr((const uint8_t *)&enter, sizeof(enter), sizeof(enter), NULL, 0, NULL);

    if (add_service("netd", NODE_NETD) == 0) {
        fprintf(stderr, "[svbox-netd] registered 'netd'\n");
    }
    if (add_service("dnsresolver", NODE_DNSRESOLVER) == 0) {
        fprintf(stderr, "[svbox-netd] registered 'dnsresolver'\n");
    }
    if (add_service("media.audio_policy", NODE_AUDIO_POLICY) == 0) {
        fprintf(stderr, "[svbox-netd] registered 'media.audio_policy'\n");
    }
    if (add_service("media.audio_flinger", NODE_AUDIO_FLINGER) == 0) {
        fprintf(stderr, "[svbox-netd] registered 'media.audio_flinger'\n");
    }
    fflush(stderr);

    uint8_t rbuf[1024];
    for (;;) {
        size_t rcons = 0;
        if (binder_wr(NULL, 0, 0, rbuf, sizeof(rbuf), &rcons) < 0) {
            break;
        }
        size_t pos = 0;
        while (pos + 4 <= rcons) {
            uint32_t bcmd = 0;
            memcpy(&bcmd, rbuf + pos, 4);
            pos += 4;
            if (bcmd == SV_BR_NOOP || bcmd == SV_BR_TRANSACTION_COMPLETE ||
                bcmd == SV_BR_SPAWN_LOOPER || bcmd == SV_BR_DEAD_REPLY ||
                bcmd == SV_BR_FAILED_REPLY) {
                continue;
            } else if (bcmd == SV_BR_INCREFS || bcmd == SV_BR_ACQUIRE ||
                       bcmd == SV_BR_RELEASE || bcmd == SV_BR_DECREFS) {
                pos += 16;
            } else if (bcmd == SV_BR_TRANSACTION) {
                struct sv_binder_transaction_data tr;
                memcpy(&tr, rbuf + pos, sizeof(tr));
                pos += sizeof(tr);
                handle_transaction(&tr);
            } else if (bcmd == SV_BR_REPLY) {
                pos += sizeof(struct sv_binder_transaction_data);
            } else {
                break;
            }
        }
    }
    return 0;
}
