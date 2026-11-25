/*
 * ipc_qnx.c — TARGET BACKEND (QNX 8.0): native Neutrino message passing.
 *
 * This file compiles only under __QNX__ and implements the ipc.h
 * interface over MsgSend/MsgReceive/MsgReply, following the pattern
 * proven by the Day 3–4 spike (qemu/spike/):
 *
 *   endpoint listener   ->  name_attach(NULL, name, 0): one channel
 *                           per endpoint, registered in the global
 *                           name space (/dev/name/local/<name>).
 *   client connection   ->  name_open() + a HELLO rendezvous: the
 *                           client MsgSends an internal HELLO carrying
 *                           its pid and its own private channel id;
 *                           the server records the connection under
 *                           the kernel-supplied scoid of that send and
 *                           ConnectAttach()es back to the client's
 *                           channel (the server->client send path,
 *                           used by echo-style peers; the pipeline
 *                           apps themselves only send client->server).
 *   vg_ipc_send()       ->  MsgSend() on the conn's coid.  Synchronous
 *                           rendezvous: the sender inherits priority
 *                           to the server thread while it handles the
 *                           message — the priority-inheritance property
 *                           the alarm path relies on.
 *   vg_ipc_recv()       ->  messages are received by the backend
 *                           "pump" (below), copied into a per-conn
 *                           slot and the sender MsgReply()d at once —
 *                           receive -> queue -> reply, never hold a
 *                           sender blocked across DSP/alarm work.
 *
 * The pump is the heart of the port.  All connections accepted from
 * one listener arrive on that listener's single channel, so the
 * backend demultiplexes by the sender's scoid (struct _msg_info on
 * receive; struct _pulse.scoid on disconnect pulses) into per-conn
 * slots:
 *
 *   slot A — next message for vg_ipc_recv(); its sender was replied
 *            the moment it was queued.
 *   slot B — one more received message whose sender is deliberately
 *            left unreplied (rcvid held) until slot A drains: with
 *            synchronous senders this bounds each conn to two
 *            messages in flight and gives hard backpressure beyond
 *            that, instead of an unbounded queue.
 *
 * Peer death surfaces exactly like a socket hangup on the host
 * backend: the kernel delivers _PULSE_CODE_DISCONNECT on the channel
 * (name_attach channels get it — observed in the spike, code -33);
 * the conn is marked dead, queued messages still drain, and then
 * vg_ipc_recv() returns -1 and vg_ipc_wait_any() reports the conn
 * ready so the app can close it.
 *
 * vg_ipc_wait_any() is how the app event loops wait: it scans the
 * slots, then pumps the shared channel with TimerTimeout() armed for
 * the remaining budget.  Timeouts elsewhere map to TimerTimeout()
 * around MsgReceive the same way.
 *
 * The runtime directory is still used on target — not for sockets,
 * but for the artefacts that live there on both backends (audit log,
 * supervisor status file, child logs); vg_ipc_listen() creates it.
 */

#ifdef __QNX__

#include "ipc.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "vg_time.h"

/* ---- wire framing (backend-private; payloads are the vg.h structs) -- */

struct vg_ipc_hdr {
    uint32_t type;
    uint32_t length;
    uint64_t ts;
};

/* Internal handshake message, sent once by vg_ipc_connect(). */
#define VG_IPC_TYPE_HELLO 0xFFFFFF01u

struct vg_ipc_hello {
    uint32_t pid;           /* sender pid (cross-checked vs _msg_info) */
    uint32_t chid;          /* sender's private channel (reply path)   */
};

/* MsgReply status values: the value MsgSend() returns to the sender. */
#define VG_IPC_REPLY_OK   0
#define VG_IPC_REPLY_FAIL 1

/* ---- state ------------------------------------------------------------ */

typedef struct {
    int      valid;
    uint32_t type;
    uint64_t ts;
    uint32_t len;
    uint8_t  data[VG_IPC_MAX_PAYLOAD];
} vg_ipc_slot_t;

