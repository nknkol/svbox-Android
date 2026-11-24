/* SPDX-License-Identifier: MIT */
/* svbox-binderd: standalone userspace Android Binder daemon for svbox --vdev.
 *
 * Implements 64-bit Linux Android Binder protocol v8 over AF_UNIX SOCK_STREAM
 * + SCM_RIGHTS + per-process shared memfd ring buffers, completely external to
 * svbox core. Supports multiple concurrent processes, epoll-based loopers
 * (Android 11 C++ servicemanager), synchronous/oneway transactions, handle/node
 * translation, BC_TRANSACTION_SG, and BINDER_TYPE_FD kernel FD passing. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#include "vdev_proto.h"

#ifndef SYS_memfd_create
#define SYS_memfd_create 279
#endif
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#define MAX_PROCS          256
#define INIT_NODES         4096
#define INIT_PROC_HANDLES  512
#define MAX_THREADS        4096
#define MAX_CONNS          4096
#define HASH_BUCKETS       8192
#define PROC_MEM_SIZE      (2u * 1024u * 1024u) /* 2 MiB per process */
#define MAX_ALLOCS         1024
#define INLINE_BLK_HANDLES 16

static bool g_verbose = false;
#define DLOG(fmt, ...) do { if (g_verbose) fprintf(stderr, "[binderd] " fmt "\n", ##__VA_ARGS__); } while (0)

struct mem_block {
    uint32_t  off;
    uint32_t  len;
    bool      used;
    uint32_t  handle_count;
    uint32_t  handles[INLINE_BLK_HANDLES];
    uint32_t *extra_handles;
    uint32_t  extra_cap;
};

struct bhandle {
    uint32_t handle;
    uint32_t node_id;
    int32_t  strong_refs;
    int32_t  weak_refs;
    int32_t  in_flight;
    bool     released_strong;
    bool     has_death;
    bool     death_sent;
    uint64_t death_cookie;
    bool     used;
};

struct bmsg {
    uint32_t br_cmd;
    uint8_t  payload[96]; /* BR_TRANSACTION/REPLY = 64B, ptr_cookie = 16B, etc. */
    uint32_t payload_len;
    int      fds[VDEV_MAX_FDS];
    uint64_t fd_patch_offsets[VDEV_MAX_FDS];
    uint32_t fd_count;
    /* For BR_TRANSACTION: tracks caller thread to deliver BC_REPLY back to. */
    uint32_t reply_to_thread_id;
    bool     is_oneway;
    uint64_t data_guest_ptr;
    struct bmsg *next;
};

struct bproc {
    bool     used;
    uint32_t proc_id;
    uint32_t pid;
    uint32_t uid;
    uint32_t gid;
    int      notify_fd;
    int      memfd;
    uint8_t *mem_base;
    uint32_t mem_size;
    uint64_t guest_base;
    uint64_t guest_map_len;
    uint32_t next_handle;
    uint32_t max_threads;
    uint32_t requested_threads;
    uint32_t requested_threads_started;
    struct mem_block allocs[MAX_ALLOCS];
    struct bhandle  *handles;
    int              handles_cap;
    struct bmsg     *inbox_head;
    struct bmsg     *inbox_tail;
    uint32_t         async_node_count;
};

struct bnode {
    bool     used;
    uint32_t node_id;
    uint32_t owner_proc_id;
    uint64_t ptr;
    uint64_t cookie;
    bool     is_ctx_mgr;
    bool     has_weak_ref;
    bool     has_strong_ref;
    bool     pending_weak_ref;
    bool     pending_strong_ref;
    bool     has_async_txn;
    int32_t  handle_count;
    int32_t  strong_ref_total;
    uint64_t async_buf_ptr;
    struct bmsg *async_head;
    struct bmsg *async_tail;
    int      id_hash_next;
    int      ptr_hash_next;
};

struct bthread {
    bool     used;
    uint32_t thread_id;
    uint32_t proc_id;
    uint32_t pid;
    uint32_t tid;
    int      rpc_fd;
    bool     is_looper;
    bool     is_registered_looper;
    bool     waiting_read;
    uint32_t waiting_reply_count;
    uint32_t parked_read_cap;
    uint32_t parked_write_consumed;
    /* Reply stack for nested synchronous transactions being served by this thread. */
    uint32_t reply_stack[16];
    int      reply_top;
    struct bmsg *direct_head;
    struct bmsg *direct_tail;
    int      hash_next;
};

static struct bproc   g_procs[MAX_PROCS];
static struct bnode  *g_nodes = NULL;
static int            g_nodes_cap = 0;
static struct bthread g_threads[MAX_THREADS];
static int            g_thread_buckets[HASH_BUCKETS];
static int            g_node_id_buckets[HASH_BUCKETS];
static int            g_node_ptr_buckets[HASH_BUCKETS];
static uint32_t g_next_proc_id   = 1;
static uint32_t g_next_node_id   = 1;
static uint32_t g_next_thread_id = 1;
static uint32_t g_ctx_mgr_node_id = 0;

static int g_epfd = -1;
static int g_listen_fd = -1;

/* Track unknown/untyped newly accepted sockets until first message arrives. */
enum conn_kind { CONN_NEW = 0, CONN_NOTIFY = 1, CONN_THREAD = 2 };
struct conn_entry {
    bool used;
    int  fd;
    enum conn_kind kind;
    uint32_t owner_id; /* proc_id or thread_id */
};
static struct conn_entry g_conns[MAX_CONNS];

static uint32_t align8(uint32_t n) { return (n + 7u) & ~7u; }

/* ---- Socket framing helpers ---- */
static int send_all_fds(int fd, const void *buf, size_t len, const int *fds, uint32_t fd_count)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        struct iovec iov = { (void *)(p + sent), len - sent };
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        char cbuf[CMSG_SPACE(sizeof(int) * VDEV_MAX_FDS)];
        if (sent == 0 && fd_count > 0) {
            memset(cbuf, 0, sizeof(cbuf));
            msg.msg_control = cbuf;
            msg.msg_controllen = CMSG_SPACE(sizeof(int) * fd_count);
            struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int) * fd_count);
            memcpy(CMSG_DATA(c), fds, sizeof(int) * fd_count);
        }
        ssize_t n = sendmsg(fd, &msg, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int recv_all_fds(int fd, void *buf, size_t len, int *out_fds, uint32_t max_fds, uint32_t *got_fds)
{
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    if (got_fds)
        *got_fds = 0;
    while (got < len) {
        struct iovec iov = { p + got, len - got };
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        char cbuf[CMSG_SPACE(sizeof(int) * VDEV_MAX_FDS)];
        if (out_fds && max_fds > 0) {
            memset(cbuf, 0, sizeof(cbuf));
            msg.msg_control = cbuf;
            msg.msg_controllen = sizeof(cbuf);
        }
        ssize_t n = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        if (out_fds && max_fds > 0) {
            for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                    size_t nf = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                    const int *rfds = (const int *)CMSG_DATA(c);
                    for (size_t i = 0; i < nf; i++) {
                        if (got_fds && *got_fds < max_fds)
                            out_fds[(*got_fds)++] = rfds[i];
                        else
                            close(rfds[i]);
                    }
                }
            }
        }
        got += (size_t)n;
    }
    return 0;
}

static int send_reply(int fd, uint32_t op, uint32_t proc_id, int32_t status,
                      const void *payload, uint32_t payload_len,
                      const int *fds, uint32_t fd_count)
{
    struct vdev_hdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = VDEV_PROTO_MAGIC;
    hdr.op = op;
    hdr.status = status;
    hdr.payload_len = payload_len;
    hdr.proc_id = proc_id;
    hdr.fd_count = fd_count;
    if (send_all_fds(fd, &hdr, sizeof(hdr), fds, fd_count) < 0)
        return -1;
    if (payload_len > 0 && send_all_fds(fd, payload, payload_len, NULL, 0) < 0)
        return -1;
    return 0;
}

/* ---- Process / Node / Handle / Memory helpers ---- */
static inline uint32_t hash_u32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    return x & (HASH_BUCKETS - 1u);
}

static inline uint32_t hash_node_ptr(uint32_t owner_proc_id, uint64_t ptr)
{
    uint64_t h = ptr ^ ((uint64_t)owner_proc_id * 0x9e3779b97f4a7c15ULL);
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (uint32_t)h & (HASH_BUCKETS - 1u);
}

static void thread_hash_insert(int idx)
{
    uint32_t b = hash_u32(g_threads[idx].thread_id);
    g_threads[idx].hash_next = g_thread_buckets[b];
    g_thread_buckets[b] = idx;
}

static void thread_hash_remove(int idx)
{
    uint32_t b = hash_u32(g_threads[idx].thread_id);
    int *pp = &g_thread_buckets[b];
    while (*pp >= 0) {
        if (*pp == idx) {
            *pp = g_threads[idx].hash_next;
            g_threads[idx].hash_next = -1;
            return;
        }
        pp = &g_threads[*pp].hash_next;
    }
    g_threads[idx].hash_next = -1;
}

