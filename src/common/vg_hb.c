#include "vg_hb.h"
#include "vg_time.h"

void vg_hb_init(vg_hb_t *hb, uint32_t role)
{
    hb->role    = role;
    hb->seq     = 0;
    hb->last_ms = 0;
    hb->conn    = NULL;
}

void vg_hb_beat(vg_hb_t *hb)
{
    uint64_t now = vg_now_ms();
    if (now - hb->last_ms < VG_HB_PERIOD_MS)
        return;
    hb->last_ms = now;

    if (!hb->conn) {
        /* Short retry budget: connecting must not stall the main loop. */
        hb->conn = vg_ipc_connect(VG_EP_SUPERVISOR, 200);
        if (!hb->conn)
            return;
    }

    vg_heartbeat_t beat;
    beat.sender = hb->role;
    beat.seq    = ++hb->seq;
    if (vg_ipc_send(hb->conn, VG_MSG_HEARTBEAT, &beat, sizeof(beat), now) != 0) {
        vg_ipc_close(hb->conn);
        hb->conn = NULL;
    }
}

void vg_hb_close(vg_hb_t *hb)
{
    if (hb->conn) {
        vg_ipc_close(hb->conn);
        hb->conn = NULL;
    }
}
