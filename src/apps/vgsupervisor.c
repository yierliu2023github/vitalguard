/*
 * vgsupervisor — host supervisor for the VitalGuard process set.
 *
 * Launches vitalgen / vitaldsp / alarmgr in dependency order
 * (alarm -> dsp -> gen), collects their heartbeats, restarts them
 * when they die or go silent (policy in src/supervisor/), accepts
 * the demo's "kill dsp" fault command, and publishes a status file
 * ($VG_RUNTIME_DIR/supervisor.status) that faultctl and the tests
 * read.
 *
 * On QNX this role is played by QPLMS + slm configuration, not by a
 * hand-written process; the host supervisor exists to develop and
 * prove the *policy* (ordering, backoff, budgets, liveness) before
 * the port.  See docs/design.md.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>

#include "vg.h"
#include "vg_time.h"
#include "vg_log.h"
#include "vg_path.h"
#include "ipc.h"
#include "supervisor.h"

#define MAX_CONNS 16

static vg_sup_t g_sup;
static char     g_bindir[PATH_MAX];
static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int spawn_child(uint32_t role)
{
    vg_child_t *c = vg_sup_by_role(&g_sup, role);
    char path[PATH_MAX], logpath[PATH_MAX];
    pid_t pid;
    int fd;

    if (!c)
        return -1;
    if (snprintf(path, sizeof(path), "%s/%s", g_bindir, c->name) >=
        (int)sizeof(path))
        return -1;
    snprintf(logpath, sizeof(logpath), "%s/%s.log",
             vg_ipc_runtime_dir(), c->name);

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        fd = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO)
                close(fd);
        }
        execl(path, c->name, (char *)NULL);
        _exit(127);
    }
    vg_sup_started(&g_sup, role, pid, vg_now_ms());
    vg_log("started %s pid=%d", c->name, (int)pid);
    return 0;
}

static void write_status(uint64_t now)
{
    char tmp[PATH_MAX], final[PATH_MAX];
    FILE *f;
    int i;

    snprintf(tmp, sizeof(tmp), "%s/supervisor.status.tmp",
             vg_ipc_runtime_dir());
    snprintf(final, sizeof(final), "%s/supervisor.status",
             vg_ipc_runtime_dir());
    f = fopen(tmp, "w");
    if (!f)
        return;
    for (i = 0; i < g_sup.n; i++) {
        vg_child_t *c = &g_sup.children[i];
        fprintf(f, "%s pid=%d restarts=%u hb_age_ms=%lld state=%s\n",
                c->name,
                c->running ? (int)c->pid : -1,
                c->restarts,
                c->running ? (long long)(now - c->last_hb_ms) : -1LL,
                c->gave_up ? "gave_up" : (c->running ? "running" : "down"));
    }
    fclose(f);
    rename(tmp, final);
}

int main(int argc, char **argv)
{
    vg_ipc_listener_t *lst;
    vg_ipc_conn_t *conns[MAX_CONNS];
    struct sigaction sa;
    uint64_t last_status = 0;
    int i;

    (void)argc;
    vg_log_set_tag("vgsupervisor");

    /* Portable self-location (see vg_path.h): $VG_BINDIR, else
     * /proc/self/exe on Linux, else argv[0]/PATH resolution — the
     * route that works on QNX, whose procfs has no exe path link. */
    if (vg_self_dir(g_bindir, sizeof(g_bindir),
                    argc > 0 ? argv[0] : NULL) != 0) {
        vg_log("cannot resolve own executable directory");
        return 1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);

    lst = vg_ipc_listen(VG_EP_SUPERVISOR);
    if (!lst) {
        vg_log("cannot listen on %s", VG_EP_SUPERVISOR);
        return 1;
    }
    for (i = 0; i < MAX_CONNS; i++)
        conns[i] = NULL;

    vg_sup_init(&g_sup);
    vg_sup_add(&g_sup, "alarmgr",  VG_PROC_ALARM);
    vg_sup_add(&g_sup, "vitaldsp", VG_PROC_DSP);
    vg_sup_add(&g_sup, "vitalgen", VG_PROC_GEN);

    /* Dependency order: alarm manager first, then DSP, then source. */
    spawn_child(VG_PROC_ALARM);
    vg_sleep_ms(300);
    spawn_child(VG_PROC_DSP);
    vg_sleep_ms(300);
    spawn_child(VG_PROC_GEN);

    while (!g_stop) {
        int w, status;
        uint64_t now;
        pid_t pid;

        w = vg_ipc_wait_any(lst, conns, MAX_CONNS, 200);
        now = vg_now_ms();

        if (w == VG_IPC_WAIT_LISTENER) {
            vg_ipc_conn_t *c = vg_ipc_accept(lst, 0);
            if (c) {
                int placed = 0;
                for (i = 0; i < MAX_CONNS; i++)
                    if (!conns[i]) {
                        conns[i] = c;
                        placed = 1;
                        break;
                    }
                if (!placed)
                    vg_ipc_close(c);
            }
        } else if (w >= 0) {
            /* One connection is ready.  vg_ipc_recv() drains queued
             * messages before reporting a disconnect (-1): the
             * native form of the read-before-hangup rule — one-shot
             * clients (faultctl) send and close immediately, and
             * their command must still be delivered. */
            uint8_t buf[VG_IPC_MAX_PAYLOAD];
            uint32_t type;
            int r = vg_ipc_recv(conns[w], &type, NULL, buf, sizeof(buf), 0);
            if (r < 0) {
                vg_ipc_close(conns[w]);
                conns[w] = NULL;
            } else if (r == 1 && type == VG_MSG_HEARTBEAT) {
                const vg_heartbeat_t *hb = (const vg_heartbeat_t *)buf;
                vg_sup_heartbeat(&g_sup, hb->sender, hb->seq, now);
            } else if (r == 1 && type == VG_MSG_FAULT_CMD) {
                const vg_fault_cmd_t *cmd = (const vg_fault_cmd_t *)buf;
                if (cmd->kind == VG_FAULT_KILL_DSP) {
                    vg_child_t *dsp = vg_sup_by_role(&g_sup, VG_PROC_DSP);
                    if (dsp && dsp->running) {
                        vg_log("fault: killing vitaldsp pid=%d",
                               (int)dsp->pid);
                        kill(dsp->pid, SIGKILL);
                    }
                }
            }
        }

        /* Reap children and apply restart policy. */
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            vg_child_t *c = vg_sup_by_pid(&g_sup, pid);
            if (c) {
                int64_t backoff = vg_sup_exited(&g_sup, c->role, now);
                if (backoff >= 0)
                    vg_log("%s exited; restart in %lld ms",
                           c->name, (long long)backoff);
                else
                    vg_log("%s exited; restart budget exhausted, "
                           "leaving it down", c->name);
            }
        }

        /* Hung children: silence past the heartbeat timeout. */
        for (i = 0; i < g_sup.n; i++) {
            vg_child_t *c = &g_sup.children[i];
            if (vg_sup_hung(c, now)) {
                vg_log("%s silent for %llu ms; killing",
                       c->name, (unsigned long long)(now - c->last_hb_ms));
                kill(c->pid, SIGKILL);
                /* mark heartbeat fresh so we don't kill every loop
                 * until waitpid reaps it */
                c->last_hb_ms = now;
            }
        }

        /* Due restarts. */
        for (i = 0; i < g_sup.n; i++) {
            vg_child_t *c = &g_sup.children[i];
            if (!c->running && !c->gave_up && c->next_restart_ms &&
                now >= c->next_restart_ms)
                spawn_child(c->role);
        }

        if (now - last_status >= 1000) {
            last_status = now;
            write_status(now);
        }
    }

    vg_log("shutting down; terminating children");
    for (i = 0; i < g_sup.n; i++) {
        vg_child_t *c = &g_sup.children[i];
        if (c->running)
            kill(c->pid, SIGTERM);
    }
    vg_sleep_ms(300);
    for (i = 0; i < g_sup.n; i++) {
        vg_child_t *c = &g_sup.children[i];
        if (c->running)
            kill(c->pid, SIGKILL);
    }
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    return 0;
}