static void node_hash_insert(int idx)
{
    uint32_t bid = hash_u32(g_nodes[idx].node_id);
    g_nodes[idx].id_hash_next = g_node_id_buckets[bid];
    g_node_id_buckets[bid] = idx;

    uint32_t bptr = hash_node_ptr(g_nodes[idx].owner_proc_id, g_nodes[idx].ptr);
    g_nodes[idx].ptr_hash_next = g_node_ptr_buckets[bptr];
    g_node_ptr_buckets[bptr] = idx;
}

static void node_hash_remove(int idx)
{
    uint32_t bid = hash_u32(g_nodes[idx].node_id);
    int *pp = &g_node_id_buckets[bid];
    while (*pp >= 0) {
        if (*pp == idx) {
            *pp = g_nodes[idx].id_hash_next;
            g_nodes[idx].id_hash_next = -1;
            break;
        }
        pp = &g_nodes[*pp].id_hash_next;
    }
    g_nodes[idx].id_hash_next = -1;

    uint32_t bptr = hash_node_ptr(g_nodes[idx].owner_proc_id, g_nodes[idx].ptr);
    pp = &g_node_ptr_buckets[bptr];
    while (*pp >= 0) {
        if (*pp == idx) {
            *pp = g_nodes[idx].ptr_hash_next;
            g_nodes[idx].ptr_hash_next = -1;
            break;
        }
        pp = &g_nodes[*pp].ptr_hash_next;
    }
    g_nodes[idx].ptr_hash_next = -1;
}

static struct bproc *find_proc(uint32_t proc_id)
{
    for (int i = 0; i < MAX_PROCS; i++)
        if (g_procs[i].used && g_procs[i].proc_id == proc_id)
            return &g_procs[i];
    return NULL;
}

static struct bthread *find_thread(uint32_t thread_id)
{
    if (!thread_id)
        return NULL;
    uint32_t b = hash_u32(thread_id);
    for (int idx = g_thread_buckets[b]; idx >= 0; idx = g_threads[idx].hash_next) {
        if (g_threads[idx].used && g_threads[idx].thread_id == thread_id)
            return &g_threads[idx];
    }
    return NULL;
}

static void node_check_release(struct bnode *node);

static struct bnode *find_node(uint32_t node_id)
{
    if (!node_id)
        return NULL;
    uint32_t b = hash_u32(node_id);
    for (int idx = g_node_id_buckets[b]; idx >= 0; idx = g_nodes[idx].id_hash_next) {
        if (g_nodes[idx].used && g_nodes[idx].node_id == node_id)
            return &g_nodes[idx];
    }
    return NULL;
}

static struct bnode *find_node_by_ptr(uint32_t owner_proc_id, uint64_t ptr)
{
    uint32_t bptr = hash_node_ptr(owner_proc_id, ptr);
    for (int idx = g_node_ptr_buckets[bptr]; idx >= 0; idx = g_nodes[idx].ptr_hash_next) {
        if (g_nodes[idx].used && g_nodes[idx].owner_proc_id == owner_proc_id && g_nodes[idx].ptr == ptr)
            return &g_nodes[idx];
    }
    return NULL;
}

static struct bnode *get_or_create_node(uint32_t owner_proc_id, uint64_t ptr, uint64_t cookie)
{
    uint32_t bptr = hash_node_ptr(owner_proc_id, ptr);
    for (int idx = g_node_ptr_buckets[bptr]; idx >= 0; idx = g_nodes[idx].ptr_hash_next) {
        if (g_nodes[idx].used && g_nodes[idx].owner_proc_id == owner_proc_id && g_nodes[idx].ptr == ptr)
            return &g_nodes[idx];
    }
    for (int i = 0; i < g_nodes_cap; i++) {
        if (!g_nodes[i].used) {
            memset(&g_nodes[i], 0, sizeof(g_nodes[i]));
            g_nodes[i].used = true;
            g_nodes[i].node_id = g_next_node_id++;
            g_nodes[i].owner_proc_id = owner_proc_id;
            g_nodes[i].ptr = ptr;
            g_nodes[i].cookie = cookie;
            g_nodes[i].id_hash_next = -1;
            g_nodes[i].ptr_hash_next = -1;
            node_hash_insert(i);
            return &g_nodes[i];
        }
    }
    int new_cap = g_nodes_cap > 0 ? g_nodes_cap * 2 : INIT_NODES;
    struct bnode *grown = (struct bnode *)realloc(g_nodes, (size_t)new_cap * sizeof(struct bnode));
    if (!grown)
        return NULL;
    memset(grown + g_nodes_cap, 0, (size_t)(new_cap - g_nodes_cap) * sizeof(struct bnode));
    int idx = g_nodes_cap;
    g_nodes = grown;
    g_nodes_cap = new_cap;
    g_nodes[idx].used = true;
    g_nodes[idx].node_id = g_next_node_id++;
    g_nodes[idx].owner_proc_id = owner_proc_id;
    g_nodes[idx].ptr = ptr;
    g_nodes[idx].cookie = cookie;
    g_nodes[idx].id_hash_next = -1;
    g_nodes[idx].ptr_hash_next = -1;
    node_hash_insert(idx);
    return &g_nodes[idx];
}

static struct bhandle *find_handle_entry(struct bproc *proc, uint32_t handle)
{
    if (!proc || !proc->handles)
        return NULL;
    for (int i = 0; i < proc->handles_cap; i++) {
        if (proc->handles[i].used && proc->handles[i].handle == handle)
            return &proc->handles[i];
    }
    return NULL;
}

static struct bnode *lookup_handle_node(struct bproc *proc, uint32_t handle)
{
    if (handle == 0)
        return find_node(g_ctx_mgr_node_id);
    struct bhandle *he = find_handle_entry(proc, handle);
    return he ? find_node(he->node_id) : NULL;
}

static struct bhandle *get_or_create_handle_entry(struct bproc *proc, uint32_t node_id)
{
    if (!proc || node_id == 0 || node_id == g_ctx_mgr_node_id)
        return NULL;
    for (int i = 0; i < proc->handles_cap; i++) {
        if (proc->handles[i].used && proc->handles[i].node_id == node_id)
            return &proc->handles[i];
    }
    for (int i = 0; i < proc->handles_cap; i++) {
        if (!proc->handles[i].used) {
            memset(&proc->handles[i], 0, sizeof(proc->handles[i]));
            proc->handles[i].used = true;
            proc->handles[i].handle = proc->next_handle++;
            if (proc->next_handle == 0) proc->next_handle = 1;
            proc->handles[i].node_id = node_id;
            struct bnode *nd = find_node(node_id);
            if (nd) nd->handle_count++;
            return &proc->handles[i];
        }
    }
    int new_cap = proc->handles_cap > 0 ? proc->handles_cap * 2 : INIT_PROC_HANDLES;
    struct bhandle *grown = (struct bhandle *)realloc(proc->handles, (size_t)new_cap * sizeof(struct bhandle));
    if (!grown)
        return NULL;
    memset(grown + proc->handles_cap, 0, (size_t)(new_cap - proc->handles_cap) * sizeof(struct bhandle));
    int idx = proc->handles_cap;
    proc->handles = grown;
    proc->handles_cap = new_cap;
    proc->handles[idx].used = true;
    proc->handles[idx].handle = proc->next_handle++;
    if (proc->next_handle == 0) proc->next_handle = 1;
    proc->handles[idx].node_id = node_id;
    struct bnode *nd = find_node(node_id);
    if (nd) nd->handle_count++;
    return &proc->handles[idx];
}

static void maybe_reclaim_handle(struct bproc *proc, struct bhandle *he)
{
    (void)proc;
    if (!he || !he->used)
        return;
    if (he->strong_refs > 0 || he->weak_refs > 0 || he->in_flight > 0 ||
        he->has_death || he->death_sent)
        return;
    uint32_t nid = he->node_id;
    he->used = false;
    he->node_id = 0;
    if (nid != 0) {
        struct bnode *nd = find_node(nid);
        if (nd) {
            if (nd->handle_count > 0) nd->handle_count--;
            node_check_release(nd);
        }
    }
}

static void mem_block_add_handle(struct mem_block *blk, uint32_t handle)
{
    if (!blk || handle == 0)
        return;
    if (blk->handle_count < INLINE_BLK_HANDLES) {
        blk->handles[blk->handle_count++] = handle;
        return;
    }
    uint32_t eidx = blk->handle_count - INLINE_BLK_HANDLES;
    if (eidx >= blk->extra_cap) {
        uint32_t ncap = blk->extra_cap > 0 ? blk->extra_cap * 2 : 32;
        uint32_t *ng = (uint32_t *)realloc(blk->extra_handles, ncap * sizeof(uint32_t));
        if (!ng)
            return;
        blk->extra_handles = ng;
        blk->extra_cap = ncap;
    }
    blk->extra_handles[eidx] = handle;
    blk->handle_count++;
}

static void mem_block_release_handles(struct bproc *proc, struct mem_block *blk)
{
    if (!blk)
        return;
    for (uint32_t i = 0; i < blk->handle_count; i++) {
        uint32_t h = (i < INLINE_BLK_HANDLES) ? blk->handles[i] : blk->extra_handles[i - INLINE_BLK_HANDLES];
        struct bhandle *he = find_handle_entry(proc, h);
        if (he) {
            uint32_t nid = he->node_id;
            if (he->in_flight > 0) {
                he->in_flight--;
                struct bnode *nd = nid ? find_node(nid) : NULL;
                if (nd && nd->strong_ref_total > 0) {
                    nd->strong_ref_total--;
                    if (nd->strong_ref_total == 0)
                        node_check_release(nd);
                }
            }
            maybe_reclaim_handle(proc, he);
        }
    }
    blk->handle_count = 0;
    if (blk->extra_handles) {
        free(blk->extra_handles);
        blk->extra_handles = NULL;
        blk->extra_cap = 0;
    }
}