struct vg_ipc_chan {
    int             chid;
    name_attach_t  *attach;     /* listener channels only, else NULL   */
    vg_ipc_conn_t **conns;      /* every live conn receiving here      */
    int             nconns;
    int             cap;
    vg_ipc_conn_t **pending;    /* conns awaiting vg_ipc_accept()      */
    int             npending;
    int             pcap;
    vg_ipc_conn_t  *self_conn;  /* client channels: the owning conn    */
};

struct vg_ipc_conn {
    struct vg_ipc_chan *chan;   /* channel we receive on               */
    int  coid;                  /* send path to the peer's channel     */
    int  scoid;                 /* peer's connection id on our channel,
                                   or -1 until its first message       */
    int  dead;                  /* disconnect pulse seen for the peer  */
    vg_ipc_slot_t a;            /* next message for vg_ipc_recv()      */
    vg_ipc_slot_t b;            /* received; sender awaiting reply     */
    int  b_rcvid;               /* rcvid held for slot b, or -1        */
};

struct vg_ipc_listener {
    struct vg_ipc_chan *chan;
};

/* ---- small registry helpers ------------------------------------------- */

static int ptr_array_push(vg_ipc_conn_t ***arr, int *n, int *cap,
                          vg_ipc_conn_t *c)
{
    if (*n >= *cap) {
        int ncap = *cap ? *cap * 2 : 4;
        vg_ipc_conn_t **na = realloc(*arr, (size_t)ncap * sizeof(*na));
        if (!na)
            return -1;
        *arr = na;
        *cap = ncap;
    }
    (*arr)[(*n)++] = c;
    return 0;
}

static void ptr_array_remove(vg_ipc_conn_t **arr, int *n, vg_ipc_conn_t *c)
{
    int i;
    for (i = 0; i < *n; i++) {
        if (arr[i] == c) {
            memmove(&arr[i], &arr[i + 1],
                    (size_t)(*n - i - 1) * sizeof(*arr));
            (*n)--;
            return;
        }
    }
}

static vg_ipc_conn_t *chan_find_scoid(struct vg_ipc_chan *chan, int scoid)
{
    int i;
    for (i = 0; i < chan->nconns; i++)
        if (chan->conns[i]->scoid == scoid)
            return chan->conns[i];
    return NULL;
}

static vg_ipc_conn_t *conn_new(struct vg_ipc_chan *chan)
{
    vg_ipc_conn_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->chan   = chan;
    c->coid   = -1;
    c->scoid  = -1;
    c->b_rcvid = -1;
    if (ptr_array_push(&chan->conns, &chan->nconns, &chan->cap, c) != 0) {
        free(c);
        return NULL;
    }
    return c;
}

/* ---- the pump ------------------------------------------------------------ */

/* Deliver one received message to its conn's slots (see header). */
static void chan_deliver(vg_ipc_conn_t *conn, const struct vg_ipc_hdr *hdr,
                         const uint8_t *payload, int rcvid)
{
    vg_ipc_slot_t *slot;

    if (!conn->a.valid)
        slot = &conn->a;
    else if (!conn->b.valid)
        slot = &conn->b;
    else {
        /* Unreachable with synchronous senders (a client cannot have
         * a third send in flight); reject rather than wedge the pump. */
        MsgReply(rcvid, VG_IPC_REPLY_FAIL, NULL, 0);
        return;
    }
    slot->type = hdr->type;
    slot->ts   = hdr->ts;
    slot->len  = hdr->length;
    if (hdr->length > 0)
        memcpy(slot->data, payload, hdr->length);
    slot->valid = 1;
    if (slot == &conn->a)
        MsgReply(rcvid, VG_IPC_REPLY_OK, NULL, 0);
    else
        conn->b_rcvid = rcvid;      /* reply deferred = backpressure */
}

/* Receive and dispatch one kernel event on chan.
 * Returns 1 when something was dispatched, 0 on timeout, -1 on error. */
