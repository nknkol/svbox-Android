/* SPDX-License-Identifier: MIT */
/* svbox-netd: Android 11 INetd + IDnsResolver + /dev/socket/fwmarkd bridge
 * backed by svbox's generic userspace virtual network daemon (svbox-net).
 *
 * Bridges Android's ConnectivityService / EthernetTracker / IpClient / Vpn.java
 * and Bionic's libnetd_client.so onto svbox-net's generic Linux virtual
 * interface, routing, UID-policy, DNS, and socket-inode protection APIs. */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "vdev_proto.h"
#include "net_proto.h"

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
#define NODE_MEDIA_METRICS      0x1006ull

/* Android 11 INetd transaction codes (verified from services.jar) */
#define TX_IS_ALIVE                       1u
#define TX_FIREWALL_REPLACE_UID_CHAIN     2u
#define TX_BANDWIDTH_ENABLE_DATA_SAVER    3u
#define TX_NETWORK_CREATE_PHYSICAL        4u
#define TX_NETWORK_CREATE_VPN             5u
#define TX_NETWORK_DESTROY                6u
#define TX_NETWORK_ADD_INTERFACE          7u
#define TX_NETWORK_REMOVE_INTERFACE       8u
#define TX_NETWORK_ADD_UID_RANGES         9u
#define TX_NETWORK_REMOVE_UID_RANGES      10u
#define TX_TETHER_APPLY_DNS_INTERFACES    13u
#define TX_INTERFACE_ADD_ADDRESS          15u
#define TX_INTERFACE_DEL_ADDRESS          16u
#define TX_GET_PROC_SYS_NET               17u
#define TX_NETWORK_ADD_ROUTE              62u
#define TX_NETWORK_REMOVE_ROUTE           63u
#define TX_NETWORK_GET_DEFAULT            66u
#define TX_NETWORK_SET_DEFAULT            67u
#define TX_NETWORK_CLEAR_DEFAULT          68u
#define TX_NETWORK_SET_PROTECT_ALLOW      73u
#define TX_NETWORK_SET_PROTECT_DENY       74u
#define TX_NETWORK_CAN_PROTECT            75u
#define TX_INTERFACE_GET_LIST             80u
#define TX_INTERFACE_GET_CFG              81u
#define TX_INTERFACE_SET_CFG              82u
#define TX_INTERFACE_CLEAR_ADDRS          84u
#define TX_INTERFACE_SET_MTU              86u
#define TX_REGISTER_UNSOL_LISTENER        90u
#define TX_GET_OEM_NETD                   94u
#define TX_NETWORK_ADD_ROUTE_PARCEL       97u
#define TX_NETWORK_UPDATE_ROUTE_PARCEL    98u
#define TX_NETWORK_REMOVE_ROUTE_PARCEL    99u

/* Android 11 IDnsResolver transaction codes */
#define DNS_TX_IS_ALIVE                   1u
#define DNS_TX_SET_RESOLVER_CONFIG        3u

/* Android 11 INetdUnsolicitedEventListener transaction codes */
#define UNSOL_ON_IFACE_ADDR_UPDATED       4u
#define UNSOL_ON_IFACE_ADDR_REMOVED       5u
#define UNSOL_ON_IFACE_ADDED              6u
#define UNSOL_ON_IFACE_REMOVED            7u
#define UNSOL_ON_IFACE_LINK_STATE         9u

/* Android 11 libnetd_client FwmarkCommand */
#define FWMARK_SELECT_NETWORK             2
#define FWMARK_PROTECT_FROM_VPN           3

struct fwmark_cmd {
    int32_t  cmd_id;
    uint32_t net_id;
    uint32_t uid;
    uint32_t tag;
};

static int g_ctrl_sock = -1;
static int g_rpc_sock = -1;
static uint32_t g_proc_id = 0;
static uint8_t *g_mmap_base = NULL;
static size_t g_mmap_len = 0;

static char g_net_sock_path[256] = {0};
static char g_fwmark_sock_path[256] = {0};
static const struct sv_net_state *g_net_state = NULL;

static int32_t g_default_netid = 100;
static int32_t g_phys_netid = 100;
static int32_t g_vpn_netid = 0;
static char    g_vpn_iface[16] = {0};

#define MAX_UNSOL_LISTENERS 8
static uint32_t g_unsol_handles[MAX_UNSOL_LISTENERS];
static int      g_unsol_count = 0;

/* Accumulated VPN UID ranges (can be added/removed incrementally by Vpn.java) */
static struct sv_net_uid_range g_vpn_uids[SV_NET_MAX_UID_RULES];
static uint32_t g_vpn_uid_count = 0;

/* ---- Low-level Unix Socket Helpers ---- */