/* Simple first-fit allocator inside proc->mem_base[0..mem_size). */
static int proc_mem_alloc(struct bproc *proc, uint32_t size, uint32_t *out_off, int *out_idx)
{
    uint32_t need = align8(size < 8 ? 8 : size);
    /* Find lowest free gap [cand, cand+need) not overlapping any used block. */
    uint32_t cand = 0;
    for (;;) {
        if (cand + need > proc->mem_size)
            return -ENOMEM;
        bool overlap = false;
        for (int i = 0; i < MAX_ALLOCS; i++) {
            if (!proc->allocs[i].used)
                continue;
            uint32_t a = proc->allocs[i].off;
            uint32_t b = a + proc->allocs[i].len;
            if (!(cand + need <= a || cand >= b)) {
                cand = align8(b);
                overlap = true;
                break;
            }
        }
        if (!overlap)
            break;
    }
    for (int i = 0; i < MAX_ALLOCS; i++) {
        if (!proc->allocs[i].used) {
            proc->allocs[i].used = true;
            proc->allocs[i].off = cand;
            proc->allocs[i].len = need;
            proc->allocs[i].handle_count = 0;
            proc->allocs[i].extra_handles = NULL;
            proc->allocs[i].extra_cap = 0;
            *out_off = cand;
            if (out_idx) *out_idx = i;
            return 0;
        }
    }
    return -ENOMEM;
}

static void proc_mem_free(struct bproc *proc, uint64_t guest_ptr)
{
    if (!proc->guest_base || guest_ptr < proc->guest_base)
        return;
    uint64_t off = guest_ptr - proc->guest_base;
    for (int i = 0; i < MAX_ALLOCS; i++) {
        if (proc->allocs[i].used && proc->allocs[i].off == off) {
            proc->allocs[i].used = false;
            mem_block_release_handles(proc, &proc->allocs[i]);
            return;
        }
    }
}

/* ---- Message queues & wakeups ---- */
static void free_bmsg(struct bmsg *m)
{
    for (uint32_t i = 0; i < m->fd_count; i++)
        if (m->fds[i] >= 0)
            close(m->fds[i]);
    free(m);
}

static void push_direct(struct bthread *t, struct bmsg *m)
{
    m->next = NULL;
    if (t->direct_tail)
        t->direct_tail->next = m;
    else
        t->direct_head = m;
    t->direct_tail = m;
}

static struct bmsg *pop_direct(struct bthread *t)
{
    struct bmsg *m = t->direct_head;
    if (!m)
        return NULL;
    t->direct_head = m->next;
    if (!t->direct_head)
        t->direct_tail = NULL;
    m->next = NULL;
    return m;
}

static void signal_proc_notify(struct bproc *proc)
{
    if (proc->notify_fd >= 0) {
        char b = '!';
        (void)send(proc->notify_fd, &b, 1, MSG_DONTWAIT | MSG_NOSIGNAL);
    }
}

static void push_inbox(struct bproc *proc, struct bmsg *m)
{
    m->next = NULL;
    if (proc->inbox_tail)
        proc->inbox_tail->next = m;
    else
        proc->inbox_head = m;
    proc->inbox_tail = m;
    signal_proc_notify(proc);
}

static struct bmsg *pop_inbox(struct bproc *proc)
{
    struct bmsg *m = proc->inbox_head;
    if (!m)
        return NULL;
    proc->inbox_head = m->next;
    if (!proc->inbox_head)
        proc->inbox_tail = NULL;
    m->next = NULL;
    return m;
}

static struct bmsg *make_simple_br(uint32_t br_cmd, const void *body, uint32_t body_len)
{
    struct bmsg *m = (struct bmsg *)calloc(1, sizeof(*m));
    m->br_cmd = br_cmd;
    if (body && body_len > 0) {
        memcpy(m->payload, body, body_len);
        m->payload_len = body_len;
    }
    return m;
}

static bool should_spawn_looper(struct bproc *proc, struct bthread *excluding)
{
    if (!proc || proc->max_threads == 0)
        return false;
    if (proc->requested_threads > 0)
        return false;
    if (proc->requested_threads_started >= proc->max_threads)
        return false;
    for (int i = 0; i < MAX_THREADS; i++) {
        struct bthread *ot = &g_threads[i];
        if (ot->used && ot != excluding && ot->proc_id == proc->proc_id &&
            ot->is_looper && ot->waiting_read && ot->waiting_reply_count == 0)
            return false;
    }
    return true;
}

/* Try to satisfy a parked thread's BINDER_WRITE_READ read request. */
static void try_deliver_to_thread(struct bthread *t)
{
    if (!t->waiting_read)
        return;
    struct bproc *proc = find_proc(t->proc_id);
    if (!proc)
        return;

    /* Pick ready messages: direct queue first, then proc inbox if looper and not blocked waiting for a sync reply. */
    uint8_t br_buf[65536];
    uint32_t br_off = 0;
    uint32_t cap = t->parked_read_cap;
    if (cap > sizeof(br_buf))
        cap = sizeof(br_buf);

    int send_fds[VDEV_MAX_FDS];
    uint64_t patch_offs[VDEV_MAX_FDS];
    uint32_t fd_count = 0;

    bool got_any = false;
    bool spawned = false;
    while (br_off < cap) {
        struct bmsg *m = NULL;
        if (t->direct_head) {
            if (br_off + 4 + t->direct_head->payload_len > cap ||
                fd_count + t->direct_head->fd_count > VDEV_MAX_FDS)
                break;
            if (!spawned && t->is_looper && t->direct_head->br_cmd == SV_BR_TRANSACTION &&
                should_spawn_looper(proc, t) &&
                br_off + 4 + 4 + t->direct_head->payload_len <= cap) {
                uint32_t sp = SV_BR_SPAWN_LOOPER;
                memcpy(br_buf + br_off, &sp, 4);
                br_off += 4;
                proc->requested_threads++;
                spawned = true;
            }
            m = pop_direct(t);
            if (m && m->br_cmd == SV_BR_TRANSACTION && !m->is_oneway) {
                if (t->reply_top < 16)
                    t->reply_stack[t->reply_top++] = m->reply_to_thread_id;
            }
        } else if (t->is_looper && t->waiting_reply_count == 0 && proc->inbox_head && !got_any) {
            /* Deliver one inbox transaction at a time so other loopers can help. */
            if (br_off + 4 + proc->inbox_head->payload_len > cap ||
                fd_count + proc->inbox_head->fd_count > VDEV_MAX_FDS)
                break;
            if (!spawned && should_spawn_looper(proc, t) &&
                br_off + 4 + 4 + proc->inbox_head->payload_len <= cap) {
                uint32_t sp = SV_BR_SPAWN_LOOPER;
                memcpy(br_buf + br_off, &sp, 4);
                br_off += 4;
                proc->requested_threads++;
                spawned = true;
            }
            m = pop_inbox(proc);
            if (m && m->br_cmd == SV_BR_TRANSACTION && !m->is_oneway) {
                if (t->reply_top < 16)
                    t->reply_stack[t->reply_top++] = m->reply_to_thread_id;
            }
        } else {
            break;
        }
        if (!m)
            break;

        memcpy(br_buf + br_off, &m->br_cmd, 4);
        br_off += 4;
        if (m->payload_len > 0) {
            memcpy(br_buf + br_off, m->payload, m->payload_len);
            br_off += m->payload_len;
        }
        for (uint32_t i = 0; i < m->fd_count; i++) {
            send_fds[fd_count] = m->fds[i];
            patch_offs[fd_count] = m->fd_patch_offsets[i];
            m->fds[i] = -1; /* ownership transferred to send_fds */
            fd_count++;
        }
        if (m->br_cmd == SV_BR_REPLY || m->br_cmd == SV_BR_DEAD_REPLY || m->br_cmd == SV_BR_FAILED_REPLY) {
            if (t->waiting_reply_count > 0)
                t->waiting_reply_count--;
        }
        free_bmsg(m);
        got_any = true;
        /* Stop after delivering a transaction or reply so guest processes it right away. */
        if (br_off > 0 && t->direct_head == NULL)
            break;
    }

    if (!got_any)
        return;

    size_t rep_sz = sizeof(struct vdev_binder_wr_rep) + fd_count * sizeof(uint64_t) + br_off;
    uint8_t *rep_buf = (uint8_t *)malloc(rep_sz);
    struct vdev_binder_wr_rep *wrep = (struct vdev_binder_wr_rep *)rep_buf;
    wrep->write_consumed = t->parked_write_consumed;
    wrep->read_bytes = br_off;
    wrep->fd_patch_count = fd_count;
    wrep->reserved = 0;
    if (fd_count > 0)
        memcpy(rep_buf + sizeof(*wrep), patch_offs, fd_count * sizeof(uint64_t));
    if (br_off > 0)
        memcpy(rep_buf + sizeof(*wrep) + fd_count * sizeof(uint64_t), br_buf, br_off);

    t->waiting_read = false;
    t->parked_write_consumed = 0;
    send_reply(t->rpc_fd, VDEV_OP_BINDER_WR, t->proc_id, 0, rep_buf, (uint32_t)rep_sz, send_fds, fd_count);
    for (uint32_t i = 0; i < fd_count; i++)
        close(send_fds[i]);
    free(rep_buf);

    /* If inbox still has pending messages, re-signal notify_fd for epoll_wait. */
    if (proc->inbox_head)
        signal_proc_notify(proc);
}

