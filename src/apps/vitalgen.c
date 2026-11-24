/*
 * vitalgen — synthetic patient (sensor simulator).
 *
 * Generates ECG / pleth / respiration blocks at 10 Hz and streams
 * them to vitaldsp.  Listens on its own endpoint for fault-injection
 * commands from faultctl (rhythm changes, desaturations, apnea) —
 * the same console the demo drives live.
 *
 * On QNX this process keeps its shape; only the transport underneath
 * changes (see src/ipc/).
 */
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vg.h"
#include "vg_time.h"
#include "vg_log.h"
#include "vg_hb.h"
#include "ipc.h"
#include "signals.h"

static vg_ecg_t  *g_ecg;
static vg_spo2_t *g_spo2;
static vg_resp_t *g_resp;

static void handle_fault(const vg_fault_cmd_t *cmd)
{
    double dur_s = cmd->duration_ms / 1000.0;
    switch (cmd->kind) {
    case VG_FAULT_RHYTHM:
        vg_ecg_set_rhythm(g_ecg, (vg_rhythm_t)cmd->rhythm, dur_s);
        vg_log("fault: rhythm -> %u for %.1f s", cmd->rhythm, dur_s);
        break;
    case VG_FAULT_DESAT:
        vg_spo2_desat(g_spo2, cmd->fparam, dur_s);
        vg_log("fault: desaturate to %.0f%% for %.1f s", cmd->fparam, dur_s);
        break;
    case VG_FAULT_APNEA:
        vg_resp_apnea(g_resp, dur_s);
        vg_log("fault: apnea for %.1f s", dur_s);
        break;
    default:
        vg_log("fault: unknown kind %u", cmd->kind);
        break;
    }
}

int main(void)
{
    uint32_t seed = 12345;
    const char *seed_env = getenv("VG_SEED");
    vg_ipc_listener_t *fault_lst;
    vg_ipc_conn_t *fault_conn = NULL;
    vg_ipc_conn_t *dsp_conn = NULL;
    vg_hb_t hb;
    uint64_t next_block;

    vg_log_set_tag("vitalgen");
    signal(SIGPIPE, SIG_IGN);
    if (seed_env)
        seed = (uint32_t)strtoul(seed_env, NULL, 10);

    g_ecg  = vg_ecg_new(seed, 72.0f);
    g_spo2 = vg_spo2_new(seed ^ 0x51AB, 98.0f);
    g_resp = vg_resp_new(seed ^ 0x9E37, 13.0f);
    if (!g_ecg || !g_spo2 || !g_resp) {
        vg_log("generator init failed");
        return 1;
    }

    fault_lst = vg_ipc_listen(VG_EP_GEN);
    if (!fault_lst) {
        vg_log("cannot listen on %s", VG_EP_GEN);
        return 1;
    }
    vg_hb_init(&hb, VG_PROC_GEN);

    dsp_conn = vg_ipc_connect(VG_EP_DSP, 10000);
    if (!dsp_conn)
        vg_log("vitaldsp not reachable yet; will keep retrying");
    else
        vg_log("connected to vitaldsp");

    next_block = vg_now_ms();
    for (;;) {
        uint64_t now = vg_now_ms();
        int timeout = (int)(next_block > now ? next_block - now : 0);
        struct pollfd pfds[2];
        int nfds = 0, rc;

        pfds[nfds].fd = vg_ipc_listener_fd(fault_lst);
        pfds[nfds].events = POLLIN;
        nfds++;
        if (fault_conn) {
            pfds[nfds].fd = vg_ipc_conn_fd(fault_conn);
            pfds[nfds].events = POLLIN;
            nfds++;
        }
        rc = poll(pfds, nfds, timeout);
        if (rc > 0) {
            if (pfds[0].revents & POLLIN) {
                vg_ipc_conn_t *c = vg_ipc_accept(fault_lst, 0);
                if (c) {
                    if (fault_conn)
                        vg_ipc_close(fault_conn);
                    fault_conn = c;
                }
            }
            if (fault_conn && nfds > 1 && (pfds[1].revents & POLLIN)) {
                vg_fault_cmd_t cmd;
                uint32_t type;
                if (vg_ipc_recv(fault_conn, &type, NULL,
                                &cmd, sizeof(cmd), 0) == 1 &&
                    type == VG_MSG_FAULT_CMD)
                    handle_fault(&cmd);
            }
            if (fault_conn && nfds > 1 && (pfds[1].revents & (POLLHUP | POLLERR))) {
                vg_ipc_close(fault_conn);
                fault_conn = NULL;
            }
        }

        vg_hb_beat(&hb);

        if (vg_now_ms() >= next_block) {
            vg_sample_block_t block;
            int i;
            block.t0_ms = (uint64_t)(vg_ecg_time_s(g_ecg) * 1000.0);
            for (i = 0; i < VG_BLOCK_SAMPLES; i++) {
                block.ecg[i]   = vg_ecg_next(g_ecg);
                block.pleth[i] = vg_spo2_pleth_next(
                    g_spo2, vg_ecg_nominal_hr(g_ecg));
                block.resp[i]  = vg_resp_next(g_resp);
            }
            vg_spo2_tick(g_spo2, VG_BLOCK_MS / 1000.0);
            block.spo2_pct = vg_spo2_value(g_spo2);

            if (!dsp_conn)
                dsp_conn = vg_ipc_connect(VG_EP_DSP, 200);
            if (dsp_conn &&
                vg_ipc_send(dsp_conn, VG_MSG_SAMPLE_BLOCK,
                            &block, sizeof(block), block.t0_ms) != 0) {
                vg_log("lost connection to vitaldsp; will reconnect");
                vg_ipc_close(dsp_conn);
                dsp_conn = NULL;
            }
            next_block += VG_BLOCK_MS;
            if (next_block < vg_now_ms())   /* fell behind: resync */
                next_block = vg_now_ms() + VG_BLOCK_MS;
        }
    }
    return 0;
}
