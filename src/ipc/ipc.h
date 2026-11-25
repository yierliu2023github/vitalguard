/*
 * ipc.h — VitalGuard transport abstraction.
 *
 * One interface, two backends:
 *
 *   ipc_posix.c  Host development backend: Unix domain sockets in the
 *                runtime directory ($VG_RUNTIME_DIR, default
 *                /tmp/vitalguard).  Length-prefixed framing of the
 *                vg.h payloads.  This backend is the reference
 *                implementation and is what the host tests exercise.
 *
 *   ipc_qnx.c    Target backend (compiles only under __QNX__):
 *                native QNX message passing — name_attach / name_open,
 *                MsgSend / MsgReceive / MsgReply with priority
 *                inheritance on the alarm path.  See that file for
 *                the rendezvous mapping (HELLO handshake, per-conn
 *                slots, disconnect pulses).  The message layouts in
 *                vg.h are shared verbatim with the host backend.
 *
 * Semantics offered to applications (both backends):
 *   - named endpoints (VG_EP_*), one listener per name;
 *   - reliable, ordered, message-framed connections;
 *   - blocking send; receive with timeout (poll-style loops stay
 *     responsive so heartbeats and timers keep running).
 *
 * Event loops do NOT poll() backend fds directly: they call
 * vg_ipc_wait_any(), which each backend implements over its own
 * wait primitive (poll() on host; the shared channel's MsgReceive
 * pump on QNX, where a process's connections all arrive on the one
 * listener channel and are demultiplexed by the backend).  The fd
 * accessors remain as a host-backend convenience.
 */
#ifndef VG_IPC_H
#define VG_IPC_H

#include <stdint.h>
#include <stddef.h>

#define VG_IPC_MAX_PAYLOAD 4096u

typedef struct vg_ipc_conn     vg_ipc_conn_t;
typedef struct vg_ipc_listener vg_ipc_listener_t;

/* Directory holding endpoint sockets / pid & status files.
 * $VG_RUNTIME_DIR or "/tmp/vitalguard".  Created on demand. */
const char *vg_ipc_runtime_dir(void);

/* Server side: create a listener bound to `name` (VG_EP_*).
 * Returns NULL on failure.  Stale socket files are replaced. */
vg_ipc_listener_t *vg_ipc_listen(const char *name);

/* Accept one connection.  timeout_ms < 0 blocks indefinitely,
 * 0 polls, > 0 waits at most that long.  NULL on timeout/error. */
vg_ipc_conn_t *vg_ipc_accept(vg_ipc_listener_t *lst, int timeout_ms);

/* Client side: connect to `name`, retrying for up to timeout_ms
 * (the server may still be starting).  NULL on failure. */
vg_ipc_conn_t *vg_ipc_connect(const char *name, int timeout_ms);

/* Send one framed message.  Returns 0 on success, -1 on error. */
int vg_ipc_send(vg_ipc_conn_t *conn, uint32_t type,
                const void *payload, uint32_t length, uint64_t timestamp_ms);

/* Receive one framed message.
 * Returns  1 when a message was stored,
 *          0 on timeout,
 *         -1 on error / orderly peer shutdown.
 * *type_out / *ts_out receive header fields (either may be NULL);
 * payload is copied into buf (capacity buf_size); messages larger
 * than buf_size are truncated and reported as errors. */
int vg_ipc_recv(vg_ipc_conn_t *conn, uint32_t *type_out, uint64_t *ts_out,
                void *buf, uint32_t buf_size, int timeout_ms);

void vg_ipc_close(vg_ipc_conn_t *conn);
void vg_ipc_listener_close(vg_ipc_listener_t *lst);

/* ---- event-loop wait -------------------------------------------------- */

#define VG_IPC_WAIT_TIMEOUT   (-1)  /* nothing became ready in time    */
#define VG_IPC_WAIT_LISTENER  (-2)  /* listener has a pending connect  */

/* Block until the listener has a pending connection or one of
 * conns[0..nconns) has a message (or a peer disconnect) pending, for
 * at most timeout_ms (< 0 waits indefinitely, 0 polls).
 *
 * Returns VG_IPC_WAIT_LISTENER, an index into conns[], or
 * VG_IPC_WAIT_TIMEOUT.  lst may be NULL (watch conns only) and
 * conns/nconns may be NULL/0 (watch the listener only); NULL entries
 * in conns[] are skipped.
 *
 * After a conn index is returned, the caller is expected to call
 * vg_ipc_recv() on it: a pending disconnect surfaces there as a -1
 * return once queued messages are drained, exactly like a socket
 * hangup does on the host backend.
 *
 * QNX note: the watched conns must all belong to lst (accepted from
 * it), or — with lst == NULL — be a single client conn; they share
 * one receive channel, which is what the backend pumps. */
int vg_ipc_wait_any(vg_ipc_listener_t *lst, vg_ipc_conn_t **conns,
                    int nconns, int timeout_ms);

/* poll() support (host backend; the QNX backend returns -1 — its
 * event loops use vg_ipc_wait_any() instead). */
int vg_ipc_conn_fd(const vg_ipc_conn_t *conn);
int vg_ipc_listener_fd(const vg_ipc_listener_t *lst);

#endif /* VG_IPC_H */