static void wake_proc_waiters(struct bproc *proc);

static void node_check_release(struct bnode *node)
{
    if (!node || !node->used || node->ptr == 0 || node->is_ctx_mgr)
        return;
    struct bproc *owner = find_proc(node->owner_proc_id);
    if (!owner)
        return;
    /* O(1) strong/weak reference check maintained across all handle operations. */
    bool any_strong = (node->strong_ref_total > 0);
    bool any_weak   = (node->handle_count > 0);
    uint64_t pc[2] = { node->ptr, node->cookie };
    if (!any_strong && node->has_strong_ref && !node->pending_strong_ref) {
        node->has_strong_ref = false;
        push_inbox(owner, make_simple_br(SV_BR_RELEASE, pc, sizeof(pc)));
        wake_proc_waiters(owner);
    }
    if (!any_weak && !node->pending_strong_ref && !node->pending_weak_ref) {
        if (node->has_weak_ref) {
            node->has_weak_ref = false;
            push_inbox(owner, make_simple_br(SV_BR_DECREFS, pc, sizeof(pc)));
            wake_proc_waiters(owner);
        }
        if (!node->has_async_txn) {
            node_hash_remove((int)(node - g_nodes));
            node->used = false;
        }
    }
}

static void node_inc_refs(struct bnode *node, bool strong, struct bthread *prefer_t)
{
    if (!node || !node->used || node->ptr == 0)
        return;
    struct bproc *owner = find_proc(node->owner_proc_id);
    if (!owner)
        return;
    uint64_t pc[2] = { node->ptr, node->cookie };
    bool send_weak = !node->has_weak_ref;
    bool send_strong = strong && !node->has_strong_ref;
    if (!send_weak && !send_strong)
        return;
    if (send_weak) {
        node->has_weak_ref = true;
        node->pending_weak_ref = true;
    }
    if (send_strong) {
        node->has_strong_ref = true;
        node->pending_strong_ref = true;
    }
    if (prefer_t && prefer_t->used && prefer_t->proc_id == owner->proc_id) {
        if (send_weak)
            push_direct(prefer_t, make_simple_br(SV_BR_INCREFS, pc, sizeof(pc)));
        if (send_strong)
            push_direct(prefer_t, make_simple_br(SV_BR_ACQUIRE, pc, sizeof(pc)));
    } else {
        if (send_weak)
            push_inbox(owner, make_simple_br(SV_BR_INCREFS, pc, sizeof(pc)));
        if (send_strong)
            push_inbox(owner, make_simple_br(SV_BR_ACQUIRE, pc, sizeof(pc)));
        wake_proc_waiters(owner);
    }
}

static void wake_proc_waiters(struct bproc *proc)
{
    for (int i = 0; i < MAX_THREADS && proc->inbox_head; i++) {
        if (g_threads[i].used && g_threads[i].proc_id == proc->proc_id &&
            g_threads[i].is_looper && g_threads[i].waiting_read &&
            g_threads[i].waiting_reply_count == 0) {
            try_deliver_to_thread(&g_threads[i]);
        }
    }
    if (proc->inbox_head)
        signal_proc_notify(proc);
}

