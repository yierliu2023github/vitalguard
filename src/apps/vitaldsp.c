/*
 * vitaldsp — signal-processing server.
 *
 * Receives sample blocks from vitalgen, runs the DSP core (QRS
 * detection, SpO2 averaging, respiration rate, condition persistence)
 * and forwards vitals + alarm transitions to alarmgr.
 *
 * On QNX this becomes a message-passing server whose thread inherits
 * the priority of whoever sends to it — in particular the alarm
 * manager's queries are never stuck behind routine processing
 * (priority inheritance; see docs/design.md).
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "vg.h"
#include "vg_time.h"
#include "vg_log.h"
#include "vg_hb.h"
#include "ipc.h"
#include "dsp.h"

static vg_ipc_conn_t *g_alarm_conn;

static vg_ipc_conn_t *alarm_connect(void)
{
    if (!g_alarm_conn)
        g_alarm_conn = vg_ipc_connect(VG_EP_ALARM, 200);
    return g_alarm_conn;
}

static int alarm_send(uint32_t type, const void *payload, uint32_t len,
                      uint64_t ts)
{
    if (!alarm_connect())
        return -1;
    if (vg_ipc_send(g_alarm_conn, type, payload, len, ts) != 0) {
        vg_ipc_close(g_alarm_conn);
        g_alarm_conn = NULL;
        return -1;
    }
    return 0;
}

int main(void)
{
    vg_ipc_listener_t *lst;
    vg_ipc_conn_t *gen_conn = NULL;
    vg_dsp_t *dsp;
    vg_hb_t hb;
    uint64_t last_vitals_log = 0;

    vg_log_set_tag("vitaldsp");
    signal(SIGPIPE, SIG_IGN);
    dsp = vg_dsp_new(NULL);
    if (!dsp) {
        vg_log("dsp init failed");
        return 1;
    }
    lst = vg_ipc_listen(VG_EP_DSP);
    if (!lst) {
        vg_log("cannot listen on %s", VG_EP_DSP);
        return 1;
    }
    vg_hb_init(&hb, VG_PROC_DSP);
    if (alarm_connect())
        vg_log("connected to alarmgr");

    for (;;) {
        vg_ipc_conn_t *watch[1];
        int nwatch = 0, w, r;
        vg_sample_block_t block;
        vg_vitals_t vitals;
        vg_alarm_event_t events[8];
        uint32_t type;
        int nev, i;

        if (gen_conn)
            watch[nwatch++] = gen_conn;
        w = vg_ipc_wait_any(gen_conn ? NULL : lst, watch, nwatch, 200);
        vg_hb_beat(&hb);
        if (w == VG_IPC_WAIT_TIMEOUT)
            continue;

        if (w == VG_IPC_WAIT_LISTENER) {
            gen_conn = vg_ipc_accept(lst, 0);
            if (gen_conn)
                vg_log("vitalgen connected");
            continue;
        }

        /* gen_conn is ready: a block, or its peer went away (recv
         * reports that as -1 once the queue is drained). */
        r = vg_ipc_recv(gen_conn, &type, NULL,
                        &block, sizeof(block), 0);
        if (r < 0) {
            vg_log("vitalgen connection lost");
            vg_ipc_close(gen_conn);
            gen_conn = NULL;
            continue;
        }
        if (r != 1 || type != VG_MSG_SAMPLE_BLOCK)
            continue;

        nev = vg_dsp_process_block(dsp, &block, &vitals,
                                   events, 8);
        alarm_send(VG_MSG_VITALS, &vitals, sizeof(vitals),
                   vitals.t_ms);
        for (i = 0; i < nev; i++)
            alarm_send(VG_MSG_ALARM_EVENT, &events[i],
                       sizeof(events[i]), events[i].t_event_ms);

        if (vitals.t_ms - last_vitals_log >= 5000) {
            last_vitals_log = vitals.t_ms;
            vg_log("t=%llus HR=%.0f SpO2=%.1f RR=%.0f cond=0x%x",
                   (unsigned long long)(vitals.t_ms / 1000),
                   vitals.hr_bpm, vitals.spo2_pct, vitals.resp_rate,
                   vitals.conditions_mask);
        }
    }
    return 0;
}