static int chan_pump(struct vg_ipc_chan *chan, int timeout_ms)
{
    uint8_t raw[sizeof(struct vg_ipc_hdr) + VG_IPC_MAX_PAYLOAD];
    struct _msg_info info;
    int rcvid;

    if (timeout_ms >= 0) {
        uint64_t ns = (uint64_t)timeout_ms * 1000000ull;
        if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE,
                         NULL, &ns, NULL) == -1)
            return -1;
    }
    rcvid = MsgReceive(chan->chid, raw, sizeof(raw), &info);
    if (rcvid == -1)
        return (errno == ETIMEDOUT) ? 0 : -1;

    if (rcvid == 0) {                       /* a pulse, not a message */
        struct _pulse *pulse = (struct _pulse *)raw;
        if (pulse->code == _PULSE_CODE_DISCONNECT) {
            vg_ipc_conn_t *c = chan_find_scoid(chan, pulse->scoid);
            if (c)
                c->dead = 1;
            else if (chan->self_conn)
                chan->self_conn->dead = 1;
        }
        return 1;
    }

    {
        const struct vg_ipc_hdr *hdr = (const struct vg_ipc_hdr *)raw;
        const uint8_t *payload = raw + sizeof(*hdr);
        vg_ipc_conn_t *conn;

        if (hdr->length > VG_IPC_MAX_PAYLOAD) {
            MsgReply(rcvid, VG_IPC_REPLY_FAIL, NULL, 0);
            return 1;
        }

        if (hdr->type == VG_IPC_TYPE_HELLO) {
            const struct vg_ipc_hello *hello =
                (const struct vg_ipc_hello *)payload;
            conn = chan_find_scoid(chan, info.scoid);
            if (!conn) {
                conn = conn_new(chan);
                if (conn) {
                    conn->scoid = info.scoid;
                    /* Reply path: attach to the client's channel. */
                    conn->coid = ConnectAttach(0, (pid_t)info.pid,
                                               (int)hello->chid,
                                               _NTO_SIDE_CHANNEL, 0);
                    ptr_array_push(&chan->pending, &chan->npending,
                                   &chan->pcap, conn);
                }
            }
            MsgReply(rcvid, conn ? VG_IPC_REPLY_OK : VG_IPC_REPLY_FAIL,
                     NULL, 0);
            return 1;
        }

        conn = chan_find_scoid(chan, info.scoid);
        if (!conn && chan->self_conn) {
            /* Our private client channel: the only possible peer is
             * the server we connected to. */
            conn = chan->self_conn;
            conn->scoid = info.scoid;
        }
        if (!conn) {
            /* A client that skipped HELLO (our own connect() never
             * does): adopt it as an implicit connection so its
             * messages are not lost.  It has no known reply channel,
             * so sends on this conn will fail cleanly. */
            conn = conn_new(chan);
            if (!conn) {
                MsgReply(rcvid, VG_IPC_REPLY_FAIL, NULL, 0);
                return 1;
            }
            conn->scoid = info.scoid;
            ptr_array_push(&chan->pending, &chan->npending,
                           &chan->pcap, conn);
        }
        chan_deliver(conn, hdr, payload, rcvid);
        return 1;
    }
}

/* Pop conn's next queued message into the caller's buffer.
 * 1 = message, 0 = nothing queued, -1 = peer gone, queue drained,
 * or queued message larger than the caller's buffer. */
static int conn_pop(vg_ipc_conn_t *conn, uint32_t *type_out,
                    uint64_t *ts_out, void *buf, uint32_t buf_size)
{
    vg_ipc_slot_t *a = &conn->a;
    int rc;

    if (!a->valid && conn->b.valid) {   /* defensive; see chan_deliver */
        conn->a = conn->b;
        conn->b.valid = 0;
        if (conn->b_rcvid != -1) {
            MsgReply(conn->b_rcvid, VG_IPC_REPLY_OK, NULL, 0);
            conn->b_rcvid = -1;
        }
        a = &conn->a;
    }
    if (!a->valid)
        return conn->dead ? -1 : 0;

    if (a->len > buf_size) {
        rc = -1;
    } else {
        if (a->len > 0)
            memcpy(buf, a->data, a->len);
        if (type_out)
            *type_out = a->type;
        if (ts_out)
            *ts_out = a->ts;
        rc = 1;
    }
    a->valid = 0;
    if (conn->b.valid) {                /* promote b, release sender */
        conn->a = conn->b;
        conn->b.valid = 0;
        if (conn->b_rcvid != -1) {
            MsgReply(conn->b_rcvid, VG_IPC_REPLY_OK, NULL, 0);
            conn->b_rcvid = -1;
        }
    }
    return rc;
}