/* ---- Transaction routing & object translation ---- */
static void handle_transaction(struct bthread *sender_t, struct bproc *sender_p,
                               uint32_t bc_cmd,
                               const struct sv_binder_transaction_data *in_txn,
                               const uint8_t *tdata, const uint8_t *toffs,
                               const uint8_t *sg_data, uint32_t sg_raw_len,
                               int *in_fds, uint32_t in_fd_count)
{
    bool is_reply = (bc_cmd == SV_BC_REPLY || bc_cmd == SV_BC_REPLY_SG);
    bool is_oneway = !is_reply && ((in_txn->flags & SV_TF_ONE_WAY) != 0);

    struct bproc *target_p = NULL;
    struct bthread *reply_t = NULL;
    struct bnode *target_node = NULL;
    uint32_t target_node_id = 0;

    if (is_reply) {
        if (sender_t->reply_top <= 0) {
            DLOG("BC_REPLY with empty reply_stack on tid=%u", sender_t->tid);
            push_direct(sender_t, make_simple_br(SV_BR_FAILED_REPLY, NULL, 0));
            return;
        }
        uint32_t caller_tid = sender_t->reply_stack[--sender_t->reply_top];
        reply_t = find_thread(caller_tid);
        if (!reply_t) {
            push_direct(sender_t, make_simple_br(SV_BR_DEAD_REPLY, NULL, 0));
            return;
        }
        target_p = find_proc(reply_t->proc_id);
        if (!target_p) {
            push_direct(sender_t, make_simple_br(SV_BR_DEAD_REPLY, NULL, 0));
            return;
        }
    } else {
        target_node = lookup_handle_node(sender_p, in_txn->target.handle);
        if (!target_node) {
            DLOG("BC_TRANSACTION handle=%u code=%u -> DEAD (no node)", in_txn->target.handle, in_txn->code);
            if (is_oneway)
                push_direct(sender_t, make_simple_br(SV_BR_TRANSACTION_COMPLETE, NULL, 0));
            else
                push_direct(sender_t, make_simple_br(SV_BR_DEAD_REPLY, NULL, 0));
            return;
        }
        target_node_id = target_node->node_id;
        target_p = find_proc(target_node->owner_proc_id);
        if (!target_p) {
            push_direct(sender_t, make_simple_br(SV_BR_DEAD_REPLY, NULL, 0));
            return;
        }
    }

    /* Pre-compute aligned scatter-gather buffer requirement. */
    size_t n_offs = (size_t)in_txn->offsets_size / sizeof(uint64_t);
    uint32_t sg_alloc_bytes = 0;
    for (size_t k = 0; k < n_offs; k++) {
        uint64_t o;
        memcpy(&o, toffs + k * sizeof(uint64_t), sizeof(uint64_t));
        if (o + sizeof(struct sv_binder_buffer_object) <= in_txn->data_size) {
            uint32_t otype;
            memcpy(&otype, tdata + o, 4);
            if (otype == SV_BINDER_TYPE_PTR) {
                struct sv_binder_buffer_object bbo;
                memcpy(&bbo, tdata + o, sizeof(bbo));
                sg_alloc_bytes += align8((uint32_t)(bbo.length ? bbo.length : 8));
            }
        }
    }

    /* Allocate space in target_p's shared memfd for data + offsets + sg buffers. */
    uint32_t data_aligned = align8((uint32_t)in_txn->data_size);
    uint32_t offs_aligned = align8((uint32_t)in_txn->offsets_size);
    uint32_t total_alloc = data_aligned + offs_aligned + sg_alloc_bytes;
    uint32_t mem_off = 0;
    int alloc_idx = -1;
    if (proc_mem_alloc(target_p, total_alloc, &mem_off, &alloc_idx) < 0) {
        DLOG("target proc=%u memfd OOM (need=%u)", target_p->proc_id, total_alloc);
        push_direct(sender_t, make_simple_br(SV_BR_FAILED_REPLY, NULL, 0));
        return;
    }

    uint8_t *dst_data = target_p->mem_base + mem_off;
    uint8_t *dst_offs = dst_data + data_aligned;
    uint32_t sg_dst_off = mem_off + data_aligned + offs_aligned;
    uint32_t sg_src_off = 0;
    if (in_txn->data_size > 0)
        memcpy(dst_data, tdata, (size_t)in_txn->data_size);
    if (in_txn->offsets_size > 0)
        memcpy(dst_offs, toffs, (size_t)in_txn->offsets_size);

    struct bmsg *msg = (struct bmsg *)calloc(1, sizeof(*msg));
    msg->br_cmd = is_reply ? SV_BR_REPLY : SV_BR_TRANSACTION;
    msg->is_oneway = is_oneway;
    msg->reply_to_thread_id = sender_t->thread_id;

    uint8_t *sg_host_ptrs[128];
    uint64_t sg_lengths[128];
    uint32_t sg_mem_offs[128];
    memset(sg_host_ptrs, 0, sizeof(sg_host_ptrs));
    memset(sg_lengths, 0, sizeof(sg_lengths));
    memset(sg_mem_offs, 0, sizeof(sg_mem_offs));

    /* Translate flat_binder_object / binder_fd_object / binder_buffer_object / binder_fd_array_object entries. */
    for (size_t k = 0; k < n_offs; k++) {
        uint64_t o;
        memcpy(&o, dst_offs + k * sizeof(uint64_t), sizeof(uint64_t));
        if (o + sizeof(struct sv_flat_binder_object) > in_txn->data_size)
            continue;
        uint32_t otype;
        memcpy(&otype, dst_data + o, 4);
        if (otype == SV_BINDER_TYPE_PTR && o + sizeof(struct sv_binder_buffer_object) <= in_txn->data_size) {
            struct sv_binder_buffer_object bbo;
            memcpy(&bbo, dst_data + o, sizeof(bbo));
            uint32_t blen = (uint32_t)bbo.length;
            uint8_t *dst_b = target_p->mem_base + sg_dst_off;
            if (blen > 0 && sg_src_off + blen <= sg_raw_len)
                memcpy(dst_b, sg_data + sg_src_off, blen);
            sg_src_off += blen;
            uint64_t guest_ptr = target_p->guest_base + sg_dst_off;
            bbo.buffer = guest_ptr;
            memcpy(dst_data + o, &bbo, sizeof(bbo));
            if (k < 128) {
                sg_host_ptrs[k] = dst_b;
                sg_lengths[k] = blen;
                sg_mem_offs[k] = sg_dst_off;
            }
            if ((bbo.flags & SV_BINDER_BUFFER_FLAG_HAS_PARENT) &&
                bbo.parent < k && bbo.parent < 128 && sg_host_ptrs[bbo.parent] &&
                bbo.parent_offset + 8 <= sg_lengths[bbo.parent]) {
                memcpy(sg_host_ptrs[bbo.parent] + bbo.parent_offset, &guest_ptr, 8);
            }
            sg_dst_off += align8(blen ? blen : 8);
            continue;
        }
        if (otype == SV_BINDER_TYPE_FDA && o + sizeof(struct sv_binder_fd_array_object) <= in_txn->data_size) {
            struct sv_binder_fd_array_object fda;
            memcpy(&fda, dst_data + o, sizeof(fda));
            if (fda.parent < k && fda.parent < 128 && sg_host_ptrs[fda.parent]) {
                for (uint64_t j = 0; j < fda.num_fds; j++) {
                    uint64_t foff = fda.parent_offset + j * 4;
                    if (foff + 4 > sg_lengths[fda.parent])
                        break;
                    int32_t fidx = -1;
                    memcpy(&fidx, sg_host_ptrs[fda.parent] + foff, 4);
                    if (fidx >= 0 && (uint32_t)fidx < in_fd_count && in_fds[fidx] >= 0 &&
                        msg->fd_count < VDEV_MAX_FDS) {
                        int duped = fcntl(in_fds[fidx], F_DUPFD_CLOEXEC, 3);
                        if (duped >= 0) {
                            msg->fds[msg->fd_count] = duped;
                            msg->fd_patch_offsets[msg->fd_count] = sg_mem_offs[fda.parent] + foff;
                            msg->fd_count++;
                        }
                    }
                }
            }
            continue;
        }
        struct sv_flat_binder_object obj;
        memcpy(&obj, dst_data + o, sizeof(obj));
        if (obj.type == SV_BINDER_TYPE_BINDER || obj.type == SV_BINDER_TYPE_WEAK_BINDER) {
            bool is_strong = (obj.type == SV_BINDER_TYPE_BINDER);
            struct bnode *node = get_or_create_node(sender_p->proc_id, obj.binder, obj.cookie);
            if (node && target_p->proc_id != sender_p->proc_id) {
                uint32_t nid = node->node_id;
                uint32_t h = 0;
                if (nid != g_ctx_mgr_node_id) {
                    struct bhandle *he = get_or_create_handle_entry(target_p, nid);
                    if (he) {
                        h = he->handle;
                        he->in_flight++;
                        struct bnode *nd = find_node(nid);
                        if (nd) nd->strong_ref_total++;
                        if (alloc_idx >= 0)
                            mem_block_add_handle(&target_p->allocs[alloc_idx], h);
                    } else {
                        fprintf(stderr, "[binderd] ERROR: failed to allocate handle for proc=%u\n", target_p->proc_id);
                        h = UINT32_MAX;
                    }
                }
                node_inc_refs(find_node(nid), is_strong, sender_t);
                obj.type = is_strong ? SV_BINDER_TYPE_HANDLE : SV_BINDER_TYPE_WEAK_HANDLE;
                obj.handle = h;
                obj.cookie = 0;
                memcpy(dst_data + o, &obj, sizeof(obj));
                DLOG("  obj BINDER(proc=%u ptr=0x%llx) -> HANDLE(%u) in proc=%u",
                     sender_p->proc_id, (unsigned long long)obj.binder, h, target_p->proc_id);
            }
        } else if (obj.type == SV_BINDER_TYPE_HANDLE || obj.type == SV_BINDER_TYPE_WEAK_HANDLE) {
            bool is_strong = (obj.type == SV_BINDER_TYPE_HANDLE);
            struct bnode *node = lookup_handle_node(sender_p, obj.handle);
            if (node) {
                uint32_t nid = node->node_id;
                if (node->owner_proc_id == target_p->proc_id) {
                    obj.type = is_strong ? SV_BINDER_TYPE_BINDER : SV_BINDER_TYPE_WEAK_BINDER;
                    obj.binder = node->ptr;
                    obj.cookie = node->cookie;
                } else {
                    uint32_t h = 0;
                    if (nid != g_ctx_mgr_node_id) {
                        struct bhandle *he = get_or_create_handle_entry(target_p, nid);
                        if (he) {
                            h = he->handle;
                            he->in_flight++;
                            struct bnode *nd = find_node(nid);
                            if (nd) nd->strong_ref_total++;
                            if (alloc_idx >= 0)
                                mem_block_add_handle(&target_p->allocs[alloc_idx], h);
                        } else {
                            fprintf(stderr, "[binderd] ERROR: failed to forward handle for proc=%u\n", target_p->proc_id);
                            h = UINT32_MAX;
                        }
                        node_inc_refs(find_node(nid), is_strong, NULL);
                    }
                    obj.handle = h;
                    obj.cookie = 0;
                }
                memcpy(dst_data + o, &obj, sizeof(obj));
            }
        } else if (obj.type == SV_BINDER_TYPE_FD) {
            struct sv_binder_fd_object fobj;
            memcpy(&fobj, dst_data + o, sizeof(fobj));
            uint32_t fidx = fobj.fd;
            if (fidx < in_fd_count && in_fds[fidx] >= 0 && msg->fd_count < VDEV_MAX_FDS) {
                int duped = fcntl(in_fds[fidx], F_DUPFD_CLOEXEC, 3);
                if (duped >= 0) {
                    msg->fds[msg->fd_count] = duped;
                    /* Byte offset inside target_p->memfd of fobj.fd (offsetof=8). */
                    msg->fd_patch_offsets[msg->fd_count] = mem_off + o + 8;
                    msg->fd_count++;
                }
            }
        }
    }

    /* Re-resolve target_node in case get_or_create_node() reallocated g_nodes. */
    target_node = target_node_id ? find_node(target_node_id) : NULL;

    struct sv_binder_transaction_data out_txn;
    memset(&out_txn, 0, sizeof(out_txn));
    if (!is_reply && target_node) {
        out_txn.target.ptr = target_node->ptr;
        out_txn.cookie = target_node->cookie;
    }
    out_txn.code = in_txn->code;
    out_txn.flags = in_txn->flags;
    out_txn.sender_pid = (int32_t)sender_p->pid;
    out_txn.sender_euid = sender_p->uid;
    out_txn.data_size = in_txn->data_size;
    out_txn.offsets_size = in_txn->offsets_size;
    out_txn.data.ptr.buffer = target_p->guest_base + mem_off;
    out_txn.data.ptr.offsets = target_p->guest_base + mem_off + data_aligned;

    memcpy(msg->payload, &out_txn, sizeof(out_txn));
    msg->payload_len = sizeof(out_txn);
    msg->data_guest_ptr = out_txn.data.ptr.buffer;

    DLOG("%s proc=%u(pid=%u) -> proc=%u(pid=%u) handle=%u code=0x%x flags=0x%x data=%llu offs=%llu",
         is_reply ? "BC_REPLY" : "BC_TXN",
         sender_p->proc_id, sender_p->pid, target_p->proc_id, target_p->pid,
         in_txn->target.handle, in_txn->code, in_txn->flags,
         (unsigned long long)in_txn->data_size, (unsigned long long)in_txn->offsets_size);

    /* Sender always gets BR_TRANSACTION_COMPLETE first. */
    push_direct(sender_t, make_simple_br(SV_BR_TRANSACTION_COMPLETE, NULL, 0));

    if (is_reply) {
        push_direct(reply_t, msg);
        try_deliver_to_thread(reply_t);
    } else if (is_oneway && target_node) {
        /* Linux Binder rule: serialize TF_ONE_WAY transactions per target node
         * until BC_FREE_BUFFER releases the previous oneway buffer. */
        if (target_node->has_async_txn) {
            msg->next = NULL;
            if (target_node->async_tail)
                target_node->async_tail->next = msg;
            else
                target_node->async_head = msg;
            target_node->async_tail = msg;
        } else {
            target_node->has_async_txn = true;
            target_node->async_buf_ptr = msg->data_guest_ptr;
            target_p->async_node_count++;
            push_inbox(target_p, msg);
            wake_proc_waiters(target_p);
        }
    } else {
        if (!is_oneway)
            sender_t->waiting_reply_count++;
        /* Linux Binder nested-transaction rule: if a synchronous transaction
         * calls back into a process that is currently waiting in our caller
         * stack (e.g. SurfaceFlinger -> HWC registerCallback -> onHotplug),
         * deliver directly to that waiting caller thread to avoid deadlock. */
        struct bthread *nested_target = NULL;
        if (!is_oneway) {
            struct bthread *cur = sender_t;
            for (int depth = 0; depth < 16 && cur && cur->reply_top > 0; depth++) {
                struct bthread *caller = find_thread(cur->reply_stack[cur->reply_top - 1]);
                if (!caller || !caller->used)
                    break;
                if (caller->proc_id == target_p->proc_id) {
                    nested_target = caller;
                    break;
                }
                cur = caller;
            }
        }
        if (nested_target) {
            push_direct(nested_target, msg);
            try_deliver_to_thread(nested_target);
        } else {
            push_inbox(target_p, msg);
            wake_proc_waiters(target_p);
        }
    }
}

