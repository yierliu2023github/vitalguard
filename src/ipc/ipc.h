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
 *                native QNX message passing — ChannelCreate /
 *                ConnectAttach / MsgSend / MsgReceive / MsgReply with
 *                priority inheritance on the alarm path.  It is a
 *                clearly marked port-target skeleton; the message
 *                layouts in vg.h are shared verbatim.
 *
 * Semantics offered to applications (both backends):
 *   - named endpoints (VG_EP_*), one listener per name;
 *   - reliable, ordered, message-framed connections;
 *   - blocking send; receive with timeout (poll-style loops stay
 *     responsive so heartbeats and timers keep running).
 *
 * The fd accessors are a host-backend convenience used by the apps to
 * build poll() sets; the QNX backend provides equivalents over its
 * channel ids so the app structure survives the port.
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

/* poll() support (host backend; QNX backend maps to its wait set). */
int vg_ipc_conn_fd(const vg_ipc_conn_t *conn);
int vg_ipc_listener_fd(const vg_ipc_listener_t *lst);

#endif /* VG_IPC_H */