static uint64_t deadline_from(int timeout_ms)
{
    if (timeout_ms < 0)
        return UINT64_MAX;
    return vg_now_ms() + (uint64_t)timeout_ms;
}

static int remaining_ms(uint64_t deadline)
{
    uint64_t now = vg_now_ms();
    if (deadline == UINT64_MAX)
        return -1;
    if (now >= deadline)
        return 0;
    return (int)(deadline - now);
}

/* ---- public interface ---------------------------------------------------- */

const char *vg_ipc_runtime_dir(void)
{
    const char *dir = getenv("VG_RUNTIME_DIR");
    return (dir && dir[0]) ? dir : "/tmp/vitalguard";
}

vg_ipc_listener_t *vg_ipc_listen(const char *name)
{
    vg_ipc_listener_t *lst;
    struct vg_ipc_chan *chan;
    name_attach_t *attach;

    /* The runtime dir holds the audit log / status files on target
     * too (no sockets there — those are the host backend's). */
    mkdir(vg_ipc_runtime_dir(), 0777);

    attach = name_attach(NULL, name, 0);
    if (!attach)
        return NULL;

    chan = calloc(1, sizeof(*chan));
    lst  = calloc(1, sizeof(*lst));
    if (!chan || !lst) {
        free(chan);
        free(lst);
        name_detach(attach, 0);
        return NULL;
    }
    chan->chid   = attach->chid;
    chan->attach = attach;
    lst->chan    = chan;
    return lst;
}

vg_ipc_conn_t *vg_ipc_accept(vg_ipc_listener_t *lst, int timeout_ms)
{
    uint64_t deadline = deadline_from(timeout_ms);

    for (;;) {
        if (lst->chan->npending > 0) {
            vg_ipc_conn_t *c = lst->chan->pending[0];
            ptr_array_remove(lst->chan->pending, &lst->chan->npending, c);
            return c;
        }
        if (remaining_ms(deadline) == 0)
            return NULL;
        if (chan_pump(lst->chan, remaining_ms(deadline)) < 0)
            return NULL;
    }
}

vg_ipc_conn_t *vg_ipc_connect(const char *name, int timeout_ms)
{
    struct vg_ipc_chan *chan;
    vg_ipc_conn_t *conn;
    uint64_t deadline = deadline_from(timeout_ms);
    int chid;

    chid = ChannelCreate(_NTO_CHF_DISCONNECT);
    if (chid == -1)
        return NULL;
    chan = calloc(1, sizeof(*chan));
    if (!chan) {
        ChannelDestroy(chid);
        return NULL;
    }
    chan->chid = chid;
    conn = conn_new(chan);
    if (!conn) {
        ChannelDestroy(chid);
        free(chan);
        return NULL;
    }
    chan->self_conn = conn;

    for (;;) {
        int coid = name_open(name, 0);
        if (coid != -1) {
            struct {
                struct vg_ipc_hdr  hdr;
                struct vg_ipc_hello hello;
            } msg;
            int rc;

            memset(&msg, 0, sizeof(msg));
            msg.hdr.type   = VG_IPC_TYPE_HELLO;
            msg.hdr.length = sizeof(msg.hello);
            msg.hdr.ts     = vg_now_ms();
            msg.hello.pid  = (uint32_t)getpid();
            msg.hello.chid = (uint32_t)chid;
            rc = MsgSend(coid, &msg, sizeof(msg), NULL, 0);
            if (rc == VG_IPC_REPLY_OK) {
                conn->coid = coid;  /* sends ride the listener channel */
                return conn;
            }
            ConnectDetach(coid);
        }
        if (remaining_ms(deadline) == 0)
            break;
        vg_sleep_ms(25);
    }

    ptr_array_remove(chan->conns, &chan->nconns, conn);
    free(conn);
    ChannelDestroy(chid);
    free(chan);
    return NULL;
}