static void handle_binder_wr(struct bthread *t, const uint8_t *buf, uint32_t len,
                             int *in_fds, uint32_t in_fd_count)
{
    struct bproc *proc = find_proc(t->proc_id);
    if (!proc || len < sizeof(struct vdev_binder_wr_req)) {
        send_reply(t->rpc_fd, VDEV_OP_BINDER_WR, t->proc_id, -EINVAL, NULL, 0, NULL, 0);
        return;
    }
    struct vdev_binder_wr_req wreq;
    memcpy(&wreq, buf, sizeof(wreq));
    const uint8_t *p = buf + sizeof(wreq);
    uint32_t plen = len - (uint32_t)sizeof(wreq);
    if (wreq.packed_len < plen)
        plen = wreq.packed_len;

    uint32_t off = 0;
    while (off + 4 <= plen) {
        uint32_t cmd;
        memcpy(&cmd, p + off, 4);
        uint32_t asz = (cmd >> 16) & 0x3fffu;
        if (off + 4 + asz > plen)
            break;
        const uint8_t *arg = p + off + 4;

        if (cmd == SV_BC_TRANSACTION || cmd == SV_BC_REPLY ||
            cmd == SV_BC_TRANSACTION_SG || cmd == SV_BC_REPLY_SG) {
            struct sv_binder_transaction_data txn;
            memcpy(&txn, arg, sizeof(txn));
            uint32_t extra = (uint32_t)(txn.data_size + txn.offsets_size);
            if (off + 4 + asz + extra > plen)
                break;
            const uint8_t *tdata = arg + asz;
            const uint8_t *toffs = tdata + txn.data_size;
            uint32_t sg_raw_len = 0;
            size_t n_offs = (size_t)txn.offsets_size / sizeof(uint64_t);
            for (size_t k = 0; k < n_offs; k++) {
                uint64_t o;
                memcpy(&o, toffs + k * sizeof(uint64_t), sizeof(uint64_t));
                if (o + sizeof(struct sv_binder_buffer_object) <= txn.data_size) {
                    uint32_t otype;
                    memcpy(&otype, tdata + o, 4);
                    if (otype == SV_BINDER_TYPE_PTR) {
                        struct sv_binder_buffer_object bbo;
                        memcpy(&bbo, tdata + o, sizeof(bbo));
                        sg_raw_len += (uint32_t)bbo.length;
                    }
                }
            }
            if (off + 4 + asz + extra + sg_raw_len > plen)
                break;
            const uint8_t *sg_data = toffs + txn.offsets_size;
            handle_transaction(t, proc, cmd, &txn, tdata, toffs, sg_data, sg_raw_len, in_fds, in_fd_count);
            off += 4 + asz + extra + sg_raw_len;
            continue;
        }
        switch (cmd) {
        case SV_BC_FREE_BUFFER: {
            uint64_t ptr;
            memcpy(&ptr, arg, 8);
            proc_mem_free(proc, ptr);
            /* Advance serialized TF_ONE_WAY queue on any node whose active async buffer was freed. */
            if (proc->async_node_count > 0) {
                for (int ni = 0; ni < g_nodes_cap; ni++) {
                    struct bnode *nd = &g_nodes[ni];
                    if (nd->used && nd->owner_proc_id == proc->proc_id &&
                        nd->has_async_txn && nd->async_buf_ptr == ptr) {
                        if (nd->async_head) {
                            struct bmsg *nxt = nd->async_head;
                            nd->async_head = nxt->next;
                            if (!nd->async_head) nd->async_tail = NULL;
                            nxt->next = NULL;
                            nd->async_buf_ptr = nxt->data_guest_ptr;
                            push_inbox(proc, nxt);
                            wake_proc_waiters(proc);
                        } else {
                            nd->has_async_txn = false;
                            nd->async_buf_ptr = 0;
                            if (proc->async_node_count > 0) proc->async_node_count--;
                            node_check_release(nd);
                        }
                        break;
                    }
                }
            }
            break;
        }
        case SV_BC_ENTER_LOOPER:
            t->is_looper = true;
            break;
        case SV_BC_REGISTER_LOOPER:
            t->is_looper = true;
            if (!t->is_registered_looper) {
                t->is_registered_looper = true;
                if (proc->requested_threads > 0)
                    proc->requested_threads--;
                proc->requested_threads_started++;
            }
            break;
        case SV_BC_EXIT_LOOPER:
            t->is_looper = false;
            break;
        case SV_BC_INCREFS:
        case SV_BC_ACQUIRE: {
            uint32_t h = 0;
            memcpy(&h, arg, 4);
            struct bhandle *he = find_handle_entry(proc, h);
            struct bnode *n = lookup_handle_node(proc, h);
            if (he) {
                if (cmd == SV_BC_ACQUIRE) {
                    he->strong_refs++;
                    he->released_strong = false;
                    if (n) n->strong_ref_total++;
                } else {
                    he->weak_refs++;
                }
            }
            if (n)
                node_inc_refs(n, cmd == SV_BC_ACQUIRE, NULL);
            break;
        }
        case SV_BC_RELEASE:
        case SV_BC_DECREFS: {
            uint32_t h = 0;
            memcpy(&h, arg, 4);
            struct bhandle *he = find_handle_entry(proc, h);
            if (he) {
                uint32_t nid = he->node_id;
                if (cmd == SV_BC_RELEASE) {
                    if (he->strong_refs > 0) {
                        he->strong_refs--;
                        struct bnode *nd = nid ? find_node(nid) : NULL;
                        if (nd && nd->strong_ref_total > 0)
                            nd->strong_ref_total--;
                    }
                    if (he->strong_refs == 0) {
                        he->released_strong = true;
                        if (nid != 0)
                            node_check_release(find_node(nid));
                    }
                } else if (cmd == SV_BC_DECREFS && he->weak_refs > 0) {
                    he->weak_refs--;
                }
                maybe_reclaim_handle(proc, he);
            }
            break;
        }
        case SV_BC_INCREFS_DONE:
        case SV_BC_ACQUIRE_DONE: {
            if (asz >= 16) {
                uint64_t ptr = 0;
                memcpy(&ptr, arg, 8);
                struct bnode *node = find_node_by_ptr(proc->proc_id, ptr);
                if (node) {
                    if (cmd == SV_BC_INCREFS_DONE)
                        node->pending_weak_ref = false;
                    else
                        node->pending_strong_ref = false;
                    node_check_release(node);
                }
            }
            break;
        }
        case SV_BC_DEAD_BINDER_DONE: {
            uint64_t cookie = 0;
            if (asz >= 8)
                memcpy(&cookie, arg, 8);
            if (proc->handles) {
                for (int hi = 0; hi < proc->handles_cap; hi++) {
                    struct bhandle *he = &proc->handles[hi];
                    if (he->used && he->death_sent && he->death_cookie == cookie) {
                        he->death_sent = false;
                        maybe_reclaim_handle(proc, he);
                        break;
                    }
                }
            }
            break;
        }
        case SV_BC_REQUEST_DEATH_NOTIFICATION: {
            uint32_t handle = 0;
            uint64_t cookie = 0;
            if (asz >= sizeof(struct sv_binder_handle_cookie)) {
                struct sv_binder_handle_cookie hc;
                memcpy(&hc, arg, sizeof(hc));
                handle = hc.handle;
                cookie = hc.cookie;
            }
            struct bhandle *he = find_handle_entry(proc, handle);
            if (he) {
                struct bnode *n = find_node(he->node_id);
                if (!n || !n->used) {
                    he->death_sent = true;
                    he->death_cookie = cookie;
                    push_direct(t, make_simple_br(SV_BR_DEAD_BINDER, &cookie, 8));
                } else {
                    he->has_death = true;
                    he->death_cookie = cookie;
                }
            } else if (handle != 0) {
                /* Unknown/already-reclaimed handle: notify caller immediately that target is dead. */
                push_direct(t, make_simple_br(SV_BR_DEAD_BINDER, &cookie, 8));
            }
            break;
        }
        case SV_BC_CLEAR_DEATH_NOTIFICATION: {
            uint32_t handle = 0;
            uint64_t cookie = 0;
            if (asz >= sizeof(struct sv_binder_handle_cookie)) {
                struct sv_binder_handle_cookie hc;
                memcpy(&hc, arg, sizeof(hc));
                handle = hc.handle;
                cookie = hc.cookie;
            }
            struct bhandle *he = find_handle_entry(proc, handle);
            if (he) {
                he->has_death = false;
                maybe_reclaim_handle(proc, he);
            }
            push_direct(t, make_simple_br(SV_BR_CLEAR_DEATH_NOTIFICATION_DONE, &cookie, 8));
            break;
        }
        default:
            DLOG("unhandled BC cmd=0x%x asz=%u", cmd, asz);
            break;
        }
        off += 4 + asz;
    }

    if (wreq.read_cap == 0) {
        struct vdev_binder_wr_rep wrep;
        memset(&wrep, 0, sizeof(wrep));
        wrep.write_consumed = wreq.raw_write_size;
        send_reply(t->rpc_fd, VDEV_OP_BINDER_WR, t->proc_id, 0, &wrep, sizeof(wrep), NULL, 0);
        if (proc->inbox_head)
            signal_proc_notify(proc);
        return;
    }

    t->waiting_read = true;
    t->parked_read_cap = wreq.read_cap;
    t->parked_write_consumed = wreq.raw_write_size;
    try_deliver_to_thread(t);
}

