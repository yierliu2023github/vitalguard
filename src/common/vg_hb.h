/*
 * vg_hb.h — heartbeat client helper used by every supervised child.
 *
 * Each child connects to the supervisor endpoint and emits a
 * VG_MSG_HEARTBEAT every VG_HB_PERIOD_MS of wall time.  Sending is
 * best-effort: if the supervisor is not up (or restarts), the helper
 * silently retries — a child must never die because its monitor blinked.
 * On QNX the same helper maps onto a pulse/message to the QPLMS/slm
 * monitor channel.
 */
#ifndef VG_HB_H
#define VG_HB_H

#include <stdint.h>
#include "vg.h"
#include "ipc.h"

#define VG_HB_PERIOD_MS 500u

typedef struct {
    uint32_t       role;        /* VG_PROC_*                       */
    uint32_t       seq;
    uint64_t       last_ms;     /* wall time of last send attempt  */
    vg_ipc_conn_t *conn;        /* NULL when currently unconnected */
} vg_hb_t;

/* Initialise (does not connect yet). */
void vg_hb_init(vg_hb_t *hb, uint32_t role);

/* Call from the process main loop; sends a heartbeat when due. */
void vg_hb_beat(vg_hb_t *hb);

/* Release the connection, if any. */
void vg_hb_close(vg_hb_t *hb);

#endif /* VG_HB_H */