int vg_ipc_send(vg_ipc_conn_t *conn, uint32_t type,
                const void *payload, uint32_t length, uint64_t timestamp_ms)
{
    uint8_t raw[sizeof(struct vg_ipc_hdr) + VG_IPC_MAX_PAYLOAD];
    struct vg_ipc_hdr *hdr = (struct vg_ipc_hdr *)raw;
    int rc;

    if (!conn || conn->coid == -1 || length > VG_IPC_MAX_PAYLOAD)
        return -1;
    hdr->type   = type;
    hdr->length = length;
    hdr->ts     = timestamp_ms;
    if (length > 0)
        memcpy(raw + sizeof(*hdr), payload, length);
    rc = MsgSend(conn->coid, raw, sizeof(*hdr) + length, NULL, 0);
    return (rc == VG_IPC_REPLY_OK) ? 0 : -1;
}

int vg_ipc_recv(vg_ipc_conn_t *conn, uint32_t *type_out, uint64_t *ts_out,
                void *buf, uint32_t buf_size, int timeout_ms)
{
    uint64_t deadline = deadline_from(timeout_ms);

    if (!conn)
        return -1;
    for (;;) {
        int r = conn_pop(conn, type_out, ts_out, buf, buf_size);
        if (r != 0)
            return r;
        if (remaining_ms(deadline) == 0)
            return 0;
        if (chan_pump(conn->chan, remaining_ms(deadline)) < 0)
            return -1;
    }
}

int vg_ipc_wait_any(vg_ipc_listener_t *lst, vg_ipc_conn_t **conns,
                    int nconns, int timeout_ms)
{
    struct vg_ipc_chan *chan = lst ? lst->chan
                                   : (nconns > 0 && conns ? conns[0]->chan
                                                          : NULL);
    uint64_t deadline = deadline_from(timeout_ms);

    for (;;) {
        int i;
        if (lst && lst->chan->npending > 0)
            return VG_IPC_WAIT_LISTENER;
        for (i = 0; i < nconns; i++)
            if (conns[i] && (conns[i]->a.valid || conns[i]->dead))
                return i;
        if (remaining_ms(deadline) == 0 || !chan)
            return VG_IPC_WAIT_TIMEOUT;
        if (chan_pump(chan, remaining_ms(deadline)) < 0)
            return VG_IPC_WAIT_TIMEOUT;
    }
}

void vg_ipc_close(vg_ipc_conn_t *conn)
{
    struct vg_ipc_chan *chan;

    if (!conn)
        return;
    chan = conn->chan;
    /* Release a sender held in slot b, or it would block forever. */
    if (conn->b.valid && conn->b_rcvid != -1)
        MsgReply(conn->b_rcvid, VG_IPC_REPLY_FAIL, NULL, 0);
    if (conn->coid != -1)
        ConnectDetach(conn->coid);
    ptr_array_remove(chan->pending, &chan->npending, conn);
    ptr_array_remove(chan->conns, &chan->nconns, conn);
    if (chan->self_conn == conn) {
        /* A client conn owns its private channel. */
        ChannelDestroy(chan->chid);
        free(chan->conns);
        free(chan->pending);
        free(chan);
    }
    free(conn);
}

void vg_ipc_listener_close(vg_ipc_listener_t *lst)
{
    struct vg_ipc_chan *chan;
    int i;

    if (!lst)
        return;
    chan = lst->chan;
    for (i = chan->nconns - 1; i >= 0; i--)
        vg_ipc_close(chan->conns[i]);
    name_detach(chan->attach, 0);
    free(chan->conns);
    free(chan->pending);
    free(chan);
    free(lst);
}

int vg_ipc_conn_fd(const vg_ipc_conn_t *conn)
{
    /* No fd exists on the native backend; event loops use
     * vg_ipc_wait_any().  Kept for source compatibility. */
    (void)conn;
    return -1;
}

int vg_ipc_listener_fd(const vg_ipc_listener_t *lst)
{
    (void)lst;
    return -1;
}

#else  /* !__QNX__ */

/* Host builds compile this file to an empty translation unit; the
 * POSIX backend provides the implementation.  (ISO C forbids a
 * completely empty file, hence this typedef.) */
typedef int vg_ipc_qnx_unused_t;

#endif /* __QNX__ */