/* ---- Connection lifecycle ---- */
static void drop_conn(struct conn_entry *ce)
{
    int fd = ce->fd;
    epoll_ctl(g_epfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
    if (ce->kind == CONN_THREAD) {
        struct bthread *t = find_thread(ce->owner_id);
        if (t) {
            /* Unblock any synchronous callers waiting on this dead thread. */
            while (t->reply_top > 0) {
                uint32_t caller_tid = t->reply_stack[--t->reply_top];
                struct bthread *ct = find_thread(caller_tid);
                if (ct && ct->used) {
                    push_direct(ct, make_simple_br(SV_BR_DEAD_REPLY, NULL, 0));
                    try_deliver_to_thread(ct);
                }
            }
            while (t->direct_head)
                free_bmsg(pop_direct(t));
            if (t->is_registered_looper) {
                struct bproc *tp = find_proc(t->proc_id);
                if (tp && tp->requested_threads_started > 0)
                    tp->requested_threads_started--;
            }
            thread_hash_remove((int)(t - g_threads));
            t->used = false;
        }
    } else if (ce->kind == CONN_NOTIFY) {
        struct bproc *p = find_proc(ce->owner_id);
        if (p) {
            DLOG("proc=%u (pid=%u) closed", p->proc_id, p->pid);
            if (p->mem_base && p->mem_base != MAP_FAILED)
                munmap(p->mem_base, p->mem_size);
            if (p->memfd >= 0)
                close(p->memfd);
            while (p->inbox_head) {
                struct bmsg *m = pop_inbox(p);
                if (m->br_cmd == SV_BR_TRANSACTION && !m->is_oneway) {
                    struct bthread *ct = find_thread(m->reply_to_thread_id);
                    if (ct && ct->used) {
                        push_direct(ct, make_simple_br(SV_BR_DEAD_REPLY, NULL, 0));
                        try_deliver_to_thread(ct);
                    }
                }
                free_bmsg(m);
            }
            /* Free any overflow handle arrays tracked in pending mem_blocks. */
            for (int ai = 0; ai < MAX_ALLOCS; ai++) {
                if (p->allocs[ai].extra_handles) {
                    free(p->allocs[ai].extra_handles);
                    p->allocs[ai].extra_handles = NULL;
                    p->allocs[ai].extra_cap = 0;
                }
                p->allocs[ai].handle_count = 0;
                p->allocs[ai].used = false;
            }
            /* Release all handles held by this dying process so target owners get BR_RELEASE/BR_DECREFS. */
            uint32_t *rel_nodes = NULL;
            int rel_count = 0;
            if (p->handles && p->handles_cap > 0) {
                rel_nodes = (uint32_t *)malloc((size_t)p->handles_cap * sizeof(uint32_t));
                for (int hi = 0; hi < p->handles_cap; hi++) {
                    if (p->handles[hi].used) {
                        uint32_t nid = p->handles[hi].node_id;
                        if (nid != 0) {
                            struct bnode *nd = find_node(nid);
                            if (nd) {
                                int32_t sr = p->handles[hi].strong_refs + p->handles[hi].in_flight;
                                nd->strong_ref_total = (nd->strong_ref_total > sr) ? (nd->strong_ref_total - sr) : 0;
                                if (nd->handle_count > 0) nd->handle_count--;
                            }
                            if (rel_nodes)
                                rel_nodes[rel_count++] = nid;
                        }
                        p->handles[hi].used = false;
                        p->handles[hi].node_id = 0;
                        p->handles[hi].strong_refs = 0;
                        p->handles[hi].weak_refs = 0;
                        p->handles[hi].in_flight = 0;
                        p->handles[hi].has_death = false;
                        p->handles[hi].death_sent = false;
                    }
                }
                free(p->handles);
                p->handles = NULL;
                p->handles_cap = 0;
            }
            for (int ri = 0; ri < rel_count; ri++)
                node_check_release(find_node(rel_nodes[ri]));
            free(rel_nodes);

            for (int i = 0; i < g_nodes_cap; i++) {
                if (g_nodes[i].used && g_nodes[i].owner_proc_id == p->proc_id) {
                    uint32_t dead_nid = g_nodes[i].node_id;
                    /* Deliver BR_DEAD_BINDER to any live process watching this node,
                     * and detach dead_nid from every handle referencing it so slots reclaim. */
                    for (int pi = 0; pi < MAX_PROCS; pi++) {
                        struct bproc *op = &g_procs[pi];
                        if (!op->used || op->proc_id == p->proc_id || !op->handles)
                            continue;
                        for (int hi = 0; hi < op->handles_cap; hi++) {
                            struct bhandle *oh = &op->handles[hi];
                            if (oh->used && oh->node_id == dead_nid) {
                                if (oh->has_death) {
                                    uint64_t cookie = oh->death_cookie;
                                    oh->has_death = false;
                                    oh->death_sent = true;
                                    push_inbox(op, make_simple_br(SV_BR_DEAD_BINDER, &cookie, 8));
                                    wake_proc_waiters(op);
                                }
                                oh->node_id = 0;
                                maybe_reclaim_handle(op, oh);
                            }
                        }
                    }
                    if (g_ctx_mgr_node_id == g_nodes[i].node_id)
                        g_ctx_mgr_node_id = 0;
                    while (g_nodes[i].async_head) {
                        struct bmsg *am = g_nodes[i].async_head;
                        g_nodes[i].async_head = am->next;
                        free_bmsg(am);
                    }
                    g_nodes[i].async_tail = NULL;
                    g_nodes[i].has_async_txn = false;
                    node_hash_remove(i);
                    g_nodes[i].used = false;
                }
            }
            p->used = false;
        }
    }
    ce->used = false;
}

static void handle_conn_event(struct conn_entry *ce)
{
    if (ce->kind == CONN_NOTIFY) {
        /* Notify FD only exists to detect EOF/close from guest.
         * Note: recv()==0 does NOT touch errno, so check n==0 explicitly! */
        char tmp[16];
        errno = 0;
        ssize_t n = recv(ce->fd, tmp, sizeof(tmp), MSG_DONTWAIT);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            drop_conn(ce);
        return;
    }

    struct vdev_hdr hdr;
    int in_fds[VDEV_MAX_FDS];
    uint32_t in_fd_count = 0;
    if (recv_all_fds(ce->fd, &hdr, sizeof(hdr), in_fds, VDEV_MAX_FDS, &in_fd_count) < 0) {
        drop_conn(ce);
        return;
    }
    if (hdr.magic != VDEV_PROTO_MAGIC || hdr.payload_len > VDEV_MAX_PAYLOAD) {
        for (uint32_t i = 0; i < in_fd_count; i++) close(in_fds[i]);
        drop_conn(ce);
        return;
    }
    uint8_t *payload = NULL;
    if (hdr.payload_len > 0) {
        payload = (uint8_t *)malloc(hdr.payload_len);
        if (recv_all_fds(ce->fd, payload, hdr.payload_len, NULL, 0, NULL) < 0) {
            free(payload);
            for (uint32_t i = 0; i < in_fd_count; i++) close(in_fds[i]);
            drop_conn(ce);
            return;
        }
    }

    switch (hdr.op) {
    case VDEV_OP_OPEN: {
        if (hdr.payload_len < sizeof(struct vdev_open_req)) {
            send_reply(ce->fd, VDEV_OP_OPEN, 0, -EINVAL, NULL, 0, NULL, 0);
            break;
        }
        struct vdev_open_req req;
        memcpy(&req, payload, sizeof(req));
        struct bproc *p = NULL;
        for (int i = 0; i < MAX_PROCS; i++) {
            if (!g_procs[i].used) {
                p = &g_procs[i];
                break;
            }
        }
        if (!p) {
            send_reply(ce->fd, VDEV_OP_OPEN, 0, -ENOMEM, NULL, 0, NULL, 0);
            break;
        }
        int mfd = (int)syscall(SYS_memfd_create, "svbox-binder", MFD_CLOEXEC);
        if (mfd < 0 || ftruncate(mfd, PROC_MEM_SIZE) != 0) {
            if (mfd >= 0) close(mfd);
            send_reply(ce->fd, VDEV_OP_OPEN, 0, -ENOMEM, NULL, 0, NULL, 0);
            break;
        }
        void *base = mmap(NULL, PROC_MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
        if (base == MAP_FAILED) {
            close(mfd);
            send_reply(ce->fd, VDEV_OP_OPEN, 0, -ENOMEM, NULL, 0, NULL, 0);
            break;
        }
        struct bhandle *harr = (struct bhandle *)calloc(INIT_PROC_HANDLES, sizeof(struct bhandle));
        if (!harr) {
            munmap(base, PROC_MEM_SIZE);
            close(mfd);
            send_reply(ce->fd, VDEV_OP_OPEN, 0, -ENOMEM, NULL, 0, NULL, 0);
            break;
        }
        memset(p, 0, sizeof(*p));
        p->used = true;
        p->proc_id = g_next_proc_id++;
        p->pid = req.pid;
        p->uid = req.uid;
        p->gid = req.gid;
        p->notify_fd = ce->fd;
        p->memfd = mfd;
        p->mem_base = (uint8_t *)base;
        p->mem_size = PROC_MEM_SIZE;
        p->next_handle = 1;
        p->handles = harr;
        p->handles_cap = INIT_PROC_HANDLES;

        ce->kind = CONN_NOTIFY;
        ce->owner_id = p->proc_id;

        struct vdev_open_rep rep = { p->proc_id, 10, 250, PROC_MEM_SIZE };
        send_reply(ce->fd, VDEV_OP_OPEN, p->proc_id, 0, &rep, sizeof(rep), &mfd, 1);
        DLOG("OPEN proc=%u pid=%u uid=%u path=%s", p->proc_id, p->pid, p->uid, req.guest_path);
        break;
    }
    case VDEV_OP_ATTACH_THREAD: {
        if (hdr.payload_len < sizeof(struct vdev_attach_req)) {
            send_reply(ce->fd, VDEV_OP_ATTACH_THREAD, 0, -EINVAL, NULL, 0, NULL, 0);
            break;
        }
        struct vdev_attach_req req;
        memcpy(&req, payload, sizeof(req));
        struct bproc *p = find_proc(req.proc_id);
        struct bthread *t = NULL;
        for (int i = 0; i < MAX_THREADS; i++) {
            if (!g_threads[i].used) {
                t = &g_threads[i];
                break;
            }
        }
        if (!p || !t) {
            send_reply(ce->fd, VDEV_OP_ATTACH_THREAD, req.proc_id, -ENOENT, NULL, 0, NULL, 0);
            break;
        }
        memset(t, 0, sizeof(*t));
        t->used = true;
        t->thread_id = g_next_thread_id++;
        t->proc_id = req.proc_id;
        t->pid = req.pid;
        t->tid = req.tid;
        t->rpc_fd = ce->fd;
        t->hash_next = -1;
        thread_hash_insert((int)(t - g_threads));
        ce->kind = CONN_THREAD;
        ce->owner_id = t->thread_id;
        send_reply(ce->fd, VDEV_OP_ATTACH_THREAD, req.proc_id, 0, NULL, 0, NULL, 0);
        DLOG("ATTACH thread=%u proc=%u tid=%u", t->thread_id, t->proc_id, t->tid);
        break;
    }
    case VDEV_OP_MMAP: {
        struct bproc *p = find_proc(hdr.proc_id);
        if (!p || hdr.payload_len < sizeof(struct vdev_mmap_req)) {
            send_reply(ce->fd, VDEV_OP_MMAP, hdr.proc_id, -EINVAL, NULL, 0, NULL, 0);
            break;
        }
        struct vdev_mmap_req mreq;
        memcpy(&mreq, payload, sizeof(mreq));
        p->guest_base = mreq.guest_base;
        p->guest_map_len = mreq.length;
        DLOG("MMAP proc=%u guest_base=0x%llx len=%llu", p->proc_id,
             (unsigned long long)p->guest_base, (unsigned long long)p->guest_map_len);
        send_reply(ce->fd, VDEV_OP_MMAP, hdr.proc_id, 0, NULL, 0, NULL, 0);
        break;
    }
    case VDEV_OP_BINDER_CTL: {
        struct bproc *p = find_proc(hdr.proc_id);
        int32_t rc = 0;
        if (!p) {
            rc = -EBADF;
        } else if (hdr.arg0 == SV_BINDER_SET_MAX_THREADS) {
            uint32_t mt = 15;
            if (hdr.payload_len >= 4)
                memcpy(&mt, payload, 4);
            if (mt > 64) mt = 64;
            p->max_threads = mt;
            DLOG("SET_MAX_THREADS proc=%u max=%u", p->proc_id, p->max_threads);
        } else if (hdr.arg0 == SV_BINDER_SET_CONTEXT_MGR || hdr.arg0 == SV_BINDER_SET_CONTEXT_MGR_EXT) {
            if (g_ctx_mgr_node_id != 0) {
                rc = -EBUSY;
            } else {
                uint64_t ptr = 0, cookie = 0;
                if (hdr.arg0 == SV_BINDER_SET_CONTEXT_MGR_EXT && hdr.payload_len >= sizeof(struct sv_flat_binder_object)) {
                    struct sv_flat_binder_object obj;
                    memcpy(&obj, payload, sizeof(obj));
                    ptr = obj.binder;
                    cookie = obj.cookie;
                }
                struct bnode *n = get_or_create_node(p->proc_id, ptr, cookie);
                if (n) {
                    n->is_ctx_mgr = true;
                    g_ctx_mgr_node_id = n->node_id;
                    DLOG("SET_CONTEXT_MGR proc=%u (pid=%u) node=%u ptr=0x%llx",
                         p->proc_id, p->pid, n->node_id, (unsigned long long)ptr);
                } else {
                    rc = -ENOMEM;
                }
            }
        }
        send_reply(ce->fd, VDEV_OP_BINDER_CTL, hdr.proc_id, rc, NULL, 0, NULL, 0);
        break;
    }
    case VDEV_OP_BINDER_WR: {
        struct bthread *t = (ce->kind == CONN_THREAD) ? find_thread(ce->owner_id) : NULL;
        if (!t) {
            send_reply(ce->fd, VDEV_OP_BINDER_WR, hdr.proc_id, -EBADF, NULL, 0, NULL, 0);
        } else {
            handle_binder_wr(t, payload, hdr.payload_len, in_fds, in_fd_count);
        }
        break;
    }
    default:
        send_reply(ce->fd, hdr.op, hdr.proc_id, -ENOTTY, NULL, 0, NULL, 0);
        break;
    }

    free(payload);
    for (uint32_t i = 0; i < in_fd_count; i++)
        close(in_fds[i]);
}

int main(int argc, char **argv)
{
    const char *sock_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0)
            g_verbose = true;
        else
            sock_path = argv[i];
    }
    if (!sock_path) {
        fprintf(stderr, "Usage: %s [-v] <unix-socket-path>\n", argv[0]);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    for (int i = 0; i < HASH_BUCKETS; i++) {
        g_thread_buckets[i] = -1;
        g_node_id_buckets[i] = -1;
        g_node_ptr_buckets[i] = -1;
    }
    for (int i = 0; i < MAX_THREADS; i++)
        g_threads[i].hash_next = -1;

    g_nodes = (struct bnode *)calloc(INIT_NODES, sizeof(struct bnode));
    if (!g_nodes) {
        fprintf(stderr, "binderd: OOM allocating g_nodes\n");
        return 1;
    }
    g_nodes_cap = INIT_NODES;
    for (int i = 0; i < INIT_NODES; i++) {
        g_nodes[i].id_hash_next = -1;
        g_nodes[i].ptr_hash_next = -1;
    }

    unlink(sock_path);
    g_listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (g_listen_fd < 0) {
        perror("socket");
        return 1;
    }
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, sock_path, sizeof(sa.sun_path) - 1);
    if (bind(g_listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(g_listen_fd, 512) < 0) {
        perror("listen");
        return 1;
    }

    g_epfd = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = NULL };
    epoll_ctl(g_epfd, EPOLL_CTL_ADD, g_listen_fd, &ev);

    DLOG("listening on %s", sock_path);
    struct epoll_event events[256];
    for (;;) {
        int n = epoll_wait(g_epfd, events, 256, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            if (events[i].data.ptr == NULL) {
                int cfd = accept4(g_listen_fd, NULL, NULL, SOCK_CLOEXEC);
                if (cfd < 0)
                    continue;
                struct conn_entry *ce = NULL;
                for (int k = 0; k < MAX_CONNS; k++) {
                    if (!g_conns[k].used) {
                        ce = &g_conns[k];
                        break;
                    }
                }
                if (!ce) {
                    fprintf(stderr, "[binderd] ERROR: MAX_CONNS (%d) exhausted\n", MAX_CONNS);
                    close(cfd);
                    continue;
                }
                memset(ce, 0, sizeof(*ce));
                ce->used = true;
                ce->fd = cfd;
                ce->kind = CONN_NEW;
                struct epoll_event cev = { .events = EPOLLIN | EPOLLRDHUP, .data.ptr = ce };
                epoll_ctl(g_epfd, EPOLL_CTL_ADD, cfd, &cev);
            } else {
                handle_conn_event((struct conn_entry *)events[i].data.ptr);
            }
        }
    }
    return 0;
}