static int send_all(int fd, const void *buf, size_t n)
{
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

static int recv_msg_fds(int fd, void *buf, size_t n, int *out_fds, int max_fds, int *got_fds)
{
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

static int connect_unix(const char *sock_path)
{
    if (!sock_path || !sock_path[0]) return -1;
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

/* ---- svbox-net RPC Client Helpers ---- */

static int net_rpc(uint32_t op, uint64_t arg0, const void *req_buf, uint32_t req_len,
                   struct vdev_hdr *rep_out, int *fd_out)
{
    if (fd_out) *fd_out = -1;
    if (!g_net_sock_path[0]) return -1;
    int fd = connect_unix(g_net_sock_path);
    if (fd < 0) return -1;
    struct vdev_hdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = VDEV_PROTO_MAGIC;
    hdr.op = op;
    hdr.payload_len = req_len;
    hdr.arg0 = arg0;
    if (send_all(fd, &hdr, sizeof(hdr)) < 0 ||
        (req_len > 0 && send_all(fd, req_buf, req_len) < 0)) {
        close(fd);
        return -1;
    }
    struct vdev_hdr rep;
    int rfds[2] = { -1, -1 };
    int nfds = 0;
    if (recv_msg_fds(fd, &rep, sizeof(rep), rfds, 2, &nfds) < 0 || rep.magic != VDEV_PROTO_MAGIC) {
        for (int i = 0; i < nfds; i++) close(rfds[i]);
        close(fd);
        return -1;
    }
    if (rep.payload_len > 0) {
        uint8_t sink[256];
        size_t rem = rep.payload_len;
        while (rem > 0) {
            size_t ch = rem > sizeof(sink) ? sizeof(sink) : rem;
            if (recv_msg_fds(fd, sink, ch, NULL, 0, NULL) < 0) break;
            rem -= ch;
        }
    }
    close(fd);
    if (fd_out && nfds > 0) {
        *fd_out = rfds[0];
        for (int i = 1; i < nfds; i++) close(rfds[i]);
    } else {
        for (int i = 0; i < nfds; i++) close(rfds[i]);
    }
    if (rep_out) *rep_out = rep;
    return rep.status;
}

static const struct sv_net_state *ensure_net_state(void)
{
    if (g_net_state) return g_net_state;
    if (!g_net_sock_path[0]) return NULL;
    struct vdev_hdr rep;
    int mfd = -1;
    if (net_rpc(NET_OP_GET_STATE, 0, NULL, 0, &rep, &mfd) == 0 && mfd >= 0) {
        size_t sz = rep.arg0 >= sizeof(struct sv_net_state)
            ? (size_t)rep.arg0 : sizeof(struct sv_net_state);
        void *p = mmap(NULL, sz, PROT_READ, MAP_SHARED, mfd, 0);
        close(mfd);
        if (p != MAP_FAILED) {
            const struct sv_net_state *st = (const struct sv_net_state *)p;
            if (st->magic == SV_NET_STATE_MAGIC && st->version == SV_NET_STATE_VERSION) {
                g_net_state = st;
                return st;
            }
            munmap(p, sz);
        }
    } else if (mfd >= 0) {
        close(mfd);
    }
    return NULL;
}

static void push_vpn_uid_rules(void)
{
    struct sv_net_uid_rules_req req;
    memset(&req, 0, sizeof(req));
    if (g_vpn_netid != 0 && g_vpn_uid_count > 0) {
        req.mode = 1u; /* allowlist: only UIDs in ranges go through VPN TUN */
        req.count = g_vpn_uid_count;
        memcpy(req.ranges, g_vpn_uids, g_vpn_uid_count * sizeof(struct sv_net_uid_range));
    } else {
        req.mode = 0u;
        req.count = 0u;
    }
    net_rpc(NET_OP_UID_RULES, 0, &req, sizeof(req), NULL, NULL);
}

/* ---- /dev/socket/fwmarkd Server Thread ---- */

static void *fwmarkd_thread(void *arg)
{
    (void)arg;
    if (!g_fwmark_sock_path[0]) return NULL;
    unlink(g_fwmark_sock_path);
    int srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (srv < 0) return NULL;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, g_fwmark_sock_path, sizeof(sa.sun_path) - 1);
    if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(srv, 64) < 0) {
        close(srv);
        return NULL;
    }
    chmod(g_fwmark_sock_path, 0666);
    fprintf(stderr, "[svbox-netd] fwmarkd listening on %s\n", g_fwmark_sock_path);
    fflush(stderr);

    for (;;) {
        int cfd = accept4(srv, NULL, NULL, SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        uint8_t buf[128];
        struct iovec iov = { buf, sizeof(buf) };
        union {
            struct cmsghdr align;
            char data[CMSG_SPACE(sizeof(int) * 4)];
        } cbuf;
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cbuf.data;
        msg.msg_controllen = sizeof(cbuf.data);

        ssize_t n = recvmsg(cfd, &msg, MSG_CMSG_CLOEXEC);
        int passed_fd = -1;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                size_t nb = c->cmsg_len - CMSG_LEN(0);
                int cnt = (int)(nb / sizeof(int));
                for (int i = 0; i < cnt; i++) {
                    int rfd = -1;
                    memcpy(&rfd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
                    if (passed_fd < 0) passed_fd = rfd;
                    else close(rfd);
                }
            }
        }

        if (n >= (ssize_t)sizeof(struct fwmark_cmd)) {
            struct fwmark_cmd fc;
            memcpy(&fc, buf, sizeof(fc));
            if (passed_fd >= 0 &&
                (fc.cmd_id == FWMARK_PROTECT_FROM_VPN ||
                 (fc.cmd_id == FWMARK_SELECT_NETWORK && (int32_t)fc.net_id == g_phys_netid && g_vpn_netid != 0))) {
                struct stat st;
                memset(&st, 0, sizeof(st));
                if (fstat(passed_fd, &st) == 0 && st.st_ino != 0) {
                    net_rpc(NET_OP_PROTECT_INO, (uint64_t)st.st_ino, NULL, 0, NULL, NULL);
                    fprintf(stderr, "[svbox-netd] fwmarkd cmd=%d protected socket ino=%llu\n",
                            fc.cmd_id, (unsigned long long)st.st_ino);
                    fflush(stderr);
                }
            }
        }
        if (passed_fd >= 0) close(passed_fd);
        int32_t err = 0;
        (void)send_all(cfd, &err, sizeof(err));
        close(cfd);
    }
    close(srv);
    return NULL;
}

/* ---- Binder VDEV Client ---- */

static int vdev_init(const char *sock_path)
{
    int cfd = connect_unix(sock_path);
    if (cfd < 0) return -1;

    struct vdev_open_req oreq;
    memset(&oreq, 0, sizeof(oreq));
    oreq.pid = (uint32_t)getpid();
    oreq.tid = (uint32_t)getpid();
    oreq.vdev_type = VDEV_TYPE_BINDER;
    oreq.flags = 2;
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
    int memfd = -1;
    if (nfds >= 2) { close(fds[0]); memfd = fds[1]; }
    else if (nfds == 1) { memfd = fds[0]; }
    g_proc_id = orep.proc_id;
    size_t len = orep.memfd_size ? (size_t)orep.memfd_size : (1024u * 1024u);
    if (memfd < 0) { close(cfd); return -1; }
    void *mapped = mmap(NULL, len, PROT_READ, MAP_SHARED, memfd, 0);
    close(memfd);
    if (mapped == MAP_FAILED) { close(cfd); return -1; }
    g_mmap_base = (uint8_t *)mapped;
    g_mmap_len = len;

    int rfd = connect_unix(sock_path);
    if (rfd < 0) { close(cfd); return -1; }
    struct vdev_attach_req areq = { g_proc_id, (uint32_t)getpid(), (uint32_t)getpid(), 0 };
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = VDEV_PROTO_MAGIC;
    hdr.op = VDEV_OP_ATTACH_THREAD;
    hdr.proc_id = g_proc_id;
    hdr.payload_len = sizeof(areq);
    if (send_all(rfd, &hdr, sizeof(hdr)) < 0 || send_all(rfd, &areq, sizeof(areq)) < 0 ||
        recv_msg_fds(rfd, &rsp, sizeof(rsp), NULL, 0, NULL) < 0 || rsp.status < 0) {
        close(rfd); close(cfd); return -1;
    }

    struct vdev_mmap_req mreq = { (uint64_t)(uintptr_t)mapped, (uint64_t)len };
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = VDEV_PROTO_MAGIC;
    hdr.op = VDEV_OP_MMAP;
    hdr.proc_id = g_proc_id;
    hdr.payload_len = sizeof(mreq);
    if (send_all(rfd, &hdr, sizeof(hdr)) < 0 || send_all(rfd, &mreq, sizeof(mreq)) < 0 ||
        recv_msg_fds(rfd, &rsp, sizeof(rsp), NULL, 0, NULL) < 0 || rsp.status < 0) {
        close(rfd); close(cfd); return -1;
    }
    g_ctrl_sock = cfd;
    g_rpc_sock = rfd;
    return 0;
}

static int binder_wr(const uint8_t *packed, size_t packed_len, size_t raw_write_size,
                     uint8_t *rbuf, size_t rcap, size_t *rconsumed)
{
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
    size_t skip = (size_t)wrep.fd_patch_count * sizeof(uint64_t);
    while (skip > 0) {
        uint8_t sink[256];
        size_t ch = skip > sizeof(sink) ? sizeof(sink) : skip;
        if (recv_msg_fds(g_rpc_sock, sink, ch, NULL, 0, NULL) < 0) return -1;
        skip -= ch;
    }
    size_t to_read = wrep.read_bytes <= rcap ? (size_t)wrep.read_bytes : rcap;
    if (to_read > 0 && recv_msg_fds(g_rpc_sock, rbuf, to_read, NULL, 0, NULL) < 0)
        return -1;
    if (wrep.read_bytes > to_read) {
        size_t rem = wrep.read_bytes - to_read;
        while (rem > 0) {
            uint8_t sink[256];
            size_t ch = rem > sizeof(sink) ? sizeof(sink) : rem;
            if (recv_msg_fds(g_rpc_sock, sink, ch, NULL, 0, NULL) < 0) return -1;
            rem -= ch;
        }
    }
    if (rconsumed) *rconsumed = to_read;
    return 0;
}

static void free_buffer(uint64_t ptr)
{
    if (!ptr) return;
    uint8_t buf[12];
    uint32_t cmd = SV_BC_FREE_BUFFER;
    memcpy(buf, &cmd, 4);
    memcpy(buf + 4, &ptr, 8);
    binder_wr(buf, sizeof(buf), sizeof(buf), NULL, 0, NULL);
}

static void acquire_handle(uint32_t handle)
{
    uint8_t buf[16];
    uint32_t c1 = SV_BC_INCREFS, c2 = SV_BC_ACQUIRE;
    memcpy(buf + 0, &c1, 4); memcpy(buf + 4, &handle, 4);
    memcpy(buf + 8, &c2, 4); memcpy(buf + 12, &handle, 4);
    binder_wr(buf, sizeof(buf), sizeof(buf), NULL, 0, NULL);
}

/* ---- Parcel Writer & Reader ---- */

struct parcel {
    uint8_t  buf[4096];
    size_t   len;
    uint64_t offs[8];
    size_t   noffs;
};

static void p_i32(struct parcel *p, int32_t v)
{
    if (p->len + 4 > sizeof(p->buf)) return;
    memcpy(p->buf + p->len, &v, 4);
    p->len += 4;
}

static void p_u32(struct parcel *p, uint32_t v)
{
    if (p->len + 4 > sizeof(p->buf)) return;
    memcpy(p->buf + p->len, &v, 4);
    p->len += 4;
}

static void p_str16(struct parcel *p, const char *s)
{
    if (!s) { p_i32(p, -1); return; }
    int32_t n = (int32_t)strlen(s);
    if (p->len + 4 + (size_t)(n + 1) * 2 + 4 > sizeof(p->buf)) return;
    p_i32(p, n);
    for (int32_t i = 0; i <= n; i++) {
        uint16_t ch = (uint16_t)(unsigned char)s[i];
        memcpy(p->buf + p->len, &ch, 2);
        p->len += 2;
    }
    while (p->len & 3) p->buf[p->len++] = 0;
}

static void p_iface_token(struct parcel *p, const char *iface)
{
    p_u32(p, 0x80000000u);
    p_i32(p, -1);
    p_u32(p, 0x53595354u); /* 'SYST' */
    p_str16(p, iface);
}

static void p_binder(struct parcel *p, uint64_t node_ptr, uint64_t cookie)
{
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

struct preader {
    const uint8_t *buf;
    size_t len;
    size_t pos;
};

static int pr_i32(struct preader *r, int32_t *out)
{
    if (r->pos + 4 > r->len) return -1;
    memcpy(out, r->buf + r->pos, 4);
    r->pos += 4;
    return 0;
}

static int pr_str16(struct preader *r, char *out, size_t cap)
{
    int32_t n = 0;
    if (pr_i32(r, &n) < 0) return -1;
    if (n < 0) { if (cap) out[0] = '\0'; return 0; }
    size_t raw = (size_t)(n + 1) * 2u;
    size_t aligned = (raw + 3u) & ~3u;
    if (r->pos + aligned > r->len) return -1;
    size_t copy = (size_t)n < cap - 1 ? (size_t)n : (cap ? cap - 1 : 0);
    for (size_t i = 0; i < copy; i++) {
        uint16_t ch = 0;
        memcpy(&ch, r->buf + r->pos + i * 2, 2);
        out[i] = (ch < 128) ? (char)ch : '?';
    }
    if (cap) out[copy] = '\0';
    r->pos += aligned;
    return 0;
}

static int pr_skip_iface_token(struct preader *r)
{
    if (r->len < 16) return -1;
    uint32_t w2 = 0;
    memcpy(&w2, r->buf + 8, 4);
    if (w2 == 0x53595354u /* SYST */ || w2 == 0x56454e44u /* VEND */ || w2 == 0x44454641u /* DEFA */) {
        r->pos = 12;
    } else {
        r->pos = 4;
    }
    char desc[128];
    return pr_str16(r, desc, sizeof(desc));
}

static void send_oneway_txn(uint32_t handle, uint32_t code, const struct parcel *p)
{
    uint8_t packed[4 + sizeof(struct sv_binder_transaction_data) + 4096];
    size_t plen = 0;
    uint32_t cmd = SV_BC_TRANSACTION;
    struct sv_binder_transaction_data tr;
    memset(&tr, 0, sizeof(tr));
    tr.target.handle = handle;
    tr.code = code;
    tr.flags = SV_TF_ONE_WAY;
    tr.data_size = p->len;
    tr.offsets_size = 0;
    memcpy(packed + plen, &cmd, 4); plen += 4;
    memcpy(packed + plen, &tr, sizeof(tr)); plen += sizeof(tr);
    memcpy(packed + plen, p->buf, p->len); plen += p->len;
    binder_wr(packed, plen, 4 + sizeof(tr), NULL, 0, NULL);
}

static void notify_unsol_addr(int added, const char *ifname, const char *addr_cidr)
{
    struct parcel p;
    memset(&p, 0, sizeof(p));
    p_iface_token(&p, "android.net.INetdUnsolicitedEventListener");
    p_str16(&p, addr_cidr);
    p_str16(&p, ifname);
    p_i32(&p, 0x80); /* IFA_F_PERMANENT */
    p_i32(&p, 0);    /* RT_SCOPE_UNIVERSE */
    for (int i = 0; i < g_unsol_count; i++)
        send_oneway_txn(g_unsol_handles[i], added ? UNSOL_ON_IFACE_ADDR_UPDATED : UNSOL_ON_IFACE_ADDR_REMOVED, &p);
}

static void notify_unsol_link_state(const char *ifname, int up)
{
    struct parcel p;
    memset(&p, 0, sizeof(p));
    p_iface_token(&p, "android.net.INetdUnsolicitedEventListener");
    p_str16(&p, ifname);
    p_i32(&p, up ? 1 : 0);
    for (int i = 0; i < g_unsol_count; i++)
        send_oneway_txn(g_unsol_handles[i], UNSOL_ON_IFACE_LINK_STATE, &p);
}

static int parse_cidr(const char *cidr, int *is_v6, uint32_t *v4_out, uint8_t v6_out[16], uint32_t *pfx_out)
{
    char tmp[128];
    strncpy(tmp, cidr, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *slash = strchr(tmp, '/');
    int has_slash = 0;
    uint32_t pfx = 32;
    if (slash) {
        *slash++ = '\0';
        pfx = (uint32_t)atoi(slash);
        has_slash = 1;
    }
    struct in_addr a4;
    if (inet_pton(AF_INET, tmp, &a4) == 1) {
        *is_v6 = 0;
        *v4_out = a4.s_addr;
        *pfx_out = has_slash ? (pfx <= 32 ? pfx : 32) : 32;
        return 0;
    }
    struct in6_addr a6;
    if (inet_pton(AF_INET6, tmp, &a6) == 1) {
        *is_v6 = 1;
        memcpy(v6_out, &a6, 16);
        *pfx_out = has_slash ? (pfx <= 128 ? pfx : 128) : 128;
        return 0;
    }
    return -1;
}

static void apply_route_change(int add, int32_t net_id, const char *ifname,
                               const char *dest_cidr, const char *nexthop)
{
    int is_v6 = 0;
    uint32_t d4 = 0, pfx = 0;
    uint8_t d6[16] = {0};
    if (parse_cidr(dest_cidr, &is_v6, &d4, d6, &pfx) < 0)
        return;

    struct sv_net_rtcfg_req rq;
    memset(&rq, 0, sizeof(rq));
    strncpy(rq.ifname, ifname, sizeof(rq.ifname) - 1);
    rq.prefix = pfx;
    /* Give VPN routes higher priority (metric=10) than physical eth0 (metric=100). */
    rq.metric = (net_id == g_vpn_netid && g_vpn_netid != 0) ? 10u : 100u;
    /* Keep main routing table (254) so standard Linux ip route / netlink readers see all routes. */
    rq.table = 254u;

    if (!is_v6) {
        rq.action = add ? SV_RTCFG_ADD4 : SV_RTCFG_DEL4;
        rq.dst_v4 = d4;
        if (nexthop && nexthop[0]) {
            struct in_addr gw;
            if (inet_pton(AF_INET, nexthop, &gw) == 1) rq.gw_v4 = gw.s_addr;
        }
    } else {
        rq.action = add ? SV_RTCFG_ADD6 : SV_RTCFG_DEL6;
        memcpy(rq.dst_v6, d6, 16);
        if (nexthop && nexthop[0]) {
            struct in6_addr gw6;
            if (inet_pton(AF_INET6, nexthop, &gw6) == 1) memcpy(rq.gw_v6, &gw6, 16);
        }
    }
    net_rpc(NET_OP_ROUTE_CFG, 0, &rq, sizeof(rq), NULL, NULL);
}

static void write_iface_cfg_parcel(struct parcel *reply, const struct sv_net_iface *f, const char *fallback_name)
{
    char mac_str[32] = "02:00:00:00:00:01";
    char ip_str[32] = "0.0.0.0";
    int32_t prefix = 0;
    const char *name = fallback_name;
    uint32_t flags = SV_IFF_UP | SV_IFF_RUNNING;
    if (f) {
        name = f->name;
        snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                 f->mac[0], f->mac[1], f->mac[2], f->mac[3], f->mac[4], f->mac[5]);
        if (f->ipv4_addr) {
            struct in_addr a = { f->ipv4_addr };
            inet_ntop(AF_INET, &a, ip_str, sizeof(ip_str));
            prefix = (int32_t)f->ipv4_prefix;
        }
        flags = f->flags;
    }

    p_i32(reply, 1); /* non-null InterfaceConfigurationParcel */
    size_t sz_pos = reply->len;
    p_i32(reply, 0); /* placeholder for parcelable_size */
    p_str16(reply, name);
    p_str16(reply, mac_str);
    p_str16(reply, ip_str);
    p_i32(reply, prefix);

    const char *flist[6];
    int fcnt = 0;
    flist[fcnt++] = (flags & SV_IFF_UP) ? "up" : "down";
    if (flags & SV_IFF_RUNNING)     flist[fcnt++] = "running";
    if (flags & SV_IFF_BROADCAST)   flist[fcnt++] = "broadcast";
    if (flags & SV_IFF_MULTICAST)   flist[fcnt++] = "multicast";
    if (flags & SV_IFF_LOOPBACK)    flist[fcnt++] = "loopback";
    if (flags & SV_IFF_POINTOPOINT) flist[fcnt++] = "point-to-point";
    p_i32(reply, fcnt);
    for (int i = 0; i < fcnt; i++) p_str16(reply, flist[i]);

    int32_t psize = (int32_t)(reply->len - sz_pos);
    memcpy(reply->buf + sz_pos, &psize, 4);
}

static int add_service(const char *name, uint64_t node_ptr)
{
    struct parcel p;
    memset(&p, 0, sizeof(p));
    p_iface_token(&p, "android.os.IServiceManager");
    p_str16(&p, name);
    p_binder(&p, node_ptr, node_ptr);
    p_i32(&p, 0);
    p_i32(&p, 8);

    size_t offs_bytes = p.noffs * sizeof(uint64_t);
    uint8_t packed[4 + sizeof(struct sv_binder_transaction_data) + 1024 + 64];
    size_t plen = 0;
    uint32_t cmd = SV_BC_TRANSACTION;
    struct sv_binder_transaction_data tr;
    memset(&tr, 0, sizeof(tr));
    tr.target.handle = 0;
    tr.code = 3;
    tr.flags = SV_TF_ACCEPT_FDS;
    tr.data_size = p.len;
    tr.offsets_size = offs_bytes;
    memcpy(packed + plen, &cmd, 4); plen += 4;
    memcpy(packed + plen, &tr, sizeof(tr)); plen += sizeof(tr);
    memcpy(packed + plen, p.buf, p.len); plen += p.len;
    if (offs_bytes > 0) { memcpy(packed + plen, p.offs, offs_bytes); plen += offs_bytes; }

    uint8_t rbuf[512];
    size_t rcons = 0;
    if (binder_wr(packed, plen, 4 + sizeof(tr), rbuf, sizeof(rbuf), &rcons) < 0)
        return -1;
    for (int tries = 0; tries < 8; tries++) {
        size_t pos = 0;
        int got_reply = 0;
        while (pos + 4 <= rcons) {
            uint32_t bcmd = 0;
            memcpy(&bcmd, rbuf + pos, 4); pos += 4;
            if (bcmd == SV_BR_NOOP || bcmd == SV_BR_TRANSACTION_COMPLETE || bcmd == SV_BR_SPAWN_LOOPER) continue;
            else if (bcmd == SV_BR_INCREFS || bcmd == SV_BR_ACQUIRE || bcmd == SV_BR_RELEASE || bcmd == SV_BR_DECREFS) pos += 16;
            else if (bcmd == SV_BR_REPLY) {
                struct sv_binder_transaction_data rtr;
                memcpy(&rtr, rbuf + pos, sizeof(rtr)); pos += sizeof(rtr);
                got_reply = 1;
                free_buffer(rtr.data.ptr.buffer);
            } else break;
        }
        if (got_reply) return 0;
        if (binder_wr(NULL, 0, 0, rbuf, sizeof(rbuf), &rcons) < 0) return -1;
    }
    return 0;
}

static void handle_transaction(const struct sv_binder_transaction_data *tr)
{
    struct parcel reply;
    memset(&reply, 0, sizeof(reply));

    const uint64_t node = tr->target.ptr;
    const uint32_t code = tr->code;
    struct preader pr = {
        .buf = (tr->data.ptr.buffer && tr->data_size > 0) ? (const uint8_t *)(uintptr_t)tr->data.ptr.buffer : NULL,
        .len = (size_t)tr->data_size,
        .pos = 0,
    };

    /* Post-reply oneway notifications (sent after replying so caller isn't stalled). */
    int post_addr_event = 0; /* 1=add, -1=del */
    char post_ifname[32] = {0};
    char post_addr_cidr[64] = {0};
    int post_link_event = 0; /* 1=up, -1=down */

    if (code == PING_TRANSACTION) {
        /* empty */
    } else if (code == INTERFACE_TRANSACTION) {
        if (node == NODE_DNSRESOLVER) p_str16(&reply, "android.net.IDnsResolver");
        else if (node == NODE_OEMNETD) p_str16(&reply, "com.android.internal.net.IOemNetd");
        else if (node == NODE_AUDIO_POLICY) p_str16(&reply, "android.media.IAudioPolicyService");
        else if (node == NODE_AUDIO_FLINGER) p_str16(&reply, "android.media.IAudioFlinger");
        else if (node == NODE_MEDIA_METRICS) p_str16(&reply, "android.media.IMediaMetricsService");
        else p_str16(&reply, "android.net.INetd");
    } else if (code == GET_INTERFACE_VERSION) {
        p_i32(&reply, 0);
        p_i32(&reply, node == NODE_DNSRESOLVER ? 6 : (node == NODE_OEMNETD ? 1 : 4));
    } else if (code == GET_INTERFACE_HASH) {
        p_i32(&reply, 0);
        p_str16(&reply, "");
    } else if (node == NODE_AUDIO_POLICY || node == NODE_AUDIO_FLINGER || node == NODE_MEDIA_METRICS) {
        p_i32(&reply, 0); p_i32(&reply, 0); p_i32(&reply, 0); p_i32(&reply, 0);
    } else if (node == NODE_NETD) {
        const struct sv_net_state *nst = ensure_net_state();
        if (code == TX_IS_ALIVE || code == TX_FIREWALL_REPLACE_UID_CHAIN ||
            code == TX_BANDWIDTH_ENABLE_DATA_SAVER || code == TX_TETHER_APPLY_DNS_INTERFACES ||
            code == TX_NETWORK_CAN_PROTECT) {
            p_i32(&reply, 0);
            p_i32(&reply, 1);
        } else if (code == TX_GET_PROC_SYS_NET) {
            p_i32(&reply, 0);
            p_str16(&reply, "0");
        } else if (code == TX_GET_OEM_NETD) {
            p_i32(&reply, 0);
            p_binder(&reply, NODE_OEMNETD, NODE_OEMNETD);
        } else if (code == TX_NETWORK_GET_DEFAULT) {
            p_i32(&reply, 0);
            p_i32(&reply, g_default_netid);
        } else if (code == TX_NETWORK_SET_DEFAULT) {
            if (pr.buf && pr_skip_iface_token(&pr) == 0) {
                int32_t nid = 0;
                if (pr_i32(&pr, &nid) == 0) g_default_netid = nid;
            }
            p_i32(&reply, 0);
        } else if (code == TX_NETWORK_CLEAR_DEFAULT) {
            g_default_netid = 0;
            p_i32(&reply, 0);
        } else if (code == TX_INTERFACE_GET_LIST) {
            p_i32(&reply, 0);
            if (nst) {
                int cnt = 0;
                for (int i = 0; i < SV_NET_MAX_IFACES; i++)
                    if (nst->ifaces[i].active) cnt++;
                p_i32(&reply, cnt);
                for (int i = 0; i < SV_NET_MAX_IFACES; i++)
                    if (nst->ifaces[i].active) p_str16(&reply, nst->ifaces[i].name);
            } else {
                p_i32(&reply, 2);
                p_str16(&reply, "lo");
                p_str16(&reply, "eth0");
            }
        } else if (code == TX_INTERFACE_GET_CFG) {
            char ifname[32] = "eth0";
            if (pr.buf && pr_skip_iface_token(&pr) == 0)
                pr_str16(&pr, ifname, sizeof(ifname));
            const struct sv_net_iface *found = NULL;
            if (nst) {
                for (int i = 0; i < SV_NET_MAX_IFACES; i++) {
                    if (nst->ifaces[i].active && strcmp(nst->ifaces[i].name, ifname) == 0) {
                        found = &nst->ifaces[i];
                        break;
                    }
                }
            }
            p_i32(&reply, 0);
            write_iface_cfg_parcel(&reply, found, ifname);
        } else if (code == TX_INTERFACE_SET_CFG) {
            if (pr.buf && pr_skip_iface_token(&pr) == 0) {
                int32_t non_null = 0, psz = 0, pfx = 0, fcnt = 0;
                char ifname[32] = {0}, hw[32] = {0}, ip[64] = {0};
                if (pr_i32(&pr, &non_null) == 0 && non_null && pr_i32(&pr, &psz) == 0 &&
                    pr_str16(&pr, ifname, sizeof(ifname)) == 0 &&
                    pr_str16(&pr, hw, sizeof(hw)) == 0 &&
                    pr_str16(&pr, ip, sizeof(ip)) == 0 &&
                    pr_i32(&pr, &pfx) == 0 &&
                    pr_i32(&pr, &fcnt) == 0) {
                    int set_up = -1;
                    for (int i = 0; i < fcnt && i < 16; i++) {
                        char fl[32] = {0};
                        if (pr_str16(&pr, fl, sizeof(fl)) < 0) break;
                        if (strcmp(fl, "up") == 0) set_up = 1;
                        else if (strcmp(fl, "down") == 0) set_up = 0;
                    }
                    if (set_up >= 0 && ifname[0]) {
                        struct sv_net_ifcfg_req rq;
                        memset(&rq, 0, sizeof(rq));
                        rq.action = SV_IFCFG_SET_FLAGS;
                        strncpy(rq.ifname, ifname, sizeof(rq.ifname) - 1);
                        rq.flags = set_up ? (SV_IFF_UP | SV_IFF_RUNNING | SV_IFF_BROADCAST | SV_IFF_MULTICAST) : 0u;
                        net_rpc(NET_OP_IFACE_CFG, 0, &rq, sizeof(rq), NULL, NULL);
                        strncpy(post_ifname, ifname, sizeof(post_ifname) - 1);
                        post_link_event = set_up ? 1 : -1;
                    }
                    if (ip[0] && strcmp(ip, "0.0.0.0") != 0 && pfx > 0 && ifname[0]) {
                        struct in_addr a4;
                        if (inet_pton(AF_INET, ip, &a4) == 1) {
                            struct sv_net_ifcfg_req rq;
                            memset(&rq, 0, sizeof(rq));
                            rq.action = SV_IFCFG_ADD_ADDR4;
                            strncpy(rq.ifname, ifname, sizeof(rq.ifname) - 1);
                            rq.ipv4_addr = a4.s_addr;
                            rq.prefix = (uint32_t)pfx;
                            net_rpc(NET_OP_IFACE_CFG, 0, &rq, sizeof(rq), NULL, NULL);
                            strncpy(post_ifname, ifname, sizeof(post_ifname) - 1);
                            snprintf(post_addr_cidr, sizeof(post_addr_cidr), "%s/%d", ip, pfx);
                            post_addr_event = 1;
                        }
                    }
                }
            }
            p_i32(&reply, 0);
        } else if (code == TX_INTERFACE_ADD_ADDRESS || code == TX_INTERFACE_DEL_ADDRESS) {
            if (pr.buf && pr_skip_iface_token(&pr) == 0) {
                char ifname[32] = {0}, addr[64] = {0};
                int32_t pfx = 0;
                if (pr_str16(&pr, ifname, sizeof(ifname)) == 0 &&
                    pr_str16(&pr, addr, sizeof(addr)) == 0 &&
                    pr_i32(&pr, &pfx) == 0 && ifname[0] && addr[0]) {
                    struct sv_net_ifcfg_req rq;
                    memset(&rq, 0, sizeof(rq));
                    strncpy(rq.ifname, ifname, sizeof(rq.ifname) - 1);
                    rq.prefix = (uint32_t)(pfx >= 0 ? pfx : 0);
                    struct in_addr a4;
                    struct in6_addr a6;
                    if (inet_pton(AF_INET, addr, &a4) == 1) {
                        rq.action = (code == TX_INTERFACE_ADD_ADDRESS) ? SV_IFCFG_ADD_ADDR4 : SV_IFCFG_DEL_ADDR4;
                        rq.ipv4_addr = a4.s_addr;
                        net_rpc(NET_OP_IFACE_CFG, 0, &rq, sizeof(rq), NULL, NULL);
                    } else if (inet_pton(AF_INET6, addr, &a6) == 1) {
                        rq.action = (code == TX_INTERFACE_ADD_ADDRESS) ? SV_IFCFG_ADD_ADDR6 : SV_IFCFG_DEL_ADDR6;
                        memcpy(rq.ipv6_addr, &a6, 16);
                        net_rpc(NET_OP_IFACE_CFG, 0, &rq, sizeof(rq), NULL, NULL);
                    }
                    strncpy(post_ifname, ifname, sizeof(post_ifname) - 1);
                    snprintf(post_addr_cidr, sizeof(post_addr_cidr), "%s/%d", addr, pfx);
                    post_addr_event = (code == TX_INTERFACE_ADD_ADDRESS) ? 1 : -1;
                    fprintf(stderr, "[svbox-netd] %s(%s, %s/%d)\n",
                            code == TX_INTERFACE_ADD_ADDRESS ? "interfaceAddAddress" : "interfaceDelAddress",
                            ifname, addr, pfx);
                    fflush(stderr);
                }
            }
            p_i32(&reply, 0);
        } else if (code == TX_INTERFACE_CLEAR_ADDRS) {
            if (pr.buf && pr_skip_iface_token(&pr) == 0) {
                char ifname[32] = {0};
                if (pr_str16(&pr, ifname, sizeof(ifname)) == 0 && ifname[0]) {
                    /* Preserve eth0's configured static address if NetworkStack resets before re-adding;
                     * only clear dynamically managed interfaces like tun0. */
                    if (strcmp(ifname, "eth0") != 0) {
                        struct sv_net_ifcfg_req rq;
                        memset(&rq, 0, sizeof(rq));
                        rq.action = SV_IFCFG_CLEAR_ADDRS;
                        strncpy(rq.ifname, ifname, sizeof(rq.ifname) - 1);
                        net_rpc(NET_OP_IFACE_CFG, 0, &rq, sizeof(rq), NULL, NULL);
                    }
                }
            }
            p_i32(&reply, 0);
        } else if (code == TX_INTERFACE_SET_MTU) {
            if (pr.buf && pr_skip_iface_token(&pr) == 0) {
                char ifname[32] = {0};
                int32_t mtu = 0;
                if (pr_str16(&pr, ifname, sizeof(ifname)) == 0 && pr_i32(&pr, &mtu) == 0 && ifname[0]) {
                    struct sv_net_ifcfg_req rq;
                    memset(&rq, 0, sizeof(rq));
                    rq.action = SV_IFCFG_SET_MTU;
                    strncpy(rq.ifname, ifname, sizeof(rq.ifname) - 1);
                    rq.mtu = (uint32_t)mtu;
                    net_rpc(NET_OP_IFACE_CFG, 0, &rq, sizeof(rq), NULL, NULL);
                }
            }
            p_i32(&reply, 0);
        } else if (code == TX_NETWORK_CREATE_PHYSICAL) {
            if (pr.buf && pr_skip_iface_token(&pr) == 0) {
                int32_t nid = 0;
                if (pr_i32(&pr, &nid) == 0) g_phys_netid = nid;
            }
            p_i32(&reply, 0);
        } else if (code == TX_NETWORK_CREATE_VPN) {
            if (pr.buf && pr_skip_iface_token(&pr) == 0) {
                int32_t nid = 0;
                if (pr_i32(&pr, &nid) == 0) {
                    g_vpn_netid = nid;
                    g_vpn_iface[0] = '\0';
                    g_vpn_uid_count = 0;
                    fprintf(stderr, "[svbox-netd] networkCreateVpn(netId=%d)\n", nid);
                    fflush(stderr);
                }
            }
            p_i32(&reply, 0);
        } else if (code == TX_NETWORK_DESTROY) {
            if (pr.buf && pr_skip_iface_token(&pr) == 0) {
                int32_t nid = 0;
                if (pr_i32(&pr, &nid) == 0 && nid == g_vpn_netid && nid != 0) {
                    fprintf(stderr, "[svbox-netd] networkDestroy(vpn netId=%d iface=%s)\n", nid, g_vpn_iface);
                    fflush(stderr);
                    if (g_vpn_iface[0]) {
                        struct sv_net_rtcfg_req rq;
                        memset(&rq, 0, sizeof(rq));
                        rq.action = SV_RTCFG_CLEAR_IFACE;
                        strncpy(rq.ifname, g_vpn_iface, sizeof(rq.ifname) - 1);
                        net_rpc(NET_OP_ROUTE_CFG, 0, &rq, sizeof(rq), NULL, NULL);
                    }
                    g_vpn_netid = 0;
                    g_vpn_iface[0] = '\0';
                    g_vpn_uid_count = 0;
                    push_vpn_uid_rules();
                    struct sv_net_dns_req dreq;
                    memset(&dreq, 0, sizeof(dreq));
                    net_rpc(NET_OP_SET_DNS, 0, &dreq, sizeof(dreq), NULL, NULL);
                }
            }
            p_i32(&reply, 0);
        } else if (code == TX_NETWORK_ADD_INTERFACE || code == TX_NETWORK_REMOVE_INTERFACE) {
            if (pr.buf && pr_skip_iface_token(&pr) == 0) {
                int32_t nid = 0;
                char ifname[32] = {0};
                if (pr_i32(&pr, &nid) == 0 && pr_str16(&pr, ifname, sizeof(ifname)) == 0) {
                    if (nid == g_vpn_netid && g_vpn_netid != 0) {
                        if (code == TX_NETWORK_ADD_INTERFACE)
                            strncpy(g_vpn_iface, ifname, sizeof(g_vpn_iface) - 1);
                        else
                            g_vpn_iface[0] = '\0';
                    }
                    fprintf(stderr, "[svbox-netd] %s(netId=%d, %s)\n",
                            code == TX_NETWORK_ADD_INTERFACE ? "networkAddInterface" : "networkRemoveInterface",
                            nid, ifname);
                    fflush(stderr);
                }
            }
            p_i32(&reply, 0);
        } else if (code == TX_NETWORK_ADD_ROUTE || code == TX_NETWORK_REMOVE_ROUTE) {
            if (pr.buf && pr_skip_iface_token(&pr) == 0) {
                int32_t nid = 0;
                char ifname[32] = {0}, dest[64] = {0}, gw[64] = {0};
                if (pr_i32(&pr, &nid) == 0 &&
                    pr_str16(&pr, ifname, sizeof(ifname)) == 0 &&
                    pr_str16(&pr, dest, sizeof(dest)) == 0 &&
                    pr_str16(&pr, gw, sizeof(gw)) == 0) {
                    apply_route_change(code == TX_NETWORK_ADD_ROUTE, nid, ifname, dest, gw);
                    fprintf(stderr, "[svbox-netd] %s(netId=%d, %s, %s, gw=%s)\n",
                            code == TX_NETWORK_ADD_ROUTE ? "networkAddRoute" : "networkRemoveRoute",
                            nid, ifname, dest, gw);
                    fflush(stderr);
                }
            }
            p_i32(&reply, 0);
        } else if (code == TX_NETWORK_ADD_ROUTE_PARCEL || code == TX_NETWORK_UPDATE_ROUTE_PARCEL ||
                   code == TX_NETWORK_REMOVE_ROUTE_PARCEL) {
            if (pr.buf && pr_skip_iface_token(&pr) == 0) {
                int32_t nid = 0, non_null = 0, psz = 0;
                char dest[64] = {0}, ifname[32] = {0}, gw[64] = {0};
                if (pr_i32(&pr, &nid) == 0 && pr_i32(&pr, &non_null) == 0 && non_null &&
                    pr_i32(&pr, &psz) == 0 &&
                    pr_str16(&pr, dest, sizeof(dest)) == 0 &&
                    pr_str16(&pr, ifname, sizeof(ifname)) == 0 &&
                    pr_str16(&pr, gw, sizeof(gw)) == 0) {
                    apply_route_change(code != TX_NETWORK_REMOVE_ROUTE_PARCEL, nid, ifname, dest, gw);
                    fprintf(stderr, "[svbox-netd] routeParcel(code=%u netId=%d %s %s gw=%s)\n",
                            code, nid, ifname, dest, gw);
                    fflush(stderr);
                }
            }
            p_i32(&reply, 0);
        } else if (code == TX_NETWORK_ADD_UID_RANGES || code == TX_NETWORK_REMOVE_UID_RANGES) {
            if (pr.buf && pr_skip_iface_token(&pr) == 0) {
                int32_t nid = 0, cnt = 0;
                if (pr_i32(&pr, &nid) == 0 && pr_i32(&pr, &cnt) == 0 && nid == g_vpn_netid && nid != 0) {
                    for (int i = 0; i < cnt && i < 64; i++) {
                        int32_t non_null = 0, psz = 0, u0 = 0, u1 = 0;
                        if (pr_i32(&pr, &non_null) < 0) break;
                        if (!non_null) continue;
                        size_t start_pos = pr.pos;
                        if (pr_i32(&pr, &psz) < 0 || pr_i32(&pr, &u0) < 0 || pr_i32(&pr, &u1) < 0) break;
                        if (psz > 0 && start_pos + (size_t)psz <= pr.len) pr.pos = start_pos + (size_t)psz;
                        if (code == TX_NETWORK_ADD_UID_RANGES) {
                            if (g_vpn_uid_count < SV_NET_MAX_UID_RULES) {
                                g_vpn_uids[g_vpn_uid_count].start = (uint32_t)u0;
                                g_vpn_uids[g_vpn_uid_count].stop = (uint32_t)u1;
                                g_vpn_uid_count++;
                            }
                        } else {
                            for (uint32_t k = 0; k < g_vpn_uid_count; k++) {
                                if (g_vpn_uids[k].start == (uint32_t)u0 && g_vpn_uids[k].stop == (uint32_t)u1) {
                                    g_vpn_uids[k] = g_vpn_uids[--g_vpn_uid_count];
                                    break;
                                }
                            }
                        }
                    }
                    push_vpn_uid_rules();
                    fprintf(stderr, "[svbox-netd] vpn uidRanges updated: count=%u\n", g_vpn_uid_count);
                    fflush(stderr);
                }
            }
            p_i32(&reply, 0);
        } else if (code == TX_REGISTER_UNSOL_LISTENER) {
            if (tr->offsets_size >= sizeof(uint64_t) && tr->data.ptr.offsets && pr.buf) {
                uint64_t obj_off = 0;
                memcpy(&obj_off, (const void *)(uintptr_t)tr->data.ptr.offsets, sizeof(uint64_t));
                if (obj_off + sizeof(struct sv_flat_binder_object) <= tr->data_size) {
                    struct sv_flat_binder_object fbo;
                    memcpy(&fbo, pr.buf + obj_off, sizeof(fbo));
                    if (fbo.type == SV_BINDER_TYPE_HANDLE && g_unsol_count < MAX_UNSOL_LISTENERS) {
                        acquire_handle(fbo.handle);
                        g_unsol_handles[g_unsol_count++] = fbo.handle;
                        fprintf(stderr, "[svbox-netd] registered unsolicited listener handle=%u (total=%d)\n",
                                fbo.handle, g_unsol_count);
                        fflush(stderr);
                    }
                }
            }
            p_i32(&reply, 0);
        } else {
            p_i32(&reply, 0);
            p_i32(&reply, 0);
            p_i32(&reply, 0);
            p_i32(&reply, 0);
        }
    } else if (node == NODE_DNSRESOLVER) {
        if (code == DNS_TX_IS_ALIVE) {
            p_i32(&reply, 0);
            p_i32(&reply, 1);
        } else if (code == DNS_TX_SET_RESOLVER_CONFIG) {
            if (pr.buf && pr_skip_iface_token(&pr) == 0) {
                int32_t non_null = 0, psz = 0, nid = 0, tmp = 0, scnt = 0;
                if (pr_i32(&pr, &non_null) == 0 && non_null && pr_i32(&pr, &psz) == 0 &&
                    pr_i32(&pr, &nid) == 0 &&
                    pr_i32(&pr, &tmp) == 0 && pr_i32(&pr, &tmp) == 0 &&
                    pr_i32(&pr, &tmp) == 0 && pr_i32(&pr, &tmp) == 0 &&
                    pr_i32(&pr, &tmp) == 0 && pr_i32(&pr, &tmp) == 0 &&
                    pr_i32(&pr, &scnt) == 0) {
                    struct sv_net_dns_req dreq;
                    memset(&dreq, 0, sizeof(dreq));
                    for (int i = 0; i < scnt && i < 16; i++) {
                        char ip[64] = {0};
                        if (pr_str16(&pr, ip, sizeof(ip)) < 0) break;
                        struct in_addr a4;
                        if (inet_pton(AF_INET, ip, &a4) == 1 && dreq.count < SV_NET_MAX_DNS)
                            dreq.servers_v4[dreq.count++] = a4.s_addr;
                    }
                    if (nid == g_vpn_netid && g_vpn_netid != 0 && dreq.count > 0) {
                        net_rpc(NET_OP_SET_DNS, 0, &dreq, sizeof(dreq), NULL, NULL);
                        fprintf(stderr, "[svbox-netd] setResolverConfiguration(vpn netId=%d servers=%u)\n",
                                nid, dreq.count);
                        fflush(stderr);
                    }
                }
            }
            p_i32(&reply, 0);
        } else {
            p_i32(&reply, 0); p_i32(&reply, 0); p_i32(&reply, 0); p_i32(&reply, 0);
        }
    } else {
        p_i32(&reply, 0);
        p_i32(&reply, 0);
    }

    if ((tr->flags & SV_TF_ONE_WAY) == 0) {
        size_t offs_bytes = reply.noffs * sizeof(uint64_t);
        uint8_t packed[4 + sizeof(struct sv_binder_transaction_data) + 4096 + 64 + 12];
        size_t plen = 0, raw_w = 0;
        uint32_t rcmd = SV_BC_REPLY;
        struct sv_binder_transaction_data rtr;
        memset(&rtr, 0, sizeof(rtr));
        rtr.data_size = reply.len;
        rtr.offsets_size = offs_bytes;
        memcpy(packed + plen, &rcmd, 4); plen += 4;
        memcpy(packed + plen, &rtr, sizeof(rtr)); plen += sizeof(rtr);
        raw_w += 4 + sizeof(rtr);
        if (reply.len > 0) { memcpy(packed + plen, reply.buf, reply.len); plen += reply.len; }
        if (offs_bytes > 0) { memcpy(packed + plen, reply.offs, offs_bytes); plen += offs_bytes; }
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

    if (post_link_event != 0 && post_ifname[0])
        notify_unsol_link_state(post_ifname, post_link_event > 0);
    if (post_addr_event != 0 && post_ifname[0] && post_addr_cidr[0])
        notify_unsol_addr(post_addr_event > 0, post_ifname, post_addr_cidr);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <binder.sock> [--net-sock PATH] [--fwmark-sock PATH] [--no-audio-stubs]\n", argv[0]);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    const char *binder_sock = argv[1];
    int register_audio_stubs = 1;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--no-audio-stubs") == 0) {
            register_audio_stubs = 0;
        } else if (strcmp(argv[i], "--net-sock") == 0 && i + 1 < argc) {
            strncpy(g_net_sock_path, argv[++i], sizeof(g_net_sock_path) - 1);
        } else if (strcmp(argv[i], "--fwmark-sock") == 0 && i + 1 < argc) {
            strncpy(g_fwmark_sock_path, argv[++i], sizeof(g_fwmark_sock_path) - 1);
        }
    }

    if (g_net_sock_path[0]) {
        for (int i = 0; i < 40; i++) {
            if (ensure_net_state() != NULL) break;
            usleep(50000);
        }
    }

    if (g_fwmark_sock_path[0]) {
        pthread_t tid;
        if (pthread_create(&tid, NULL, fwmarkd_thread, NULL) == 0)
            pthread_detach(tid);
    }

    for (int i = 0; i < 40; i++) {
        if (vdev_init(binder_sock) == 0) break;
        usleep(100000);
    }
    if (g_rpc_sock < 0) {
        fprintf(stderr, "[svbox-netd] failed to connect to %s\n", binder_sock);
        return 1;
    }

    uint32_t enter = SV_BC_ENTER_LOOPER;
    binder_wr((const uint8_t *)&enter, sizeof(enter), sizeof(enter), NULL, 0, NULL);

    if (add_service("netd", NODE_NETD) == 0)
        fprintf(stderr, "[svbox-netd] registered 'netd' (net_state=%s)\n", g_net_state ? "connected" : "fallback");
    if (add_service("dnsresolver", NODE_DNSRESOLVER) == 0)
        fprintf(stderr, "[svbox-netd] registered 'dnsresolver'\n");
    if (add_service("media.metrics", NODE_MEDIA_METRICS) == 0)
        fprintf(stderr, "[svbox-netd] registered 'media.metrics'\n");
    if (register_audio_stubs) {
        add_service("media.audio_policy", NODE_AUDIO_POLICY);
        add_service("media.audio_flinger", NODE_AUDIO_FLINGER);
    }
    fflush(stderr);

    uint8_t rbuf[2048];
    for (;;) {
        size_t rcons = 0;
        if (binder_wr(NULL, 0, 0, rbuf, sizeof(rbuf), &rcons) < 0) break;
        size_t pos = 0;
        while (pos + 4 <= rcons) {
            uint32_t bcmd = 0;
            memcpy(&bcmd, rbuf + pos, 4); pos += 4;
            if (bcmd == SV_BR_NOOP || bcmd == SV_BR_TRANSACTION_COMPLETE ||
                bcmd == SV_BR_SPAWN_LOOPER || bcmd == SV_BR_DEAD_REPLY ||
                bcmd == SV_BR_FAILED_REPLY) {
                continue;
            } else if (bcmd == SV_BR_INCREFS || bcmd == SV_BR_ACQUIRE ||
                       bcmd == SV_BR_RELEASE || bcmd == SV_BR_DECREFS) {
                pos += 16;
            } else if (bcmd == SV_BR_TRANSACTION) {
                struct sv_binder_transaction_data tr;
                memcpy(&tr, rbuf + pos, sizeof(tr)); pos += sizeof(tr);
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
