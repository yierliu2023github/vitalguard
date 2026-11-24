/*
 * ipc_qnx.c — TARGET BACKEND (QNX 8.0).  PORT TARGET — NOT YET VALIDATED.
 *
 * This file compiles only under __QNX__ and is the designated port of
 * ipc_posix.c to native QNX Neutrino message passing:
 *
 *   endpoint listener   ->  ChannelCreate() + a well-known name attach
 *                           (name_attach() in a global namespace, or a
 *                           path in the resource-manager namespace)
 *   client connection   ->  ConnectAttach() to the server's channel
 *   vg_ipc_send()       ->  MsgSend()   (synchronous rendezvous; the
 *                           server thread inherits the client's
 *                           priority — this is the priority-inheritance
 *                           property the alarm path relies on)
 *   vg_ipc_recv()       ->  MsgReceive() + MsgReply() once consumed
 *   asynchronous events ->  MsgSendPulse() for heartbeat/alarm pulses
 *
 * Design notes for the port (see docs/design.md, "QNX mapping"):
 *   - One channel per endpoint; the header's `type` becomes the
 *     message's _msg_info-independent discriminator, payloads are the
 *     vg.h structs verbatim (same machine, native byte order).
 *   - The alarm manager's channel is created with _NTO_CHF_FIXED_PRIORITY
 *     considerations reviewed against the SDP 8.0 docs during the port;
 *     the DSP server must reply promptly so senders are never blocked
 *     behind DSP work (receive -> queue -> reply, process async).
 *   - Timeouts map to TimerTimeout() around MsgReceive/MsgSend.
 *
 * The skeleton below fixes the intended structure.  Function bodies
 * marked PORT are to be completed and validated on the QEMU target
 * once the SDP is installed; nothing in the host build depends on
 * them, and the host backend remains the reference implementation.
 */

#ifdef __QNX__

#include "ipc.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

/* PORT: connection state — a QNX connection id (coid) for clients;
 * for servers, the rcvid of the currently held message is tracked by
 * the receive/reply pairing in vg_ipc_recv/vg_ipc_send. */
struct vg_ipc_conn {
    int coid;               /* ConnectAttach() id, client side       */
    int chid;               /* channel we receive on, server side    */
    int rcvid;              /* sender awaiting MsgReply, or -1       */
};

struct vg_ipc_listener {
    int chid;               /* ChannelCreate() id                    */
    name_attach_t *attach;  /* global name registration              */
};

const char *vg_ipc_runtime_dir(void)
{
    /* PORT: on target, runtime artefacts live under /tmp as on host. */
    const char *dir = getenv("VG_RUNTIME_DIR");
    return (dir && dir[0]) ? dir : "/tmp/vitalguard";
}

vg_ipc_listener_t *vg_ipc_listen(const char *name)
{
    vg_ipc_listener_t *lst = calloc(1, sizeof(*lst));
    if (!lst)
        return NULL;
    /* PORT: lst->chid = ChannelCreate(0);
     *       lst->attach = name_attach(NULL, name, 0);
     * Fail cleanly (return NULL) until the port lands so no caller
     * mistakes this skeleton for a working transport. */
    (void)name;
    free(lst);
    return NULL;
}

vg_ipc_conn_t *vg_ipc_accept(vg_ipc_listener_t *lst, int timeout_ms)
{
    /* PORT: block in MsgReceive() on lst->chid (with TimerTimeout for
     * timeout_ms) and wrap the first sender as a connection whose
     * replies are MsgReply()s to its rcvid. */
    (void)lst; (void)timeout_ms;
    return NULL;
}

vg_ipc_conn_t *vg_ipc_connect(const char *name, int timeout_ms)
{
    /* PORT: name_open(name, 0) with the same retry-until-deadline
     * policy as the POSIX backend. */
    (void)name; (void)timeout_ms;
    return NULL;
}

int vg_ipc_send(vg_ipc_conn_t *conn, uint32_t type,
                const void *payload, uint32_t length, uint64_t timestamp_ms)
{
    /* PORT: build an iov from {header, payload} and MsgSend() it on
     * conn->coid; the rendezvous gives us priority inheritance and
     * flow control for free. */
    (void)conn; (void)type; (void)payload; (void)length; (void)timestamp_ms;
    return -1;
}

int vg_ipc_recv(vg_ipc_conn_t *conn, uint32_t *type_out, uint64_t *ts_out,
                void *buf, uint32_t buf_size, int timeout_ms)
{
    /* PORT: MsgReceive() into a staging buffer; copy out the payload;
     * MsgReply() the sender immediately after queueing (never hold a
     * sender blocked across DSP/alarm work). */
    (void)conn; (void)type_out; (void)ts_out;
    (void)buf; (void)buf_size; (void)timeout_ms;
    return -1;
}

void vg_ipc_close(vg_ipc_conn_t *conn)
{
    /* PORT: ConnectDetach(conn->coid); free. */
    free(conn);
}

void vg_ipc_listener_close(vg_ipc_listener_t *lst)
{
    /* PORT: name_detach(lst->attach, 0); ChannelDestroy(lst->chid). */
    free(lst);
}

int vg_ipc_conn_fd(const vg_ipc_conn_t *conn)
{
    /* QNX apps wait with MsgReceivePulse / ionotify equivalents;
     * this accessor exists only for source compatibility. */
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
