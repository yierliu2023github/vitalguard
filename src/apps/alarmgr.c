/*
 * alarmgr — alarm manager process.
 *
 * Owns the alarm state machine (src/alarm/): receives vitals (its
 * clock tick) and condition transitions from vitaldsp, operator
 * commands (ack / silence) from faultctl, and writes every alarm
 * transition to the audit log — stdout plus
 * $VG_RUNTIME_DIR/alarm_events.log.
 *
 * On QNX the audit trail targets slogger2; the log callback below is
 * the seam where that port lands.
 */
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vg.h"
#include "vg_time.h"
#include "vg_log.h"
#include "vg_hb.h"
#include "ipc.h"
#include "alarm.h"

#define MAX_CONNS 8

static FILE *g_audit;

static void audit_log(void *ctx, const char *line)
{
    (void)ctx;
    fprintf(stdout, "ALARM %s\n", line);
    fflush(stdout);
    if (g_audit) {
        fprintf(g_audit, "%s\n", line);
        fflush(g_audit);
    }
}

int main(void)
{
    char path[512];
    vg_ipc_listener_t *lst;
    vg_ipc_conn_t *conns[MAX_CONNS];
    vg_alarm_mgr_t *mgr;
    vg_hb_t hb;
    uint64_t last_tick_ms = 0;   /* signal-clock time of last vitals */
    int i;

    vg_log_set_tag("alarmgr");
    signal(SIGPIPE, SIG_IGN);
    snprintf(path, sizeof(path), "%s/alarm_events.log",
             vg_ipc_runtime_dir());
    g_audit = fopen(path, "a");
    if (!g_audit)
        vg_log("warning: cannot open audit log %s", path);

    mgr = vg_alarm_mgr_new(NULL, audit_log, NULL);
    if (!mgr) {
        vg_log("alarm manager init failed");
        return 1;
    }
    lst = vg_ipc_listen(VG_EP_ALARM);
    if (!lst) {
        vg_log("cannot listen on %s", VG_EP_ALARM);
        return 1;
    }
    for (i = 0; i < MAX_CONNS; i++)
        conns[i] = NULL;
    vg_hb_init(&hb, VG_PROC_ALARM);
    vg_log("alarm manager ready");

    for (;;) {
        struct pollfd pfds[1 + MAX_CONNS];
        int nfds = 0, rc;

        pfds[nfds].fd = vg_ipc_listener_fd(lst);
        pfds[nfds].events = POLLIN;
        nfds++;
        for (i = 0; i < MAX_CONNS; i++) {
            if (conns[i]) {
                pfds[nfds].fd = vg_ipc_conn_fd(conns[i]);
                pfds[nfds].events = POLLIN;
                pfds[nfds].revents = 0;
                nfds++;
            }
        }
        rc = poll(pfds, nfds, 200);
        vg_hb_beat(&hb);
        if (rc <= 0)
            continue;

        if (pfds[0].revents & POLLIN) {
            vg_ipc_conn_t *c = vg_ipc_accept(lst, 0);
            if (c) {
                int placed = 0;
                for (i = 0; i < MAX_CONNS; i++) {
                    if (!conns[i]) {
                        conns[i] = c;
                        placed = 1;
                        break;
                    }
                }
                if (!placed)
                    vg_ipc_close(c);
            }
        }

        /* Map poll slots back to connection slots: rebuild the same
         * ordering used above. */
        {
            int slot = 1;
            for (i = 0; i < MAX_CONNS; i++) {
                uint8_t buf[VG_IPC_MAX_PAYLOAD];
                uint32_t type;
                int r;
                if (!conns[i])
                    continue;
                if (slot >= nfds)
                    break;
                if (!(pfds[slot].revents & (POLLIN | POLLHUP | POLLERR))) {
                    slot++;
                    continue;
                }
                /* Read BEFORE honouring a hangup: faultctl sends a
                 * command and closes immediately, so the socket
                 * shows POLLIN|POLLHUP together — closing on HUP
                 * first would discard the unread command. */
                if (!(pfds[slot].revents & POLLIN)) {
                    vg_ipc_close(conns[i]);
                    conns[i] = NULL;
                    slot++;
                    continue;
                }
                r = vg_ipc_recv(conns[i], &type, NULL,
                                buf, sizeof(buf), 0);
                if (r < 0 || (r == 0 &&
                              (pfds[slot].revents & (POLLHUP | POLLERR)))) {
                    vg_ipc_close(conns[i]);
                    conns[i] = NULL;
                    slot++;
                    continue;
                }
                if (r == 1) {
                    vg_alarm_note_t notes[8];
                    if (type == VG_MSG_VITALS) {
                        const vg_vitals_t *v = (const vg_vitals_t *)buf;
                        last_tick_ms = v->t_ms;
                        vg_alarm_mgr_tick(mgr, v->t_ms, notes, 8);
                    } else if (type == VG_MSG_ALARM_EVENT) {
                        const vg_alarm_event_t *ev =
                            (const vg_alarm_event_t *)buf;
                        vg_alarm_mgr_event(mgr, ev, notes, 8);
                    } else if (type == VG_MSG_ALARM_CMD) {
                        const vg_alarm_cmd_t *cmd =
                            (const vg_alarm_cmd_t *)buf;
                        /* Operator commands act at the manager's
                         * current signal-clock time: the timestamp of
                         * the most recent vitals tick. */
                        if (cmd->action == VG_ALARM_CMD_ACK)
                            vg_alarm_mgr_ack(mgr, cmd->alarm_id,
                                             last_tick_ms, notes, 8);
                        else if (cmd->action == VG_ALARM_CMD_SILENCE)
                            vg_alarm_mgr_silence(mgr, cmd->alarm_id,
                                                 last_tick_ms,
                                                 notes, 8);
                    }
                }
                slot++;
            }
        }
    }
    return 0;
}
